/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2026  Tim Van Dyke <tim.vandyke123@gmail.com>
 *
 *  Nintendo Switch Pro Controller cable pairing.
 *
 *  The Pro Controller connects only to the host whose Bluetooth address and
 *  link key it has stored in its SPI flash (x2000). A host fills that flash
 *  through the wired 3-step protocol (subcmd 0x01), so pairing on Linux means
 *  driving the controller over its USB hidraw node:
 *
 *    1. start the wired UART session (0x80 02, 0x03, 0x02),
 *    2. read the controller's address (subcmd 0x02, device info),
 *    3. run the 3-step (host address, GET_LTK, save),
 *    4. store the returned LTK in bluetoothd and the kernel before the
 *       controller connects.
 *
 *  RE docs settle that step 0x01 0x02 returns the *stored* key from the
 *  active x2000 section (subcommands notes: "Acquire the XORed LTK hash";
 *  SPI flash notes: "it keeps the active section and the current LTK used
 *  with Switch can be acquired") — no fresh-key generation. So an already
 *  paired controller does not need the 3-step at all: read x2000 (0x10) and
 *  skip if magic 0x95 + stored MAC == ours (read-then-decide, PLAN P8).
 *
 *  The shared cable-pairing infrastructure (CablePairingType, struct
 *  cable_pairing, the PlayStation device table) lives in sixaxis.h; everything
 *  Nintendo-specific lives here.
 */

#ifndef _PROCON_H_
#define _PROCON_H_

#include "profiles/input/sixaxis.h"

#define PROCON_VID	0x057e
#define PROCON_PID	0x2009
#define PROCON_NAME	"Pro Controller"

/*
 * Every host->controller subcommand uses output report 0x01, over USB and over
 * the Bluetooth interrupt channel alike:
 *
 *      [0]     report id (PROCON_REPORT_SUBCMD)
 *      [1]     packet counter, low nibble
 *      [2..9]  rumble data (zero here)
 *      [10]    subcommand id
 *      [11..]  subcommand payload
 *
 * The controller answers with report 0x21:
 *
 *      [13]    ack, >= 0x80 on success, 0x00 on NACK
 *      [14]    subcommand id echo
 *      [15..]  reply payload
 */
#define PROCON_REPORT_SUBCMD	0x01
#define PROCON_REPORT_ACK	0x21

/* Wired session commands: 2-byte writes [0x80][cmd], answered by [0x81][cmd]. */
#define PROCON_USB_REPORT_CMD	0x80
#define PROCON_USB_REPORT_ACK	0x81
#define PROCON_USB_CMD_HANDSHAKE	0x02	/* (re)start the UART session */
#define PROCON_USB_CMD_BAUDRATE_3M	0x03	/* switch the session to 3 Mbit */

/* Subcommands handled here; all other Pro Controller subcommands belong to
 * hid-nintendo and must not be sent by a second writer. */
#define PROCON_SUBCMD_BT_MANUAL_PAIR	0x01	/* the wired 3-step */
#define PROCON_SUBCMD_REQ_DEV_INFO	0x02	/* controller address */
#define PROCON_SUBCMD_SET_SHIPMENT_STATE 0x08	/* 0x00 = clear */
#define PROCON_SUBCMD_GET_PAGE_LIST	0x05	/* 0x01 = host list in memory */
#define PROCON_SUBCMD_SPI_READ		0x10	/* read x2000 pairing info */

/* Payload selectors for subcmd 0x01 (manual pairing). */
#define PROCON_PAIR_HOST_MAC	0x01	/* send our address */
#define PROCON_PAIR_GET_LTK	0x02	/* returns the stored LTK (XOR 0xAA) */
#define PROCON_PAIR_SAVE	0x03	/* commit the pairing to flash */

/* subcmd 0x08 00 clears the shipment low-power state (SPI x5000). On this
 * unit x5000 already reads 0xFF (shipment-normal); the wired arm at dock is
 * therefore a no-op kept for console-order parity (ledger R4). */
#define PROCON_SHIPMENT_CLEAR	0x00

/* x2000 pairing-info section layout (SPI flash notes, stride 0x26). */
#define PROCON_SPI_PAIR_MAGIC	0x95
#define PROCON_SPI_PAIR_OFFSET	0x2000
#define PROCON_SPI_MAC_OFFSET	0x04	/* 6B host MAC, big-endian */
#define PROCON_SPI_LTK_OFFSET	0x0a	/* 16B LTK, little-endian */

int procon_usb_session_init(int fd);
int procon_arm_wired(int fd);
int procon_get_device_bdaddr(int fd, bdaddr_t *bdaddr);
int procon_pair(int fd, const bdaddr_t *host, uint8_t ltk[16]);
int procon_acquire_ltk(int fd, const bdaddr_t *host, uint8_t ltk[16]);

/* Nintendo cable-pairing device table, the counterpart of get_pairing() in
 * sixaxis.h. Used by the udev plugin and by the shared input-profile gates. */
const struct cable_pairing *get_nintendo_pairing(uint16_t vid, uint16_t pid,
							const char *name);

/* The controller's HID service record, captured verbatim from bluetoothd's
 * SDP cache during a genuine pairing of a retail Pro Controller. It is a
 * property of the controller's firmware and identical across units (no
 * machine- or key-specific data). P7: verify with a real SDP browse on 5.86
 * before relying on it. */
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