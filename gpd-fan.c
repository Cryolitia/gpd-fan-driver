// SPDX-License-Identifier: GPL-2.0+

/* Platform driver for GPD devices that expose fan control via hwmon sysfs.
 *
 * Fan control is provided via pwm interface in the range [0-255].
 * Each model has a different range in the EC, the written value is scaled to
 * accommodate for that.
 *
 * Based on this repo:
 * https://github.com/Cryolitia/gpd-fan-driver
 *
 * Copyright (c) 2024 Cryolitia PukNgae
 */

#define OUT_OF_TREE

#include <linux/acpi.h>
#include <linux/dmi.h>
#include <linux/hwmon.h>
#include <linux/ioport.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/platform_device.h>

#ifdef OUT_OF_TREE
#include <linux/debugfs.h>
#include <linux/hwmon-sysfs.h>
#include <linux/jiffies.h>
#include <linux/version.h>
#endif

#define DRIVER_NAME "gpdfan"
#define GPD_PWM_CTR_OFFSET 0x1841

static char *gpd_fan_board = "";
module_param(gpd_fan_board, charp, 0444);

// EC read/write locker
// Should never access EC at the same time, otherwise system down.
static DEFINE_MUTEX(gpd_fan_lock);

enum gpd_board {
	win_mini,
	win4_6800u,
	win_max_2,
	duo,
};

enum FAN_PWM_ENABLE {
	DISABLE		= 0,
	MANUAL		= 1,
	AUTOMATIC	= 2,
};

static struct {
	enum FAN_PWM_ENABLE pwm_enable;
	u8 pwm_value;

#ifdef OUT_OF_TREE
	u16 read_rpm_cached;
	u8 read_pwm_cached;

	// minium 1000 mill seconds
	u32 update_interval_per_second;

	unsigned long read_rpm_last_update;
	unsigned long read_pwm_last_update;
#endif

	const struct gpd_fan_drvdata *drvdata;
} gpd_driver_priv;

struct gpd_fan_drvdata {
	const char *board_name; /* Board name for module param comparison */
	const enum gpd_board board;

	const u8 addr_port;
	const u8 data_port;
	const u16 manual_control_enable;
	const u16 rpm_read;
	const u16 pwm_write;
	const u16 pwm_max;
};

static struct gpd_fan_drvdata gpd_win_mini_drvdata = {
	.board_name		= "win_mini",
	.board			= win_mini,

	.addr_port		= 0x4E,
	.data_port		= 0x4F,
	.manual_control_enable	= 0x047A,
	.rpm_read		= 0x0478,
	.pwm_write		= 0x047A,
	.pwm_max		= 244,
};

static struct gpd_fan_drvdata gpd_duo_drvdata = {
	.board_name		= "duo",
	.board			= duo,

	.addr_port		= 0x4E,
	.data_port		= 0x4F,
	.manual_control_enable	= 0x047A,
	.rpm_read		= 0x0478,
	.pwm_write		= 0x047A,
	.pwm_max		= 244,
};

static struct gpd_fan_drvdata gpd_win4_drvdata = {
	.board_name		= "win4",
	.board			= win4_6800u,

	.addr_port		= 0x2E,
	.data_port		= 0x2F,
	.manual_control_enable	= 0xC311,
	.rpm_read		= 0xC880,
	.pwm_write		= 0xC311,
	.pwm_max		= 127,
};

static struct gpd_fan_drvdata gpd_wm2_drvdata = {
	.board_name		= "wm2",
	.board			= win_max_2,

	.addr_port		= 0x4E,
	.data_port		= 0x4F,
	.manual_control_enable	= 0x0275,
	.rpm_read		= 0x0218,
	.pwm_write		= 0x1809,
	.pwm_max		= 184,
};

