/* Copyright 2022 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include <zephyr/logging/log.h>

#include "charge_state_v2.h"
#include "chipset.h"
#include "hooks.h"
#include "usb_mux.h"
#include "system.h"
#include "driver/charger/isl923x_public.h"
#include "driver/retimer/anx7483_public.h"
#include "driver/tcpm/tcpci.h"
#include "driver/tcpm/raa489000.h"
#include "temp_sensor/temp_sensor.h"
#include "nissa_common.h"

LOG_MODULE_DECLARE(nissa, CONFIG_NISSA_LOG_LEVEL);

struct tcpc_config_t tcpc_config[CONFIG_USB_PD_PORT_MAX_COUNT] = {
	{
		.bus_type = EC_BUS_TYPE_I2C,
		.i2c_info = {
			.port = I2C_PORT_USB_C0_TCPC,
			.addr_flags = RAA489000_TCPC0_I2C_FLAGS,
		},
		.drv = &raa489000_tcpm_drv,
		/* RAA489000 implements TCPCI 2.0 */
		.flags = TCPC_FLAGS_TCPCI_REV2_0 |
			TCPC_FLAGS_VBUS_MONITOR,
	},
	{ /* sub-board */
		.bus_type = EC_BUS_TYPE_I2C,
		.i2c_info = {
			.port = I2C_PORT_USB_C1_TCPC,
			.addr_flags = RAA489000_TCPC0_I2C_FLAGS,
		},
		.drv = &raa489000_tcpm_drv,
		/* RAA489000 implements TCPCI 2.0 */
		.flags = TCPC_FLAGS_TCPCI_REV2_0 |
			TCPC_FLAGS_VBUS_MONITOR,
	},
};

int board_is_sourcing_vbus(int port)
{
	int regval;

	tcpc_read(port, TCPC_REG_POWER_STATUS, &regval);
	return !!(regval & TCPC_REG_POWER_STATUS_SOURCING_VBUS);
}

int board_set_active_charge_port(int port)
{
	int is_real_port = (port >= 0 && port < CONFIG_USB_PD_PORT_MAX_COUNT);
	int i;
	int old_port;

	if (!is_real_port && port != CHARGE_PORT_NONE)
		return EC_ERROR_INVAL;

	old_port = charge_manager_get_active_charge_port();

	LOG_INF("New chg p%d", port);

	/* Disable all ports. */
	if (port == CHARGE_PORT_NONE) {
		for (i = 0; i < CONFIG_USB_PD_PORT_MAX_COUNT; i++) {
			tcpc_write(i, TCPC_REG_COMMAND,
				   TCPC_REG_COMMAND_SNK_CTRL_LOW);
			raa489000_enable_asgate(i, false);
		}

		return EC_SUCCESS;
	}

	/* Check if port is sourcing VBUS. */
	if (board_is_sourcing_vbus(port)) {
		LOG_WRN("Skip enable p%d", port);
		return EC_ERROR_INVAL;
	}

	/*
	 * Turn off the other ports' sink path FETs, before enabling the
	 * requested charge port.
	 */
	for (i = 0; i < CONFIG_USB_PD_PORT_MAX_COUNT; i++) {
		if (i == port)
			continue;

		if (tcpc_write(i, TCPC_REG_COMMAND,
			       TCPC_REG_COMMAND_SNK_CTRL_LOW))
			LOG_WRN("p%d: sink path disable failed.", i);
		raa489000_enable_asgate(i, false);
	}

	/*
	 * Stop the charger IC from switching while changing ports.  Otherwise,
	 * we can overcurrent the adapter we're switching to. (crbug.com/926056)
	 */
	if (old_port != CHARGE_PORT_NONE)
		charger_discharge_on_ac(1);

	/* Enable requested charge port. */
	if (raa489000_enable_asgate(port, true) ||
	    tcpc_write(port, TCPC_REG_COMMAND,
		       TCPC_REG_COMMAND_SNK_CTRL_HIGH)) {
		LOG_WRN("p%d: sink path enable failed.", port);
		charger_discharge_on_ac(0);
		return EC_ERROR_UNKNOWN;
	}

	/* Allow the charger IC to begin/continue switching. */
	charger_discharge_on_ac(0);

	return EC_SUCCESS;
}

