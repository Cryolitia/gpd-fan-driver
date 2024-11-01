// SPDX-License-Identifier: GPL-2.0+

/* Platform driver for GPD devices that expose fan control via hwmon sysfs.
 *
 * Fan control is provided via pwm interface in the range [0-255].
 * Each model has a different range in the EC, the written value is scaled to accommodate for that.
 *
 * Based on this repo:
 * https://github.com/Cryolitia/gpd-fan-driver
 *
 * Copyright (c) 2024 Cryolitia PukNgae
 */

#include <linux/acpi.h>
#include <linux/dmi.h>
#include <linux/hwmon.h>
#include <linux/ioport.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/platform_device.h>

#define DRIVER_NAME "gpdfan"

// model param, see document
static char *gpd_fan_board = "";
module_param(gpd_fan_board, charp, 0444);

// EC read/write locker
// Should never access EC at the same time, otherwise system down.
static DEFINE_MUTEX(gpd_fan_lock);

enum gpd_board {
	win_mini,
	win4_6800u,
	win_max_2,
};

enum FAN_PWM_ENABLE {
	DISABLE = 0,
	MANUAL = 1,
	AUTOMATIC = 2,
};

static struct {
	enum FAN_PWM_ENABLE pwm_enable;
	u8 pwm_value;

	u16 fan_speed_cached;
	u8 read_pwm_cached;

	unsigned long fan_speed_last_update;
	unsigned long read_pwm_last_update;

	const struct gpd_board_quirk *quirk;
} gpd_driver_priv;

struct gpd_board_quirk {
	const char *board_name; /* Board name for module param comparison */
	const enum gpd_board board;

	const u8 addr_port;
	const u8 data_port;
	const u16 manual_control_enable;
	const u16 rpm_read;
	const u16 pwm_write;
	const u16 pwm_max;
};

static struct gpd_board_quirk gpd_win_mini_quirk = {
	.board_name = "win_mini",
	.board = win_mini,

	.addr_port = 0x4E,
	.data_port = 0x4F,
	.manual_control_enable = 0x047A,
	.rpm_read = 0x0478,
	.pwm_write = 0x047A,
	.pwm_max = 244,
};

static struct gpd_board_quirk gpd_win4_quirk = {
	.board_name = "win4",
	.board = win4_6800u,

	.addr_port = 0x2E,
	.data_port = 0x2F,
	.manual_control_enable = 0xC311,
	.rpm_read = 0xC880,
	.pwm_write = 0xC311,
	.pwm_max = 127,
};

static struct gpd_board_quirk gpd_wm2_quirk = {
	.board_name = "wm2",
	.board = win_max_2,

	.addr_port = 0x4E,
	.data_port = 0x4F,
	.manual_control_enable = 0x0275,
	.rpm_read = 0x0218,
	.pwm_write = 0x1809,
	.pwm_max = 184,
};

static const struct dmi_system_id dmi_table[] = {
	{
		// GPD Win Mini
		// GPD Win Mini with AMD Ryzen 8840U
		.matches	= {
					DMI_MATCH(DMI_SYS_VENDOR, "GPD"),
					DMI_MATCH(DMI_PRODUCT_NAME, "G1617-01")
				},
		.driver_data	= &gpd_win_mini_quirk,
	},
	{
		// GPD Win 4 with AMD Ryzen 6800U
		.matches	= {
					DMI_MATCH(DMI_SYS_VENDOR, "GPD"),
					DMI_MATCH(DMI_PRODUCT_NAME, "G1618-04"),
					DMI_MATCH(DMI_BOARD_VERSION, "Default string"),
				},
		.driver_data	= &gpd_win4_quirk,
	},
	{
		// GPD Win 4 with Ryzen 7840U
		.matches	= {
					DMI_MATCH(DMI_SYS_VENDOR, "GPD"),
					DMI_MATCH(DMI_PRODUCT_NAME, "G1618-04"),
					DMI_MATCH(DMI_BOARD_VERSION, "Ver. 1.0"),
				},
		// Since 7840U, win4 uses the same quirk as wm2
		.driver_data	= &gpd_wm2_quirk,
	},
	{
		// GPD Win Max 2 with Ryzen 6800U
		// GPD Win Max 2 2023 with Ryzen 7840U
		// GPD Win Max 2 2024 with Ryzen 8840U
		.matches	= {
					DMI_MATCH(DMI_SYS_VENDOR, "GPD"),
					DMI_MATCH(DMI_PRODUCT_NAME, "G1619-04"),
				},
		.driver_data	= &gpd_wm2_quirk,
	},
	{}
};

