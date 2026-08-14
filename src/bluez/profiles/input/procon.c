// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2026  Tim Van Dyke <tim.vandyke123@gmail.com>
 *
 *  Nintendo Switch Pro Controller wired cable-pairing protocol (see the
 *  RE sources in README §11).
 *
 *  All subcmds run over the hidraw fd of the USB-connected controller, which
 *  the stock hid-nintendo driver creates on bind (HID_CONNECT_HIDRAW).
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <unistd.h>
#include <glib.h>

#include "bluetooth/bluetooth.h"
#include "src/log.h"

#include "profiles/input/procon.h"

/* Send one subcommand over the hidraw fd and wait for the 0x21 reply.
 * Send: write 64-byte output report 0x01, then poll for the matching
 * subcmd reply. Returns 0 on ACK (>= 0x80), negative errno
 * otherwise. On success, reply_out points into `buf` (full 64-byte report;
 * caller indexes [15..] for payload). */
static int procon_send_subcmd(int fd, uint8_t *counter, uint8_t subcmd,
				const uint8_t *data, size_t len,
				uint8_t buf[64])
{
	uint8_t pkt[64];
	fd_set rfds;
	struct timeval tv;
	int ret;

	memset(pkt, 0, sizeof(pkt));
	pkt[0] = PROCON_REPORT_SUBCMD;
	pkt[1] = (*counter) & 0x0f;
	pkt[10] = subcmd;
	if (data && len)
		memcpy(pkt + 11, data, len);

	(*counter)++;

	ret = write(fd, pkt, sizeof(pkt));
	if (ret < 0)
		return -errno;

	for (;;) {
		FD_ZERO(&rfds);
		FD_SET(fd, &rfds);
		tv.tv_sec = 2;
		tv.tv_usec = 0;

		ret = select(fd + 1, &rfds, NULL, NULL, &tv);
		if (ret < 0) {
			if (errno == EINTR)
				continue;
			return -errno;
		}
		if (ret == 0) {
			error("procon: subcmd 0x%02x: no reply", subcmd);
			return -ETIMEDOUT;
		}

		ret = read(fd, buf, 64);
		if (ret < 0) {
			if (errno == EINTR)
				continue;
			return -errno;
		}

		/* Input report 0x21 = subcmd reply:
		 * [13] = ACK (0x80+ = ok, 0x00 = NACK), [14] = subcmd id */
		if (buf[0] == PROCON_REPORT_ACK && buf[14] == subcmd) {
			if (buf[13] < 0x80) {
				error("procon: subcmd 0x%02x NACK (ack=0x%02x)",
					subcmd, buf[13]);
				return -EIO;
			}
			return 0;
		}
	}
}

/* Send a 2-byte USB-mode command [0x80][cmd] and wait for the [0x81][cmd]
 * reply. These are raw 2-byte writes on the same hidraw fd as the
 * subcommands; they start the wired UART session with the BT chip. */
static int procon_usb_cmd(int fd, uint8_t cmd)
{
	uint8_t pkt[2] = { 0x80, cmd };
	uint8_t buf[64];
	fd_set rfds;
	struct timeval tv;
	int ret;

	ret = write(fd, pkt, sizeof(pkt));
	if (ret < 0)
		return -errno;

	for (;;) {
		FD_ZERO(&rfds);
		FD_SET(fd, &rfds);
		tv.tv_sec = 2;
		tv.tv_usec = 0;

		ret = select(fd + 1, &rfds, NULL, NULL, &tv);
		if (ret < 0) {
			if (errno == EINTR)
				continue;
			return -errno;
		}
		if (ret == 0) {
			error("procon: usb 0x80 0x%02x: no reply", cmd);
			return -ETIMEDOUT;
		}

		ret = read(fd, buf, sizeof(buf));
		if (ret < 0) {
			if (errno == EINTR)
				continue;
			return -errno;
		}
		if (ret >= 2 && buf[0] == 0x81 && buf[1] == cmd) {
			info("procon: usb 0x80 0x%02x ack", cmd);
			return 0;
		}
	}
}

/* Start the wired UART session with the controller's BT chip.
 * 80 02, 80 03 (3 Mbit), 80 02 — the second 0x02 is required for the baud
 * switch to take effect. Must run BEFORE any
 * subcommand, or the chip never answers (hid-nintendo's fork probe is
 * passive and sends nothing). Called once at session start (setup_device). */
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

/* subcmd 0x02 (REQ_DEV_INFO): reply data[4..9] = controller BT address.
 * Kernel hid-nintendo.c joycon_read_info(): ctlr->mac_addr[j] =
 * subcmd_reply.data[4 + j]. The raw 0x21 report's payload starts at byte 15,
 * so data[0] == buf[15] and the MAC lives at buf[19..25]. The MAC comes back
 * in display (big-endian) order; bdaddr_t wants wire order, so baswap. */
int procon_get_device_bdaddr(int fd, bdaddr_t *bdaddr)
{
	uint8_t buf[64];
	uint8_t counter = 0;
	int ret;

	ret = procon_send_subcmd(fd, &counter, PROCON_SUBCMD_REQ_DEV_INFO,
					NULL, 0, buf);
	if (ret < 0)
		return ret;

	baswap(bdaddr, (bdaddr_t *)(buf + 19));

	return 0;
}