static const struct dmi_system_id dmi_table[] = {
	{
		// GPD Win Mini
		// GPD Win Mini with AMD Ryzen 8840U
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "GPD"),
			DMI_MATCH(DMI_PRODUCT_NAME, "G1617-01")
		},
		.driver_data = &gpd_win_mini_drvdata,
	},
	{
		// GPD Win Mini
		// GPD Win Mini with AMD Ryzen HX370
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "GPD"),
			DMI_MATCH(DMI_PRODUCT_NAME, "G1617-02")
		},
		.driver_data = &gpd_win_mini_drvdata,
	},
	{
		// GPD Win Mini
		// GPD Win Mini with AMD Ryzen HX370
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "GPD"),
			DMI_MATCH(DMI_PRODUCT_NAME, "G1617-02-L")
		},
		.driver_data = &gpd_win_mini_drvdata,
	},
	{
		// GPD Win 4 with AMD Ryzen 6800U
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "GPD"),
			DMI_MATCH(DMI_PRODUCT_NAME, "G1618-04"),
			DMI_MATCH(DMI_BOARD_VERSION, "Default string"),
		},
		.driver_data = &gpd_win4_drvdata,
	},
	{
		// GPD Win 4 with Ryzen 7840U
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "GPD"),
			DMI_MATCH(DMI_PRODUCT_NAME, "G1618-04"),
			DMI_MATCH(DMI_BOARD_VERSION, "Ver. 1.0"),
		},
		// Since 7840U, win4 uses the same drvdata as wm2
		.driver_data = &gpd_wm2_drvdata,
	},
	{
		// GPD Win 4 with Ryzen 7840U (another)
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "GPD"),
			DMI_MATCH(DMI_PRODUCT_NAME, "G1618-04"),
			DMI_MATCH(DMI_BOARD_VERSION, "Ver.1.0"),
		},
		.driver_data = &gpd_wm2_drvdata,
	},
	{
		// GPD Win Max 2 with Ryzen 6800U
		// GPD Win Max 2 2023 with Ryzen 7840U
		// GPD Win Max 2 2024 with Ryzen 8840U
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "GPD"),
			DMI_MATCH(DMI_PRODUCT_NAME, "G1619-04"),
		},
		.driver_data = &gpd_wm2_drvdata,
	},
	{
		// GPD Win Max 2 with AMD Ryzen HX370
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "GPD"),
			DMI_MATCH(DMI_PRODUCT_NAME, "G1619-05"),
		},
		.driver_data = &gpd_wm2_drvdata,
	},
	{
		// GPD Duo
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "GPD"),
			DMI_MATCH(DMI_PRODUCT_NAME, "G1622-01"),
		},
		.driver_data = &gpd_duo_drvdata,
	},
	{
		// GPD Duo (another)
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "GPD"),
			DMI_MATCH(DMI_PRODUCT_NAME, "G1622-01-L"),
		},
		.driver_data = &gpd_duo_drvdata,
	},
	{
		// GPD Pocket 4
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "GPD"),
			DMI_MATCH(DMI_PRODUCT_NAME, "G1628-04"),
		},
		.driver_data = &gpd_win_mini_drvdata,
	},
	{
		// GPD Pocket 4 (another)
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "GPD"),
			DMI_MATCH(DMI_PRODUCT_NAME, "G1628-04-L"),
		},
		.driver_data = &gpd_win_mini_drvdata,
	},
	{}
};

static const struct gpd_fan_drvdata *gpd_module_drvdata[] = {
	&gpd_win_mini_drvdata, &gpd_win4_drvdata, &gpd_wm2_drvdata, NULL
};

