/* Copyright 2017 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 *
 * Power and battery LED control for HX20
 */

#include "battery.h"
#include "charge_state.h"
#include "chipset.h"
#include "ec_commands.h"
#include "led_common.h"
#include "led_pwm.h"
#include "lid_switch.h"
#include "pwm.h"
#include "hooks.h"
#include "host_command.h"
#include "util.h"
#include "gpio.h"

#define LED_TICKS_PER_CYCLE 10
#define LED_ON_TICKS 5

/* at 8-bit mode one cycle = 8ms */
#define BREATH_ON_LENGTH	62
#define BREATH_OFF_LENGTH	200


const enum ec_led_id supported_led_ids[] = {
	EC_LED_ID_LEFT_LED,
	EC_LED_ID_RIGHT_LED,
	EC_LED_ID_POWER_LED,
};
const int supported_led_ids_count = ARRAY_SIZE(supported_led_ids);

int power_button_enable = 0;

struct pwm_led led_color_map[EC_LED_COLOR_COUNT] = {
				/* Red, Green, Blue */
	[EC_LED_COLOR_RED]    = {   8,   0,   0 },
	[EC_LED_COLOR_GREEN]  = {   0,   8,   0 },
	[EC_LED_COLOR_BLUE]   = {   0,   0,   8 },
	[EC_LED_COLOR_YELLOW] = {   4,   5,   0 },
	[EC_LED_COLOR_WHITE]  = {   4,  10,   5 },
	[EC_LED_COLOR_AMBER]  = {   9,   1,   0 },
};

struct pwm_led pwr_led_color_map[EC_LED_COLOR_COUNT] = {
				/* White, Green, Red */
	[EC_LED_COLOR_RED]    = {   0,   0,  15 },
	[EC_LED_COLOR_GREEN]  = {   0,  15,   0 },
	[EC_LED_COLOR_BLUE]   = {   0,   0,   0 },
	[EC_LED_COLOR_YELLOW] = {   0,   5,  10 },
	[EC_LED_COLOR_WHITE]  = {  15,   0,   0 },
	[EC_LED_COLOR_AMBER]  = {   0,   5,  30 },
};

struct pwm_led breath_led_color_map[EC_LED_COLOR_COUNT] = {
				/* White, Green, Red */
	[EC_LED_COLOR_WHITE]  = {  50,   0,   0 },
};

struct pwm_led pwm_leds[CONFIG_LED_PWM_COUNT] = {
	[PWM_LED0] = {
		/* left port LEDs */
		.ch0 = PWM_CH_DB0_LED_RED,
		.ch1 = PWM_CH_DB0_LED_GREEN,
		.ch2 = PWM_CH_DB0_LED_BLUE,
		.enable = &pwm_enable,
		.set_duty = &pwm_set_duty,
	},
	[PWM_LED1] = {
		/* right port LEDs */
		.ch0 = PWM_CH_DB1_LED_RED,
		.ch1 = PWM_CH_DB1_LED_GREEN,
		.ch2 = PWM_CH_DB1_LED_BLUE,
		.enable = &pwm_enable,
		.set_duty = &pwm_set_duty,
	},
	[PWM_LED2] = {
		/* Power Button LEDs */
		.ch0 = PWM_CH_FPR_LED_RED,
		.ch1 = PWM_CH_FPR_LED_GREEN,
		.ch2 = PWM_CH_FPR_LED_BLUE,
		.enable = &pwm_enable,
		.set_duty = &pwm_set_duty,
	},
};

void set_pwr_led_color(enum pwm_led_id id, int color)
{
	struct pwm_led duty = { 0 };
	const struct pwm_led *led = &pwm_leds[id];

	if ((id >= CONFIG_LED_PWM_COUNT) || (id < 0) ||
	    (color >= EC_LED_COLOR_COUNT) || (color < -1))
		return;
		
	if (color != -1) {
		duty.ch0 = pwr_led_color_map[color].ch0;
		duty.ch1 = pwr_led_color_map[color].ch1;
		duty.ch2 = pwr_led_color_map[color].ch2;
	}

	if (led->ch0 != (enum pwm_channel)PWM_LED_NO_CHANNEL)
		led->set_duty(led->ch0, duty.ch0);
	if (led->ch1 != (enum pwm_channel)PWM_LED_NO_CHANNEL)
		led->set_duty(led->ch1, duty.ch1);
	if (led->ch2 != (enum pwm_channel)PWM_LED_NO_CHANNEL)
		led->set_duty(led->ch2, duty.ch2);
}

