#include <linux/acpi.h>
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

struct model_address_config {
    const u8 addr_port;
    const u8 data_port;
    const u16 manual_enable;
    const u16 fan_speed_read;
    const u16 pwm_write;
    const u16 pwm_max;
};

//static const struct model_address_config g1618_04_driver_data = {
//        .addr_port = 0x2E,
//        .data_port = 0x2F,
//        .manual_enable = 0x11,
//        .fan_speed_read = 0x0880,
//        .pwm_write = 0x11,
//        .pwm_max = 127,
//};

static const struct model_address_config g1619_04_driver_data = {
        .addr_port = 0x4E,
        .data_port = 0x4F,
        .manual_enable = 0x0275,
        .fan_speed_read = 0x0218,
        .pwm_write = 0x1809,
        .pwm_max = 184,
};

static const struct dmi_system_id gpd_devices[] = {
//        {
//            .ident = "GPD Win 4",
//            .matches = {
//                    DMI_MATCH(DMI_SYS_VENDOR, "GPD"),
//                    DMI_MATCH(DMI_PRODUCT_NAME, "G1618-04"),
//            },
//            .driver_data = (void*) &g1618_04_driver_data
//        },
        {
                .ident = "GPD Win Max 2",
                .matches = {
                        DMI_MATCH(DMI_SYS_VENDOR, "GPD"),
                        DMI_MATCH(DMI_PRODUCT_NAME, "G1619-04"),
                },
                .driver_data = (void *) &g1619_04_driver_data
        }
};

struct gpd_fan_private_data {
    // 0: disable (full speed)
    // 1: manual
    // 2: automatic
    u8 mode;
    u8 pwm_value;

    u16 cached_fan_speed;
    unsigned long last_update;

    struct model_address_config *config;
};

static int gpd_ecram_read(struct model_address_config *config, u16 offset, u16 *val) {
    int ret = mutex_lock_interruptible(&gpd_fan_locker);
    if (ret)
        return ret;

    u16 addr_port = config->addr_port;
    u16 data_port = config->data_port;

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
    *val = inw(data_port);

    mutex_unlock(&gpd_fan_locker);
    return 0;
}

static int gpd_read_fan(struct gpd_fan_private_data *data, long *val) {
    if (time_after(jiffies, data->last_update + HZ)) {
        u16 var;
        int ret = gpd_ecram_read(data->config, data->config->fan_speed_read, &var);
        if (ret)
            return ret;

        data->cached_fan_speed = (var & 0xFF) << 8 | (var >> 8);
        data->last_update = jiffies;
    }

    *val = data->cached_fan_speed;
    return 0;
}

static int gpd_ecram_write(struct model_address_config *config, u16 offset, u8 value) {
    int ret = mutex_lock_interruptible(&gpd_fan_locker);
    if (ret)
        return ret;

    u16 addr_port = config->addr_port;
    u16 data_port = config->data_port;

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

static int gpd_fan_auto_control(struct model_address_config *config, bool enable) {
    if (enable)
        return gpd_ecram_write(config, config->manual_enable, 0);
    else
        return gpd_ecram_write(config, config->manual_enable, 1);
}

static int gpd_write_fan(struct model_address_config *config, u8 val) {
    u8 var = (u8) (val * config->pwm_max / 255);

    int ret = gpd_ecram_write(config, config->pwm_write, var);
    if (ret)
        return ret;
    else
        return 0;
}

static int gpd_fan_set_mode(struct gpd_fan_private_data *data) {
    switch (data->mode) {
        case 0: {
            int ret = gpd_write_fan(data->config, 255);
            if (ret)
                return ret;

            return gpd_fan_auto_control(data->config, false);
        }
        case 1: {
            int ret = gpd_write_fan(data->config, data->pwm_value);
            if (ret)
                return ret;

            return gpd_fan_auto_control(data->config, false);
        }
        case 2:
            return gpd_fan_auto_control(data->config, true);
        default:
            return -EINVAL;
    }
}

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
        return S_IRUSR | S_IRGRP | S_IROTH;
    }
    return 0;
}