/* Helper functions to handle EC read/write */
static int gpd_ecram_read(const struct gpd_fan_drvdata *drvdata, u16 offset,
			  u8 *val)
{
	int ret;
	u16 addr_port = drvdata->addr_port;
	u16 data_port = drvdata->data_port;

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

static int gpd_ecram_write(const struct gpd_fan_drvdata *drvdata, u16 offset,
			   u8 value)
{
	int ret;
	u16 addr_port = drvdata->addr_port;
	u16 data_port = drvdata->data_port;

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

#ifdef OUT_OF_TREE

static int gpd_read_pwm(void);
static int gpd_read_rpm(void);

#define DEFINE_GPD_READ_CACHED(name, type)                                          \
	static int gpd_##name##_cached(void)                                        \
	{                                                                           \
		if (time_after(                                                     \
			    jiffies,                                                \
			    gpd_driver_priv.name##_last_update +                    \
				    HZ * gpd_driver_priv                            \
						    .update_interval_per_second)) { \
			int ret = gpd_##name();                                     \
			if (ret)                                                    \
				return ret;                                         \
			gpd_driver_priv.name##_cached = (type)ret;                  \
			gpd_driver_priv.name##_last_update = jiffies;               \
		}                                                                   \
		return 0;                                                           \
	}

DEFINE_GPD_READ_CACHED(read_rpm, u16);
DEFINE_GPD_READ_CACHED(read_pwm, u8);

#endif

static int gpd_generic_read_rpm(void)
{
	u8 high, low;
	int ret;
	const struct gpd_fan_drvdata *const drvdata = gpd_driver_priv.drvdata;

	ret = gpd_ecram_read(drvdata, drvdata->rpm_read, &high);
	if (ret)
		return ret;

	ret = gpd_ecram_read(drvdata, drvdata->rpm_read + 1, &low);
	if (ret)
		return ret;

	return (u16)high << 8 | low;
}

static int gpd_win4_read_rpm(void)
{
	const struct gpd_fan_drvdata *const drvdata = gpd_driver_priv.drvdata;
	u8 pwm_ctr_reg;
	int ret;

	gpd_ecram_read(drvdata, GPD_PWM_CTR_OFFSET, &pwm_ctr_reg);

	if (pwm_ctr_reg != 0x7F)
		gpd_ecram_write(drvdata, GPD_PWM_CTR_OFFSET, 0x7F);

	ret = gpd_generic_read_rpm();

	if (ret < 0)
		return ret;

	if (ret == 0) {
		// re-init EC
		u8 chip_id;

		gpd_ecram_read(drvdata, 0x2000, &chip_id);
		if (chip_id == 0x55) {
			u8 chip_ver;

			if (gpd_ecram_read(drvdata, 0x1060, &chip_ver))
				gpd_ecram_write(drvdata, 0x1060,
						chip_ver | 0x80);
		}
	}

	return ret;
}

static int gpd_wm2_read_rpm(void)
{
	const struct gpd_fan_drvdata *const drvdata = gpd_driver_priv.drvdata;

	for (u16 pwm_ctr_offset = GPD_PWM_CTR_OFFSET;
	     pwm_ctr_offset <= GPD_PWM_CTR_OFFSET + 2; pwm_ctr_offset++) {
		u8 PWMCTR;

		gpd_ecram_read(drvdata, pwm_ctr_offset, &PWMCTR);

		if (PWMCTR != 0xB8)
			gpd_ecram_write(drvdata, pwm_ctr_offset, 0xB8);
	}

	return gpd_generic_read_rpm();
}

// Read value for fan1_input
static int gpd_read_rpm(void)
{
	switch (gpd_driver_priv.drvdata->board) {
	case win_mini:
	case duo:
		return gpd_generic_read_rpm();
	case win4_6800u:
		return gpd_win4_read_rpm();
	case win_max_2:
		return gpd_wm2_read_rpm();
	}

	return 0;
}

static int gpd_wm2_read_pwm(void)
{
	const struct gpd_fan_drvdata *const drvdata = gpd_driver_priv.drvdata;
	u8 var;
	int ret = gpd_ecram_read(drvdata, drvdata->pwm_write, &var);

	if (ret < 0)
		return ret;

	return var * 255 / drvdata->pwm_max;
}

// Read value for pwm1
static int gpd_read_pwm(void)
{
	switch (gpd_driver_priv.drvdata->board) {
	case win_mini:
	case duo:
	case win4_6800u:
		return gpd_driver_priv.pwm_value;
	case win_max_2:
		return gpd_wm2_read_pwm();
	}
	return 0;
}

static int gpd_generic_write_pwm(u8 val)
{
	const struct gpd_fan_drvdata *const drvdata = gpd_driver_priv.drvdata;
	u8 pwm_reg;

	// PWM value's range in EC is 1 - pwm_max, cast 0 - 255 to it.
	pwm_reg = val * (drvdata->pwm_max - 1) / 255 + 1;
	return gpd_ecram_write(drvdata, drvdata->pwm_write, pwm_reg);
}

static int gpd_win_mini_write_pwm(u8 val)
{
	if (gpd_driver_priv.pwm_enable == MANUAL)
		return gpd_generic_write_pwm(val);
	else
		return -EPERM;
}

static int gpd_duo_write_pwm_twice(u8 val)
{
	int ret;
	ret = gpd_generic_write_pwm(val);

	if (ret)
		return ret;

	return gpd_generic_write_pwm(val+1);
}

static int gpd_duo_write_pwm(u8 val)
{
	if (gpd_driver_priv.pwm_enable == MANUAL)
		return gpd_duo_write_pwm_twice(val);
	else
		return -EPERM;
}

static int gpd_wm2_write_pwm(u8 val)
{
	if (gpd_driver_priv.pwm_enable != DISABLE)
		return gpd_generic_write_pwm(val);
	else
		return -EPERM;
}

// Write value for pwm1
static int gpd_write_pwm(u8 val)
{
	switch (gpd_driver_priv.drvdata->board) {
	case win_mini:
		return gpd_win_mini_write_pwm(val);
	case duo:
		return gpd_duo_write_pwm(val);
	case win4_6800u:
		return gpd_generic_write_pwm(val);
	case win_max_2:
		return gpd_wm2_write_pwm(val);
	}

	return 0;
}

static int gpd_win_mini_set_pwm_enable(enum FAN_PWM_ENABLE pwm_enable)
{
	const struct gpd_fan_drvdata *drvdata;

	switch (pwm_enable) {
	case DISABLE:
		return gpd_generic_write_pwm(255);
	case MANUAL:
		return gpd_generic_write_pwm(gpd_driver_priv.pwm_value);
	case AUTOMATIC:
		drvdata = gpd_driver_priv.drvdata;
		return gpd_ecram_write(drvdata, drvdata->pwm_write, 0);
	}

	return 0;
}

static int gpd_duo_set_pwm_enable(enum FAN_PWM_ENABLE pwm_enable)
{
	const struct gpd_fan_drvdata *drvdata;

	switch (pwm_enable) {
	case DISABLE:
		return gpd_duo_write_pwm_twice(255);
	case MANUAL:
		return gpd_duo_write_pwm_twice(gpd_driver_priv.pwm_value);
	case AUTOMATIC:
		drvdata = gpd_driver_priv.drvdata;
		return gpd_ecram_write(drvdata, drvdata->pwm_write, 0);
	}

	return 0;
}

static int gpd_wm2_set_pwm_enable(enum FAN_PWM_ENABLE enable)
{
	const struct gpd_fan_drvdata *const drvdata = gpd_driver_priv.drvdata;
	int ret;

	switch (enable) {
	case DISABLE: {
		ret = gpd_generic_write_pwm(255);

		if (ret)
			return ret;

		return gpd_ecram_write(drvdata, drvdata->manual_control_enable,
				       1);
	}
	case MANUAL: {
		ret = gpd_generic_write_pwm(gpd_driver_priv.pwm_value);

		if (ret)
			return ret;

		return gpd_ecram_write(drvdata, drvdata->manual_control_enable,
				       1);
	}
	case AUTOMATIC: {
		ret = gpd_ecram_write(drvdata, drvdata->manual_control_enable,
				      0);

		return ret;
	}
	}

	return 0;
}

// Write value for pwm1_enable
static int gpd_set_pwm_enable(enum FAN_PWM_ENABLE enable)
{
	switch (gpd_driver_priv.drvdata->board) {
	case win_mini:
	case win4_6800u:
		return gpd_win_mini_set_pwm_enable(enable);
	case duo:
		return gpd_duo_set_pwm_enable(enable);
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
#ifdef OUT_OF_TREE
		case hwmon_pwm_mode:
			return 0444;
#endif
		case hwmon_pwm_enable:
		case hwmon_pwm_input:
			return 0644;
		default:
			return 0;
		}
	}
#ifdef OUT_OF_TREE
	if (type == hwmon_chip && attr == hwmon_chip_update_interval) {
		return 0644;
	}
#endif
	return 0;
}

static int gpd_fan_hwmon_read(__always_unused struct device *dev,
			      enum hwmon_sensor_types type, u32 attr,
			      __always_unused int channel, long *val)
{
	if (type == hwmon_fan) {
		if (attr == hwmon_fan_input) {
#ifdef OUT_OF_TREE
			int ret = gpd_read_rpm_cached();
#else
			int ret = gpd_read_rpm();
#endif

			if (ret < 0)
				return ret;

			*val = ret;
			return 0;
		}
		return -EOPNOTSUPP;
	} else if (type == hwmon_pwm) {
		int ret;

		switch (attr) {
#ifdef OUT_OF_TREE
		case hwmon_pwm_mode:
			*val = 1;
			return 0;
#endif
		case hwmon_pwm_enable:
			*val = gpd_driver_priv.pwm_enable;
			return 0;
		case hwmon_pwm_input:
#ifdef OUT_OF_TREE
			ret = gpd_read_pwm_cached();
#else
			ret = gpd_read_pwm();
#endif

			if (ret < 0)
				return ret;

			*val = ret;
			return 0;
		default:
			return -EOPNOTSUPP;
		}
	}
#ifdef OUT_OF_TREE
	if (type == hwmon_chip && attr == hwmon_chip_update_interval) {
		*val = 1000 * gpd_driver_priv.update_interval_per_second;
		return 0;
	}
#endif

	return -EOPNOTSUPP;
}

static int gpd_fan_hwmon_write(__always_unused struct device *dev,
			       enum hwmon_sensor_types type, u32 attr,
			       __always_unused int channel, long val)
{
	u8 var;

	if (type == hwmon_pwm) {
		switch (attr) {
		case hwmon_pwm_enable:
			if (!in_range(val, 0, 3))
				return -EINVAL;

			gpd_driver_priv.pwm_enable = val;

			return gpd_set_pwm_enable(gpd_driver_priv.pwm_enable);
		case hwmon_pwm_input:
			var = clamp_val(val, 0, 255);

			gpd_driver_priv.pwm_value = var;

			return gpd_write_pwm(var);
		default:
			return -EOPNOTSUPP;
		}
	}
#ifdef OUT_OF_TREE
	if (type == hwmon_chip) {
		if (attr == hwmon_chip_update_interval) {
			int interval = val / 1000;
			if (interval < 1)
				interval = 1;
			gpd_driver_priv.update_interval_per_second = interval;
			return 0;
		}
	}
#endif

	return -EOPNOTSUPP;
}

static const struct hwmon_ops gpd_fan_ops = {
	.is_visible = gpd_fan_hwmon_is_visible,
	.read = gpd_fan_hwmon_read,
	.write = gpd_fan_hwmon_write,
};

static const struct hwmon_channel_info *gpd_fan_hwmon_channel_info[] = {
#ifdef OUT_OF_TREE
	HWMON_CHANNEL_INFO(chip, HWMON_C_UPDATE_INTERVAL),
	HWMON_CHANNEL_INFO(fan, HWMON_F_INPUT),
	HWMON_CHANNEL_INFO(pwm,
			   HWMON_PWM_INPUT | HWMON_PWM_ENABLE | HWMON_PWM_MODE),
#else
	HWMON_CHANNEL_INFO(fan, HWMON_F_INPUT),
	HWMON_CHANNEL_INFO(pwm, HWMON_PWM_INPUT | HWMON_PWM_ENABLE),
#endif
	NULL
};

static struct hwmon_chip_info gpd_fan_chip_info = {
	.ops = &gpd_fan_ops,
	.info = gpd_fan_hwmon_channel_info
};

#ifdef OUT_OF_TREE
struct dentry *DEBUG_FS_ENTRY = NULL;

static int debugfs_manual_control_get(void *data, u64 *val)
{
	const struct gpd_fan_drvdata *address = gpd_driver_priv.drvdata;
	u8 u8_val;

	int ret = gpd_ecram_read(address, address->manual_control_enable,
				 &u8_val);
	*val = (u64)u8_val;
	return ret;
}

static int debugfs_manual_control_set(void *data, u64 val)
{
	const struct gpd_fan_drvdata *address = gpd_driver_priv.drvdata;
	return gpd_ecram_write(address, address->manual_control_enable,
			       clamp_val(val, 0, 255));
}

static int debugfs_pwm_get(void *data, u64 *val)
{
	const struct gpd_fan_drvdata *address = gpd_driver_priv.drvdata;
	u8 u8_val;

	int ret = gpd_ecram_read(address, address->pwm_write, &u8_val);
	*val = (u64)u8_val;
	return ret;
}

static int debugfs_pwm_set(void *data, u64 val)
{
	const struct gpd_fan_drvdata *address = gpd_driver_priv.drvdata;
	return gpd_ecram_write(address, address->pwm_write,
			       clamp_val(val, 0, 255));
}

DEFINE_DEBUGFS_ATTRIBUTE(debugfs_manual_control_fops,
			 debugfs_manual_control_get, debugfs_manual_control_set,
			 "%llu\n");
DEFINE_DEBUGFS_ATTRIBUTE(debugfs_pwm_fops, debugfs_pwm_get, debugfs_pwm_set,
			 "%llu\n");

#endif

static int gpd_fan_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	const struct resource *res;
	const struct device *hwdev;
	const struct resource *region;

	res = platform_get_resource(pdev, IORESOURCE_IO, 0);
	if (IS_ERR(res))
		return dev_err_probe(dev, PTR_ERR(res),
				     "Failed to get platform resource\n");

	region = devm_request_region(dev, res->start,
				     resource_size(res), DRIVER_NAME);
	if (IS_ERR(region))
		return dev_err_probe(dev, PTR_ERR(region),
				     "Failed to request region\n");

	hwdev = devm_hwmon_device_register_with_info(dev,
						     DRIVER_NAME,
						     NULL,
						     &gpd_fan_chip_info,
						     NULL);
	if (IS_ERR(hwdev))
		return dev_err_probe(dev, PTR_ERR(region),
				     "Failed to register hwmon device\n");

#ifdef OUT_OF_TREE
	struct dentry *debug_fs_entry = debugfs_create_dir(DRIVER_NAME, NULL);
	if (!IS_ERR(debug_fs_entry)) {
		DEBUG_FS_ENTRY = debug_fs_entry;
		debugfs_create_file_size("manual_control_reg",
					 S_IRUSR | S_IWUSR, DEBUG_FS_ENTRY,
					 NULL, &debugfs_manual_control_fops,
					 sizeof(u8));
		debugfs_create_file_size("pwm_reg", S_IRUSR | S_IWUSR,
					 DEBUG_FS_ENTRY, NULL,
					 &debugfs_pwm_fops, sizeof(u8));
	}

	pr_info("GPD Devices fan driver probed\n");
#endif

	return 0;
}