/* The wired reconnect-keeper arm: subcmd 0x08 00 (clear shipment mode, SPI
 * x5000) — the console's USB command #2 (c2j redock: 0x02 → 0x08 00 → x04 →
 * 0x03 30, plan §3). Without it the controller stays in shipment mode and
 * NEVER pages the host on a button press. The Switch sends it after EVERY
 * connection (RE doc "Switch always sends x08 00 after every connection"),
 * including re-docking an already-paired controller — so setup_device arms
 * on every plug-in, and procon_pair re-arms before the 3-step. */
int procon_arm_wired(int fd)
{
	uint8_t buf[64];
	uint8_t data[1] = { PROCON_ARM_CLEAR_SHIPMENT_DATA };
	uint8_t counter = 0;
	int ret;

	ret = procon_send_subcmd(fd, &counter, PROCON_ARM_CLEAR_SHIPMENT,
					data, 1, buf);
	if (ret < 0)
		return ret;
	info("procon: armed (0x08 00 shipment cleared)");

	return 0;
}

/* The wired 3-step pairing:
 *
 *   step 1: subcmd 0x01, data[0]=0x01 (PAIR_STEP_HOST_MAC) + host MAC
 *           (HCI wire order — bdaddr_t is already wire order). Reply data[0]
 *           echoes 0x01, data[1..7] = controller MAC (LE; ignored here).
 *   step 2: subcmd 0x01, data[0]=0x02 (PAIR_STEP_GET_LTK). The controller
 *           returns the key in its flash order (Little-Endian), each byte
 *           XORed with 0xAA; the host uses it BYTE-REVERSED (verified against
 *           a genuine SSP pairing — controller flash bdfdff7b... == bluetoothd
 *           key f00a4af6...). Loading the un-reversed key fails auth 0x05.
 *   step 3: subcmd 0x01, data[0]=0x03 (PAIR_STEP_SAVE) — commit to flash.
 *
 * The controller GENERATES a fresh LTK, saves it + our MAC to its own SPI
 * flash, and hands the LTK to us over the wire. The plugin must then register
 * that LTK in bluetoothd storage + the kernel before the controller connects.
 */
int procon_pair(int fd, const bdaddr_t *host, uint8_t ltk[16])
{
	uint8_t buf[64];
	uint8_t data[8];
	uint8_t counter = 0;
	int ret, i;

	/* Re-establish the UART session before the 3-step. The session was
	 * initialized at setup_device (for the BDADDR probe), but the cable
	 * authorization gap (agent prompt) lets the controller's BT chip go
	 * idle and drop it — subcmds then get no reply. Re-initialize just
	 * before the 3-step so the chip is guaranteed awake. */
	ret = procon_usb_session_init(fd);
	if (ret < 0)
		return ret;

	/* Console order (c2j): ... 0x08 00 (arm) right before the pairing
	 * write. The Switch re-arms after every connection (bt_pairer.cpp:1427;
	 * RE doc "Switch always sends x08 00 after every connection") — a
	 * first-ever pairing must arm too, or the freshly-paired controller
	 * stays in shipment mode and never pages the host on a button press. */
	data[0] = PROCON_ARM_CLEAR_SHIPMENT_DATA;
	ret = procon_send_subcmd(fd, &counter, PROCON_ARM_CLEAR_SHIPMENT,
					data, 1, buf);
	if (ret < 0)
		return ret;
	info("procon: armed (0x08 00 shipment cleared)");

	/* Step 1: present our BT MAC, get the controller's MAC */
	data[0] = PROCON_PAIR_HOST_MAC;
	memcpy(data + 1, host, 6);
	ret = procon_send_subcmd(fd, &counter, PROCON_SUBCMD_BT_MANUAL_PAIR,
					data, 7, buf);
	if (ret < 0)
		return ret;
	if (buf[15] != PROCON_PAIR_HOST_MAC)
		return -EIO;
	info("procon: 3-step step 1 done (host MAC sent)");

	/* Step 2: get the LTK (each byte XORed with 0xAA), then byte-reverse */
	data[0] = PROCON_PAIR_GET_LTK;
	ret = procon_send_subcmd(fd, &counter, PROCON_SUBCMD_BT_MANUAL_PAIR,
					data, 1, buf);
	if (ret < 0)
		return ret;
	if (buf[15] != PROCON_PAIR_GET_LTK)
		return -EIO;
	for (i = 0; i < 16; i++)
		ltk[i] = buf[31 - i] ^ 0xAA;
	info("procon: 3-step step 2 done (LTK acquired)");

	/* Step 3: commit pairing info to controller flash */
	data[0] = PROCON_PAIR_SAVE;
	ret = procon_send_subcmd(fd, &counter, PROCON_SUBCMD_BT_MANUAL_PAIR,
					data, 1, buf);
	if (ret < 0)
		return ret;
	info("procon: 3-step step 3 done (saved on controller)");

	return 0;
}
