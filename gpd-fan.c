#include <linux/acpi.h>
#include <linux/debugfs.h>
#include <linux/dmi.h>
#include <linux/hwmon.h>
#include <linux/hwmon-sysfs.h>
#include <linux/ioport.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/platform_device.h>

#define DRIVER_NAME "gpdfan"

static DEFINE_MUTEX(gpd_fan_locker);

enum FUN_PWM_ENABLE {
    DISABLE = 0,
    MANUAL = 1,
    AUTOMATIC = 2,
};

struct driver_private_data {
    enum FUN_PWM_ENABLE pwm_enable;
    u8 pwm_value;

    u16 cached_fan_speed;
    // minium 1000 mill seconds
    u32 update_interval_per_second;
    unsigned long last_update;

    const struct model_quirk *const quirk;
};

struct model_ec_address {
    const u8 addr_port;
    const u8 data_port;
    const u16 manual_control_enable;
    const u16 rpm_read;
    const u16 pwm_write;
    const u16 pwm_max;
};

struct model_quirk {
    const struct model_ec_address address;

    int (*const read_rpm)(struct driver_private_data *, long *);

    int (*const set_pwm_enable)(const struct driver_private_data *, enum FUN_PWM_ENABLE);

    int (*const write_pwm)(const struct driver_private_data *, u8);
};

static int gpd_ecram_read(const struct model_ec_address *const address, const u16 offset, u8 *const val) {
    int ret = mutex_lock_interruptible(&gpd_fan_locker);
    if (ret)
        return ret;

    u16 addr_port = address->addr_port;
    u16 data_port = address->data_port;

    outb(0x2E, addr_port);
    outb(0x11, data_port);
    outb(0x2F, addr_port);
    outb((u8) ((offset >> 8) & 0xFF), data_port);

    outb(0x2E, addr_port);
    outb(0x10, data_port);
    outb(0x2F, addr_port);
    outb((u8) (offset & 0xFF), data_port);

    outb(0x2E, addr_port);
    outb(0x12, data_port);
    outb(0x2F, addr_port);
    *val = inb(data_port);

    mutex_unlock(&gpd_fan_locker);
    return 0;
}

static int gpd_ecram_write(const struct model_ec_address *const address, const u16 offset, const u8 value) {
    int ret = mutex_lock_interruptible(&gpd_fan_locker);
    if (ret)
        return ret;

    u16 addr_port = address->addr_port;
    u16 data_port = address->data_port;

    outb(0x2E, addr_port);
    outb(0x11, data_port);
    outb(0x2F, addr_port);
    outb((u8) ((offset >> 8) & 0xFF), data_port);

    outb(0x2E, addr_port);
    outb(0x10, data_port);
    outb(0x2F, addr_port);
    outb((u8) (offset & 0xFF), data_port);

    outb(0x2E, addr_port);
    outb(0x12, data_port);
    outb(0x2F, addr_port);
    outb(value, data_port);

    mutex_unlock(&gpd_fan_locker);
    return 0;
}

static int gpd_read_rpm_uncached(const struct driver_private_data *const data, long *const val) {
    u8 high, low;
    int ret;
    const struct model_ec_address *const address = &data->quirk->address;

    ret = gpd_ecram_read(address, address->rpm_read, &high);
    if (ret)
        return ret;
    ret = gpd_ecram_read(address, address->rpm_read + 1, &low);
    if (ret)
        return ret;

    *val = high << 8 | low;
    return 0;
}

static int gpd_read_rpm_cached(struct driver_private_data *const data, long *const val,
                               int (*read_rpm_uncached)(const struct driver_private_data *, long *)) {
    if (time_after(jiffies, data->last_update + HZ * data->update_interval_per_second)) {
        long var;
        int ret = read_rpm_uncached(data, &var);
        if (ret)
            return ret;

        data->cached_fan_speed = var;
        data->last_update = jiffies;
    }

    *val = data->cached_fan_speed;
    return 0;
}

static int gpd_read_rpm(struct driver_private_data *const data, long *const val) {
    return gpd_read_rpm_cached(data, val, gpd_read_rpm_uncached);
}