static const struct gpd_board_quirk *gpd_module_quirks[] = {
	&gpd_win_mini_quirk, &gpd_win4_quirk, &gpd_wm2_quirk, NULL
};

/* Helper functions to handle EC read/write */
static int gpd_ecram_read(const struct gpd_board_quirk *quirk, u16 offset,
			  u8 *val)
{
	int ret;
	u16 addr_port = quirk->addr_port;
	u16 data_port = quirk->data_port;

	ret = mutex_lock_interruptible(&gpd_fan_lock);

	if (ret)
		return ret;

	outb(0x2E, addr_port);
	outb(0x11, data_port);
	outb(0x2F, addr_port);
	outb((u8)((offset >> 8) & 0xFF), data_port);

	outb(0x2E, addr_port);
	outb(0x10, data_port);
	outb(0x2F, addr_port);
	outb((u8)(offset & 0xFF), data_port);

	outb(0x2E, addr_port);
	outb(0x12, data_port);
	outb(0x2F, addr_port);
	*val = inb(data_port);

	mutex_unlock(&gpd_fan_lock);
	return 0;
}

static int gpd_ecram_write(const struct gpd_board_quirk *quirk, u16 offset,
			   u8 value)
{
	int ret;
	u16 addr_port = quirk->addr_port;
	u16 data_port = quirk->data_port;

	ret = mutex_lock_interruptible(&gpd_fan_lock);

	if (ret)
		return ret;

	outb(0x2E, addr_port);
	outb(0x11, data_port);
	outb(0x2F, addr_port);
	outb((u8)((offset >> 8) & 0xFF), data_port);

	outb(0x2E, addr_port);
	outb(0x10, data_port);
	outb(0x2F, addr_port);
	outb((u8)(offset & 0xFF), data_port);

	outb(0x2E, addr_port);
	outb(0x12, data_port);
	outb(0x2F, addr_port);
	outb(value, data_port);

	mutex_unlock(&gpd_fan_lock);
	return 0;
}

static s32 gpd_generic_read_rpm_uncached(void)
{
	u8 high, low;
	int ret;
	const struct gpd_board_quirk *const quirk = gpd_driver_priv.quirk;

	ret = gpd_ecram_read(quirk, quirk->rpm_read, &high);
	if (ret)
		return ret;
	ret = gpd_ecram_read(quirk, quirk->rpm_read + 1, &low);
	if (ret)
		return ret;

	return (u16)high << 8 | low;
}

static s32 gpd_win4_read_rpm_uncached(void)
{
	const struct gpd_board_quirk *const quirk = gpd_driver_priv.quirk;
	u8 PWMCTR;
	s32 ret;

	gpd_ecram_read(quirk, 0x1841, &PWMCTR);
	if (PWMCTR != 0x7F)
		gpd_ecram_write(quirk, 0x1841, 0x7F);

	ret = gpd_generic_read_rpm_uncached();

	if (ret < 0)
		return ret;

	if (ret == 0) {
		//re-init EC
		u8 chip_id;

		gpd_ecram_read(quirk, 0x2000, &chip_id);
		if (chip_id == 0x55) {
			u8 chip_ver;

			if (gpd_ecram_read(quirk, 0x1060, &chip_ver))
				gpd_ecram_write(quirk, 0x1060, chip_ver | 0x80);
		}
	}
	return ret;
}

