# Copyright 2022 The ChromiumOS Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

register_npcx_project(
    project_name="lotus",
    zephyr_board="npcx9m3f",
    dts_overlays=[
        here / "adc.dts",
        here / "battery.dts",
        here / "gpio.dts",
        here / "i2c.dts",
        here / "interrupts.dts",
        here / "led_policy.dts",
        here / "pwm_leds.dts",
        here / "lotus.dts",
    ],
)