static int gpd_write_pwm(const struct driver_private_data *const data, const u8 val) {
    const struct model_ec_address *const address = &data->quirk->address;
    u8 actual = val * (address->pwm_max - 1) / 255 + 1;
    return gpd_ecram_write(address, address->pwm_write, actual);
}

static int gpd_win_mini_set_pwm_enable(const struct driver_private_data *const data,
                                       const enum FUN_PWM_ENABLE pwm_enable) {
    switch (pwm_enable) {
        case DISABLE:
            return gpd_write_pwm(data, 255);
        case MANUAL:
            return gpd_write_pwm(data, data->pwm_value);
        case AUTOMATIC:
            return gpd_write_pwm(data, 0);
    }
    return 0;
}

static int gpd_win_mini_write_pwm(const struct driver_private_data *const data, const u8 val) {
    if (data->pwm_enable == MANUAL) {
        return gpd_write_pwm(data, val);
    }
    return 0;
}

static const struct model_quirk gpd_win_mini_quirk = {
    .address = {
        .addr_port = 0x4E,
        .data_port = 0x4F,
        .manual_control_enable = 0x047A,
        .rpm_read = 0x0478,
        .pwm_write = 0x047A,
        .pwm_max = 244,
    },
    .read_rpm = gpd_read_rpm,
    .set_pwm_enable = gpd_win_mini_set_pwm_enable,
    .write_pwm = gpd_win_mini_write_pwm,
};

static int gpd_win4_read_rpm_uncached(const struct driver_private_data *const data, long *const val) {
    const struct model_ec_address *const address = &data->quirk->address;

    u8 PWMCTR;
    gpd_ecram_read(address, 0x1841, &PWMCTR);
    if (PWMCTR != 0x7F) {
        gpd_ecram_write(address, 0x1841, 0x7F);
    }

    int ret = gpd_read_rpm_uncached(data, val);
    if (ret)
        return ret;

    if (*val == 0) {
        //re-init EC
        u8 chip_id;
        gpd_ecram_read(address, 0x2000, &chip_id);
        if (chip_id == 0x55) {
            u8 chip_ver;
            if (gpd_ecram_read(address, 0x1060, &chip_ver)) {
                gpd_ecram_write(address, 0x1060, chip_ver | 0x80);
            }
        }
    }
    return 0;
}

static int gpd_win4_read_rpm(struct driver_private_data *const data, long *const val) {
    return gpd_read_rpm_cached(data, val, gpd_win4_read_rpm_uncached);
}

static const struct model_quirk gpd_win4_quirk = {
    .address = {
        .addr_port = 0x2E,
        .data_port = 0x2F,
        .manual_control_enable = 0xC311,
        .rpm_read = 0xC880,
        .pwm_write = 0xC311,
        .pwm_max = 127,
    },
    .read_rpm = gpd_win4_read_rpm,
    // same as GPD Win Mini
    .set_pwm_enable = gpd_win_mini_set_pwm_enable,
    // same as GPD Win Mini
    .write_pwm = gpd_win_mini_write_pwm,
};

static int gpd_wm2_read_rpm_uncached(const struct driver_private_data *const data, long *const val) {
    const struct model_ec_address *const address = &data->quirk->address;
    for (u16 pwm_ctr_offset = 0x1841; pwm_ctr_offset <= 0x1843; pwm_ctr_offset++) {
        u8 PWMCTR;
        gpd_ecram_read(address, pwm_ctr_offset, &PWMCTR);
        if (PWMCTR != 0xB8) {
            gpd_ecram_write(address, pwm_ctr_offset, 0xB8);
        }
    }
    return gpd_read_rpm_uncached(data, val);
}

static int gpd_wm2_read_rpm(struct driver_private_data *const data, long *const val) {
    return gpd_read_rpm_cached(data, val, gpd_wm2_read_rpm_uncached);
}