static s32 gpd_wm2_read_rpm_uncached(void)
{
	const struct gpd_board_quirk *const quirk = gpd_driver_priv.quirk;

	for (u16 pwm_ctr_offset = 0x1841; pwm_ctr_offset <= 0x1843;
	     pwm_ctr_offset++) {
		u8 PWMCTR;

		gpd_ecram_read(quirk, pwm_ctr_offset, &PWMCTR);
		if (PWMCTR != 0xB8)
			gpd_ecram_write(quirk, pwm_ctr_offset, 0xB8);
	}
	return gpd_generic_read_rpm_uncached();
}

// Read value for fan1_input
static s32 gpd_read_rpm(void)
{
	// Update per 1000 milliseconds
	if (time_after(jiffies, gpd_driver_priv.fan_speed_last_update + HZ)) {
		s32 ret = 0;

		switch (gpd_driver_priv.quirk->board) {
		case win_mini: {
			ret = gpd_generic_read_rpm_uncached();
			break;
		}
		case win4_6800u: {
			ret = gpd_win4_read_rpm_uncached();
			break;
		}
		case win_max_2: {
			ret = gpd_wm2_read_rpm_uncached();
			break;
		}
		}

		if (ret < 0)
			return ret;

		gpd_driver_priv.fan_speed_cached = ret;
		gpd_driver_priv.fan_speed_last_update = jiffies;
	}
	return gpd_driver_priv.fan_speed_cached;
}

static s16 gpd_wm2_read_pwm_uncached(void)
{
	const struct gpd_board_quirk *const quirk = gpd_driver_priv.quirk;
	u8 var;
	int ret = gpd_ecram_read(quirk, quirk->pwm_write, &var);

	if (ret < 0)
		return (s16)ret;

	return (s16)(var * 255 / quirk->pwm_max);
}

// Read value for pwm1
static s16 gpd_read_pwm(void)
{
	switch (gpd_driver_priv.quirk->board) {
	case win_mini:
	case win4_6800u:
		return gpd_driver_priv.pwm_value;
	case win_max_2:
		// Update per 1000 milliseconds
		if (time_after(jiffies,
			       gpd_driver_priv.read_pwm_last_update + HZ)) {
			s16 ret = gpd_wm2_read_pwm_uncached();

			if (ret < 0)
				return ret;

			gpd_driver_priv.read_pwm_cached = ret;
			gpd_driver_priv.read_pwm_last_update = jiffies;
		}
		return gpd_driver_priv.read_pwm_cached;
	}
	return 0;
}

static int gpd_generic_write_pwm(u8 val)
{
	const struct gpd_board_quirk *const quirk = gpd_driver_priv.quirk;
	u8 actual;

	// PWM value's range in EC is 1 - pwm_max, cast 0 - 255 to it.
	actual = val * (quirk->pwm_max - 1) / 255 + 1;
	return gpd_ecram_write(quirk, quirk->pwm_write, actual);
}

static int gpd_win_mini_write_pwm(u8 val)
{
	if (gpd_driver_priv.pwm_enable == MANUAL)
		return gpd_generic_write_pwm(val);
	return 0;
}

static int gpd_wm2_write_pwm(u8 val)
{
	if (gpd_driver_priv.pwm_enable != DISABLE)
		return gpd_generic_write_pwm(val);

	return 0;
}

// Write value for pwm1
static int gpd_write_pwm(u8 val)
{
	switch (gpd_driver_priv.quirk->board) {
	case win_mini:
		return gpd_win_mini_write_pwm(val);
	case win4_6800u:
		return gpd_generic_write_pwm(val);
	case win_max_2:
		return gpd_wm2_write_pwm(val);
	}
	return 0;
}