#ifdef OUT_OF_TREE
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 11, 0)
static int gpd_fan_remove(__always_unused struct platform_device *pdev)
#else
static void gpd_fan_remove(__always_unused struct platform_device *pdev)
#endif
#else
static void gpd_fan_remove(__always_unused struct platform_device *pdev)
#endif
{
	gpd_driver_priv.pwm_enable = AUTOMATIC;
	gpd_set_pwm_enable(AUTOMATIC);
#ifdef OUT_OF_TREE

	if (!IS_ERR_OR_NULL(DEBUG_FS_ENTRY)) {
		debugfs_remove_recursive(DEBUG_FS_ENTRY);
		DEBUG_FS_ENTRY = NULL;
	}

	pr_info("GPD Devices fan driver removed\n");

#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 11, 0)
	return 0;
#endif

#endif
}

static struct platform_driver gpd_fan_driver = {
	.probe = gpd_fan_probe,
	.remove = gpd_fan_remove,
	.driver = {
		.name = KBUILD_MODNAME,
	},
};

static struct platform_device *gpd_fan_platform_device;

static int __init gpd_fan_init(void)
{
	const struct gpd_fan_drvdata *match = NULL;

	for (const struct gpd_fan_drvdata **p = gpd_module_drvdata; *p; p++) {
		if (strcmp(gpd_fan_board, (*p)->board_name) == 0) {
			match = *p;
			break;
		}
	}

	if (!match) {
		const struct dmi_system_id *dmi_match =
			dmi_first_match(dmi_table);
		if (dmi_match)
			match = dmi_match->driver_data;
	}

#ifdef OUT_OF_TREE
	if (!match) {
		pr_err("GPD Devices not supported\n");
		return -ENODEV;
	} else {
		pr_info("Loading GPD fan model quirk: %s\n", match->board_name);
	}
#else
	if (!match)
		return -ENODEV;
#endif

	gpd_driver_priv.pwm_enable = AUTOMATIC;
	gpd_driver_priv.pwm_value = 255;
	gpd_driver_priv.drvdata = match;

#ifdef OUT_OF_TREE
	gpd_driver_priv.read_pwm_cached = 0;
	gpd_driver_priv.read_rpm_cached = 0;
	gpd_driver_priv.update_interval_per_second = 1;
	gpd_driver_priv.read_pwm_last_update = jiffies;
	gpd_driver_priv.read_rpm_last_update = jiffies;
#endif

	struct resource gpd_fan_resources[] = {
		{
			.start = match->addr_port,
			.end = match->data_port,
			.flags = IORESOURCE_IO,
		},
	};

	gpd_fan_platform_device = platform_create_bundle(&gpd_fan_driver,
							 gpd_fan_probe,
							 gpd_fan_resources,
							 1, NULL, 0);

	if (IS_ERR(gpd_fan_platform_device)) {
		pr_warn("Failed to create platform device\n");
		return PTR_ERR(gpd_fan_platform_device);
	}

#ifdef OUT_OF_TREE
	pr_info("GPD Devices fan driver loaded\n");
#endif

	return 0;
}

static void __exit gpd_fan_exit(void)
{
	platform_device_unregister(gpd_fan_platform_device);
	platform_driver_unregister(&gpd_fan_driver);
#ifdef OUT_OF_TREE
	pr_info("GPD Devices fan driver unloaded\n");
#endif
}

MODULE_DEVICE_TABLE(dmi, dmi_table);

module_init(gpd_fan_init);
module_exit(gpd_fan_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Cryolitia PukNgae <Cryolitia@gmail.com>");
MODULE_DESCRIPTION("GPD Devices fan control driver");