static int gpd_wm2_set_pwm_enable(const struct driver_private_data *const data, const enum FUN_PWM_ENABLE enable) {
    const struct model_ec_address *const address = &data->quirk->address;
    switch (enable) {
        case DISABLE: {
            int ret = gpd_write_pwm(data, 255);
            if (ret)
                return ret;

            return gpd_ecram_write(address, address->manual_control_enable, 1);
        }
        case MANUAL: {
            int ret = gpd_write_pwm(data, data->pwm_value);
            if (ret)
                return ret;

            return gpd_ecram_write(address, address->manual_control_enable, 1);
        }
        case AUTOMATIC:
            return gpd_ecram_write(address, address->manual_control_enable, 0);
    }
    return 0;
}

static int gpd_wm2_write_pwm(const struct driver_private_data *const data, const u8 val) {
    if (data->pwm_enable != DISABLE) {
        return gpd_write_pwm(data, val);
    }
    return 0;
}

static const struct model_quirk gpd_wm2_quirk = {
    .address = {
        .addr_port = 0x4E,
        .data_port = 0x4F,
        .manual_control_enable = 0x0275,
        .rpm_read = 0x0218,
        .pwm_write = 0x1809,
        .pwm_max = 184,
    },
    .read_rpm = gpd_wm2_read_rpm,
    .set_pwm_enable = gpd_wm2_set_pwm_enable,
    .write_pwm = gpd_wm2_write_pwm,
};

static const struct dmi_system_id gpd_devices[] = {
    {
        // GPD Win Mini
        // GPD Win Mini with AMD Ryzen 8840U
        .matches = {
            DMI_MATCH(DMI_SYS_VENDOR, "GPD"),
            DMI_MATCH(DMI_PRODUCT_NAME, "G1617-01")
        },
        .driver_data = (void *) &gpd_win_mini_quirk,
    },
    {
        // GPD Win 4 with AMD Ryzen 6800U
        .matches = {
            DMI_MATCH(DMI_SYS_VENDOR, "GPD"),
            DMI_MATCH(DMI_PRODUCT_NAME, "G1618-04"),
            DMI_MATCH(DMI_PRODUCT_VERSION, "Default string"),
        },
        .driver_data = (void *) &gpd_win4_quirk,
    },
    {
        // GPD Win 4 with Ryzen 7840U
        .matches = {
            DMI_MATCH(DMI_SYS_VENDOR, "GPD"),
            DMI_MATCH(DMI_PRODUCT_NAME, "G1618-04"),
            DMI_MATCH(DMI_PRODUCT_VERSION, "Ver. 1.0"),
        },
        .driver_data = (void *) &gpd_wm2_quirk,
    },
    {
        // GPD Win Max 2 with Ryzen 6800U
        // GPD Win Max 2 2023 with Ryzen 7840U
        // GPD Win Max 2 2024 with Ryzen 8840U
        .matches = {
            DMI_MATCH(DMI_SYS_VENDOR, "GPD"),
            DMI_MATCH(DMI_PRODUCT_NAME, "G1619-04"),
        },
        .driver_data = (void *) &gpd_wm2_quirk,
    },
};

static umode_t
gpd_fan_hwmon_is_visible(__attribute__((unused)) const void *drvdata, enum hwmon_sensor_types type, u32 attr,
                         __attribute__((unused)) int channel) {
    if (type == hwmon_fan && attr == hwmon_fan_input) {
        return S_IRUSR | S_IRGRP | S_IROTH;
    } else if (type == hwmon_pwm) {
        switch (attr) {
            case hwmon_pwm_mode:
                return S_IRUSR | S_IRGRP | S_IROTH;
            case hwmon_pwm_enable:
            case hwmon_pwm_input:
                return S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH;
        }
    } else if (type == hwmon_chip && attr == hwmon_chip_update_interval) {
        return S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH;
    }
    return 0;
}

