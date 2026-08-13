# ProBlue — Nintendo Switch Pro Controller native pairing on Linux

**Wire a Pro Controller to a PC and have it pair, reconnect, and work — no GUI
dialogs, no agents, no hacks. The way the Switch does it.**

ProBlue is two small patches, one to **BlueZ** (the Linux Bluetooth stack) and
one to the kernel's **`hid-nintendo`** driver, that together make the Nintendo
Switch Pro Controller a fully first-class citizen on Linux: plug it into USB,
it pairs itself (or re-pairs/reconnects if it was already known), and from then
on you just press a button and it connects — with or without any Bluetooth UI
open.

---

## Table of contents

1. [Why this exists](#1-why-this-exists)
2. [What the Switch actually does (the RE groundwork)](#2-what-the-switch-actually-does)
3. [Architecture: the two patches](#3-architecture-the-two-patches)
4. [The BlueZ patch — file by file](#4-the-bluez-patch--file-by-file)
5. [The kernel patch — `hid-nintendo.c`](#5-the-kernel-patch--hid-nintendoc)
6. [How a dock + wake + reconnect works, end to end](#6-how-a-dock--wake--reconnect-works-end-to-end)
7. [Building & installing](#7-building--installing)
   - [7.0 Before you start](#70-before-you-start-know-your-versions)
   - [7.1 The kernel module](#71-part-a--the-kernel-module)
   - [7.2 BlueZ](#72-part-b--bluez)
   - [7.3 Upgrading](#73-upgrading-kernel-updates-bluez-updates)
   - [7.4 Troubleshooting](#74-troubleshooting)
   - [7.5 Reproducing the patches](#75-reproducing-the-patches-from-pristine)
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

## 3. Architecture: the two patches

```
┌──────────────────────────────────────────────────────────────┐
│ BlueZ (userspace)                                             │
│   plugins/sixaxis.c        udev: USB plug-in → setup_device   │
│   profiles/input/procon.c  wired 3-step, session init, arm    │
│   profiles/input/device.c  per-connection BT arm (reconnect)  │
│   src/adapter.c            link-key store, connectable-keep   │
└──────────────────────────────┬───────────────────────────────┘
                               │ hidraw (USB)
┌──────────────────────────────┴───────────────────────────────┐
│ kernel hid-nintendo (fork)   passive probe: binds + hidraw    │
│   no init handshake / input / LED / battery device on EITHER  │
│   transport (USB or BT); BT radio stays alive while plugged   │
└──────────────────────────────┬───────────────────────────────┘
                               │
                    Pro Controller (BT BR/EDR)
```

- **BlueZ patch** — adds the Nintendo wired cable-pairing protocol to the
  existing "sixaxis" cable-pairing machinery (the PS3/PS4 flow it already
  has), plus a per-connection "arm" that keeps the controller wakeable.
- **Kernel patch** — makes the `hid-nintendo` driver bind passively on
  **every transport, USB and BT**: it creates the hidraw node (so BlueZ can
  talk to the controller) but does **not** run the stock init handshake and
  creates **no input/LED/battery device at all** on either transport.
  Crucially, it never sends the `0x80 04` USB-lock command that would drop
  the controller out of pair mode. The BT side is passive too, not stock:
  with BlueZ 5.84 the controller's BT input is served by bluetoothd's HID
  profile over uhid (the default), so the kernel driver must not create a
  competing BT input device. The cost of this fork-wide passivity — no
  kernel input/LED/rumble/battery on any transport, for any supported device
  (Pro Controller, Joy-Cons, SFC30, GEN, N64, Charging Grip) — is spelled
  out in §5 and §9.

The controller is always owned by the pairing flow on whichever side it's
connected: wired subcommands over USB hidraw during docking, input + arm over
the BT interrupt channel once connected. Nothing fights over the controller.

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
| `procon_usb_session_init()` | `0x80 02` → `0x80 03` (3 Mbit) → `0x80 02` — the wired UART session handshake with the controller's BT chip. **Must run before any subcommand** or the chip never answers (the kernel fork's probe is passive and sends nothing). |
| `procon_get_device_bdaddr()` | `subcmd 0x02` device-info probe; returns the controller's BT MAC (byte-swapped from display order). |
| `procon_arm_wired()` | `subcmd 0x08 00` — clear shipment mode so the controller can wake from button presses (the reconnect-keeper). |
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
  plug-in: run `procon_usb_session_init()` (the passive kernel fork sends
  nothing, so we must start the session), probe the device MAC
  (`subcmd 0x02`), and **arm on every plug-in** (`0x08 00`, non-fatal on
  failure — the Switch re-arms after every connection).
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

### `profiles/input/device.c` — the per-connection BT arm (reconnect-keeper)

`input_device_connected()` now sends the Switch's post-connection arm
**before** `hidp_add_connection()`: `0x08 00` (clear shipment / LPM-to-sleep)
and `0x30 01` (player-1 LED) over the BT interrupt channel, with report
framing identical to USB. Verified against real behavior: an un-armed
controller does a connect-and-self-terminate dance (Remote User Terminated
0x13 ~150 ms after PSM 19) and can give up entirely; an armed one sticks on
the first connection attempt. Because every connect path funnels through
`input_device_connected()` (inbound and outbound/auto-reconnect), the arm
goes out on **every** connection, not just the first — which is exactly the
Switch's documented "after every connection" behavior.

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

## 5. The kernel patch — `hid-nintendo.c`

> **Status (2026-08): this patch's necessity is under investigation.**
> Code analysis shows the BlueZ patch drives the wired flow entirely
> itself: `procon_usb_session_init()` re-establishes the USB session
> (`0x80 02/03/02`) and every subcmd runs over the plugin's own hidraw fd
> (which the stock driver also creates — `HID_CONNECT_HIDRAW`). The stock
> driver's USB init, including the `0x80 04` "no timeout" command, is
> runtime-only: it writes no SPI pairing records and is not re-run on any
> radio event, so it may not interfere with pairing at all. The pending
> test: **stock kernel + BlueZ-only**, plug → pair logs → unplug → button →
> BT reconnect + input. If it passes, this patch is retired and ProBlue
> becomes a BlueZ-only project. Until that test runs, the install
> instructions (§7) assume the fork is present. Second behavior the test
> must check: replugging over a live BT session — on stock the driver
> re-inits and sends `0x80 04`, which may switch the controller to USB
> mode (a device-identity swap, workaroundable since pairing persists in
> the controller's flash and unplug + button reconnects).

One file, one purpose: **make every instance of the driver passive** so
BlueZ's wired pairing flow can talk to the controller over hidraw without
the kernel driver fighting it — and so the driver never creates a competing
input device on any transport.

The passivity is transport-wide: `joycon_is_passive()` returns true for
**both** `BUS_USB` and `BUS_BLUETOOTH`, for **all** devices in the driver's
id table (the Pro Controller, Joy-Con L/R, SFC30, GEN, N64, and the Charging
Grip). Earlier drafts of this README said "the BT side stays stock" — that
was wrong; this section describes what the patch actually does.

Why BT must be passive too: with stock BlueZ 5.84, a Pro Controller's BT
input is served by bluetoothd's HID profile over **uhid** (the default —
`input_device_connadd()` creates a uhid device whenever uhid is available).
If the kernel driver also bound over BT and created its own `/dev/input`
device, the two would fight over the same controller. Making the kernel side
passive on BT means: when the uhid path is used (the normal case) input
comes through bluetoothd; if the kernel HIDP path is ever used (uhid
unavailable or disabled), the driver binds but does nothing, so it cannot
conflict. The ProBlue BlueZ patch is what actually delivers input and the
post-connection arm over the BT interrupt channel (§4).

Stock behavior that breaks cable pairing: on USB bind, stock `hid-nintendo`
runs the `0x80` handshake, the `0x80 04` USB-only lock, baud-rate switch,
and config subcommands — which on re-enumeration (including after the
`0x06 02` radio-power event) **knocks the controller out of pair mode**.
The fork (`src/kernel/hid-nintendo.c`, patch in
`patches/hid-nintendo-keep-bt-radio.patch`):

1. **`joycon_init()` sends nothing on any transport** — no handshake, no
   `0x80 04` lock, no baud switch, no config subcommands, no
   calibration/IMU/report-mode/rumble setup.
2. **`nintendo_hid_probe()` returns early on every transport** after init,
   before creating input/LED/battery devices. `ctlr_state` stays at INIT, so
   input reports hit `joycon_ctlr_handle_event` and are ignored — no
   NULL-`ctlr->input` deref.
3. **`nintendo_hid_resume()` is a no-op on every transport** — it must not
   flip `ctlr_state` back to READ while `ctlr->input` is NULL.
4. Removes the now-unused `joycon_send_usb()` (dead code, warning-free build).

The patch is transport-explicit (`joycon_is_passive()` checks USB or BT
rather than "always true") so a future transport added to the id table has
to be reviewed against this decision. `hid-ids.h` is untouched — the kernel
already has `USB_DEVICE_ID_NINTENDO_PROCON (0x2009)`.

**What you give up (important):** this module removes the kernel driver's
input/LED/rumble/battery support on **all** transports for **all** the
devices it knows, not just the Pro Controller over USB:

- **Wired USB input** for the Pro Controller, SFC30, GEN, N64 and the
  Charging Grip is gone — no `/dev/input` device appears over USB.
- **BT input** for Joy-Cons and the others is provided by bluetoothd's HID
  profile (uhid) — the same path stock BlueZ usually uses — but if your
  system has uhid unavailable or disabled (kernel HIDP mode), BT input for
  these devices stops working.
- There is currently **no switch** to turn passivity off. If you use
  Joy-Cons or wired USB play, do not install this module (see §9).

**Verified:** the pristine base was oracle-checked against the running kernel
via `srcversion` during development (method: §7.1 A3);
`src/kernel/hid-nintendo.c` == pristine + patch, byte-for-byte.

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
3. `input_device_connected()` sends the **arm** over the BT interrupt channel
   (`0x08 00` + `0x30 01`) *before* `hidp_add_connection()` — so the
   controller sticks, and stays wakeable for the next cycle.
4. Input streams — delivered by bluetoothd's HID profile over **uhid** (the
   kernel fork creates no BT input device; see §5). When the controller
   sleeps again, the host drops the idle link (link supervision timeout),
   re-enters the accept-list-driven page scan state, and is ready for the
   next button press.

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
install ProBlue properly — from source, against *their* kernel and *their*
BlueZ, with no hacks, no "just copy this binary", and nothing that breaks on
the next kernel or package update.

There are two independent parts, and you need both:

| Part | What it is | Where it lives |
|---|---|---|
| **A. Kernel module** (`hid-nintendo`) | The patched controller driver | `patches/hid-nintendo-keep-bt-radio.patch` |
| **B. BlueZ** (`bluetoothd`) | The patched Bluetooth daemon | `patches/bluez-5.84-procon.patch` |

Install A first, then B.

---

### 7.0 Before you start: know your versions

The patches were written against specific bases. Your job is to make them fit
*your* system — the instructions below tell you exactly how, and how to verify
the fit before you build anything.

```bash
uname -r                      # kernel version (e.g. 6.8.0-137-generic)
bluetoothd --version          # BlueZ version (e.g. 5.84)
```

- The **kernel patch** targets the `hid-nintendo.c` from kernel **6.8-era**.
  Its necessity is under active investigation (§5): the BlueZ patch drives
  the wired pairing flow itself, and a stock-kernel + BlueZ-only test is
  pending. If it stays, newer kernels (6.10+) need a real port, not a
  clean apply — the driver has drifted since 6.8 (rumble rate-limiter
  rework, Switch 2 controller work landed upstream). §7.1 steps 2–4 handle
  this and *verify* you got it right before you build.
- The **BlueZ patch** targets stock BlueZ **5.84**. Distros ship various
  versions (Ubuntu 24.04 ships 5.84, Arch ships newer). §7.2 covers building
  5.84 from source, which works everywhere.

**Rule:** never build against "a fetched mainline copy" of anything. Build
against the exact source your system uses (kernel: the source your running
kernel was compiled from; BlueZ: a released 5.84 tarball). The verification
steps below exist precisely to catch a wrong base.

---

### 7.1 Part A — the kernel module

#### A1. Install the build prerequisites

You need the kernel headers for your *running* kernel, the compiler, and the
build tools:

```bash
# Debian / Ubuntu
sudo apt install linux-headers-$(uname -r) build-essential libelf-dev

# Fedora / RHEL / Rocky
sudo dnf install kernel-devel kernel-headers gcc make elfutils-libelf-devel

# Arch / Manjaro / EndeavourOS
sudo pacman -S base-devel linux-headers
```

Verify the header version matches your running kernel exactly:

```bash
ls /lib/modules/$(uname -r)/build      # must exist and match uname -r
```

> If you're not on an Ubuntu 6.8.0-137-series kernel (the one this was
> developed against) and the version-check feels risky, the oracle test in
> A3 exists exactly for this. Do not skip it.

#### A2. Get the pristine `hid-nintendo.c` for YOUR kernel

The patch must apply to the *exact* source your kernel was compiled from —
distro kernels are NOT byte-identical to mainline (Ubuntu backports fixes;
Arch adds a patchset). Distro-specific fetch procedures:
(full method: fetch + oracle in §7.1 A2-A3):

- **Ubuntu / Debian:** install the `linux-source-<ver>` package (or fetch the
  `linux-source` deb from the Ubuntu pool), extract
  `drivers/hid/hid-nintendo.c` + `drivers/hid/hid-ids.h`. There is a script
  in the repo: `tools/get_pristine_hid_nintendo.sh` (fetches, extracts,
  builds, and **oracle-verifies** in one go).
- **Arch / CachyOS:** `pkgctl repo clone linux` (or `asp checkout linux`),
  apply the `archlinux-linux` patchset to the kernel.org tarball, take the
  two files.
- **Fedora / RHEL:** `dnf download --source kernel`, extract the srpm tree,
  take the two files.
- **Generic fallback:** fetch the upstream tag matching your kernel
  (`git.kernel.org/.../linux.git`, tag `v6.8` for 6.8 kernels), then **run
  the oracle test below** — it will tell you if the fetch is wrong.

Place the pristine files somewhere clean:

```bash
mkdir -p ~/ProBlue-build/kernel && cd ~/ProBlue-build/kernel
# copy your pristine hid-nintendo.c and hid-ids.h here
```

#### A3. Verify the pristine source is really yours (the oracle test)

Build the *unpatched* pristine file against your headers and compare
`srcversion` with your running module. Equal ⇒ the source is byte-for-byte
what your kernel runs. Unequal ⇒ wrong source — do not proceed.

```bash
# from ~/ProBlue-build/kernel, with your pristine hid-nintendo.c + hid-ids.h:
cat > Makefile <<'EOF'
obj-m += hid-nintendo.o
KDIR ?= /lib/modules/$(shell uname -r)/build
all:
	make -C $(KDIR) M=$(CURDIR) modules
clean:
	make -C $(KDIR) M=$(CURDIR) clean
EOF
make

# compare against the module your kernel actually runs:
modinfo /lib/modules/$(uname -r)/kernel/drivers/hid/hid-nintendo.ko* \
        | awk '/^srcversion:/{print $2}'
modinfo hid-nintendo.ko | awk '/^srcversion:/{print $2}'
# BOTH VALUES MUST MATCH.
```

(If your distro ships compressed modules — Ubuntu `.ko.zst`, some Fedora
`.ko.xz` — decompress first: `zstd -d -c file.ko.zst > /tmp/stock.ko` then
`modinfo` that.)

#### A4. Apply the ProBlue patch

```bash
cp hid-nintendo.c hid-nintendo.c.pristine
patch -p1 < /path/to/ProBlue/patches/hid-nintendo-keep-bt-radio.patch
```

**If it applies cleanly** — verify it did the right thing, then build:

```bash
# sanity: the fork must differ from pristine in exactly the intended way
diff hid-nintendo.c hid-nintendo.c.pristine | head -30   # passive-probe changes only
make
```

**If it does NOT apply** — the kernel moved on (very likely on 6.10+). Don't
fight `patch`; port the change by hand. The patch's semantics are tiny and
 stable — port them by hand: find `joycon_init` / `joycon_using_usb` /
`nintendo_hid_probe` / `joycon_leds_create` / `joycon_input_create` in your
file and apply the three changes:

1. **`joycon_init()` USB branch sends nothing** — no `0x80` handshake, no
   `0x80 04` USB lock, no baud switch, no config subcommands. The BT path
   keeps the full stock init, in the same order.
2. **`nintendo_hid_probe()` returns early on USB** after init, before
   creating input/LED/battery devices.
3. Remove the now-unused `joycon_send_usb()`.

Then **regenerate the patch** so it's reproducible:

```bash
diff -u --label a/hid-nintendo.c --label b/hid-nintendo.c \
     hid-nintendo.c.pristine hid-nintendo.c > hid-nintendo-keep-bt-radio.patch
```

#### A5. Install it properly (DKMS, so kernel updates don't break it)

The real, update-safe way is **DKMS** — the module is rebuilt automatically
for every new kernel you install. Set it up once:

```bash
sudo mkdir -p /usr/src/hid-nintendo-ProBlue-1.0
sudo cp hid-nintendo.c hid-ids.h Makefile /usr/src/hid-nintendo-ProBlue-1.0/
sudo tee /usr/src/hid-nintendo-ProBlue-1.0/dkms.conf > /dev/null <<'EOF'
PACKAGE_NAME="hid-nintendo-ProBlue"
PACKAGE_VERSION="1.0"
BUILT_MODULE_NAME[0]="hid_nintendo"
DEST_MODULE_LOCATION[0]="/kernel/drivers/hid"
AUTOINSTALL="yes"
MAKE[0]="make -C ${kernel_source_dir} M=${dkms_tree}/${PACKAGE_NAME}/${PACKAGE_VERSION}/build modules"
CLEAN="make -C ${kernel_source_dir} M=${dkms_tree}/${PACKAGE_NAME}/${PACKAGE_VERSION}/build clean"
EOF
sudo dkms add     -m hid-nintendo-ProBlue -v 1.0
sudo dkms build   -m hid-nintendo-ProBlue -v 1.0
sudo dkms install -m hid-nintendo-ProBlue -v 1.0
sudo depmod -a
```

> DKMS ships with most distros (`dkms` package). If you truly can't use DKMS,
> the fallback is `sudo make install` (installs into
> `/lib/modules/$(uname -r)/extra/` + runs `depmod`) — but you'll have to
> repeat it after every kernel update, and the stock module will shadow the
> fork until you do.

#### A6. Load and verify

```bash
sudo modprobe hid_nintendo
lsmod | grep hid_nintendo
dmesg | grep -i "hid-nintendo"     # look for the probe messages
modinfo hid_nintendo | head -5     # version should mention your fork build
```

The USB side now binds **passively**: no USB `/dev/input` device for the
controller appears, only the hidraw node that BlueZ will use. The BT side
binds passively too — see §5 for what that means (and what it costs).
(Note: the module may already be loaded and in use — if the controller is
plugged in, unplug it, `sudo rmmod hid_nintendo`, reload, then replug.)

---

### 7.2 Part B — BlueZ

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
everything (mirrors `tools/get_pristine_hid_nintendo.sh` for the kernel):

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

- **Kernel update:** DKMS rebuilds `hid-nintendo-ProBlue` automatically —
  verify after the reboot with `dkms status` and `modinfo hid_nintendo`.
- **BlueZ update:** distro updates touch `/usr/bin/bluetoothd` /
  `/usr/libexec/bluetooth/bluetoothd`, but your fork lives in `/usr/local`
  and the drop-in keeps pointing at it, so you're unaffected. To rebase the
  ProBlue patch on a newer BlueZ release later, repeat B2–B4 against the new
  tarball (re-apply the patch; port if it doesn't apply; rebuild; reinstall).
- **Rolling back** at any time: remove the drop-in
  (`sudo rm /etc/systemd/system/bluetooth.service.d/ProBlue.conf`, `daemon-reload`,
  `systemctl restart bluetooth`), and for the module
  (`sudo dkms remove hid-nintendo-ProBlue/1.0 --all`, `sudo depmod -a`).

### 7.4 Troubleshooting

| Symptom | Likely cause / fix |
|---|---|
| `procon: usb 0x80 0x02: no reply` | Kernel fork not loaded, or stock module still bound. Check `lsmod \| grep hid_nintendo`, A6. |
| No `procon:` logs at all on plug-in | sixaxis plugin not built (`--enable-sixaxis` missing, B4) or udev not delivering the USB event — check `journalctl -u bluetooth` and replug. |
| Plug-in logs OK, but no BT reconnect | Page-scan lines not in `/etc/bluetooth/main.conf` (B6) — `bluetoothd` reads `/etc/bluetooth/main.conf` when started by systemd, NOT the tree's copy. |
| Controller connects once then never again | The controller's single host slot was overwritten (phone/Switch/another PC). Re-dock to the PC — ProBlue re-pairs on every dock by design (§2, §4). |
| Auth failure (HCI error 0x05) on connect | Key not loaded into the kernel — check for `procon: link key stored` and restart bluetooth after a fresh dock. |
| Module won't load: "version magic" | Built against wrong headers. Rebuild against `/lib/modules/$(uname -r)/build` (A1/A5). |
| DKMS build fails after kernel update | Usually missing headers for the new kernel — `sudo apt install linux-headers-$(uname -r)` (or dnf/pacman equivalent), then `sudo dkms build hid-nintendo-ProBlue/1.0`. |

### 7.5 Reproducing the patches from pristine

The `patches/` files are the canonical patch set. To regenerate them from
this repository's sources (e.g. after porting to a newer kernel/BlueZ),
diff the shipped patched sources (`src/`) against fresh pristine copies:

```bash
# BlueZ — build pristine 5.84, then diff against the shipped sources:
  #   (fetch pristine via tools/get_pristine_bluez.sh, but stop before
  #   applying the patch)
diff -u --label a/bluez-5.84 --label b/problue bluez-5.84 src/bluez \
    > patches/bluez-5.84-procon.patch

# Kernel — diff pristine hid-nintendo.c against the shipped patched file:
diff -u --label a/hid-nintendo.c --label b/hid-nintendo.c \
    <pristine-hid-nintendo.c> src/kernel/hid-nintendo.c \
    > patches/hid-nintendo-keep-bt-radio.patch
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
  when it disconnects. The fork already exports the helper; only the plugin
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

- **The kernel fork disables the driver's own input/LED/rumble/battery
  support — on every transport, for every supported device** (Pro
  Controller, Joy-Con L/R, SFC30, GEN, N64, Charging Grip). Over USB, wired
  play stops: no `/dev/input` device appears for any of these controllers.
  Over BT, input is served by bluetoothd's HID profile over uhid — normally
  equivalent to stock, but on a system where uhid is unavailable or disabled
  (kernel HIDP mode), BT input for these devices stops working. There is
  **no option to turn passivity off**; if you use Joy-Cons or wired USB
  play, do not install this module. See §5.
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
- **Kernel 6.8-era** — the hid-nintendo fork is based on Ubuntu 6.8.0; the
  patch ports forward per §7.1 A4 (fetch your kernel's pristine source via
  `tools/get_pristine_hid_nintendo.sh`, apply, or port by hand).

## 10. Upstreaming notes

Upstream status, stated plainly: the **BlueZ** side is additive and
plausibly upstreamable after review; the **kernel** side is a fork that is
**not yet in an upstreamable shape**. Everything is GPL-2.0-or-later with
BlueZ-style headers and attribution.

**Kernel patch — not upstreamable as-is (and possibly unnecessary — §5).**
It unconditionally disables the
driver's input/LED/rumble/battery support on *every* transport for *every*
device in the id table (§5), i.e. it removes stock features (wired USB play,
kernel-served BT input) with no opt-out and no scope limit. From an
upstream reviewer's point of view that is a regression. Getting it upstream
would require: (a) an opt-in (module parameter or per-device quirk),
(b) scoping passivity to the Pro Controller over USB (and revisiting
whether BT passivity is needed at all once uhid is confirmed as the only BT
route), and (c) accepting that wired USB play is unavailable in the
pairing-mode configuration. That is future work, not this release.

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
| Why is the kernel driver passive on BT too? | With BlueZ 5.84 the controller's BT input is served by bluetoothd's HID profile over uhid; a kernel BT input device would double-claim the controller. The fork makes the kernel side passive everywhere so it can never conflict (§5). |
| Why remove wired USB input? | Passivity *is* the feature: the kernel must not init/lock the controller over USB or it drops out of pair mode. The trade-off (no wired play, no kernel BT input) is documented in §5 and §9. |

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
- **Our own live captures** (2026-08-07 → 08-11), including the
  reconnect-keeper validation (arm-before-hidp, page→accept→key reply→E0→
  PSM 17/19→input streaming):
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