static int gpd_win_mini_set_pwm_enable(enum FAN_PWM_ENABLE pwm_enable)
{
	switch (pwm_enable) {
	case DISABLE:
		return gpd_generic_write_pwm(255);
	case MANUAL:
		return gpd_generic_write_pwm(gpd_driver_priv.pwm_value);
	case AUTOMATIC:
		const struct gpd_board_quirk *quirk = gpd_driver_priv.quirk;

		return gpd_ecram_write(quirk, quirk->pwm_write, 0);
	}
	return 0;
}

static int gpd_wm2_set_pwm_enable(enum FAN_PWM_ENABLE enable)
{
	const struct gpd_board_quirk *const quirk = gpd_driver_priv.quirk;
	int ret;

	switch (enable) {
	case DISABLE: {
		ret = gpd_generic_write_pwm(255);

		if (ret)
			return ret;

		return gpd_ecram_write(quirk, quirk->manual_control_enable, 1);
	}
	case MANUAL: {
		ret = gpd_generic_write_pwm(gpd_driver_priv.pwm_value);

		if (ret)
			return ret;

		return gpd_ecram_write(quirk, quirk->manual_control_enable, 1);
	}
	case AUTOMATIC: {
		ret = gpd_ecram_write(quirk, quirk->manual_control_enable, 0);

		return ret;
	}
	}
	return 0;
}

// Write value for pwm1_enable
static int gpd_set_pwm_enable(enum FAN_PWM_ENABLE enable)
{
	switch (gpd_driver_priv.quirk->board) {
	case win_mini:
	case win4_6800u:
		return gpd_win_mini_set_pwm_enable(enable);
	case win_max_2:
		return gpd_wm2_set_pwm_enable(enable);
	}
	return 0;
}

static umode_t gpd_fan_hwmon_is_visible(__always_unused const void *drvdata,
					enum hwmon_sensor_types type, u32 attr,
					__always_unused int channel)
{
	if (type == hwmon_fan && attr == hwmon_fan_input) {
		return 0444;
	} else if (type == hwmon_pwm) {
		switch (attr) {
		case hwmon_pwm_enable:
		case hwmon_pwm_input:
			return 0644;
		default:
			return 0;
		}
	}
	return 0;
}

static int gpd_fan_hwmon_read(__always_unused struct device *dev,
			      enum hwmon_sensor_types type, u32 attr,
			      __always_unused int channel, long *val)
{
	if (type == hwmon_fan) {
		if (attr == hwmon_fan_input) {
			s32 ret = gpd_read_rpm();

			if (ret < 0)
				return ret;

			*val = ret;
			return 0;
		}
		return -EOPNOTSUPP;
	}
	if (type == hwmon_pwm) {
		switch (attr) {
		case hwmon_pwm_enable:
			*val = gpd_driver_priv.pwm_enable;
			return 0;
		case hwmon_pwm_input:
			s16 ret = gpd_read_pwm();

			if (ret < 0)
				return ret;

			*val = ret;
			return 0;
		default:
			return -EOPNOTSUPP;
		}
	}
	return -EOPNOTSUPP;
}

static int gpd_fan_hwmon_write(__always_unused struct device *dev,
			       enum hwmon_sensor_types type, u32 attr,
			       __always_unused int channel, long val)
{
	if (type == hwmon_pwm) {
		switch (attr) {
		case hwmon_pwm_enable:
			if (!in_range(val, 0, 3))
				return -EINVAL;
			gpd_driver_priv.pwm_enable = val;
			return gpd_set_pwm_enable(gpd_driver_priv.pwm_enable);
		case hwmon_pwm_input:
			u8 var = clamp_val(val, 0, 255);

			gpd_driver_priv.pwm_value = var;
			return gpd_write_pwm(var);
		default:
			return -EOPNOTSUPP;
		}
	}
	return -EOPNOTSUPP;
}

static const struct hwmon_ops gpd_fan_ops = {
	.is_visible = gpd_fan_hwmon_is_visible,
	.read = gpd_fan_hwmon_read,
	.write = gpd_fan_hwmon_write,
};

