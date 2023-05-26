/* Copyright 2018 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "atomic.h"
#include "cec.h"
#include "cec_chip.h"
#include "chipset.h"
#include "console.h"
#include "gpio.h"
#include "hooks.h"
#include "host_command.h"
#include "mkbp_event.h"
#include "power_button.h"
#include "printf.h"
#include "task.h"
#include "util.h"

#ifdef CONFIG_CEC_DEBUG
#define CPRINTF(format, args...) cprintf(CC_CEC, format, ##args)
#define CPRINTS(format, args...) cprints(CC_CEC, format, ##args)
#else
#define CPRINTF(...)
#define CPRINTS(...)
#endif

/* Notification from interrupt to CEC task that data has been received */
#define TASK_EVENT_RECEIVED_DATA TASK_EVENT_CUSTOM_BIT(0)
#define TASK_EVENT_OKAY TASK_EVENT_CUSTOM_BIT(1)
#define TASK_EVENT_FAILED TASK_EVENT_CUSTOM_BIT(2)

/* CEC broadcast address. Also the highest possible CEC address */
#define CEC_BROADCAST_ADDR 15

/* Address to indicate that no logical address has been set */
#define CEC_UNREGISTERED_ADDR 255

/*
 * The CEC specification requires at least one and a maximum of
 * five resends attempts
 */
#define CEC_MAX_RESENDS 5

/*
 * Free time timing (us). Our free-time is calculated from the end of
 * the last bit (not from the start). We compensate by having one
 * free-time period less than in the spec.
 */
#define NOMINAL_BIT_TICKS CEC_US_TO_TICKS(2400)
/* Resend */
#define FREE_TIME_RS_TICKS (2 * (NOMINAL_BIT_TICKS))
/* New initiator */
#define FREE_TIME_NI_TICKS (4 * (NOMINAL_BIT_TICKS))
/* Present initiator */
#define FREE_TIME_PI_TICKS (6 * (NOMINAL_BIT_TICKS))

/* Start bit timing */
#define START_BIT_LOW_TICKS CEC_US_TO_TICKS(3700)
#define START_BIT_MIN_LOW_TICKS CEC_US_TO_TICKS(3500)
#define START_BIT_MAX_LOW_TICKS CEC_US_TO_TICKS(3900)
#define START_BIT_HIGH_TICKS CEC_US_TO_TICKS(800)
#define START_BIT_MIN_DURATION_TICKS CEC_US_TO_TICKS(4300)
#define START_BIT_MAX_DURATION_TICKS CEC_US_TO_TICKS(5700)

/* Data bit timing */
#define DATA_ZERO_LOW_TICKS CEC_US_TO_TICKS(1500)
#define DATA_ZERO_MIN_LOW_TICKS CEC_US_TO_TICKS(1300)
#define DATA_ZERO_MAX_LOW_TICKS CEC_US_TO_TICKS(1700)
#define DATA_ZERO_HIGH_TICKS CEC_US_TO_TICKS(900)
#define DATA_ZERO_MIN_DURATION_TICKS CEC_US_TO_TICKS(2050)
#define DATA_ZERO_MAX_DURATION_TICKS CEC_US_TO_TICKS(2750)

#define DATA_ONE_LOW_TICKS CEC_US_TO_TICKS(600)
#define DATA_ONE_MIN_LOW_TICKS CEC_US_TO_TICKS(400)
#define DATA_ONE_MAX_LOW_TICKS CEC_US_TO_TICKS(800)
#define DATA_ONE_HIGH_TICKS CEC_US_TO_TICKS(1800)
#define DATA_ONE_MIN_DURATION_TICKS CEC_US_TO_TICKS(2050)
#define DATA_ONE_MAX_DURATION_TICKS CEC_US_TO_TICKS(2750)

/* Time from low that it should be safe to sample an ACK */
#define NOMINAL_SAMPLE_TIME_TICKS CEC_US_TO_TICKS(1050)

