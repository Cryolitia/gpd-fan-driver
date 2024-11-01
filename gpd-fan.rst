.. SPDX-License-Identifier: GPL-2.0-or-later

Kernel driver gpd-fan
=========================

Author:
    - Cryolitia PukNgae <Cryolitia@gmail.com>

Description
------------

Handheld devices from Shenzhen GPD Technology Co., Ltd. provide fan readings and fan control through
their embedded controllers.

Supported devices
-----------------

Currently the driver supports the following handhelds:

 - GPD Win Mini (7840U)
 - GPD Win Mini (8840U)
 - GPD Win Max 2
 - GPD Win Max 2 2023 (7840U)
 - GPD Win Max 2 2024 (8840U)
 - GPD Win 4 (6800U)
 - GPD Win 4 (7840U)

Module parameters
-----------------

gpd_fan_board
  Force specific which module quirk should be used.
  Use it like "gpd_fan_board=wm2".

   - wm2
       - GPD Win 4 (7840U)
       - GPD Win Max 2 (6800U)
       - GPD Win Max 2 2023 (7840U)
       - GPD Win Max 2 2024 (8840U)
   - win4
       - GPD Win 4 (6800U)
   - win_mini
       - GPD Win Mini (7840U)
       - GPD Win Mini (8840U)

Sysfs entries
-------------

The following attributes are supported:

fan1_input
  Read Only. Reads current fan RPM.

pwm1_enable
  Read/Write. Enable manual fan control. Write "0" to disable control and run at
  full speed. Write "1" to set to manual, write "2" to let the EC control decide
  fan speed. Read this attribute to see current status.

pwm1
  Read/Write. Read this attribute to see current duty cycle in the range [0-255].
  When pwm1_enable is set to "1" (manual) write any value in the range [0-255]
  to set fan speed.