uint16_t tcpc_get_alert_status(void)
{
	uint16_t status = 0;
	int regval;

	/*
	 * The interrupt line is shared between the TCPC and BC1.2 detector IC.
	 * Therefore, go out and actually read the alert registers to report the
	 * alert status.
	 */
	if (!gpio_pin_get_dt(GPIO_DT_FROM_NODELABEL(gpio_usb_c0_int_odl))) {
		if (!tcpc_read16(0, TCPC_REG_ALERT, &regval)) {
			/* The TCPCI Rev 1.0 spec says to ignore bits 14:12. */
			if (!(tcpc_config[0].flags & TCPC_FLAGS_TCPCI_REV2_0))
				regval &= ~((1 << 14) | (1 << 13) | (1 << 12));

			if (regval)
				status |= PD_STATUS_TCPC_ALERT_0;
		}
	}

	if (board_get_usb_pd_port_count() == 2 &&
	    !gpio_pin_get_dt(GPIO_DT_FROM_ALIAS(gpio_usb_c1_int_odl))) {
		if (!tcpc_read16(1, TCPC_REG_ALERT, &regval)) {
			/* TCPCI spec Rev 1.0 says to ignore bits 14:12. */
			if (!(tcpc_config[1].flags & TCPC_FLAGS_TCPCI_REV2_0))
				regval &= ~((1 << 14) | (1 << 13) | (1 << 12));

			if (regval)
				status |= PD_STATUS_TCPC_ALERT_1;
		}
	}

	return status;
}

void pd_power_supply_reset(int port)
{
	/* Disable VBUS */
	tcpc_write(port, TCPC_REG_COMMAND, TCPC_REG_COMMAND_SRC_CTRL_LOW);

	/* Notify host of power info change. */
	pd_send_host_event(PD_EVENT_POWER_CHANGE);
}

__override void typec_set_source_current_limit(int port, enum tcpc_rp_value rp)
{
	if (port < 0 || port >= CONFIG_USB_PD_PORT_MAX_COUNT)
		return;

	raa489000_set_output_current(port, rp);
}

int pd_set_power_supply_ready(int port)
{
	int rv;

	if (port >= CONFIG_USB_PD_PORT_MAX_COUNT)
		return EC_ERROR_INVAL;

	/* Disable charging. */
	rv = tcpc_write(port, TCPC_REG_COMMAND, TCPC_REG_COMMAND_SNK_CTRL_LOW);
	if (rv)
		return rv;

	/* Our policy is not to source VBUS when the AP is off. */
	if (chipset_in_state(CHIPSET_STATE_ANY_OFF))
		return EC_ERROR_NOT_POWERED;

	/* Provide Vbus. */
	rv = tcpc_write(port, TCPC_REG_COMMAND, TCPC_REG_COMMAND_SRC_CTRL_HIGH);
	if (rv)
		return rv;

	rv = raa489000_enable_asgate(port, true);
	if (rv)
		return rv;

	/* Notify host of power info change. */
	pd_send_host_event(PD_EVENT_POWER_CHANGE);

	return EC_SUCCESS;
}

void board_reset_pd_mcu(void)
{
	/*
	 * TODO(b:147316511): could send a reset command to the TCPC here
	 * if needed.
	 */
}

/*
 * Because the TCPCs and BC1.2 chips share interrupt lines, it's possible
 * for an interrupt to be lost if one asserts the IRQ, the other does the same
 * then the first releases it: there will only be one falling edge to trigger
 * the interrupt, and the line will be held low. We handle this by running a
 * deferred check after a falling edge to see whether the IRQ is still being
 * asserted. If it is, we assume an interrupt may have been lost and we need
 * to poll each chip for events again.
 */
#define USBC_INT_POLL_DELAY_US 5000

static void poll_c0_int(void);
DECLARE_DEFERRED(poll_c0_int);
static void poll_c1_int(void);
DECLARE_DEFERRED(poll_c1_int);

static void usbc_interrupt_trigger(int port)
{
	schedule_deferred_pd_interrupt(port);
	usb_charger_task_set_event(port, USB_CHG_EVENT_BC12);
}