static const struct hwmon_channel_info *gpd_fan_hwmon_channel_info[] = {
	HWMON_CHANNEL_INFO(fan, HWMON_F_INPUT),
	HWMON_CHANNEL_INFO(pwm, HWMON_PWM_INPUT | HWMON_PWM_ENABLE), NULL
};

static struct hwmon_chip_info gpd_fan_chip_info = {
	.ops = &gpd_fan_ops,
	.info = gpd_fan_hwmon_channel_info
};

static int gpd_fan_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct gpd_driver_priv *data;
	const struct resource *plat_res;
	const struct device *dev_reg;
	const struct resource *region_res;

	data = dev_get_platdata(&pdev->dev);
	if (IS_ERR(data))
		return -ENODEV;

	plat_res = platform_get_resource(pdev, IORESOURCE_IO, 0);
	if (IS_ERR(plat_res))
		return dev_err_probe(dev, PTR_ERR(plat_res),
				     "Failed to get platform resource\n");

	region_res = devm_request_region(dev, plat_res->start,
					 resource_size(plat_res), DRIVER_NAME);
	if (IS_ERR(region_res))
		return dev_err_probe(dev, PTR_ERR(region_res),
				     "Failed to request region\n");

	dev_reg = devm_hwmon_device_register_with_info(
		dev, DRIVER_NAME, data, &gpd_fan_chip_info, NULL);
	if (IS_ERR(dev_reg))
		return dev_err_probe(dev, PTR_ERR(region_res),
				     "Failed to register hwmon device\n");

	return 0;
}

static int gpd_fan_remove(__always_unused struct platform_device *pdev)
{
	gpd_driver_priv.pwm_enable = AUTOMATIC;
	gpd_set_pwm_enable(AUTOMATIC);

	return 0;
}

static struct platform_driver gpd_fan_driver = {
	.probe	= gpd_fan_probe,
	.remove	= gpd_fan_remove,
	.driver	= {
		.name	= KBUILD_MODNAME,
	},
};

static struct platform_device *gpd_fan_platform_device;

static int __init gpd_fan_init(void)
{
	const struct gpd_board_quirk *match = NULL;

	for (const struct gpd_board_quirk **p = gpd_module_quirks; *p; p++) {
		if (strcmp(gpd_fan_board, (*p)->board_name) == 0) {
			match = *p;
			break;
		}
	}

	if (!match)
		match = dmi_first_match(dmi_table)->driver_data;

	if (!match)
		return -ENODEV;

	unsigned long jiffies_latest = jiffies - HZ;

	gpd_driver_priv.pwm_enable = AUTOMATIC;
	gpd_driver_priv.pwm_value = 255;
	gpd_driver_priv.fan_speed_cached = 0;
	gpd_driver_priv.read_pwm_cached = 0;
	gpd_driver_priv.fan_speed_last_update = jiffies_latest;
	gpd_driver_priv.read_pwm_last_update = jiffies_latest;
	gpd_driver_priv.quirk = match;

	struct resource gpd_fan_resources[] = {
		{
			.start = match->addr_port,
			.end = match->data_port,
			.flags = IORESOURCE_IO,
		},
	};

	gpd_fan_platform_device = platform_create_bundle(
		&gpd_fan_driver, gpd_fan_probe, gpd_fan_resources, 1, NULL, 0);

	if (IS_ERR(gpd_fan_platform_device)) {
		pr_warn("Failed to create platform device\n");
		return PTR_ERR(gpd_fan_platform_device);
	}

	return 0;
}

static void __exit gpd_fan_exit(void)
{
	platform_device_unregister(gpd_fan_platform_device);
	platform_driver_unregister(&gpd_fan_driver);
}

MODULE_DEVICE_TABLE(dmi, dmi_table);

module_init(gpd_fan_init) module_exit(gpd_fan_exit)

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Cryolitia <Cryolitia@gmail.com>");
MODULE_DESCRIPTION("GPD Devices fan control driver");