void enable_pwr_breath(enum pwm_led_id id, int color, uint8_t enable)
{
	struct pwm_led duty = { 0 };
	const struct pwm_led *led = &pwm_leds[id];

	if ((id >= CONFIG_LED_PWM_COUNT) || (id < 0) ||
	    (color >= EC_LED_COLOR_COUNT) || (color < -1))
		return;

	if (color != -1) {
		duty.ch0 = breath_led_color_map[color].ch0;
		duty.ch1 = breath_led_color_map[color].ch1;
		duty.ch2 = breath_led_color_map[color].ch2;
	}

	if (led->ch0 != (enum pwm_channel)PWM_LED_NO_CHANNEL)
		bbled_enable(led->ch0, duty.ch0, BREATH_ON_LENGTH, BREATH_OFF_LENGTH, enable);
	if (led->ch1 != (enum pwm_channel)PWM_LED_NO_CHANNEL)
		bbled_enable(led->ch1, duty.ch1, BREATH_ON_LENGTH, BREATH_OFF_LENGTH, enable);
	if (led->ch2 != (enum pwm_channel)PWM_LED_NO_CHANNEL)
		bbled_enable(led->ch2, duty.ch2, BREATH_ON_LENGTH, BREATH_OFF_LENGTH, enable);
}

void led_get_brightness_range(enum ec_led_id led_id, uint8_t *brightness_range)
{
	brightness_range[EC_LED_COLOR_RED] = 100;
	brightness_range[EC_LED_COLOR_GREEN] = 100;
	brightness_range[EC_LED_COLOR_YELLOW] = 100;
	brightness_range[EC_LED_COLOR_AMBER] = 100;
	brightness_range[EC_LED_COLOR_BLUE] = 100;
	brightness_range[EC_LED_COLOR_WHITE] = 100;
}

int led_set_brightness(enum ec_led_id led_id, const uint8_t *brightness)
{
	enum pwm_led_id pwm_id;

	/* Convert ec_led_id to pwm_led_id. */
	if (led_id == EC_LED_ID_LEFT_LED)
		pwm_id = PWM_LED0;
	else if (led_id == EC_LED_ID_RIGHT_LED)
		pwm_id = PWM_LED1;
	else if (led_id == EC_LED_ID_POWER_LED)
		pwm_id = PWM_LED2;
	else
		return EC_ERROR_UNKNOWN;

	if (led_id == EC_LED_ID_POWER_LED) {
		if (brightness[EC_LED_COLOR_RED])
			set_pwr_led_color(pwm_id, EC_LED_COLOR_RED);
		else if (brightness[EC_LED_COLOR_GREEN])
			set_pwr_led_color(pwm_id, EC_LED_COLOR_GREEN);
		else if (brightness[EC_LED_COLOR_BLUE])
			set_pwr_led_color(pwm_id, EC_LED_COLOR_BLUE);
		else if (brightness[EC_LED_COLOR_YELLOW])
			set_pwr_led_color(pwm_id, EC_LED_COLOR_YELLOW);
		else if (brightness[EC_LED_COLOR_WHITE])
			set_pwr_led_color(pwm_id, EC_LED_COLOR_WHITE);
		else if (brightness[EC_LED_COLOR_AMBER])
			set_pwr_led_color(pwm_id, EC_LED_COLOR_AMBER);
		else
			/* Otherwise, the "color" is "off". */
			set_pwr_led_color(pwm_id, -1);
	} else {
		if (brightness[EC_LED_COLOR_RED])
			set_pwm_led_color(pwm_id, EC_LED_COLOR_RED);
		else if (brightness[EC_LED_COLOR_GREEN])
			set_pwm_led_color(pwm_id, EC_LED_COLOR_GREEN);
		else if (brightness[EC_LED_COLOR_BLUE])
			set_pwm_led_color(pwm_id, EC_LED_COLOR_BLUE);
		else if (brightness[EC_LED_COLOR_YELLOW])
			set_pwm_led_color(pwm_id, EC_LED_COLOR_YELLOW);
		else if (brightness[EC_LED_COLOR_WHITE])
			set_pwm_led_color(pwm_id, EC_LED_COLOR_WHITE);
		else if (brightness[EC_LED_COLOR_AMBER])
			set_pwm_led_color(pwm_id, EC_LED_COLOR_AMBER);
		else
			/* Otherwise, the "color" is "off". */
			set_pwm_led_color(pwm_id, -1);
	}

	return EC_SUCCESS;
}

