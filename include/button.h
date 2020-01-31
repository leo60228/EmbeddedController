/* Copyright 2014 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

/* Button API for Chrome EC */

#ifndef __CROS_EC_BUTTON_H
#define __CROS_EC_BUTTON_H

#include "common.h"
#include "compile_time_macros.h"
#include "gpio.h"
#include "ec_commands.h"

#define BUTTON_FLAG_ACTIVE_HIGH BIT(0)

#define BUTTON_DEBOUNCE_US (30 * MSEC)

struct button_config {
	const char *name;
	enum keyboard_button_type type;
	enum gpio_signal gpio;
	uint32_t debounce_us;
	int flags;
};

enum button {
#ifdef CONFIG_VOLUME_BUTTONS
	BUTTON_VOLUME_UP,
	BUTTON_VOLUME_DOWN,
#endif /* defined(CONFIG_VOLUME_BUTTONS) */
#ifdef CONFIG_DEDICATED_RECOVERY_BUTTON
	BUTTON_RECOVERY,
#endif /* defined(CONFIG_DEDICATED_RECOVERY_BUTTON) */
	BUTTON_COUNT,
};

/* Table of buttons for the board. */
#ifndef CONFIG_BUTTONS_RUNTIME_CONFIG
extern const struct button_config buttons[];
#else
extern struct button_config buttons[];
#endif

/*
 * Buttons used to decide whether recovery is requested or not
 */
extern const struct button_config *recovery_buttons[];
extern const int recovery_buttons_count;

/*
 * Button initialization, called from main.
 */
void button_init(void);

/*
 * Reassign a button GPIO signal at runtime.
 *
 * @param button_type	Button type to reassign
 * @param gpio		GPIO to assign to the button
 *
 * Returns EC_SUCCESS if button change is accepted and made active,
 * EC_ERROR_* otherwise.
 */
int button_reassign_gpio(enum button button_type, enum gpio_signal gpio);

/*
 * Interrupt handler for button.
 *
 * @param signal	Signal which triggered the interrupt.
 */
void button_interrupt(enum gpio_signal signal);

#endif  /* __CROS_EC_BUTTON_H */
