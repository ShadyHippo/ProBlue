// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2026  Tim Van Dyke <tim.vandyke123@gmail.com>
 *
 *  Nintendo Switch Pro Controller wired pairing protocol.
 *
 *  Every command here runs over the hidraw node that hid-nintendo creates when
 *  the controller is plugged in. The driver is passive on USB (see the ProBlue
 *  hid-nintendo patch), which makes bluetoothd the only writer on that node.
 *
 *  These calls are synchronous and run from the udev plugin context: they block
 *  for the duration of one command round-trip. The controller normally answers
 *  within milliseconds; the timeout is only a safety net against a dead or
 *  unresponsive controller.
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#define _GNU_SOURCE
#include <errno.h>
#include <stddef.h>
#include <string.h>
#include <sys/select.h>
#include <unistd.h>

#include <glib.h>

#include "bluetooth/bluetooth.h"
#include "src/log.h"

#include "profiles/input/procon.h"

#define PROCON_REPLY_TIMEOUT_SEC	2

const struct cable_pairing *get_nintendo_pairing(uint16_t vid, uint16_t pid,
							const char *name)
{
	static const struct cable_pairing devices[] = {
		{
			.name	 = PROCON_NAME,
			.source	 = 0x0002,	/* USB */
			.vid	 = PROCON_VID,
			.pid	 = PROCON_PID,
			.version = 0x0000,
			.type	 = CABLE_PAIRING_PROCON,
		},
	};

	for (size_t i = 0; i < G_N_ELEMENTS(devices); i++) {
		if (devices[i].vid != vid || devices[i].pid != pid)
			continue;
		if (name && !g_str_has_suffix(name, devices[i].name))
			continue;
		return &devices[i];
	}

	return NULL;
}

/* Wait for one report on @fd. Returns the number of bytes read, 0 on timeout,
 * or a negative errno. */
static int procon_wait_reply(int fd, uint8_t buf[64])
{
	fd_set rfds;
	int ret;

	for (;;) {
		struct timeval tv = { PROCON_REPLY_TIMEOUT_SEC, 0 };

		FD_ZERO(&rfds);
		FD_SET(fd, &rfds);

		ret = select(fd + 1, &rfds, NULL, NULL, &tv);
		if (ret < 0) {
			if (errno == EINTR)
				continue;
			return -errno;
		}
		if (ret == 0)
			return 0;

		ret = read(fd, buf, 64);
		if (ret < 0) {
			if (errno == EINTR)
				continue;
			return -errno;
		}

		return ret;
	}
}

/* Write one output report 0x01 carrying @subcmd and wait for its 0x21 reply.
 * On success the full reply is left in @reply, with the payload at
 * @reply[15], and its length is returned. Returns a negative errno on error. */
static int procon_send_subcmd(int fd, uint8_t *counter, uint8_t subcmd,
				const uint8_t *data, size_t len,
				uint8_t reply[64])
{
	uint8_t pkt[64] = { 0 };
	int ret;

	pkt[0] = PROCON_REPORT_SUBCMD;
	pkt[1] = (*counter)++ & 0x0f;
	pkt[10] = subcmd;
	if (data && len)
		memcpy(pkt + 11, data, len);

	if (write(fd, pkt, sizeof(pkt)) < 0)
		return -errno;

	for (;;) {
		ret = procon_wait_reply(fd, reply);
		if (ret < 0)
			return ret;
		if (ret == 0) {
			error("procon: subcmd 0x%02x: no reply", subcmd);
			return -ETIMEDOUT;
		}

		/* Skip anything that is not the reply we are waiting for. */
		if (ret < 16 || reply[0] != PROCON_REPORT_ACK ||
						reply[14] != subcmd)
			continue;

		if (reply[13] < 0x80) {
			error("procon: subcmd 0x%02x NACK (ack=0x%02x)",
						subcmd, reply[13]);
			return -EIO;
		}

		return ret;
	}
}

/* Send a wired session command [0x80][cmd] and wait for [0x81][cmd]. */
static int procon_usb_cmd(int fd, uint8_t cmd)
{
	uint8_t pkt[2] = { PROCON_USB_REPORT_CMD, cmd };
	uint8_t reply[64];
	int ret;

	if (write(fd, pkt, sizeof(pkt)) < 0)
		return -errno;

	for (;;) {
		ret = procon_wait_reply(fd, reply);
		if (ret < 0)
			return ret;
		if (ret == 0) {
			error("procon: usb 0x80 0x%02x: no reply", cmd);
			return -ETIMEDOUT;
		}
		if (ret >= 2 && reply[0] == PROCON_USB_REPORT_ACK &&
						reply[1] == cmd) {
			info("procon: usb 0x80 0x%02x ack", cmd);
			return 0;
		}
	}
}

