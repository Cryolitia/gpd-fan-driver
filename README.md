# GPD devices fan linux kernel driver

> [!CAUTION]
> 
> This driver has not been fully tested and reviewed.
> 
> It may cause damage to your device. Use at your own risk.

Tested on GPD Win Max 2 2023 (7840U) with 6.8.4-zen1

## Should support

- GPD Win Mini (7840U)
- GPD Win Mini (8840U)
- GPD Win Max 2
- GPD Win Max 2 2023 (7840U)
- GPD Win Max 2 2024 (8840U)
- GPD Win 4 (6800U)
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
    $ echo 2 | sudo tee /sys/devices/platform/gpd_fan/hwmon/hwmon*/pwm1_enable
    ```
- Set fan speed
    ```bash
    # range: 0-255
    $ echo 127 | sudo tee /sys/devices/platform/gpd_fan/hwmon/hwmon*/pwm1
    ```

More about fields in hwmon subsystem, please read https://www.kernel.org/doc/Documentation/hwmon/sysfs-interface .

## Install

- Arch Linux(thx @dreirund): [gpd-fan-driver-dkms-git](https://aur.archlinux.org/packages/gpd-fan-driver-dkms-git)
- NixOS: `nixosModules.default`

## Datasheet

### Credit

- Bilibili: [@范东东咚咚](https://space.bilibili.com/361065271)

<table>
  <col />
  <col span="2" />
  <col />
  <col span="2" />
  <col span="2" />
  <col />
  <col />
  <tr>
    <th rowspan="2">Device</th>
    <th colspan="3">DMI</th>
    <th colspan="2">EC RAM</th>
    <th rowspan="2">Read (rpm)</th>
    <th colspan="3">Write (pwm)</th>
  </tr>
  <tr>
    <th>Manufacturer</th>
    <th>Product</th>
    <th>Version</th>
    <th>REG_ADDR</th>
    <th>REG_DATA</th>
    <th></th>
    <th>Max</th>
    <th>Auto (=0)</th>
  </tr>
  <tr>
    <th>GPD Win Mini</th>
    <td rowspan="6">GPD</td>
    <td>G1617-01</td>
    <td></td>
    <td>0x4E</td>
    <td>0x4F</td>
    <td>0x0478</td>
    <td>0x047A</td>
    <td>244</td>
    <td>0x047A</td>
  </tr>
  <tr>
    <th>GPD Win 4 6800U</th>
    <td rowspan="2">G1618-04</td>
    <td>Default string</td>
    <td>0x2E</td>
    <td>0x2F</td>
    <td>0xC880</td>
    <td>0xC311</td>
    <td>127</td>
    <td>0xC311</td>
  </tr>
  <tr>
    <th>GPD Win 4 7840U</th>
    <td>Ver. 1.0</td>
    <td rowspan="4">0x4E</td>
    <td rowspan="4">0x4F</td>
    <td rowspan="4">0x0218</td>
    <td rowspan="4">0x1809</td>
    <td rowspan="4">184</td>
    <td rowspan="4">0x0275</td>
  </tr>
  <tr>
    <th>GPD Win Max 2 6800U</th>
    <td rowspan="3">G1619-04</td>
    <td></td>
  </tr>
  <tr>
    <th>GPD Win Max 2 2023 7840U</th>
    <td></td>
  </tr>
  <tr>
    <th>GPD Win Max 2 2024 8840U</th>
    <td></td>
  </tr>
</table>
