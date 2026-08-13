/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2026  Tim Van Dyke <tim.vandyke123@gmail.com>
 *
 *  Nintendo Switch Pro Controller cable pairing (over-cable).
 *
 *  The Pro Controller is NOT a PlayStation-style cable-paired device: it does
 *  not do over-the-air SSP when connecting. It arrives carrying a link key in
 *  its SPI flash (written by the Switch's 3-step protocol) and connects only
 *  to the host whose MAC + key it has stored. The wired flow therefore must:
 *
 *    1. read the controller's BT address (subcmd 0x02, device info),
 *    2. run the wired 3-step (subcmd 0x01: x01 host-MAC -> x02 GET_LTK ->
 *       x03 save) so the controller's flash agrees with our host,
 *    3. register the acquired LTK in bluetoothd storage AND load it into the
 *       kernel (MGMT_OP_LOAD_LINK_KEYS) BEFORE the controller connects —
 *       there is no runtime add-key path in BlueZ 5.84.
 *
 *  This is the vendor-specific half of the cable-pairing mechanism. The
 *  shared mechanism (CablePairingType enum, struct cable_pairing) lives in
 *  profiles/input/sixaxis.h — legacy name, shared infrastructure; the PS
 *  device table stays there, this file owns everything Nintendo.
 */

#ifndef _PROCON_H_
#define _PROCON_H_

#include "profiles/input/sixaxis.h"

#define PROCON_VID			0x057e
#define PROCON_PID			0x2009
#define PROCON_NAME			"Pro Controller"

/* Output report 0x01 (subcmd) layout, same over USB and BT intr channel:
 *   [0] = report id 0x01, [1] = packet counter (low nibble),
 *   [2..9] = rumble (8 zero bytes), [10] = subcmd id, [11+] = data.
 * Reply report 0x21: [13] = ACK (0x80+ = ok), [14] = subcmd id, [15+] = data.
 */
#define PROCON_REPORT_SUBCMD		0x01
#define PROCON_REPORT_ACK		0x21

/* USB-mode commands (raw 2-byte writes) */
#define PROCON_USB_CMD_HANDSHAKE	0x02	/* start UART session */
#define PROCON_USB_CMD_BAUDRATE_3M	0x03	/* switch to 3 Mbit */

#define PROCON_SUBCMD_BT_MANUAL_PAIR	0x01	/* the wired 3-step pairing */
#define PROCON_SUBCMD_REQ_DEV_INFO	0x02	/* controller MAC + type */
#define PROCON_SUBCMD_SET_REPORT_MODE	0x03	/* 0x30 = standard full report */
#define PROCON_REPORT_MODE_FULL		0x30	/* data for subcmd 0x03 (standard full) */
#define PROCON_SUBCMD_SET_SHIPMENT_STATE 0x08	/* 0x00 = clear (reconnect-keeper) */
#define PROCON_SUBCMD_SET_PLAYER_LIGHTS	0x30	/* 0x01..0x08 = player LEDs */

#define PROCON_PAIR_HOST_MAC		0x01
#define PROCON_PAIR_GET_LTK		0x02
#define PROCON_PAIR_SAVE		0x03

/* The Switch's post-connection arm (bt_pairer.cpp:1427) — sent over a BT
 * interrupt channel after EVERY connection:
 *   0x08 00 : clear shipment mode (SPI x5000) + enable LPM-to-sleep, so a
 *             button press wakes the controller later (the reconnect-keeper);
 *   0x30 01 : player-1 LED.
 * Nothing in the current stack sends these (evidence 21).
 */
#define PROCON_ARM_CLEAR_SHIPMENT	0x08
#define PROCON_ARM_CLEAR_SHIPMENT_DATA	0x00
#define PROCON_ARM_PLAYER1_LED		0x30
#define PROCON_ARM_PLAYER1_LED_DATA	0x01

int procon_usb_session_init(int fd);
int procon_arm_wired(int fd);
int procon_get_device_bdaddr(int fd, bdaddr_t *bdaddr);
int procon_pair(int fd, const bdaddr_t *host, uint8_t ltk[16]);

/* The Nintendo over-cable device table (the Nintendo counterpart of
 * sixaxis.h get_pairing()). Only the Pro Controller for now; the table
 * exists so the shared input-profile gate (server.c) can recognize
 * Nintendo cable-paired devices the same way it recognizes PS ones. */
static inline const struct cable_pairing *
get_nintendo_pairing(uint16_t vid, uint16_t pid, const char *name)
{
	static const struct cable_pairing devices[] = {
		{
			.name = PROCON_NAME,
			.source = 0x0002, /* USB */
			.vid = PROCON_VID,
			.pid = PROCON_PID,
			.version = 0x0000,
			.type = CABLE_PAIRING_PROCON,
		},
	};
	guint i;

	for (i = 0; i < G_N_ELEMENTS(devices); i++) {
		if (devices[i].vid != vid)
			continue;
		if (devices[i].pid != pid)
			continue;

		if (name && !g_str_has_suffix(name, devices[i].name))
			continue;

		return &devices[i];
	}

	return NULL;
}

/* PROCON_HID_SDP_RECORD — the HID service record for the Pro Controller,
 * captured verbatim from bluetoothd's SDP cache on a genuine GUI pairing
 * (docs/golden/procon_sdp_cache, ServiceRecords 0x00010000, captured
 * 2026-08-10). Bare hex string like SIXAXIS_HID_SDP_RECORD.
 * Setting this kills the malformed-seed problem: services resolve from the
 * hardcoded record, no SDP seed/cache needed. */
#define PROCON_HID_SDP_RECORD "36017D0900000A000100000900013503191124090004"\
	"350D350619010009001135031900110900053503191002090006350909656E09006A09"\
	"01000900093508350619112409010109000D350F350D35061901000900133503190011"\
	"0901002510576972656C6573732047616D65706164090101250747616D657061640901"\
	"0225084E696E74656E646F090201090111090202080809020308210902042801090205"\
	"280109020635B035AE082225AA05010905A1010601FF85210921750895308102853009"\
	"3075089530810285310931750896690181028532093275089669018102853309337508"\
	"9669018102853F05091901291015002501750195108102050109391500250775049501"\
	"814205097504950181010501093009310933093416000027FFFF000075109504810206"\
	"01FF850109017508953091028510091075089530910285110911750895309102851209"\
	"12750895309102C009020735083506090409090100090209280109020A280109020C09"\
	"0C8009020D280009020E2800"

#endif /* _PROCON_H_ */