#define DATA_TIME(type, data) \
	((data) ? (DATA_ONE_##type##_TICKS) : (DATA_ZERO_##type##_TICKS))
#define DATA_HIGH(data) DATA_TIME(HIGH, data)
#define DATA_LOW(data) DATA_TIME(LOW, data)

/*
 * Number of short pulses seen before the debounce logic goes into ignoring
 * the bus for DEBOUNCE_WAIT_LONG instead of DEBOUNCE_WAIT_SHORT
 */
#define DEBOUNCE_CUTOFF 3

/* The limit how short a start-bit can be to trigger debounce logic */
#define DEBOUNCE_LIMIT_TICKS CEC_US_TO_TICKS(200)
/* The time we ignore the bus for the first three debounce cases */
#define DEBOUNCE_WAIT_SHORT_TICKS CEC_US_TO_TICKS(100)
/* The time we ignore the bus after the three initial debounce cases */
#define DEBOUNCE_WAIT_LONG_TICKS CEC_US_TO_TICKS(500)

/*
 * The variance in timing we allow outside of the CEC specification for
 * incoming signals. Our measurements aren't 100% accurate either, so this
 * gives some robustness.
 */
#define VALID_TOLERANCE_TICKS CEC_US_TO_TICKS(100)

/*
 * Defines used for setting capture timers to a point where we are
 * sure that if we get a timeout, something is wrong.
 */
#define CAP_START_LOW_TICKS (START_BIT_MAX_LOW_TICKS + VALID_TOLERANCE_TICKS)
#define CAP_START_HIGH_TICKS                                      \
	(START_BIT_MAX_DURATION_TICKS - START_BIT_MIN_LOW_TICKS + \
	 VALID_TOLERANCE_TICKS)
#define CAP_DATA_LOW_TICKS (DATA_ZERO_MAX_LOW_TICKS + VALID_TOLERANCE_TICKS)
#define CAP_DATA_HIGH_TICKS                                     \
	(DATA_ONE_MAX_DURATION_TICKS - DATA_ONE_MIN_LOW_TICKS + \
	 VALID_TOLERANCE_TICKS)

#define VALID_TIME(type, bit, t)                                          \
	((t) >= ((bit##_MIN_##type##_TICKS) - (VALID_TOLERANCE_TICKS)) && \
	 (t) <= (bit##_MAX_##type##_TICKS) + (VALID_TOLERANCE_TICKS))
#define VALID_LOW(bit, t) VALID_TIME(LOW, bit, t)
#define VALID_HIGH(bit, low_time, high_time)                   \
	(((low_time) + (high_time) <=                          \
	  bit##_MAX_DURATION_TICKS + VALID_TOLERANCE_TICKS) && \
	 ((low_time) + (high_time) >=                          \
	  bit##_MIN_DURATION_TICKS - VALID_TOLERANCE_TICKS))
#define VALID_DATA_HIGH(data, low_time, high_time)            \
	((data) ? VALID_HIGH(DATA_ONE, low_time, high_time) : \
		  VALID_HIGH(DATA_ZERO, low_time, high_time))

/*
 * CEC state machine states. Each state typically takes action on entry and
 * timeouts. INITIATIOR states are used for sending, FOLLOWER states are used
 *  for receiving.
 */
enum cec_state {
	CEC_STATE_DISABLED = 0,
	CEC_STATE_IDLE,
	CEC_STATE_INITIATOR_FREE_TIME,
	CEC_STATE_INITIATOR_START_LOW,
	CEC_STATE_INITIATOR_START_HIGH,
	CEC_STATE_INITIATOR_HEADER_INIT_LOW,
	CEC_STATE_INITIATOR_HEADER_INIT_HIGH,
	CEC_STATE_INITIATOR_HEADER_DEST_LOW,
	CEC_STATE_INITIATOR_HEADER_DEST_HIGH,
	CEC_STATE_INITIATOR_DATA_LOW,
	CEC_STATE_INITIATOR_DATA_HIGH,
	CEC_STATE_INITIATOR_EOM_LOW,
	CEC_STATE_INITIATOR_EOM_HIGH,
	CEC_STATE_INITIATOR_ACK_LOW,
	CEC_STATE_INITIATOR_ACK_HIGH,
	CEC_STATE_INITIATOR_ACK_VERIFY,
	CEC_STATE_FOLLOWER_START_LOW,
	CEC_STATE_FOLLOWER_START_HIGH,
	CEC_STATE_FOLLOWER_DEBOUNCE,
	CEC_STATE_FOLLOWER_HEADER_INIT_LOW,
	CEC_STATE_FOLLOWER_HEADER_INIT_HIGH,
	CEC_STATE_FOLLOWER_HEADER_DEST_LOW,
	CEC_STATE_FOLLOWER_HEADER_DEST_HIGH,
	CEC_STATE_FOLLOWER_EOM_LOW,
	CEC_STATE_FOLLOWER_EOM_HIGH,
	CEC_STATE_FOLLOWER_ACK_LOW,
	CEC_STATE_FOLLOWER_ACK_VERIFY,
	CEC_STATE_FOLLOWER_ACK_FINISH,
	CEC_STATE_FOLLOWER_DATA_LOW,
	CEC_STATE_FOLLOWER_DATA_HIGH,
};

/* Receive buffer and states */
struct cec_rx {
	/*
	 * The current incoming message being parsed. Copied to
	 * receive queue upon completion
	 */
	struct cec_msg_transfer transfer;
	/* End of Message received from source? */
	uint8_t eom;
	/* A follower NAK:ed a broadcast transfer */
	uint8_t broadcast_nak;
	/*
	 * Keep track of pulse low time to be able to verify
	 * pulse duration
	 */
	int low_ticks;
	/* Number of too short pulses seen in a row */
	int debounce_count;
};

/* Transfer buffer and states */
struct cec_tx {
	/* Outgoing message */
	struct cec_msg_transfer transfer;
	/* Message length */
	uint8_t len;
	/* Number of resends attempted in current send */
	uint8_t resends;
	/* Acknowledge received from sink? */
	uint8_t ack;
	/*
	 * When sending multiple concurrent frames,
	 * the free-time is slightly higher
	 */
	int present_initiator;
};

/*
 * Mutex for the read-offset of the rx queue. Needed since the
 * queue is read and flushed from different contexts
 */
static struct mutex rx_queue_readoffset_mutex;

/* Single state for CEC. We are INITIATOR, FOLLOWER or IDLE */
static enum cec_state cec_state;

/* Parameters and buffers for follower (receiver) state */
static struct cec_rx cec_rx;

/* Queue of completed incoming CEC messages */
static struct cec_rx_queue cec_rx_queue;

/* Parameters and buffer for initiator (sender) state */
static struct cec_tx cec_tx;

/*
 * CEC address of ourself. We ack incoming packages on this address.
 * However, the AP is responsible for writing the initiator address
 * on writes. UINT32_MAX means means that the address hasn't been
 * set by the AP yet.
 */
static uint8_t cec_addr = UINT8_MAX;

/* Events to send to AP */
static atomic_t cec_events;

int cec_transfer_get_bit(const struct cec_msg_transfer *transfer)
{
	if (transfer->byte >= MAX_CEC_MSG_LEN)
		return 0;

	return transfer->buf[transfer->byte] & (0x80 >> transfer->bit);
}

void cec_transfer_set_bit(struct cec_msg_transfer *transfer, int val)
{
	uint8_t bit_flag;

	if (transfer->byte >= MAX_CEC_MSG_LEN)
		return;
	bit_flag = 0x80 >> transfer->bit;
	transfer->buf[transfer->byte] &= ~bit_flag;
	if (val)
		transfer->buf[transfer->byte] |= bit_flag;
}

void cec_transfer_inc_bit(struct cec_msg_transfer *transfer)
{
	if (++(transfer->bit) == 8) {
		if (transfer->byte >= MAX_CEC_MSG_LEN)
			return;
		transfer->bit = 0;
		transfer->byte++;
	}
}

int cec_transfer_is_eom(const struct cec_msg_transfer *transfer, int len)
{
	if (transfer->bit)
		return 0;
	return (transfer->byte == len);
}

void cec_rx_queue_flush(struct cec_rx_queue *queue)
{
	mutex_lock(&rx_queue_readoffset_mutex);
	queue->read_offset = 0;
	mutex_unlock(&rx_queue_readoffset_mutex);
	queue->write_offset = 0;
}

struct cec_offline_policy cec_default_policy[] = {
	{
		.command = CEC_MSG_IMAGE_VIEW_ON,
		.action = CEC_ACTION_POWER_BUTTON,
	},
	{
		.command = CEC_MSG_TEXT_VIEW_ON,
		.action = CEC_ACTION_POWER_BUTTON,
	},
	/* Terminator */
	{ 0 },
};

__overridable const struct cec_config_t cec_config = {};

static enum cec_action cec_find_action(const struct cec_offline_policy *policy,
				       uint8_t command)
{
	if (policy == NULL)
		return CEC_ACTION_NONE;

	while (policy->command != 0 && policy->action != 0) {
		if (policy->command == command)
			return policy->action;
		policy++;
	}

	return CEC_ACTION_NONE;
}

int cec_process_offline_message(struct cec_rx_queue *queue, const uint8_t *msg,
				uint8_t msg_len)
{
	uint8_t command;
	char str_buf[hex_str_buf_size(msg_len)];

	if (!chipset_in_state(CHIPSET_STATE_ANY_OFF))
		/* Forward to the AP */
		return EC_ERROR_NOT_HANDLED;

	if (msg_len < 1)
		return EC_ERROR_INVAL;

	snprintf_hex_buffer(str_buf, sizeof(str_buf), HEX_BUF(msg, msg_len));
	CPRINTS("MSG: %s", str_buf);

	command = msg[1];

	if (cec_find_action(cec_config.offline_policy, command) ==
	    CEC_ACTION_POWER_BUTTON)
		/* Equal to PWRBTN_INITIAL_US (for x86). */
		power_button_simulate_press(200);

	/* Consumed */
	return EC_SUCCESS;
}

int cec_rx_queue_push(struct cec_rx_queue *queue, const uint8_t *msg,
		      uint8_t msg_len)
{
	int i;
	uint32_t offset;

	if (msg_len > MAX_CEC_MSG_LEN || msg_len == 0)
		return EC_ERROR_INVAL;

	offset = queue->write_offset;
	/* Fill in message length last, if successful. Set to zero for now */
	queue->buf[offset] = 0;
	offset = (offset + 1) % CEC_RX_BUFFER_SIZE;

	for (i = 0; i < msg_len; i++) {
		if (offset == queue->read_offset) {
			/* Buffer full */
			return EC_ERROR_OVERFLOW;
		}

		queue->buf[offset] = msg[i];
		offset = (offset + 1) % CEC_RX_BUFFER_SIZE;
	}

	/*
	 * Don't commit if we caught up with read-offset
	 * since that would indicate an empty buffer
	 */
	if (offset == queue->read_offset) {
		/* Buffer full */
		return EC_ERROR_OVERFLOW;
	}

	/* Commit the push */
	queue->buf[queue->write_offset] = msg_len;
	queue->write_offset = offset;

	return EC_SUCCESS;
}

int cec_rx_queue_pop(struct cec_rx_queue *queue, uint8_t *msg, uint8_t *msg_len)
{
	int i;

	mutex_lock(&rx_queue_readoffset_mutex);
	if (queue->read_offset == queue->write_offset) {
		/* Queue empty */
		mutex_unlock(&rx_queue_readoffset_mutex);
		*msg_len = 0;
		return -1;
	}

	/* The first byte in the buffer is the message length */
	*msg_len = queue->buf[queue->read_offset];
	if (*msg_len == 0 || *msg_len > MAX_CEC_MSG_LEN) {
		mutex_unlock(&rx_queue_readoffset_mutex);
		*msg_len = 0;
		cprintf(CC_CEC, "Invalid CEC msg size: %u\n", *msg_len);
		return -1;
	}

	queue->read_offset = (queue->read_offset + 1) % CEC_RX_BUFFER_SIZE;
	for (i = 0; i < *msg_len; i++) {
		msg[i] = queue->buf[queue->read_offset];
		queue->read_offset =
			(queue->read_offset + 1) % CEC_RX_BUFFER_SIZE;
	}

	mutex_unlock(&rx_queue_readoffset_mutex);

	return 0;
}

static void send_mkbp_event(uint32_t event)
{
	atomic_or(&cec_events, event);
	mkbp_send_event(EC_MKBP_EVENT_CEC_EVENT);
}

static void enter_state(enum cec_state new_state)
{
	int gpio = -1, timeout = -1;
	enum cec_cap_edge cap_edge = CEC_CAP_EDGE_NONE;
	uint8_t addr;

	cec_state = new_state;
	switch (new_state) {
	case CEC_STATE_DISABLED:
		gpio = 1;
		memset(&cec_rx, 0, sizeof(struct cec_rx));
		memset(&cec_tx, 0, sizeof(struct cec_tx));
		memset(&cec_rx_queue, 0, sizeof(struct cec_rx_queue));
		cec_events = 0;
		break;
	case CEC_STATE_IDLE:
		cec_tx.transfer.bit = 0;
		cec_tx.transfer.byte = 0;
		cec_rx.transfer.bit = 0;
		cec_rx.transfer.byte = 0;
		if (cec_tx.len > 0) {
			/* Execute a postponed send */
			enter_state(CEC_STATE_INITIATOR_FREE_TIME);
		} else {
			/* Wait for incoming command */
			gpio = 1;
			cap_edge = CEC_CAP_EDGE_FALLING;
			timeout = 0;
		}
		break;
	case CEC_STATE_INITIATOR_FREE_TIME:
		gpio = 1;
		cap_edge = CEC_CAP_EDGE_FALLING;
		if (cec_tx.resends)
			timeout = FREE_TIME_RS_TICKS;
		else if (cec_tx.present_initiator)
			timeout = FREE_TIME_PI_TICKS;
		else
			timeout = FREE_TIME_NI_TICKS;
		break;
	case CEC_STATE_INITIATOR_START_LOW:
		cec_tx.present_initiator = 1;
		cec_tx.transfer.bit = 0;
		cec_tx.transfer.byte = 0;
		gpio = 0;
		timeout = START_BIT_LOW_TICKS;
		break;
	case CEC_STATE_INITIATOR_START_HIGH:
		gpio = 1;
		cap_edge = CEC_CAP_EDGE_FALLING;
		timeout = START_BIT_HIGH_TICKS;
		break;
	case CEC_STATE_INITIATOR_HEADER_INIT_LOW:
	case CEC_STATE_INITIATOR_HEADER_DEST_LOW:
	case CEC_STATE_INITIATOR_DATA_LOW:
		gpio = 0;
		timeout = DATA_LOW(cec_transfer_get_bit(&cec_tx.transfer));
		break;
	case CEC_STATE_INITIATOR_HEADER_INIT_HIGH:
		gpio = 1;
		cap_edge = CEC_CAP_EDGE_FALLING;
		timeout = DATA_HIGH(cec_transfer_get_bit(&cec_tx.transfer));
		break;
	case CEC_STATE_INITIATOR_HEADER_DEST_HIGH:
	case CEC_STATE_INITIATOR_DATA_HIGH:
		gpio = 1;
		timeout = DATA_HIGH(cec_transfer_get_bit(&cec_tx.transfer));
		break;
	case CEC_STATE_INITIATOR_EOM_LOW:
		gpio = 0;
		timeout = DATA_LOW(
			cec_transfer_is_eom(&cec_tx.transfer, cec_tx.len));
		break;
	case CEC_STATE_INITIATOR_EOM_HIGH:
		gpio = 1;
		timeout = DATA_HIGH(
			cec_transfer_is_eom(&cec_tx.transfer, cec_tx.len));
		break;
	case CEC_STATE_INITIATOR_ACK_LOW:
		gpio = 0;
		timeout = DATA_LOW(1);
		break;
	case CEC_STATE_INITIATOR_ACK_HIGH:
		gpio = 1;
		/* Aim for the middle of the safe sample time */
		timeout = (DATA_ONE_LOW_TICKS + DATA_ZERO_LOW_TICKS) / 2 -
			  DATA_ONE_LOW_TICKS;
		break;
	case CEC_STATE_INITIATOR_ACK_VERIFY:
		cec_tx.ack = !gpio_get_level(CEC_GPIO_OUT);
		if ((cec_tx.transfer.buf[0] & 0x0f) == CEC_BROADCAST_ADDR) {
			/*
			 * We are sending a broadcast. Any follower can
			 * can NAK a broadcast message the same way they
			 * would ACK a direct message
			 */
			cec_tx.ack = !cec_tx.ack;
		}
		/*
		 * We are at the safe sample time. Wait
		 * until the end of this bit
		 */
		timeout = NOMINAL_BIT_TICKS - NOMINAL_SAMPLE_TIME_TICKS;
		break;
	case CEC_STATE_FOLLOWER_START_LOW:
		cec_tx.present_initiator = 0;
		cap_edge = CEC_CAP_EDGE_RISING;
		timeout = CAP_START_LOW_TICKS;
		break;
	case CEC_STATE_FOLLOWER_START_HIGH:
		cec_rx.debounce_count = 0;
		cap_edge = CEC_CAP_EDGE_FALLING;
		timeout = CAP_START_HIGH_TICKS;
		break;
	case CEC_STATE_FOLLOWER_DEBOUNCE:
		if (cec_rx.debounce_count >= DEBOUNCE_CUTOFF) {
			timeout = DEBOUNCE_WAIT_LONG_TICKS;
		} else {
			timeout = DEBOUNCE_WAIT_SHORT_TICKS;
			cec_rx.debounce_count++;
		}
		break;
	case CEC_STATE_FOLLOWER_HEADER_INIT_LOW:
	case CEC_STATE_FOLLOWER_HEADER_DEST_LOW:
	case CEC_STATE_FOLLOWER_EOM_LOW:
		cap_edge = CEC_CAP_EDGE_RISING;
		timeout = CAP_DATA_LOW_TICKS;
		break;
	case CEC_STATE_FOLLOWER_HEADER_INIT_HIGH:
	case CEC_STATE_FOLLOWER_HEADER_DEST_HIGH:
	case CEC_STATE_FOLLOWER_EOM_HIGH:
		cap_edge = CEC_CAP_EDGE_FALLING;
		timeout = CAP_DATA_HIGH_TICKS;
		break;
	case CEC_STATE_FOLLOWER_ACK_LOW:
		addr = cec_rx.transfer.buf[0] & 0x0f;
		if (addr == cec_addr) {
			/* Destination is our address, so ACK the packet */
			gpio = 0;
		}
		/* Don't ack broadcast or packets whose destinations aren't us,
		 * but continue reading.
		 */
		timeout = NOMINAL_SAMPLE_TIME_TICKS;
		break;
	case CEC_STATE_FOLLOWER_ACK_VERIFY:
		/*
		 * We are at safe sample time. A broadcast frame is considered
		 * lost if any follower pulls the line low
		 */
		if ((cec_rx.transfer.buf[0] & 0x0f) == CEC_BROADCAST_ADDR)
			cec_rx.broadcast_nak = !gpio_get_level(CEC_GPIO_OUT);
		else
			cec_rx.broadcast_nak = 0;

		/*
		 * We release the ACK at the end of data zero low
		 * period (ACK is technically a zero).
		 */
		timeout = DATA_ZERO_LOW_TICKS - NOMINAL_SAMPLE_TIME_TICKS;
		break;
	case CEC_STATE_FOLLOWER_ACK_FINISH:
		gpio = 1;
		if (cec_rx.eom || cec_rx.transfer.byte >= MAX_CEC_MSG_LEN) {
			addr = cec_rx.transfer.buf[0] & 0x0f;
			if (addr == cec_addr || addr == CEC_BROADCAST_ADDR) {
				task_set_event(TASK_ID_CEC,
					       TASK_EVENT_RECEIVED_DATA);
			}
			timeout = DATA_ZERO_HIGH_TICKS;
		} else {
			cap_edge = CEC_CAP_EDGE_FALLING;
			timeout = CAP_DATA_HIGH_TICKS;
		}
		break;
	case CEC_STATE_FOLLOWER_DATA_LOW:
		cap_edge = CEC_CAP_EDGE_RISING;
		timeout = CAP_DATA_LOW_TICKS;
		break;
	case CEC_STATE_FOLLOWER_DATA_HIGH:
		cap_edge = CEC_CAP_EDGE_FALLING;
		timeout = CAP_DATA_HIGH_TICKS;
		break;
		/* No default case, since all states must be handled explicitly
		 */
	}

	if (gpio >= 0)
		gpio_set_level(CEC_GPIO_OUT, gpio);
	if (timeout >= 0) {
		cec_tmr_cap_start(cap_edge, timeout);
	}
}

void cec_event_timeout(void)
{
	switch (cec_state) {
	case CEC_STATE_DISABLED:
	case CEC_STATE_IDLE:
		break;
	case CEC_STATE_INITIATOR_FREE_TIME:
		enter_state(CEC_STATE_INITIATOR_START_LOW);
		break;
	case CEC_STATE_INITIATOR_START_LOW:
		enter_state(CEC_STATE_INITIATOR_START_HIGH);
		break;
	case CEC_STATE_INITIATOR_START_HIGH:
		enter_state(CEC_STATE_INITIATOR_HEADER_INIT_LOW);
		break;
	case CEC_STATE_INITIATOR_HEADER_INIT_LOW:
		enter_state(CEC_STATE_INITIATOR_HEADER_INIT_HIGH);
		break;
	case CEC_STATE_INITIATOR_HEADER_INIT_HIGH:
		cec_transfer_inc_bit(&cec_tx.transfer);
		if (cec_tx.transfer.bit == 4)
			enter_state(CEC_STATE_INITIATOR_HEADER_DEST_LOW);
		else
			enter_state(CEC_STATE_INITIATOR_HEADER_INIT_LOW);
		break;
	case CEC_STATE_INITIATOR_HEADER_DEST_LOW:
		enter_state(CEC_STATE_INITIATOR_HEADER_DEST_HIGH);
		break;
	case CEC_STATE_INITIATOR_HEADER_DEST_HIGH:
		cec_transfer_inc_bit(&cec_tx.transfer);
		if (cec_tx.transfer.byte == 1)
			enter_state(CEC_STATE_INITIATOR_EOM_LOW);
		else
			enter_state(CEC_STATE_INITIATOR_HEADER_DEST_LOW);
		break;
	case CEC_STATE_INITIATOR_EOM_LOW:
		enter_state(CEC_STATE_INITIATOR_EOM_HIGH);
		break;
	case CEC_STATE_INITIATOR_EOM_HIGH:
		enter_state(CEC_STATE_INITIATOR_ACK_LOW);
		break;
	case CEC_STATE_INITIATOR_ACK_LOW:
		enter_state(CEC_STATE_INITIATOR_ACK_HIGH);
		break;
	case CEC_STATE_INITIATOR_ACK_HIGH:
		enter_state(CEC_STATE_INITIATOR_ACK_VERIFY);
		break;
	case CEC_STATE_INITIATOR_ACK_VERIFY:
		if (cec_tx.ack) {
			if (!cec_transfer_is_eom(&cec_tx.transfer,
						 cec_tx.len)) {
				/* More data in this frame */
				enter_state(CEC_STATE_INITIATOR_DATA_LOW);
			} else {
				/* Transfer completed successfully */
				cec_tx.len = 0;
				cec_tx.resends = 0;
				enter_state(CEC_STATE_IDLE);
				task_set_event(TASK_ID_CEC, TASK_EVENT_OKAY);
			}
		} else {
			if (cec_tx.resends < CEC_MAX_RESENDS) {
				/* Resend */
				cec_tx.resends++;
				enter_state(CEC_STATE_INITIATOR_FREE_TIME);
			} else {
				/* Transfer failed */
				cec_tx.len = 0;
				cec_tx.resends = 0;
				enter_state(CEC_STATE_IDLE);
				task_set_event(TASK_ID_CEC, TASK_EVENT_FAILED);
			}
		}
		break;
	case CEC_STATE_INITIATOR_DATA_LOW:
		enter_state(CEC_STATE_INITIATOR_DATA_HIGH);
		break;
	case CEC_STATE_INITIATOR_DATA_HIGH:
		cec_transfer_inc_bit(&cec_tx.transfer);
		if (cec_tx.transfer.bit == 0)
			enter_state(CEC_STATE_INITIATOR_EOM_LOW);
		else
			enter_state(CEC_STATE_INITIATOR_DATA_LOW);
		break;
	case CEC_STATE_FOLLOWER_ACK_LOW:
		enter_state(CEC_STATE_FOLLOWER_ACK_VERIFY);
		break;
	case CEC_STATE_FOLLOWER_ACK_VERIFY:
		if (cec_rx.broadcast_nak)
			enter_state(CEC_STATE_IDLE);
		else
			enter_state(CEC_STATE_FOLLOWER_ACK_FINISH);
		break;
	case CEC_STATE_FOLLOWER_START_LOW:
	case CEC_STATE_FOLLOWER_START_HIGH:
	case CEC_STATE_FOLLOWER_DEBOUNCE:
	case CEC_STATE_FOLLOWER_HEADER_INIT_LOW:
	case CEC_STATE_FOLLOWER_HEADER_INIT_HIGH:
	case CEC_STATE_FOLLOWER_HEADER_DEST_LOW:
	case CEC_STATE_FOLLOWER_HEADER_DEST_HIGH:
	case CEC_STATE_FOLLOWER_EOM_LOW:
	case CEC_STATE_FOLLOWER_EOM_HIGH:
	case CEC_STATE_FOLLOWER_ACK_FINISH:
	case CEC_STATE_FOLLOWER_DATA_LOW:
	case CEC_STATE_FOLLOWER_DATA_HIGH:
		enter_state(CEC_STATE_IDLE);
		break;
	}
}

void cec_event_cap(void)
{
	int t;
	int data;

	switch (cec_state) {
	case CEC_STATE_IDLE:
		/* A falling edge during idle, likely a start bit */
		enter_state(CEC_STATE_FOLLOWER_START_LOW);
		break;
	case CEC_STATE_INITIATOR_FREE_TIME:
	case CEC_STATE_INITIATOR_START_HIGH:
	case CEC_STATE_INITIATOR_HEADER_INIT_HIGH:
		/*
		 * A falling edge during free-time, postpone
		 * this send and listen
		 */
		cec_tx.transfer.bit = 0;
		cec_tx.transfer.byte = 0;
		enter_state(CEC_STATE_FOLLOWER_START_LOW);
		break;
	case CEC_STATE_FOLLOWER_START_LOW:
		/* Rising edge of start bit, validate low time */
		t = cec_tmr_cap_get();
		if (VALID_LOW(START_BIT, t)) {
			cec_rx.low_ticks = t;
			enter_state(CEC_STATE_FOLLOWER_START_HIGH);
		} else if (t < DEBOUNCE_LIMIT_TICKS) {
			/* Wait a bit if start-pulses are really short */
			enter_state(CEC_STATE_FOLLOWER_DEBOUNCE);
		} else {
			enter_state(CEC_STATE_IDLE);
		}
		break;
	case CEC_STATE_FOLLOWER_START_HIGH:
		if (VALID_HIGH(START_BIT, cec_rx.low_ticks, cec_tmr_cap_get()))
			enter_state(CEC_STATE_FOLLOWER_HEADER_INIT_LOW);
		else
			enter_state(CEC_STATE_IDLE);
		break;
	case CEC_STATE_FOLLOWER_HEADER_INIT_LOW:
	case CEC_STATE_FOLLOWER_HEADER_DEST_LOW:
	case CEC_STATE_FOLLOWER_DATA_LOW:
		t = cec_tmr_cap_get();
		if (VALID_LOW(DATA_ZERO, t)) {
			cec_rx.low_ticks = t;
			cec_transfer_set_bit(&cec_rx.transfer, 0);
			enter_state(cec_state + 1);
		} else if (VALID_LOW(DATA_ONE, t)) {
			cec_rx.low_ticks = t;
			cec_transfer_set_bit(&cec_rx.transfer, 1);
			enter_state(cec_state + 1);
		} else {
			enter_state(CEC_STATE_IDLE);
		}
		break;
	case CEC_STATE_FOLLOWER_HEADER_INIT_HIGH:
		t = cec_tmr_cap_get();
		data = cec_transfer_get_bit(&cec_rx.transfer);
		if (VALID_DATA_HIGH(data, cec_rx.low_ticks, t)) {
			cec_transfer_inc_bit(&cec_rx.transfer);
			if (cec_rx.transfer.bit == 4)
				enter_state(CEC_STATE_FOLLOWER_HEADER_DEST_LOW);
			else
				enter_state(CEC_STATE_FOLLOWER_HEADER_INIT_LOW);
		} else {
			enter_state(CEC_STATE_IDLE);
		}
		break;
	case CEC_STATE_FOLLOWER_HEADER_DEST_HIGH:
		t = cec_tmr_cap_get();
		data = cec_transfer_get_bit(&cec_rx.transfer);
		if (VALID_DATA_HIGH(data, cec_rx.low_ticks, t)) {
			cec_transfer_inc_bit(&cec_rx.transfer);
			if (cec_rx.transfer.bit == 0)
				enter_state(CEC_STATE_FOLLOWER_EOM_LOW);
			else
				enter_state(CEC_STATE_FOLLOWER_HEADER_DEST_LOW);
		} else {
			enter_state(CEC_STATE_IDLE);
		}
		break;
	case CEC_STATE_FOLLOWER_EOM_LOW:
		t = cec_tmr_cap_get();
		if (VALID_LOW(DATA_ZERO, t)) {
			cec_rx.low_ticks = t;
			cec_rx.eom = 0;
			enter_state(CEC_STATE_FOLLOWER_EOM_HIGH);
		} else if (VALID_LOW(DATA_ONE, t)) {
			cec_rx.low_ticks = t;
			cec_rx.eom = 1;
			enter_state(CEC_STATE_FOLLOWER_EOM_HIGH);
		} else {
			enter_state(CEC_STATE_IDLE);
		}
		break;
	case CEC_STATE_FOLLOWER_EOM_HIGH:
		t = cec_tmr_cap_get();
		data = cec_rx.eom;
		if (VALID_DATA_HIGH(data, cec_rx.low_ticks, t))
			enter_state(CEC_STATE_FOLLOWER_ACK_LOW);
		else
			enter_state(CEC_STATE_IDLE);
		break;
	case CEC_STATE_FOLLOWER_ACK_LOW:
		enter_state(CEC_STATE_FOLLOWER_ACK_FINISH);
		break;
	case CEC_STATE_FOLLOWER_ACK_FINISH:
		enter_state(CEC_STATE_FOLLOWER_DATA_LOW);
		break;
	case CEC_STATE_FOLLOWER_DATA_HIGH:
		t = cec_tmr_cap_get();
		data = cec_transfer_get_bit(&cec_rx.transfer);
		if (VALID_DATA_HIGH(data, cec_rx.low_ticks, t)) {
			cec_transfer_inc_bit(&cec_rx.transfer);
			if (cec_rx.transfer.bit == 0)
				enter_state(CEC_STATE_FOLLOWER_EOM_LOW);
			else
				enter_state(CEC_STATE_FOLLOWER_DATA_LOW);
		} else {
			enter_state(CEC_STATE_IDLE);
		}
		break;
	default:
		break;
	}
}

void cec_event_tx(void)
{
	/*
	 * If we have an ongoing receive, this transfer
	 * will start when transitioning to IDLE
	 */
	if (cec_state == CEC_STATE_IDLE) {
		/*
		 * Only update the interrupt time if it's idle, otherwise it
		 * will interfere with the timing of the current transfer.
		 */
		cec_update_interrupt_time();
		enter_state(CEC_STATE_INITIATOR_FREE_TIME);
	}
}

__overridable void cec_update_interrupt_time(void)
{
}

static int cec_send(const uint8_t *msg, uint8_t len)
{
	int i;

	if (cec_tx.len != 0)
		return -1;

	cec_tx.len = len;

	CPRINTS("Send CEC:");
	for (i = 0; i < len && i < MAX_CEC_MSG_LEN; i++)
		CPRINTS(" 0x%02x", msg[i]);

	memcpy(cec_tx.transfer.buf, msg, len);

	cec_trigger_send();

	return 0;
}

static enum ec_status hc_cec_write(struct host_cmd_handler_args *args)
{
	const struct ec_params_cec_write *params = args->params;

	if (cec_state == CEC_STATE_DISABLED)
		return EC_RES_UNAVAILABLE;

	if (args->params_size == 0 || args->params_size > MAX_CEC_MSG_LEN)
		return EC_RES_INVALID_PARAM;

	if (cec_send(params->msg, args->params_size) != 0)
		return EC_RES_BUSY;

	return EC_RES_SUCCESS;
}
DECLARE_HOST_COMMAND(EC_CMD_CEC_WRITE_MSG, hc_cec_write, EC_VER_MASK(0));

static int cec_set_enable(uint8_t enable)
{
	if (enable != 0 && enable != 1)
		return EC_RES_INVALID_PARAM;

	/* Enabling when already enabled? */
	if (enable && cec_state != CEC_STATE_DISABLED)
		return EC_RES_SUCCESS;

	/* Disabling when already disabled? */
	if (!enable && cec_state == CEC_STATE_DISABLED)
		return EC_RES_SUCCESS;

	if (enable) {
		enter_state(CEC_STATE_IDLE);

		cec_enable_timer();

		CPRINTF("CEC enabled\n");
	} else {
		cec_disable_timer();

		enter_state(CEC_STATE_DISABLED);

		CPRINTF("CEC disabled\n");
	}

	return EC_RES_SUCCESS;
}

static int cec_set_logical_addr(uint8_t logical_addr)
{
	if (logical_addr >= CEC_BROADCAST_ADDR &&
	    logical_addr != CEC_UNREGISTERED_ADDR)
		return EC_RES_INVALID_PARAM;

	cec_addr = logical_addr;
	CPRINTF("CEC address set to: %u\n", cec_addr);

	return EC_RES_SUCCESS;
}

static enum ec_status hc_cec_set(struct host_cmd_handler_args *args)
{
	const struct ec_params_cec_set *params = args->params;

	switch (params->cmd) {
	case CEC_CMD_ENABLE:
		return cec_set_enable(params->val);
	case CEC_CMD_LOGICAL_ADDRESS:
		return cec_set_logical_addr(params->val);
	}

	return EC_RES_INVALID_PARAM;
}
DECLARE_HOST_COMMAND(EC_CMD_CEC_SET, hc_cec_set, EC_VER_MASK(0));

static enum ec_status hc_cec_get(struct host_cmd_handler_args *args)
{
	struct ec_response_cec_get *response = args->response;
	const struct ec_params_cec_get *params = args->params;

	switch (params->cmd) {
	case CEC_CMD_ENABLE:
		response->val = cec_state == CEC_STATE_DISABLED ? 0 : 1;
		break;
	case CEC_CMD_LOGICAL_ADDRESS:
		response->val = cec_addr;
		break;
	default:
		return EC_RES_INVALID_PARAM;
	}

	args->response_size = sizeof(*response);

	return EC_RES_SUCCESS;
}
DECLARE_HOST_COMMAND(EC_CMD_CEC_GET, hc_cec_get, EC_VER_MASK(0));

static int cec_get_next_event(uint8_t *out)
{
	uint32_t event_out = atomic_clear(&cec_events);

	memcpy(out, &event_out, sizeof(event_out));

	return sizeof(event_out);
}
DECLARE_EVENT_SOURCE(EC_MKBP_EVENT_CEC_EVENT, cec_get_next_event);

static int cec_get_next_msg(uint8_t *out)
{
	int rv;
	uint8_t msg_len, msg[MAX_CEC_MSG_LEN];

	rv = cec_rx_queue_pop(&cec_rx_queue, msg, &msg_len);
	if (rv != 0)
		return EC_RES_UNAVAILABLE;

	memcpy(out, msg, msg_len);

	return msg_len;
}
DECLARE_EVENT_SOURCE(EC_MKBP_EVENT_CEC_MESSAGE, cec_get_next_msg);

static void cec_init(void)
{
	cec_init_timer();

	/* If RO doesn't set it, RW needs to set it explicitly. */
	gpio_set_level(CEC_GPIO_PULL_UP, 1);

	/* Ensure the CEC bus is not pulled low by default on startup. */
	gpio_set_level(CEC_GPIO_OUT, 1);

	CPRINTS("CEC initialized");
}
DECLARE_HOOK(HOOK_INIT, cec_init, HOOK_PRIO_LAST);

void cec_task(void *unused)
{
	int rv;
	uint32_t events;

	CPRINTF("CEC task starting\n");

	while (1) {
		events = task_wait_event(-1);
		if (events & TASK_EVENT_RECEIVED_DATA) {
			if (cec_process_offline_message(
				    &cec_rx_queue, cec_rx.transfer.buf,
				    cec_rx.transfer.byte) == EC_SUCCESS) {
				CPRINTS("Message consumed offline");
				/* Continue to queue message and notify AP. */
			}
			rv = cec_rx_queue_push(&cec_rx_queue,
					       cec_rx.transfer.buf,
					       cec_rx.transfer.byte);
			if (rv == EC_ERROR_OVERFLOW) {
				/* Queue full, prefer the most recent msg */
				cec_rx_queue_flush(&cec_rx_queue);
				rv = cec_rx_queue_push(&cec_rx_queue,
						       cec_rx.transfer.buf,
						       cec_rx.transfer.byte);
			}
			if (rv == EC_SUCCESS)
				mkbp_send_event(EC_MKBP_EVENT_CEC_MESSAGE);
		}
		if (events & TASK_EVENT_OKAY) {
			send_mkbp_event(EC_MKBP_CEC_SEND_OK);
			CPRINTS("SEND OKAY");
		} else if (events & TASK_EVENT_FAILED) {
			send_mkbp_event(EC_MKBP_CEC_SEND_FAILED);
			CPRINTS("SEND FAILED");
		}
	}
}
