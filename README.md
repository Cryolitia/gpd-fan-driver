# GPD devices fan linux kernel driver

> [!CAUTION]
> 
> This driver has not been fully tested and reviewed.
> 
> It may cause damage to your device. Use at your own risk.
> 
> Tested on GPD Win Max 2 2023 (7840U) with 6.8.4-zen1

## Should support

- GPD Win Max 2
- GPD Win Max 2 2023 (7840U)
- GPD Win Max 2 2024 (8840U)
- GPD Win 4 (7840U)

## Usage

- Current fan speed
    ```bash
    $ cat /sys/devices/platform/gpd_fan/hwmon/hwmon*/fan1_input
    ```
- Set fan speed control mode
    ```bash
    # 0: disable (full speed)
    # 1: manual
    # 2: auto
    $ echo 2 | sudo tee /sys/devices/platform/gpd_fan/hwmon/hwmon*/pwm*_enable
    ```
- Set fan speed
    ```bash
    # range: 0-255
    $ echo 127 | sudo tee /sys/devices/platform/gpd_fan/hwmon/hwmon*/pwm*
    ```
  