static inline void poll_usb_gpio(int port, const struct gpio_dt_spec *gpio,
				 const struct deferred_data *ud)
{
	if (!gpio_pin_get_dt(gpio)) {
		usbc_interrupt_trigger(port);
		hook_call_deferred(ud, USBC_INT_POLL_DELAY_US);
	}
}

static void poll_c0_int(void)
{
	poll_usb_gpio(0, GPIO_DT_FROM_NODELABEL(gpio_usb_c0_int_odl),
		      &poll_c0_int_data);
}

static void poll_c1_int(void)
{
	poll_usb_gpio(1, GPIO_DT_FROM_ALIAS(gpio_usb_c1_int_odl),
		      &poll_c1_int_data);
}

void usb_interrupt(enum gpio_signal signal)
{
	int port;
	const struct deferred_data *ud;

	if (signal == GPIO_SIGNAL(DT_NODELABEL(gpio_usb_c0_int_odl))) {
		port = 0;
		ud = &poll_c0_int_data;
	} else {
		port = 1;
		ud = &poll_c1_int_data;
	}
	/*
	 * We've just been called from a falling edge, so there's definitely
	 * no lost IRQ right now. Cancel any pending check.
	 */
	hook_call_deferred(ud, -1);
	/* Trigger polling of TCPC and BC1.2 in respective tasks */
	usbc_interrupt_trigger(port);
	/* Check for lost interrupts in a bit */
	hook_call_deferred(ud, USBC_INT_POLL_DELAY_US);
}

__override void board_set_charge_limit(int port, int supplier, int charge_ma,
				       int max_ma, int charge_mv)
{
	charge_ma = (charge_ma * 90) / 100;
	charge_set_input_current_limit(
		MAX(charge_ma, CONFIG_CHARGER_INPUT_CURRENT), charge_mv);
}

struct chg_curr_step {
	int on;
	int off;
	int curr_ma;
};

static const struct chg_curr_step chg_curr_table[] = {
	{ .on = 0, .off = 36, .curr_ma = 2800 },
	{ .on = 46, .off = 36, .curr_ma = 1500 },
	{ .on = 48, .off = 38, .curr_ma = 1000 },
};

/* All charge current tables must have the same number of levels */
#define NUM_CHG_CURRENT_LEVELS ARRAY_SIZE(chg_curr_table)

int charger_profile_override(struct charge_state_data *curr)
{
	int rv;
	int chg_temp_c;
	int current;
	int thermal_sensor0;
	static int current_level;
	static int prev_tmp;

	/*
	 * Precharge must be executed when communication is failed on
	 * dead battery.
	 */
	if (!(curr->batt.flags & BATT_FLAG_RESPONSIVE))
		return 0;

	current = curr->requested_current;

	rv = temp_sensor_read(
		TEMP_SENSOR_ID_BY_DEV(DT_NODELABEL(temp_charger1)),
		&thermal_sensor0);
	chg_temp_c = K_TO_C(thermal_sensor0);

	if (rv != EC_SUCCESS)
		return 0;

	if (chipset_in_state(CHIPSET_STATE_ON)) {
		if (chg_temp_c < prev_tmp) {
			if (chg_temp_c <= chg_curr_table[current_level].off)
				current_level = current_level - 1;
		} else if (chg_temp_c > prev_tmp) {
			if (chg_temp_c >= chg_curr_table[current_level + 1].on)
				current_level = current_level + 1;
		}
		/*
		 * Prevent level always minus 0 or over table steps.
		 */
		if (current_level < 0)
			current_level = 0;
		else if (current_level >= NUM_CHG_CURRENT_LEVELS)
			current_level = NUM_CHG_CURRENT_LEVELS - 1;

		prev_tmp = chg_temp_c;
		current = chg_curr_table[current_level].curr_ma;

		curr->requested_current = MIN(curr->requested_current, current);
	}
	return 0;
}

enum ec_status charger_profile_override_get_param(uint32_t param,
						  uint32_t *value)
{
	return EC_RES_INVALID_PARAM;
}

enum ec_status charger_profile_override_set_param(uint32_t param,
						  uint32_t value)
{
	return EC_RES_INVALID_PARAM;
}