static int
gpd_fan_hwmon_read(struct device *dev, enum hwmon_sensor_types type, u32 attr,
                   __attribute__((unused)) int channel,
                   long *val) {
    struct driver_private_data *data = dev_get_drvdata(dev);

    if (type == hwmon_fan) {
        switch (attr) {
            case hwmon_fan_input:
                return data->quirk->read_rpm(data, val);
        }
    } else if (type == hwmon_pwm) {
        switch (attr) {
            case hwmon_pwm_mode:
                *val = 1;
                return 0;
            case hwmon_pwm_enable:
                *val = data->pwm_enable;
                return 0;
            case hwmon_pwm_input:
                *val = data->pwm_value;
                return 0;
        }
    } else if (type == hwmon_chip) {
        switch (attr) {
            case hwmon_chip_update_interval:
                *val = 1000 * data->update_interval_per_second;
                return 0;
        }
    }
    return -EINVAL;
}

static int
gpd_fan_hwmon_write(struct device *dev, enum hwmon_sensor_types type, u32 attr,
                    __attribute__((unused)) int channel,
                    long val) {
    struct driver_private_data *data = dev_get_drvdata(dev);

    if (type == hwmon_pwm) {
        switch (attr) {
            case hwmon_pwm_enable:
                if (!in_range(val, 0, 3))
                    return -EINVAL;
                data->pwm_enable = val;
                return data->quirk->set_pwm_enable(data, data->pwm_enable);
            case hwmon_pwm_input: {
                u8 var = clamp_val(val, 0, 255);
                data->pwm_value = var;
                return data->quirk->write_pwm(data, var);
            }
        }
    } else if (type == hwmon_chip) {
        if (attr == hwmon_chip_update_interval) {
            int interval = val / 1000;
            if (interval < 1)
                interval = 1;
            data->update_interval_per_second = interval;
            return 0;
        }
    }
    return -EINVAL;
}

static const struct hwmon_ops gpd_fan_ops = {
    .is_visible = gpd_fan_hwmon_is_visible,
    .read = gpd_fan_hwmon_read,
    .write = gpd_fan_hwmon_write,
};

static const struct hwmon_channel_info *gpd_fan_hwmon_channel_info[] = {
    HWMON_CHANNEL_INFO(chip, HWMON_C_UPDATE_INTERVAL),
    HWMON_CHANNEL_INFO(fan, HWMON_F_INPUT),
    HWMON_CHANNEL_INFO(pwm, HWMON_PWM_INPUT | HWMON_PWM_ENABLE | HWMON_PWM_MODE),
    NULL
};

static struct hwmon_chip_info gpd_fan_chip_info = {
    .ops = &gpd_fan_ops,
    .info = gpd_fan_hwmon_channel_info
};

struct dentry *DEBUG_FS_ENTRY = NULL;

static int debugfs_manual_control_get(void *data, u64 *val) {
    struct model_ec_address *address = data;
    u8 u8_val;

    int ret = gpd_ecram_read(address, address->manual_control_enable, &u8_val);
    *val = (u64) u8_val;
    return ret;
}

static int debugfs_manual_control_set(void *data, u64 val) {
    struct model_ec_address *address = data;
    return gpd_ecram_write(address, address->manual_control_enable, clamp_val(val, 0, 255));
}

static int debugfs_pwm_get(void *data, u64 *val) {
    struct model_ec_address *address = data;
    u8 u8_val;

    int ret = gpd_ecram_read(address, address->pwm_write, &u8_val);
    *val = (u64) u8_val;
    return ret;
}

static int debugfs_pwm_set(void *data, u64 val) {
    struct model_ec_address *address = data;
    return gpd_ecram_write(address, address->pwm_write, clamp_val(val, 0, 255));
}

DEFINE_DEBUGFS_ATTRIBUTE(debugfs_manual_control_fops, debugfs_manual_control_get, debugfs_manual_control_set, "%llu\n");
DEFINE_DEBUGFS_ATTRIBUTE(debugfs_pwm_fops, debugfs_pwm_get, debugfs_pwm_set, "%llu\n");