static int
gpd_fan_hwmon_read(struct device *dev, enum hwmon_sensor_types type, u32 attr, __attribute__((unused)) int channel,
                   long *val) {
    struct gpd_fan_private_data *data = dev_get_drvdata(dev);

    if (type == hwmon_fan) {
        switch (attr) {
            case hwmon_fan_input:
                return gpd_read_fan(data, val);
        }
    } else if (type == hwmon_pwm) {
        switch (attr) {
            case hwmon_pwm_mode:
                *val = 1;
                return 0;
            case hwmon_pwm_enable:
                *val = data->mode;
                return 0;
            case hwmon_pwm_input:
                *val = data->pwm_value;
                return 0;
        }
    } else if (type == hwmon_chip) {
        switch (attr) {
            case hwmon_chip_update_interval:
                *val = 1000;
                return 0;
        }
    }
    return -EINVAL;
}

static int
gpd_fan_hwmon_write(struct device *dev, enum hwmon_sensor_types type, u32 attr, __attribute__((unused)) int channel,
                    long val) {
    struct gpd_fan_private_data *data = dev_get_drvdata(dev);

    if (type == hwmon_pwm) {
        switch (attr) {
            case hwmon_pwm_enable:
                if (!in_range(val, 0, 3))
                    return -EINVAL;
                data->mode = val;
                return gpd_fan_set_mode(data);
            case hwmon_pwm_input: {
                u8 var = clamp_val(val, 0, 255);
                data->pwm_value = var;
                if (data->mode == 1)
                    return gpd_write_fan(data->config, var);
                else return 0;
            }
            default:
                return -EINVAL;
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

static int gpd_fan_probe(struct platform_device *pdev) {
    struct device *dev = &pdev->dev;
    const struct dmi_system_id *match;
    struct gpd_fan_private_data *data;

    match = dmi_first_match(gpd_devices);
    if (!match) {
        pr_err("GPD Devices not supported\n");
        return -ENODEV;
    }

    data = dev_get_platdata(&pdev->dev);
    if (!data)
        return -ENODEV;

    const struct resource *res = platform_get_resource(pdev, IORESOURCE_IO, 0);
    if (!res) {
        pr_err("Failed to get platform resource\n");
        return -ENODEV;
    }

    if (!devm_request_region(dev, res->start, resource_size(res), DRIVER_NAME)) {
        pr_err("Failed to request region\n");
        return -EBUSY;
    }

    if (!devm_hwmon_device_register_with_info(dev, DRIVER_NAME, data, &gpd_fan_chip_info, NULL)) {
        pr_err("Failed to register hwmon device\n");
        return -EBUSY;
    }

    pr_info("GPD Devices fan driver probed\n");
    return 0;
}

static int gpd_fan_remove(__attribute__((unused)) struct platform_device *pdev) {
    struct gpd_fan_private_data *data = dev_get_platdata(&pdev->dev);

    data->mode = 2;
    gpd_fan_set_mode(data);

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
    if (!match) {
        pr_err("GPD Devices not supported\n");
        return -ENODEV;
    }

    struct gpd_fan_private_data data = {
            .mode = 2,
            .pwm_value = 255,
            .cached_fan_speed = 0,
            .last_update = jiffies,
            .config = match->driver_data,
    };

    struct resource gpd_fan_resources[] = {
            {
                    .start = data.config->addr_port,
                    .end = data.config->data_port,
                    .flags = IORESOURCE_IO,
            },
    };

    gpd_fan_platform_device = platform_create_bundle(&gpd_fan_driver, gpd_fan_probe, gpd_fan_resources, 1, &data,
                                                     sizeof(struct gpd_fan_private_data));
    if (IS_ERR(gpd_fan_platform_device)) {
        pr_err("Failed to create platform device\n");
        return (int) PTR_ERR(gpd_fan_platform_device);
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
MODULE_ALIAS("dmi:*:svnGPD:pnG1619-04:*");
