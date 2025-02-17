# GPD devices fan linux kernel driver

> [!CAUTION]
> 
> This driver has not been fully tested and reviewed.
> 
> It may cause damage to your device. Use at your own risk.

Continuous testing on my GPD Win Max 2 2023 (7840U) with kernel_zen in nixos-unstable

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

This driver should auto detected your device, if not, please report an issue.

For testing not detected device, use model parameter `gpd_fan_model` to specify your device.

Supported models:

- wm2
- win4
- win_mini

## Install

- Arch Linux(thx @dreirund): [gpd-fan-driver-dkms-git](https://aur.archlinux.org/packages/gpd-fan-driver-dkms-git)
- NixOS: `nixosModules.default`

## Upstreaming

The driver is tried to upstreaming to the kernel mainline. The work progress could be seen in [branch in-tree](https://github.com/Cryolitia/gpd-fan-driver/tree/in-tree) and [my linux fork](https://github.com/Cryolitia-Forks/linux/tree/b4/gpd_fan). The main branch will write whatever I'd like and the in-tree branch will catering to the preferences of kernel developers.

## Datasheet

### Credit

- Bilibili: [@范东东咚咚](https://space.bilibili.com/361065271)

<table><thead>
  <tr>
    <th colspan="2">Device</th>
    <th colspan="3">DMI</th>
    <th colspan="2">EC RAM</th>
    <th rowspan="2">Read (rpm)</th>
    <th colspan="4">Write (pwm)</th>
  </tr>
  <tr>
    <th>Series</th>
    <th>CPU</th>
    <th>Manufacturer</th>
    <th>Product</th>
    <th>Version</th>
    <th>REG_ADDR</th>
    <th>REG_DATA</th>
    <th colspan="2">Addr</th>
    <th>Max</th>
    <th>Auto (=0)</th>
  </tr></thead>
<tbody>
  <tr>
    <td rowspan="4">GPD Win Mini</td>
    <td>7840U</td>
    <td rowspan="15">GPD</td>
    <td rowspan="2">G1617-01</td>
    <td rowspan="2"></td>
    <td rowspan="8">0x4E</td>
    <td rowspan="8">0x4F</td>
    <td rowspan="8">0x0478</td>
    <td colspan="2" rowspan="6">0x047A</td>
    <td rowspan="8">244</td>
    <td rowspan="8">0x047A</td>
  </tr>
  <tr>
    <td>8840u</td>
  </tr>
  <tr>
    <td rowspan="2">HX 370</td>
    <td>G1617-02</td>
    <td></td>
  </tr>
  <tr>
    <td>G1617-02-L</td>
    <td></td>
  </tr>
  <tr>
    <td rowspan="2">GPD Pocket 4</td>
    <td rowspan="2"></td>
    <td>G1628-04</td>
    <td></td>
  </tr>
  <tr>
    <td>G1628-04-L</td>
    <td></td>
  </tr>
  <tr>
    <td rowspan="2">GPD Duo</td>
    <td rowspan="2"></td>
    <td>G1622-01</td>
    <td></td>
    <td rowspan="2">0x047A</td>
    <td rowspan="2">0x047B</td>
  </tr>
  <tr>
    <td>G1622-01-L</td>
    <td></td>
  </tr>
  <tr>
    <td rowspan="3">GPD Win 4</td>
    <td>6800U</td>
    <td rowspan="3">G1618-04</td>
    <td>Default string</td>
    <td>0x2E</td>
    <td>0x2F</td>
    <td>0xC880</td>
    <td colspan="2">0xC311</td>
    <td>127</td>
    <td>0xC311</td>
  </tr>
  <tr>
    <td rowspan="2">7840U</td>
    <td>Ver. 1.0</td>
    <td rowspan="6">0x4E</td>
    <td rowspan="6">0x4F</td>
    <td rowspan="6">0x0218</td>
    <td colspan="2" rowspan="6">0x1809</td>
    <td rowspan="6">184</td>
    <td rowspan="6">0x0275</td>
  </tr>
  <tr>
    <td>Ver.1.0</td>
  </tr>
  <tr>
    <td rowspan="4">GPD Win Max 2</td>
    <td>6800U</td>
    <td rowspan="3">G1619-04</td>
    <td rowspan="3"></td>
  </tr>
  <tr>
    <td>7840U</td>
  </tr>
  <tr>
    <td>8840U</td>
  </tr>
  <tr>
    <td>HX 370</td>
    <td>G1619-05</td>
    <td></td>
  </tr>
</tbody></table>
