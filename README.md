# ProBlue — Nintendo Switch Pro Controller native pairing on Linux

**Wire a Pro Controller to a PC and have it pair, reconnect, and work — no GUI
dialogs, no agents, no hacks. The way the Switch does it.**

ProBlue is a single patch to **BlueZ** (the Linux Bluetooth stack) that makes
the Nintendo Switch Pro Controller a fully first-class citizen on Linux: plug
it into USB, it pairs itself (or re-pairs/reconnects if it was already known),
and from then on you just press a button and it connects — with or without any
Bluetooth UI open.

---

## Table of contents

1. [Why this exists](#1-why-this-exists)
2. [What the Switch actually does (the RE groundwork)](#2-what-the-switch-actually-does)
3. [Architecture: the BlueZ patch](#3-architecture-the-bluez-patch)
4. [The BlueZ patch — file by file](#4-the-bluez-patch--file-by-file)
5. [Why there is no kernel patch (BlueZ-only)](#5-why-there-is-no-kernel-patch-bluez-only)
6. [How a dock + wake + reconnect works, end to end](#6-how-a-dock--wake--reconnect-works-end-to-end)
7. [Building & installing](#7-building--installing)
   - [7.0 Before you start](#70-before-you-start-know-your-versions)
   - [7.1 The kernel module: not needed](#71-part-a--the-kernel-module-not-needed)
   - [7.2 BlueZ](#72-bluez)
   - [7.3 Upgrading](#73-upgrading-kernel-updates-bluez-updates)
   - [7.4 Troubleshooting](#74-troubleshooting)
   - [7.5 Reproducing the patch](#75-reproducing-the-patch-from-pristine)
8. [Configuration notes](#8-configuration-notes)
9. [Known limitations & open items](#9-known-limitations--open-items)
10. [Upstreaming notes](#10-upstreaming-notes)
11. [Reverse-engineering sources](#11-reverse-engineering-sources)
12. [License](#12-license)

---

## 1. Why this exists

The Pro Controller connects over Bluetooth BR/EDR, but unlike most BT
peripherals it does **not** do over-the-air Secure Simple Pairing (SSP) on
connect. It arrives carrying a link key in its SPI flash, written by the host
it last paired with, and it only connects to the host whose MAC address + key
it has stored. The Switch writes that state over a **wired** protocol (USB/rail)
at every dock. Linux has no equivalent: stock BlueZ only knows how to pair
wireless devices over the air, and the controller will sit in "Limited
Discoverable" mode forever waiting for an inquiry that a paired-only host
never sends.

The result, without this patch: plugging the controller in does nothing; you
need a GUI that registers an agent, triggers an over-the-air SSP pair that the
controller half-rejects, and the whole thing breaks the moment no GUI is open.
ProBlue fixes the root cause: **implement the wired pairing protocol in the
Bluetooth stack itself**, so pairing is physical (you plug the cable in) and
everything else just works — the way Nintendo designed it.

## 2. What the Switch actually does

All of this comes from public RE work (see
[§11 Reverse-engineering sources](#11-reverse-engineering-sources)),
cross-checked against real captures of a genuine Switch and against the
controller's own SPI flash.

### The wired 3-step pairing (`subcmd 0x01`)

When the Switch docks a controller, it runs a three-step manual pairing
exchange over the wired (USB/rail) HID interface — see
`bluetooth_hid_subcommands_notes.md` "Subcommand 0x01: Bluetooth manual
pairing":

1. **`x01 x01 {host MAC LE}`** — send the host's Bluetooth address; the
   controller replies with its own MAC.
2. **`x01 x02`** — the controller generates a fresh 128-bit LTK, saves it to
   its SPI flash (`x2000` region), and returns it over the wire, **each byte
   XORed with `0xAA`**, in Little-Endian (flash) order.
3. **`x01 x03`** — commit the pairing record.

The key detail for a Linux host: the LTK comes back in **flash order
(Little-Endian)** and must be **byte-reversed** before use as a BR/EDR link
key. Verified against a genuine GUI pairing (controller flash
`bdfdff7b…` == bluetoothd key `f00a4af6…`); loading the un-reversed key
fails authentication with HCI error 0x05.

### The re-dock arm (`subcmd 0x08 00`)

At every dock, the console also sends **`subcmd 0x08 00`** ("Set shipment low
power state", argument `0x00`). This clears the shipment flag at SPI `x5000`
and puts the controller in normal LPM-to-sleep mode. Per the RE notes:

> "Switch always sends `x08 00` subcmd after every connection, and thus
> enabling Triggered Broadcom Fast Connect and LPM mode to SLEEP."

Without it, the controller stays in shipment mode and **cannot wake up from
button presses** — it will never page the host, so it will never reconnect.
The Switch also sends `0x30 01` (player-1 LED) after connection; we do the
same for parity.

### The device-info probe (`subcmd 0x02`)

`subcmd 0x02` ("Request device info") returns the controller's firmware and
its own Bluetooth MAC address (`data[4..9]`, big-endian → must be
byte-swapped for Linux's wire-order `bdaddr_t`). This is how the host learns
which BT device the plugged-in controller *is*, so pairing can be associated
with the right address.

## 3. Architecture: the BlueZ patch

```
┌──────────────────────────────────────────────────────────────┐
│ BlueZ (userspace, patched)                                    │
│   plugins/sixaxis.c        udev: USB plug-in → setup_device   │
│   profiles/input/procon.c  wired 3-step, session init, arm    │
│   profiles/input/device.c  per-connection serialized setup    │
│   src/adapter.c            link-key store, connectable-keep   │
└──────────────────────────────┬───────────────────────────────┘
                               │ hidraw (USB)
┌──────────────────────────────┴───────────────────────────────┐
│ kernel hid-nintendo (stock)  binds on USB, creates the hidraw │
│   node BlueZ talks to; its own init never interferes (see §5) │
└──────────────────────────────┬───────────────────────────────┘
                               │
                    Pro Controller (BT BR/EDR)
```

- **BlueZ patch** — adds the Nintendo wired cable-pairing protocol to the
  existing "sixaxis" cable-pairing machinery (the PS3/PS4 flow it already
  has), plus a per-connection serialized setup that arms the controller and
  switches it to full report mode on every BT connection (§4).
- **No kernel patch.** The **stock** `hid-nintendo` driver binds the
  controller on USB and creates the hidraw node BlueZ talks to; its own USB
  init is runtime-only and never interferes with the wired pairing flow.
  Over BT, input is served by bluetoothd's HID profile over uhid, so the
  stock driver never creates a competing BT input device. Verified
  end-to-end on a stock kernel — see §5. (A passive fork built during the
  investigation is kept in the repo as historical reference only.)

The controller is always owned by the pairing flow on whichever side it's
connected: wired subcommands over USB hidraw during docking, input + serialized
setup over the BT interrupt channel once connected. Nothing fights over the
controller.

## 4. The BlueZ patch — file by file

All changes are relative to stock BlueZ **5.84** (the patch applies to the
released 5.84 tarball from kernel.org). The patched source files ship in
`src/bluez/`; the canonical patch is `patches/bluez-5.84-procon.patch`.

### `profiles/input/procon.h` (new) — protocol + device table

- The vendor-specific half of cable pairing: all protocol constants
  (`PROCON_*`), the report layouts (`0x01` output subcmd report, `0x21` ACK
  reply), the USB-mode commands (`0x80 02` handshake, `0x80 03` 3-Mbit baud),
  the subcommand IDs, and the Nintendo over-cable device table
  (`get_nintendo_pairing()` — the Nintendo counterpart of the PS
  `get_pairing()` in `sixaxis.h`).
- `PROCON_HID_SDP_RECORD` — the controller's HID SDP service record, captured
  verbatim from bluetoothd's SDP cache during a genuine GUI pairing
  (`docs/golden/procon_sdp_cache`). Setting this kills the
  "malformed SDP seed" problem: services resolve from the hardcoded record
  with no SDP seed/cache needed. This mirrors the existing
  `SIXAXIS_HID_SDP_RECORD` mechanism.
- SPDX `GPL-2.0-or-later`, BlueZ-style header.

### `profiles/input/procon.c` (new) — the wired protocol

Four public functions, all running over the USB hidraw fd:

| Function | What it does |
|---|---|
| `procon_usb_session_init()` | `0x80 02` → `0x80 03` (3 Mbit) → `0x80 02` — the wired UART session handshake with the controller's BT chip. **Must run before any subcommand** (we re-establish the session ourselves) or the chip never answers. |
| `procon_get_device_bdaddr()` | `subcmd 0x02` device-info probe; returns the controller's BT MAC (byte-swapped from display order). |
| `procon_arm_wired()` | `subcmd 0x08 00` — clear shipment mode so the controller can wake from button presses (the wired-side arm; the BT-side reconnect-keeper is in `device.c`). |
| `procon_pair()` | The full wired 3-step (host MAC → GET_LTK → save) with the LTK byte-reverse + XOR-0xAA decode; re-inits the session first (a gap between init and the 3-step can let the chip drop the session) and re-arms before pairing, matching the Switch's order. |

Subcommand framing: 64-byte output report `0x01` (`[0]=0x01, [1]=counter,
[2..9]=rumble(0), [10]=subcmd, [11+]=data`), reply report `0x21`
(`[13]=ACK (0x80+ ok), [14]=subcmd, [15+]=data`).

### `plugins/sixaxis.c` — wiring the Pro Controller into the cable-pairing flow

The sixaxis plugin already watches udev for cable-paired devices (PS3/PS4).
We extend the same machinery:

- `get_pairing_type_for_device()` — falls back to
  `get_nintendo_pairing()` when no PS entry matches.
- `device_added()` / `setup_device()` — accept `CABLE_PAIRING_PROCON`; on
  plug-in: run `procon_usb_session_init()` (we start the wired UART session
  ourselves), probe the device MAC (`subcmd 0x02`), and **arm on every
  plug-in** (`0x08 00`, non-fatal on failure — the Switch re-arms after
  every connection).
- `setup_device()` — **PROCON is exempt from the "already known, skipping"
  early-return when the device is known but disconnected.** The Switch
  re-pairs on *every* dock, and there is no "read stored central" subcommand
  to detect whether the controller still holds our key (it can be lost by
  pairing to another host since the last dock). A trusted-but-disconnected
  Pro Controller therefore re-runs the full pairing flow on every dock —
  fresh LTK, trusted, non-temporary, accept-list. Only a currently-connected
  controller skips (charging while playing — don't yank a live session's
  key; it re-pairs next dock).
- `agent_auth_cb()` — the PROCON path runs `procon_pair()` inline, stores the
  acquired LTK via `btd_adapter_store_link_key()`, marks the device trusted,
  and sets the hardcoded HID SDP record. Cable pairing *is* the
  authorization (the user physically plugged the controller in), so no agent
  prompt is ever shown.
- `setup_device()` — also calls `btd_adapter_set_connectable(adapter, true)`
  so the host keeps page-scanning after a cable-paired controller is plugged
  in (the controller wakes by paging the host; the GUI only enables
  connectable while discoverable).

### `profiles/input/device.c` — the per-connection serialized setup (reconnect-keeper)

`input_device_connected()` runs a **serialized subcommand setup** over the BT
interrupt channel, started *before* `hidp_add_connection()` and continued by the
0x21-ack hook in `hidp_recv_intr_data()` (raw report `[1]==0x21` subcmd reply,
`[14]`=ack, `[15]`=subcmd echo):

```
0x02 probe → 0x08 00 arm → 0x03 30 report-mode full → 0x30 01 player-1 LED
```

Each subcommand is sent only after the previous one's ack (500 ms cap — a
timeout logs and continues the queue, so a dropped ack can't wedge a
connection). Queue state (`procon_setup_pos/source/last`) lives in
`struct input_device`, is cleaned up in `input_device_free()`, and restarts on
every connection.

Why serialized, and why this sequence — verified against `btmon` captures
(2026-08-13):

- A fire-and-forget batch of `[0x08 00, 0x03 30, 0x30 01]` sent in <1 ms was
  **dropped by the controller**: it processes subcommands slowly (~0.31 s per
  ack), and only the first (`0x08`) was ever acked — `0x03`/`0x30` never
  were, input stayed in `0x3F` simple mode, and the controller never left its
  search state, so it cycled connect → ~2.2 s → drop → re-page.
- The ack-waited queue fixes that: `0x08 00` clears shipment / LPM-to-sleep
  (an un-armed controller does a connect-and-self-terminate dance — Remote
  User Terminated 0x13 ~150 ms after PSM 19 — and can give up entirely),
  `0x03 30` switches to standard full report mode, `0x30 01` lights player-1.
  Host subcommand traffic on PSM 19 also keeps the link alive (host silence
  is what killed it at ~2.2 s).
- Every connect path funnels through `input_device_connected()` (inbound and
  outbound/auto-reconnect), so the setup runs on **every** connection, not
  just the first — the Switch's documented "after every connection" behavior.

The old fire-and-forget `procon_arm()` / `procon_set_report_mode_full()` /
`procon_set_player_led()` helpers were **deleted** — one sequence, no
fallbacks.

### `profiles/input/server.c` — the cable-pairing gate

`dev_is_sixaxis()` is renamed to `dev_is_cable_pairing()` (it now covers PS
*and* Nintendo) and the Pro Controller's VID/PID are added to the
`-ENOENT`-SDP-browse deferral and the unknown-device accept checks. Cable-
paired devices skip the bonded/encryption checks and get the deferred
SDP-browse on inbound connect, exactly like PS controllers.

### `profiles/input/sixaxis.h` — the shared enum

Adds `CABLE_PAIRING_PROCON` to the `CablePairingType` enum, plus a comment
clarifying this header is the *shared* cable-pairing mechanism (legacy name,
not PS3-specific).

### `src/adapter.c` / `adapter.h` — host-side support

| Change | Why |
|---|---|
| `btd_adapter_store_link_key()` + `reload_link_keys()` | Runtime BR/EDR link-key registration: persist the LTK bluetoothd-style (info file `[LinkKey]` section) and reload the whole kernel key list via `MGMT_OP_LOAD_LINK_KEYS` (the *only* runtime key-add path in 5.84 — there is no `MGMT_OP_ADD_LINK_KEY`). Required because the controller connects with the key we just wrote, before any over-the-air key exchange could happen. Also resolves the stock TODO in `sixaxis_set_central_bdaddr()` ("there ...
| `btd_adapter_set_connectable()` | Explicit page-scan control from the plugin (the Switch and phones stay connectable always; a cable-paired controller waking by paging must be heard even with the GUI's discoverable off). |
| `property_set_mode()` (DISCOVERABLE case) | With kernel conn control, turning discoverable off normally *also* clears connectable. We skip that swap while cable-paired devices exist, so page scan survives a GUI "discoverable off". |
| `adapter_start()` | The kernel clears the accept list on power-off; re-add non-temporary cable-paired BR/EDR devices on every power-on so the wake-page is heard even after a bluetoothd restart, with no GUI. |

### Page-scan tuning (config — no code change)

```ini
PageScanType=0x01        # interlaced (INTEGER 0x01, not the word "interlaced")
PageScanInterval=0x0012
PageScanWindow=0x0012    # window == interval ⇒ ~100% listen duty
```

The controller pages the host only **briefly** on wake (default host page scan
listens 11.25 ms per 1.28 s ≈ 0.9% duty, so the brief page is usually missed).
Window == interval makes the host listen essentially always, so the first
wake-page is caught. See [§8 Configuration notes](#8-configuration-notes)
for the trade-offs.

### `Makefile.plugins` — build wiring

The `if SIXAXIS` block gains `profiles/input/procon.c` + `procon.h`.
`Makefile.am:300` includes `Makefile.plugins`, so this is the correct
upstream-able location (`Makefile.in` in the tree is the regenerated
artifact; upstream regenerates it with `autoreconf`).

## 5. Why there is no kernel patch (BlueZ-only)

> **Status (2026-08-13): confirmed — the kernel patch is NOT needed.**
> The stock-kernel test that earlier revisions of this section called
> "pending" **passed**: the whole flow — wired pairing, unplug, button press,
> BT reconnect + input — works on the **stock Ubuntu 6.8.0-137
> `hid-nintendo`** module with the BlueZ patch alone. ProBlue is a BlueZ-only
> project; there is no kernel patch to build or install.

**Why a fork was once thought necessary, and why it isn't:**

- The original concern: the stock driver's USB init sends `0x80 04`
  (USB-only lock) and config subcommands, which could knock the controller
  out of pair mode or fight BlueZ's wired subcommands.
- Reality: the BlueZ patch drives the entire wired flow itself over the
  plugin's own hidraw fd — `procon_usb_session_init()` re-establishes the
  USB session (`0x80 02/03/02`) and every subcommand runs over that fd,
  which the **stock** driver also creates (`HID_CONNECT_HIDRAW`). The stock
  driver's USB init is runtime-only: it writes no SPI pairing records and is
  not re-run on radio events, so it does not interfere. The BT side never
  conflicts either: with BlueZ 5.84 the controller's BT input is served by
  bluetoothd's HID profile over **uhid**, so the stock `hid-nintendo` driver
  does not create a competing BT input device in this configuration.

**What this means in practice:**

- Install **only** the BlueZ patch (§7). The kernel module stays stock:
  nothing to build, load, or DKMS, and nothing to rebuild on kernel updates.
- The historical fork (`patches/hid-nintendo-keep-bt-radio.patch`,
  `src/kernel/hid-nintendo.c`) is kept **only as a record** of this
  investigation. **Do not install it** — it made the driver fully passive on
  every transport (no input/LED/rumble/battery device anywhere, no wired USB
  play, kernel-served BT input gone), removing working features for zero
  benefit now that the stock path is proven.

**Verification:** stock kernel + patched BlueZ 5.84, 2026-08-13: dock (wired
3-step pairing, link key registered) → unplug → button press → BT connect +
input, reliably. The full evidence trail is in `docs/maki_memories.md`
(sections 3 and 6).

## 6. How a dock + wake + reconnect works, end to end

### First-ever dock (or key lost since last dock)

1. Plug controller into USB. udev fires; the sixaxis plugin's
   `setup_device()` runs.
2. `procon_usb_session_init()` (0x80 02/03/02) starts the wired UART session;
   `subcmd 0x02` learns the controller's BT MAC; `0x08 00` arms it.
3. Cable authorization is requested; because the device is already marked
   trusted (physical plug = authorization), the gate auto-approves with no
   agent.
4. `procon_pair()` runs the 3-step: host MAC → controller generates fresh LTK
   (saved to its SPI flash, returned XOR-0xAA, LE) → save. The LTK is
   byte-reversed, stored bluetoothd-style, and loaded into the kernel via
   `MGMT_OP_LOAD_LINK_KEYS`.
5. Device becomes non-temporary, trusted, accept-listed, with the hardcoded
   HID SDP record. GUI shows it as "Paired".

### Wake and reconnect (no GUI needed, forever after)

1. Press any button on the (sleeping) controller. It wakes and **pages** the
   host — the host's page scan is on (~100% duty) because a cable-paired
   device exists, even with the GUI's discoverable off.
2. Kernel accept list says "this device is allowed" → page → accept →
   Link Key Reply with the fresh key → E0 encryption → PSM 17/19.
3. `input_device_connected()` runs the **serialized setup** over the BT
   interrupt channel *before* `hidp_add_connection()`: `0x02 probe → 0x08 00
   arm → 0x03 30 report-mode full → 0x30 01 player-1 LED`, each sent after
   the previous one's ack — so the controller sticks, leaves simple mode,
   and stays wakeable for the next cycle (§4).
4. Input streams — delivered by bluetoothd's HID profile over **uhid** (the
   stock kernel driver never creates a competing BT input device in this
   configuration; see §5). When the controller sleeps again, the host drops
   the idle link (link supervision timeout), re-enters the
   accept-list-driven page scan state, and is ready for the next button
   press.

### Multi-device / headphones

Page scan is driven by the kernel rule: page scan stays on if the adapter is
connectable **or** if any accept-list device is disconnected
(`disconnected_accept_list_entries()`, `hci_sync.c`). So headphones connected,
or a second controller, does **not** stop the host from hearing the Pro
Controller's wake-page — the controller's accept-list entry being
disconnected keeps scan on regardless of what else is connected. Verified
against the 6.8.0 kernel source.

## 7. Building & installing

This section is written so that **anyone on any Linux system** can build and
install ProBlue properly — from source, against *their* BlueZ, with no hacks,
no "just copy this binary", and nothing that breaks on the next package
update.

There is one part, and it's all you need:

| Part | What it is | Where it lives |
|---|---|---|
| **BlueZ** (`bluetoothd`) | The patched Bluetooth daemon | `patches/bluez-5.84-procon.patch` |

The kernel driver stays **stock** — ProBlue is BlueZ-only (see §5). There is
no kernel module to build or install.

---

### 7.0 Before you start: know your versions

The patch was written against a specific base. Your job is to make it fit
*your* system — the instructions below tell you exactly how, and how to verify
the fit before you build anything.

```bash
uname -r                      # kernel version (e.g. 6.8.0-137-generic)
bluetoothd --version          # BlueZ version (e.g. 5.84)
```

- **No kernel patch exists** — ProBlue is BlueZ-only, verified on a stock
  Ubuntu 6.8.0-137 kernel (§5). The `hid-nintendo` driver is whatever your
  distro ships, used as-is; there is nothing to build or rebuild on kernel
  updates.
- The **BlueZ patch** targets stock BlueZ **5.84**. Distros ship various
  versions (Ubuntu 24.04 ships 5.84, Arch ships newer). §7.2 covers building
  5.84 from source, which works everywhere.

**Rule:** never build against "a fetched mainline copy" of anything. Build
against the exact source your system uses: a released 5.84 tarball. The
verification steps below exist precisely to catch a wrong base.

---

### 7.1 Part A — the kernel module: not needed

ProBlue does **not** patch the kernel. Verified on a stock Ubuntu 6.8.0-137
`hid-nintendo` module (§5): your distro's stock module creates the hidraw
node the BlueZ plugin talks to, and its USB init never interferes with wired
pairing. There is nothing to build, load, or DKMS, and kernel updates
rebuild the stock module for you.

The repo still ships `patches/hid-nintendo-keep-bt-radio.patch` and
`src/kernel/hid-nintendo.c` **as historical reference** from the earlier
"does the stock driver interfere?" investigation. Do not install them — that
fork disabled the driver's input/LED/rumble/battery on every transport and
every supported device, removing working features for zero benefit (§5). The
old A1–A6 instructions (pristine-source oracle test, DKMS setup, etc.)
applied only to that fork and are removed.

---

### 7.2 BlueZ

#### B1. Install build dependencies

BlueZ 5.84 needs GLib, D-Bus, udev, and the classic build chain. The §B4
configuration (the `--disable-*` list) needs only a subset of the full list
below — verified in the 2026-08 dogfood build, where `libical-dev`,
`libjson-c-dev`, `libell-dev`, `liblz4-dev`, `liblzma-dev`, `libcap-ng-dev`
and `libgcrypt20-dev` were all absent and `configure` still passed:

```bash
# Debian / Ubuntu
sudo apt install build-essential autoconf automake libtool pkg-config \
                 libglib2.0-dev libdbus-1-dev libudev-dev libreadline-dev \
                 libzstd-dev

# Fedora / RHEL
sudo dnf install gcc make autoconf automake libtool pkgconfig \
                 glib2-devel dbus-devel systemd-devel readline-devel \
                 libzstd-devel

# Arch
sudo pacman -S base-devel autoconf automake libtool pkgconf glib2 dbus \
               systemd-libs readline zstd
```

The remaining packages from older versions of this list (`libical`,
`json-c`, `ell`, `lz4`, `xz`, `libcap-ng`, `libgcrypt`) are only needed if
you keep more profiles enabled by dropping some of the `--disable-*` flags
from §B4; installing them anyway is harmless.

#### B2. Get pristine BlueZ 5.84 and apply the patch

The patch is against released 5.84 — build that exact version, don't grab
"latest". The repo ships a one-shot tool that fetches, verifies, and applies
everything (a sibling of the now-historical
`tools/get_pristine_hid_nintendo.sh`):

```bash
bash tools/get_pristine_bluez.sh ~/ProBlue-build
cd ~/ProBlue-build/bluez-5.84
```

(The tool downloads `bluez-5.84.tar.xz` from kernel.org, verifies
`AC_INIT(bluez, 5.84)` in `configure.ac`, dry-runs the patch, and applies it.
Or do it by hand: `wget https://www.kernel.org/pub/linux/bluetooth/bluez-5.84.tar.xz`
+ `tar xf` + `patch -p1 < patches/bluez-5.84-procon.patch`.)

#### B3. If the patch does not apply (porting to a newer BlueZ)

Before fighting `patch`, re-check that you're on exactly 5.84 (`grep AC_INIT
configure.ac` should print `AC_INIT(bluez, 5.84)`). Porting to a newer
BlueZ is a mechanical exercise (the changed symbols — `setup_device`,
`agent_auth_cb`, `input_device_connected`, `property_set_mode`,
`adapter_start`, `store_link_key` — are stable across 5.8x; the patch's own
comments call out every anchor). After porting, regenerate the patch per
§7.5.

#### B4. Configure and build

```bash
autoreconf -fi
./configure --enable-sixaxis \
            --disable-obex --disable-mesh --disable-midi \
            --disable-nfc --disable-health --disable-test \
            --disable-btpclient --disable-manpages
make -j"$(nproc)"
```

`--enable-sixaxis` is the one that matters: it builds the sixaxis plugin,
which now contains the ProBlue code. The `--disable-*` flags just skip
optional profiles you don't need; omit them if you want everything.

Verify the Pro Controller code is actually in your binary:

```bash
strings src/bluetoothd | grep -i procon        # should show "procon: ..." strings
./src/bluetoothd --version                     # "Bluetooth daemon 5.84"
```

#### B5. Install it properly

The clean, un-hacky way: `make install` puts the fork in `/usr/local`, then
point the systemd service at it with a **drop-in override** (so distro
updates can't clobber your fork, and you can roll back by removing one file):

```bash
sudo make install

# find where it landed:
which bluetoothd          # usually /usr/local/sbin/bluetoothd

# create a systemd drop-in that overrides ExecStart:
sudo mkdir -p /etc/systemd/system/bluetooth.service.d
sudo tee /etc/systemd/system/bluetooth.service.d/ProBlue.conf > /dev/null <<EOF
[Service]
ExecStart=
ExecStart=/usr/local/sbin/bluetoothd
EOF
sudo systemctl daemon-reload
```

If your distro's unit runs `bluetoothd` from a different path
(`/usr/libexec/bluetooth/bluetoothd` on Debian/Ubuntu), that's exactly what
the `ExecStart=` override handles — an empty `ExecStart=` clears the stock
one, the second line installs yours. Check with:

```bash
systemctl cat bluetooth.service | grep -A2 ExecStart
```

#### B6. Configure the page scan (config, not code)

ProBlue needs the host to listen for the controller's brief wake-page. Put
the three lines in the system config (not just the source tree's):

```bash
sudo tee -a /etc/bluetooth/main.conf > /dev/null <<'EOF'

# ProBlue: catch the Pro Controller's brief wake-page (see README §8)
PageScanType=0x01
PageScanInterval=0x0012
PageScanWindow=0x0012
EOF
```

(`PageScanType` must be the integer `0x01`, not the word `interlaced`.)

#### B7. Start and verify

```bash
sudo systemctl daemon-reload
sudo systemctl restart bluetooth
systemctl status bluetooth          # should show "Bluetooth daemon 5.84" and your path
```

Then the moment of truth — **with no Bluetooth GUI open**:

1. Plug the controller into USB. Within a second or two you should see system
   logs for the wired session:
   ```bash
   journalctl -u bluetooth -f
   # look for: procon: usb 0x80 0x02 ack
   #           procon: armed (0x08 00 shipment cleared)
   #           procon: 3-step step 1/2/3 done
   #           procon: link key stored
   #           procon: cable pairing complete
   ```
2. Unplug it. The controller sleeps. Press any button once — it should
   connect within a second or two:
   ```bash
   # the controller now shows in bluetoothctl:
   bluetoothctl devices          # Pro Controller
   bluetoothctl info <mac>       # Paired: yes, Trusted: yes
   ```
3. Open a game (or `evtest`) — input works over BT.

If anything is missing, check §7.4 troubleshooting before touching anything.

---

### 7.3 Upgrading (kernel updates, BlueZ updates)

- **Kernel update:** nothing to do — ProBlue ships no kernel module. Your
  distro rebuilds the stock `hid-nintendo` itself on every kernel update.
- **BlueZ update:** distro updates touch `/usr/bin/bluetoothd` /
  `/usr/libexec/bluetooth/bluetoothd`, but your fork lives in `/usr/local`
  and the drop-in keeps pointing at it, so you're unaffected. To rebase the
  ProBlue patch on a newer BlueZ release later, repeat B2–B4 against the new
  tarball (re-apply the patch; port if it doesn't apply; rebuild; reinstall).
- **Rolling back** at any time: remove the drop-in
  (`sudo rm /etc/systemd/system/bluetooth.service.d/ProBlue.conf`, `daemon-reload`,
  `systemctl restart bluetooth`). There is no kernel module to remove.

### 7.4 Troubleshooting

| Symptom | Likely cause / fix |
|---|---|
| `procon: usb 0x80 0x02: no reply` | USB session init failed — check the cable/port and replug (the session is re-established on every plug-in); watch `journalctl -u bluetooth`. |
| No `procon:` logs at all on plug-in | sixaxis plugin not built (`--enable-sixaxis` missing, B4) or udev not delivering the USB event — check `journalctl -u bluetooth` and replug. |
| Plug-in logs OK, but no BT reconnect | Page-scan lines not in `/etc/bluetooth/main.conf` (B6) — `bluetoothd` reads `/etc/bluetooth/main.conf` when started by systemd, NOT the tree's copy. |
| Controller connects once then never again | The controller's single host slot was overwritten (phone/Switch/another PC). Re-dock to the PC — ProBlue re-pairs on every dock by design (§2, §4). |
| Auth failure (HCI error 0x05) on connect | Key not loaded into the kernel — check for `procon: link key stored` and restart bluetooth after a fresh dock. |

### 7.5 Reproducing the patch from pristine

`patches/bluez-5.84-procon.patch` is the canonical patch. To regenerate it
from this repository's sources (e.g. after porting to a newer BlueZ), diff
the shipped patched sources (`src/bluez/`) against a fresh pristine copy:

```bash
# BlueZ — build pristine 5.84, then diff against the shipped sources:
  #   (fetch pristine via tools/get_pristine_bluez.sh, but stop before
  #   applying the patch)
diff -u --label a/bluez-5.84 --label b/problue bluez-5.84 src/bluez \
    > patches/bluez-5.84-procon.patch

# (There is no kernel patch to reproduce — ProBlue is BlueZ-only, §5.
#  patches/hid-nintendo-keep-bt-radio.patch is historical reference only.)
```

## 8. Configuration notes

### Page-scan tuning (the battery trade-off)

The ~100% duty page scan makes the first wake-page connect instantly, at a
small radio cost (the screen dominates a laptop's battery; a modern BT radio
listening for pages is nearly free compared to active traffic). Two options
if you want to be gentler:

- **Accept it** — the simplest, and what this repo ships.
- **Dynamic (planned)** — call `btd_adapter_set_fast_connectable(adapter,
  true)` from the plugin only while a cable-paired controller exists, false
  when it disconnects. The BlueZ patch already exports the helper; only the plugin
  call sites are missing.

`PageScanType` must be an **integer** (`0x01`), not the word `interlaced` —
BlueZ's `parse_config_int()` rejects the word (seen live).

### Link supervision timeout (the "zombie" window)

After the controller sleeps, the host holds the idle ACL "Connected" for the
default ~20 s link-supervision timeout, and page scan is off during that
window (the controller's accept-list entry looks satisfied). In practice this
means: wait ~20 s after the controller sleeps, then one button press connects
instantly. Reconnects aren't frequent enough to matter, so this repo accepts
the stock 20 s. If you want it snappier, a per-Pro-Controller
`HCI_Write_Link_Supervision_Timeout` (2–3 s) from the procon plugin at
connect would shrink the window without touching the global default for other
devices — deliberately left as a follow-up.

## 9. Known limitations & open items

- **No kernel patch is used** — ProBlue is BlueZ-only (see §5). The stock
  `hid-nintendo` module keeps its normal behavior: wired USB play works, and
  kernel-served BT input works where the kernel HIDP path is used (systems
  without uhid). The historical passive fork (kept for reference, §5)
  *would* disable input/LED/rumble/battery on every transport for every
  device (Pro Controller, Joy-Con L/R, SFC30, GEN, N64, Charging Grip) —
  exactly why it was retired.
- **Page scan is kept on (connectable) indefinitely once a cable-paired
  device has been plugged in.** `setup_device()` calls
  `btd_adapter_set_connectable(adapter, true)` and nothing reverts it, and
  turning the GUI's discoverable off no longer clears connectable while
  cable-paired devices exist. This is the ~100% page-scan duty trade-off
  documented in §8; a dynamic "only while a controller is present" toggle is
  planned but not implemented.
- **One host slot on the controller.** The Pro Controller stores one pairing
  record; pairing to another host (phone, Switch, second PC) overwrites the
  PC's entry. Re-docking to the PC fixes it (the always-re-pair-on-dock
  behavior makes this self-healing — verified).
- **`Authorization request for non-connected device!?`** — observed once in
  the reconnect dance and treated as benign: cable pairing authorizes (and
  trusts) the device while it is still physically on USB, i.e. before the BT
  link exists, so bluetoothd's authorization gate can fire for a device that
  is not (yet) connected. It did not block pairing or reconnects and has not
  recurred; if it appears in normal use, capture `journalctl -u bluetooth`
  around it and report it.
- **Link key in logs.** The LTK is stored, never logged (a cleartext log was
  removed during cleanup — link keys must not appear in logs).
- **Two controllers.** Each has its own accept-list entry; page scan stays on
  while either is disconnected (kernel-verified). Multi-controller live
  testing is on the to-do list.
- **BlueZ 5.84 only** — the patch is against 5.84; porting notes for newer
  versions are in the file comments (symbols are stable across 5.8x).
- **No kernel dependency** — the stock `hid-nintendo` module is used as-is;
  nothing ProBlue-specific to rebuild on kernel updates (§7.3).

## 10. Upstreaming notes

Upstream status, stated plainly: the BlueZ patch is additive and plausibly
upstreamable after review. There is no kernel patch — ProBlue is BlueZ-only
(§5); the historical hid-nintendo fork is not upstreamable as-is (it
unconditionally disabled the driver's input/LED/rumble/battery on every
transport for every device, a regression with no opt-out), which is part of
why it was retired. Everything is GPL-2.0-or-later with BlueZ-style headers
and attribution.

**BlueZ patch — review concerns.** Plausible after discussion; reviewers
will push on: the global `property_set_mode` change (discoverable-off no
longer clears connectable whenever *any* cable-paired device exists — PS3/PS4
included), the fact that connectable is never turned back off (§8/§9), and
the Pro Controller VID/PID special-casing in the generic input profile
(`profiles/input/device.c`).

Things an upstream reviewer will ask about, answered:

| Question | Answer |
|---|---|
| Why not just use over-the-air SSP? | The controller does not SSP on connect; it connects only to the stored-MAC+key host. Wired pairing is the vendor mechanism. |
| Why trust a cable-paired device without an agent? | Cable pairing *is* authorization — the user physically plugged the controller in (same access as any USB device). The GUI pairing path trusts too. |
| Why re-pair on every dock? | The Switch does (c2j capture: host-record push at every connect), there is no "read stored central" subcommand, and the controller's single slot can be silently overwritten by another host. Fresh key per dock is the robust model. |
| Why the hardcoded SDP record? | The stock SDP seed is malformed on 5.84 for this device; the record is captured verbatim from a genuine pairing. Mirrors the existing `SIXAXIS_HID_SDP_RECORD`. |
| Why the global page-scan config? | The controller pages only briefly on wake; the stock ~0.9% duty misses it. A per-plugin `set_fast_connectable` dynamic toggle is the planned gentler alternative. |
| Does the stock kernel driver conflict? | No — verified on stock. Over USB it creates the hidraw node the plugin talks to (`HID_CONNECT_HIDRAW`) and its init is runtime-only (writes no SPI pairing records, not re-run on radio events); `procon_usb_session_init()` re-establishes the session regardless. Over BT the controller's input is served by bluetoothd over uhid, so `hid-nintendo` never creates a competing BT input device in this configuration (§5). |
| What about wired USB play? | It just works — the stock driver creates its normal USB input device alongside the hidraw node BlueZ uses. Nothing is removed; removing it was the fork's cost, and why it was retired (§5). |

## 11. Reverse-engineering sources

The protocol implementation is based on public reverse-engineering work, plus
our own live captures. Key sources (links):

- **dekuNukem — Nintendo Switch Reverse Engineering**
  `https://github.com/dekuNukem/Nintendo_Switch_Reverse_Engineering`
  - `bluetooth_hid_subcommands_notes.md` — subcommand 0x01 (manual pairing
    3-step, the XOR-0xAA LTK), 0x02 (device info), 0x08 (shipment state,
    "Switch always sends x08 00 after every connection").
  - `spi_flash_notes.md` — the x2000 pairing record layout (magic, host MAC,
    LTK in Little-Endian, capability byte).
  - `USB-HID-Notes.md` — wired frame format.
  - `packet_parse/c2j.txt` — the console's wired choreography at dock
    (device info → arm → host-record push → report mode → calibration
    reads).
- **Brikwerk — nxbt**
  `https://github.com/Brikwerk/nxbt`
  - `docs/Example Pairing Session.md` — a full Switch↔controller pairing
    session with packet annotations.
  - `nxbt/controller/` — the controller-side emulation (report framing,
    subcommand handling).
- **Our own live captures** (2026-08-07 → 08-13), including the
  reconnect-keeper validation (serialized post-connect setup, page→accept→
  key reply→E0→PSM 17/19→input streaming):
  - `docs/golden/procon_info` — bluetoothd's stored device info for a genuine
    GUI pairing (the LTK cross-check: flash `bdfdff7b…` == key `f00a4af6…`;
    **link key redacted** in the shipped copy).
  - `docs/golden/procon_sdp_cache` — the controller's SDP service record,
    source of `PROCON_HID_SDP_RECORD`.
- Working protocol notes (semi-RE, flags unverified claims) were kept
  internal during development; the verified conclusions are in §2 above.
  The patch and source comments cite a few of those internal notes by name
  (`Bluez_switch_cable_plan.md`, "c2j", "evidence 21", `bt_pairer.cpp:1427`).
  They are not shipped in this repo — they were development scratch — and
  the conclusions they reference are stated in full in §2/§4.

## 12. License

- The `LICENSE` file at the repo root is the GNU GPL **version 2** reference
  text (upstream GPLv2, June 1991).
- BlueZ patch: **GPL-2.0-or-later** (matches BlueZ; new files carry the
  BlueZ SPDX header + `Copyright (C) 2026 Tim Van Dyke <tim.vandyke123@gmail.com>`).
- Kernel patch (`hid-nintendo.c` fork): **GPL-2.0-or-later** (the stock file
  header is `SPDX-License-Identifier: GPL-2.0+`, which the fork preserves;
  `keep-bt-radio.patch` changes no licensing).
- This documentation: **CC-BY-4.0** unless noted otherwise — the canonical
  text is at <https://creativecommons.org/licenses/by/4.0/>; no separate
  CC-BY license file is shipped in this repo.

This project is not affiliated with or endorsed by Nintendo. "Nintendo",
"Switch", and "Pro Controller" are trademarks of Nintendo. The RE sources in
§11 are the work of their respective authors, attributed above.

---

*ProBlue — Nintendo Switch Pro Controller, properly.*