static void set_active_port_color(int color)
{
	int port_charging_active = 0;

	if (led_auto_control_is_enabled(EC_LED_ID_LEFT_LED)) {
		port_charging_active = gpio_get_level(GPIO_TYPEC2_VBUS_ON_EC) ||
								gpio_get_level(GPIO_TYPEC3_VBUS_ON_EC);
		set_pwm_led_color(PWM_LED0, port_charging_active ? color : -1);
	}

	if (led_auto_control_is_enabled(EC_LED_ID_RIGHT_LED)) {
		port_charging_active = gpio_get_level(GPIO_TYPEC0_VBUS_ON_EC) ||
								gpio_get_level(GPIO_TYPEC1_VBUS_ON_EC);
		set_pwm_led_color(PWM_LED1, port_charging_active ? color : -1);
	}
}

static void led_set_battery(void)
{
	static int battery_ticks;
	uint32_t chflags = charge_get_flags();

	battery_ticks++;
	/* Blink both mainboard LEDS as a warning if the chasssis is open and power is on */
	if (!gpio_get_level(GPIO_CHASSIS_OPEN)) {
		set_pwm_led_color(PWM_LED0, (battery_ticks & 0x2) ? EC_LED_COLOR_RED : -1);
		set_pwm_led_color(PWM_LED1, (battery_ticks & 0x2) ? EC_LED_COLOR_RED : -1);
		return;
	}

	switch (charge_get_state()) {
	case PWR_STATE_CHARGE:
		/* Always indicate when charging, even in suspend. */
		set_active_port_color(EC_LED_COLOR_AMBER);
		break;
	case PWR_STATE_DISCHARGE:
		if (led_auto_control_is_enabled(EC_LED_ID_RIGHT_LED)) {
			if (charge_get_percent() < 10)
				set_active_port_color((battery_ticks & 0x2) ?
					EC_LED_COLOR_RED : -1);
			else
				set_active_port_color(-1);
		}
		break;
	case PWR_STATE_ERROR:
		set_active_port_color((battery_ticks & 0x2) ?
				EC_LED_COLOR_WHITE : -1);
		break;
	case PWR_STATE_CHARGE_NEAR_FULL:
		set_active_port_color(EC_LED_COLOR_WHITE);
		break;
	case PWR_STATE_IDLE:
		if (chflags & CHARGE_FLAG_FORCE_IDLE)
			set_active_port_color((battery_ticks & 0x4) ?
					EC_LED_COLOR_AMBER : -1);
		else
			set_active_port_color(EC_LED_COLOR_WHITE);
		break;
	default:
		break;
	}

}

static void led_set_power(void)
{
	static int power_tick;

	power_tick++;

	/* don't light up when at lid close */
	if (!lid_is_open()) {
		set_pwr_led_color(PWM_LED2, -1);
		return;
	}

	if (chipset_in_state(CHIPSET_STATE_ANY_SUSPEND))
		enable_pwr_breath(PWM_LED2, EC_LED_COLOR_WHITE, 1);
	else
		enable_pwr_breath(PWM_LED2, EC_LED_COLOR_WHITE, 0);

	if (chipset_in_state(CHIPSET_STATE_ON) | power_button_enable) {
		if (charge_prevent_power_on(0))
			set_pwr_led_color(PWM_LED2, (power_tick %
				LED_TICKS_PER_CYCLE < LED_ON_TICKS) ?
				EC_LED_COLOR_RED : -1);
		else
			set_pwr_led_color(PWM_LED2, EC_LED_COLOR_WHITE);
	} else
		set_pwr_led_color(PWM_LED2, -1);
}

/* Called by hook task every TICK */
static void led_tick(void)
{
	if (led_auto_control_is_enabled(EC_LED_ID_POWER_LED))
		led_set_power();

	led_set_battery();
}

static void led_configure(void)
{
	int i;

	/*Initialize PWM channels*/
	for (i = 0; i < PWM_CH_COUNT; i++) {
		pwm_enable(i, 1);
	}
	led_tick();
}

DECLARE_HOOK(HOOK_TICK, led_tick, HOOK_PRIO_DEFAULT);
/*Run after PWM init is complete*/
DECLARE_HOOK(HOOK_INIT, led_configure, HOOK_PRIO_DEFAULT+1);

void power_button_enable_led(int enable)
{
	power_button_enable = enable;
}