/* Start the wired UART session with the controller's Bluetooth chip: 0x02,
 * 0x03 (3 Mbit), then 0x02 again to make the baud switch take effect. No
 * subcommand is answered before this has run (the passive hid-nintendo probe
 * never starts the session itself). */
int procon_usb_session_init(int fd)
{
	int ret;

	ret = procon_usb_cmd(fd, PROCON_USB_CMD_HANDSHAKE);
	if (ret < 0)
		return ret;

	ret = procon_usb_cmd(fd, PROCON_USB_CMD_BAUDRATE_3M);
	if (ret < 0)
		return ret;

	return procon_usb_cmd(fd, PROCON_USB_CMD_HANDSHAKE);
}

/* subcmd 0x08 00: clear the shipment low-power state so that a button press
 * can wake the controller and make it page the host. Used at plug-in and again
 * right before writing a new pairing, matching the console's order. */
static int procon_send_arm(int fd, uint8_t *counter)
{
	uint8_t data[1] = { PROCON_SHIPMENT_CLEAR };
	uint8_t reply[64];
	int ret;

	ret = procon_send_subcmd(fd, counter, PROCON_SUBCMD_SET_SHIPMENT_STATE,
					data, sizeof(data), reply);
	if (ret < 0)
		return ret;

	info("procon: armed (0x08 00 shipment cleared)");
	return 0;
}

int procon_arm_wired(int fd)
{
	uint8_t counter = 0;

	return procon_send_arm(fd, &counter);
}

/* subcmd 0x02 (device info). In the reply payload bytes [4..9] hold the
 * controller's Bluetooth address in display order, the same layout used by
 * hid-nintendo's joycon_read_info() (data[4 + j]). bdaddr_t is wire order, so
 * the address is swapped. */
int procon_get_device_bdaddr(int fd, bdaddr_t *bdaddr)
{
	uint8_t reply[64];
	uint8_t counter = 0;
	int ret;

	ret = procon_send_subcmd(fd, &counter, PROCON_SUBCMD_REQ_DEV_INFO,
					NULL, 0, reply);
	if (ret < 0)
		return ret;
	if (ret < 25)
		return -EIO;

	baswap(bdaddr, (bdaddr_t *)(reply + 19));
	return 0;
}

/* The wired 3-step pairing, in the console's order:
 *
 *   step 1  subcmd 0x01, payload 0x01 + our address; the controller replies
 *           with its own address (not needed here, subcmd 0x02 already gave it)
 *   step 2  subcmd 0x01, payload 0x02; the controller generates a fresh LTK,
 *           stores it together with our address in its flash, and returns it
 *           each byte XORed with 0xAA in flash (little-endian) order. As a
 *           BR/EDR link key it must be byte-reversed; loading it un-reversed
 *           fails authentication.
 *   step 3  subcmd 0x01, payload 0x03; commit the pairing.
 *
 * The caller then has to register the returned key with bluetoothd and the
 * kernel before the controller connects.
 */
int procon_pair(int fd, const bdaddr_t *host, uint8_t ltk[16])
{
	uint8_t data[8];
	uint8_t reply[64];
	uint8_t counter = 0;
	int ret, i;

	/* The session may have gone idle since setup_device(); restart it so
	 * the 3-step always starts on a live UART. */
	ret = procon_usb_session_init(fd);
	if (ret < 0)
		return ret;

	ret = procon_send_arm(fd, &counter);
	if (ret < 0)
		return ret;

	/* Step 1: hand over our address. */
	data[0] = PROCON_PAIR_HOST_MAC;
	memcpy(data + 1, host, 6);
	ret = procon_send_subcmd(fd, &counter, PROCON_SUBCMD_BT_MANUAL_PAIR,
					data, 7, reply);
	if (ret < 0)
		return ret;
	if (ret < 16 || reply[15] != PROCON_PAIR_HOST_MAC)
		return -EIO;
	info("procon: 3-step 1/3 (host address sent)");

	/* Step 2: acquire the link key. */
	data[0] = PROCON_PAIR_GET_LTK;
	ret = procon_send_subcmd(fd, &counter, PROCON_SUBCMD_BT_MANUAL_PAIR,
					data, 1, reply);
	if (ret < 0)
		return ret;
	if (ret < 32 || reply[15] != PROCON_PAIR_GET_LTK)
		return -EIO;
	for (i = 0; i < 16; i++)
		ltk[i] = reply[31 - i] ^ 0xAA;
	info("procon: 3-step 2/3 (link key acquired)");

	/* Step 3: save the pairing on the controller. */
	data[0] = PROCON_PAIR_SAVE;
	ret = procon_send_subcmd(fd, &counter, PROCON_SUBCMD_BT_MANUAL_PAIR,
					data, 1, reply);
	if (ret < 0)
		return ret;
	info("procon: 3-step 3/3 (saved on controller)");

	return 0;
}