static int gpd_fan_probe(struct platform_device *pdev) {
    struct device *dev = &pdev->dev;
    struct driver_private_data *data;

    data = dev_get_platdata(&pdev->dev);
    if (IS_ERR_OR_NULL(data))
        return -ENODEV;

    const struct resource *plat_res = platform_get_resource(pdev, IORESOURCE_IO, 0);
    if (IS_ERR_OR_NULL(plat_res)) {
        pr_err("Failed to get platform resource\n");
        return -ENODEV;
    }

    const struct resource *region_res = devm_request_region(dev, plat_res->start, resource_size(plat_res), DRIVER_NAME);
    if (IS_ERR_OR_NULL(region_res)) {
        pr_err("Failed to request region\n");
        return -EBUSY;
    }

    const struct device *dev_reg = devm_hwmon_device_register_with_info(dev, DRIVER_NAME, data, &gpd_fan_chip_info,
                                                                        NULL);
    if (IS_ERR_OR_NULL(dev_reg)) {
        pr_err("Failed to register hwmon device\n");
        return -EBUSY;
    }

    struct dentry *debug_fs_entry = debugfs_create_dir(DRIVER_NAME, NULL);
    if (!IS_ERR(debug_fs_entry)) {
        DEBUG_FS_ENTRY = debug_fs_entry;
        debugfs_create_file_size("manual_control_reg", S_IRUSR | S_IWUSR, DEBUG_FS_ENTRY,
                                 (void *) &data->quirk->address, &debugfs_manual_control_fops, sizeof(u8));
        debugfs_create_file_size("pwm_reg", S_IRUSR | S_IWUSR, DEBUG_FS_ENTRY, (void *) &data->quirk->address,
                                 &debugfs_pwm_fops, sizeof(u8));
    }

    pr_info("GPD Devices fan driver probed\n");
    return 0;
}

static int gpd_fan_remove(__attribute__((unused)) struct platform_device *pdev) {
    struct driver_private_data *data = dev_get_platdata(&pdev->dev);

    data->pwm_enable = AUTOMATIC;
    data->quirk->set_pwm_enable(data, AUTOMATIC);

    if (!IS_ERR_OR_NULL(DEBUG_FS_ENTRY)) {
        debugfs_remove_recursive(DEBUG_FS_ENTRY);
        DEBUG_FS_ENTRY = NULL;
    }

    pr_info("GPD Devices fan driver removed\n");
    return 0;
}

static struct platform_driver gpd_fan_driver = {
    .probe = gpd_fan_probe,
    .remove = gpd_fan_remove,
    .driver = {
        .name = KBUILD_MODNAME,
    },
};

static struct platform_device *gpd_fan_platform_device;

static int __init gpd_fan_init(void) {
    const struct dmi_system_id *match;

    match = dmi_first_match(gpd_devices);
    if (IS_ERR_OR_NULL(match)) {
        pr_err("GPD Devices not supported\n");
        return -ENODEV;
    }

    struct driver_private_data data = {
        .pwm_enable = AUTOMATIC,
        .pwm_value = 255,
        .cached_fan_speed = 0,
        .update_interval_per_second = 1,
        .last_update = jiffies,
        .quirk = match->driver_data,
    };

    struct resource gpd_fan_resources[] = {
        {
            .start = data.quirk->address.addr_port,
            .end = data.quirk->address.data_port,
            .flags = IORESOURCE_IO,
        },
    };

    gpd_fan_platform_device = platform_create_bundle(&gpd_fan_driver, gpd_fan_probe, gpd_fan_resources, 1, &data,
                                                     sizeof(struct driver_private_data));
    if (IS_ERR(gpd_fan_platform_device)) {
        pr_err("Failed to create platform device\n");
        return PTR_ERR(gpd_fan_platform_device);
    }

    pr_info("GPD Devices fan driver loaded\n");
    return 0;
}

static void __exit gpd_fan_exit(void) {
    platform_device_unregister(gpd_fan_platform_device);
    platform_driver_unregister(&gpd_fan_driver);
    pr_info("GPD Devices fan driver unloaded\n");
}

module_init(gpd_fan_init)

module_exit(gpd_fan_exit)

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Cryolitia <Cryolitia@gmail.com>");
MODULE_DESCRIPTION("GPD Devices fan control driver");
MODULE_ALIAS("dmi:*:svnGPD:pnG1617-01:*");
MODULE_ALIAS("dmi:*:svnGPD:pnG1618-04:*");
MODULE_ALIAS("dmi:*:svnGPD:pnG1619-04:*");
