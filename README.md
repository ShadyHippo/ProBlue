# ProBlue — Nintendo Switch Pro Controller native pairing on Linux

**Wire a Pro Controller to a PC and have it pair, reconnect, and work — no GUI
dialogs, no agents, no hacks. The way the Switch does it.**

ProBlue is a single patch to **BlueZ** (the Linux Bluetooth stack) that makes
the Nintendo Switch Pro Controller a first-class citizen on Linux: plug it
into USB, it pairs itself (or re-pairs if it was already known), and from then
on one button press connects it — with or without any Bluetooth UI open. The
kernel driver stays **stock**; there is no kernel patch.

## Table of contents

1. [Quickstart](#1-quickstart)
2. [Why this exists](#2-why-this-exists)
3. [How a dock + wake + reconnect works, end to end](#3-how-a-dock--wake--reconnect-works-end-to-end)
4. [Architecture](#4-architecture)
5. [Why there is no kernel patch (BlueZ-only)](#5-why-there-is-no-kernel-patch-bluez-only)
6. [The BlueZ patch — file by file](#6-the-bluez-patch--file-by-file)
7. [Configuration notes](#7-configuration-notes)
8. [The firmware's hostname quirk (Bluetooth power profile)](#8-the-firmwares-hostname-quirk-bluetooth-power-profile)
9. [Known limitations & open items](#9-known-limitations--open-items)
10. [Upstreaming notes](#10-upstreaming-notes)
11. [Reverse-engineering sources](#11-reverse-engineering-sources)
12. [License](#12-license)

---

## 1. Quickstart

One part, and it's all you need: a patched **bluetoothd** built from stock
BlueZ **5.84** (this repo's `patches/bluez-5.84-procon.patch`). The kernel
driver stays stock — nothing to build, load, or DKMS (§5).

**Prereq:** the usual BlueZ build chain for your distro (autoconf, automake,
libtool, pkg-config, glib2, dbus, udev, readline, zstd dev packages).

```bash
# 1. Fetch pristine BlueZ 5.84, verify it, apply the patch (one shot):
bash tools/get_pristine_bluez.sh ~/ProBlue-build

# 2. Build:
cd ~/ProBlue-build/bluez-5.84
autoreconf -fi
./configure --enable-sixaxis \
            --disable-obex --disable-mesh --disable-midi \
            --disable-nfc --disable-health --disable-test \
            --disable-btpclient --disable-manpages
make -j"$(nproc)"

# 3. Verify the Pro Controller code is in your binary:
strings src/bluetoothd | grep -i procon     # "procon: ..." strings
./src/bluetoothd --version                  # "Bluetooth daemon 5.84"

# 4. Install (systemd drop-in, so distro updates can't clobber it):
sudo make install                           # lands in /usr/local
sudo mkdir -p /etc/systemd/system/bluetooth.service.d
sudo tee /etc/systemd/system/bluetooth.service.d/ProBlue.conf > /dev/null <<EOF
[Service]
ExecStart=
ExecStart=/usr/local/sbin/bluetoothd
EOF
sudo systemctl daemon-reload && sudo systemctl restart bluetooth

# 5. Configure the page scan (catches the controller's brief wake-page):
sudo tee -a /etc/bluetooth/main.conf > /dev/null <<'EOF'
PageScanType=0x01
PageScanInterval=0x0012
PageScanWindow=0x0012
EOF
sudo systemctl restart bluetooth
```

> `PageScanType` must be the **integer** `0x01`, not the word `interlaced`
> (BlueZ's parser rejects the word). The empty `ExecStart=` in the drop-in
> clears the stock unit's path (Debian/Ubuntu use `/usr/libexec/...`); the
> second line installs yours. Check with `systemctl cat bluetooth | grep -A2 ExecStart`.

**Moment of truth — with no Bluetooth GUI open:**

1. Plug the controller into USB. Within a second or two you should see:
   ```bash
   journalctl -u bluetooth -f
   # procon: usb 0x80 0x02 ack
   # procon: armed (0x08 00 shipment cleared)
   # procon: 3-step step 1/2/3 done
   # procon: link key stored
   # procon: cable pairing complete
   ```
2. Unplug it, press any button once — it connects within a second or two:
   ```bash
   bluetoothctl devices          # Pro Controller
   bluetoothctl info <mac>       # Paired: yes, Trusted: yes
   ```
3. Open a game — input works over BT.

### Troubleshooting

| Symptom | Likely cause / fix |
|---|---|
| `procon: usb 0x80 0x02: no reply` | USB session init failed — check the cable/port and replug (the session is re-established on every plug-in). |
| No `procon:` logs at all on plug-in | sixaxis plugin not built (`--enable-sixaxis` missing) or udev isn't delivering the event — watch `journalctl -u bluetooth` and replug. |
| Plug-in logs OK, but no BT reconnect | Page-scan lines missing from `/etc/bluetooth/main.conf` (bluetoothd reads the system file, not the tree's copy). |
| Controller connects once, then never again | Its single host slot was overwritten (phone/Switch/another PC). Re-dock — ProBlue re-pairs on every dock by design (§2, §6). |
| Auth failure (HCI error 0x05) on connect | Key not loaded into the kernel — check for `procon: link key stored`, re-dock, restart bluetooth. |

### Upgrading & rolling back

- **Kernel updates:** nothing to do — no ProBlue kernel module exists.
- **BlueZ updates:** distro updates touch `/usr/bin/bluetoothd`, but your fork
  lives in `/usr/local` and the drop-in keeps pointing at it. To rebase the
  patch on a newer BlueZ later, see §10 (porting).
- **Roll back:** `sudo rm /etc/systemd/system/bluetooth.service.d/ProBlue.conf`,
  `daemon-reload`, `restart bluetooth`.

---

## 2. Why this exists

The Pro Controller connects over Bluetooth BR/EDR, but unlike most BT
peripherals it does **not** do over-the-air Secure Simple Pairing (SSP) on
connect. It arrives carrying a link key in its SPI flash, written by the host
it last paired with, and it only connects to the host whose MAC address + key
it has stored. The Switch writes that state over a **wired** protocol
(USB/rail) at every dock. Linux has no equivalent: stock BlueZ only knows how
to pair wireless devices over the air, and the controller will sit in
"Limited Discoverable" mode forever waiting for an inquiry that a paired-only
host never sends.

Without this patch, plugging the controller in does nothing; a GUI agent
pairing half-works and breaks the moment no GUI is open. ProBlue fixes the
root cause: **implement the wired pairing protocol in the Bluetooth stack
itself**, so pairing is physical (plug the cable in) and everything else just
works — the way Nintendo designed it.

The wired protocol (public RE work, §11):

- **The wired 3-step pairing** (`subcmd 0x01`):
  1. `x01 x01 {host MAC LE}` — send the host's address; the controller
     replies with its own MAC.
  2. `x01 x02` — the controller generates a fresh 128-bit LTK, saves it to
     its SPI flash (`x2000`), and returns it over the wire, each byte XORed
     with `0xAA`, in flash (Little-Endian) order. **The LTK must be
     byte-reversed before use as a BR/EDR link key** — loading it un-reversed
     fails auth with HCI error 0x05.
  3. `x01 x03` — commit the pairing record.
- **The re-dock arm** (`subcmd 0x08 00`, "set shipment low power state"):
  clears the shipment flag at SPI `x5000` and puts the controller in normal
  LPM-to-sleep mode. Without it the controller stays in shipment mode and
  **cannot wake from button presses** — it never pages the host, so it never
  reconnects. The Switch sends it after every connection; so do we, plus
  `0x30 01` (player-1 LED) for parity.
- **The device-info probe** (`subcmd 0x02`): returns the controller's
  firmware and its own BT MAC (big-endian; byte-swapped for Linux). This is
  how the host learns which BT device the plugged-in controller *is*, so
  pairing attaches to the right address.

## 3. How a dock + wake + reconnect works, end to end

### First-ever dock (or key lost since last dock)

1. Plug the controller into USB. udev fires; the sixaxis plugin's
   `setup_device()` runs.
2. `procon_usb_session_init()` (`0x80 02/03/02`) starts the wired UART
   session; `subcmd 0x02` learns the controller's BT MAC; `0x08 00` arms it.
3. Cable authorization is requested; the device is already trusted (physical
   plug = authorization), so the gate auto-approves with no agent.
4. `procon_pair()` runs the 3-step: host MAC → fresh LTK (XOR-0xAA, LE) →
   save. The LTK is byte-reversed, stored bluetoothd-style, and loaded into
   the kernel via `MGMT_OP_LOAD_LINK_KEYS`.
5. Device becomes non-temporary, trusted, accept-listed, with the hardcoded
   HID SDP record. The GUI shows it as "Paired".

### Wake and reconnect (no GUI needed, forever after)

1. Press any button on the sleeping controller. It wakes and **pages** the
   host — page scan is on (~100% duty) because a cable-paired device exists,
   even with the GUI's discoverable off.
2. Kernel accept list says "allowed" → page → accept → Link Key Reply with
   the fresh key → E0 encryption → PSM 17/19.
3. `input_device_connected()` runs the **serialized setup** over the BT
   interrupt channel *before* `hidp_add_connection()`: `0x02 probe → 0x08 00
   arm → 0x03 30 report-mode full → 0x30 01 player-1 LED`, each sent after
   the previous one's ack — so the controller sticks, leaves simple mode, and
   stays wakeable (§6).
4. Input streams over uhid. When the controller sleeps again, the host drops
   the idle link (link supervision timeout, §7) and re-enters page-scan
   state, ready for the next press.

### Multi-device / headphones

Page scan stays on if the adapter is connectable **or** if any accept-list
device is disconnected (`disconnected_accept_list_entries()`, `hci_sync.c`).
So headphones connected, or a second controller, does **not** stop the host
from hearing the wake-page — the controller's accept-list entry being
disconnected keeps scan on regardless. Verified against the 6.8.0 kernel
source.

## 4. Architecture

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
  switches it to full report mode on every BT connection (§6).
- **No kernel patch.** The **stock** `hid-nintendo` driver binds the
  controller on USB and creates the hidraw node BlueZ talks to; its own USB
  init is runtime-only and never interferes with the wired pairing flow.
  Over BT, input is served by bluetoothd's HID profile over uhid, so the
  stock driver never creates a competing BT input device. Verified
  end-to-end on a stock kernel — see §5.

The controller is always owned by the pairing flow on whichever side it's
connected: wired subcommands over USB hidraw during docking, input + serialized
setup over the BT interrupt channel once connected. Nothing fights over the
controller.

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

- Install **only** the BlueZ patch (§1). The kernel module stays stock:
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

## 6. The BlueZ patch — file by file

All changes are relative to stock BlueZ **5.84** (the patch applies to the
released 5.84 tarball from kernel.org). The patched source files ship in
`src/bluez/`; the canonical patch is `patches/bluez-5.84-procon.patch`.

### `profiles/input/procon.h` (new) — protocol + device table

The vendor-specific half of cable pairing: all protocol constants
(`PROCON_*`), the report layouts (`0x01` output subcmd report, `0x21` ACK
reply), the USB-mode commands (`0x80 02` handshake, `0x80 03` 3-Mbit baud),
the subcommand IDs, and the Nintendo over-cable device table
(`get_nintendo_pairing()` — the Nintendo counterpart of `get_pairing()` in
`sixaxis.h`). Also `PROCON_HID_SDP_RECORD`: the controller's HID SDP service
record, captured verbatim from bluetoothd's SDP cache during a genuine GUI
pairing (`docs/golden/procon_sdp_cache`). Setting it kills the "malformed SDP
seed" problem — services resolve from the hardcoded record with no SDP
seed/cache needed, mirroring the existing `SIXAXIS_HID_SDP_RECORD` mechanism.

### `profiles/input/procon.c` (new) — the wired protocol

Four public functions, all running over the USB hidraw fd:

| Function | What it does |
|---|---|
| `procon_usb_session_init()` | `0x80 02` → `0x80 03` (3 Mbit) → `0x80 02` — the wired UART session handshake with the controller's BT chip. **Must run before any subcommand** (we re-establish the session ourselves) or the chip never answers. |
| `procon_get_device_bdaddr()` | `subcmd 0x02` device-info probe; returns the controller's BT MAC (byte-swapped from display order). |
| `procon_arm_wired()` | `subcmd 0x08 00` — clear shipment mode so the controller can wake from button presses (the wired-side arm; the BT-side keeper is in `device.c`). |
| `procon_pair()` | The full wired 3-step (host MAC → GET_LTK → save) with the LTK byte-reverse + XOR-0xAA decode; re-inits the session first (a gap between init and the 3-step can let the chip drop the session) and re-arms before pairing, matching the Switch's order. |

Subcommand framing: 64-byte output report `0x01` (`[0]=0x01, [1]=counter,
[2..9]=rumble(0), [10]=subcmd, [11+]=data`), reply report `0x21`
(`[13]=ACK (0x80+ ok), [14]=subcmd, [15+]=data`).

### `plugins/sixaxis.c` — wiring the Pro Controller into the cable-pairing flow

The sixaxis plugin already watches udev for cable-paired devices (PS3/PS4).
We extend the same machinery:

- `get_pairing_type_for_device()` — falls back to `get_nintendo_pairing()`
  when no PS entry matches.
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
- `agent_auth_cb()` — the PROCON path runs `procon_pair()` inline, stores
  the acquired LTK via `btd_adapter_store_link_key()`, marks the device
  trusted, and sets the hardcoded HID SDP record. Cable pairing *is* the
  authorization (the user physically plugged the controller in), so no agent
  prompt is ever shown.
- `setup_device()` — also calls `btd_adapter_set_connectable(adapter, true)`
  so the host keeps page-scanning after a cable-paired controller is plugged
  in (the controller wakes by paging the host; the GUI only enables
  connectable while discoverable).

### `profiles/input/device.c` — the per-connection serialized setup (reconnect-keeper)

`input_device_connected()` runs a **serialized subcommand setup** over the BT
interrupt channel, started *before* `hidp_add_connection()` and continued by
the 0x21-ack hook in `hidp_recv_intr_data()` (raw report `[1]==0x21` subcmd
reply, `[14]`=ack, `[15]`=subcmd echo):

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
| `btd_adapter_store_link_key()` + `reload_link_keys()` | Runtime BR/EDR link-key registration: persist the LTK bluetoothd-style (info file `[LinkKey]` section) and reload the whole kernel key list via `MGMT_OP_LOAD_LINK_KEYS` (the *only* runtime key-add path in 5.84 — there is no `MGMT_OP_ADD_LINK_KEY`). Required because the controller connects with the key we just wrote, before any over-the-air key exchange could happen. This also resolves the stock TODO in `ds4_set_central_bdaddr()` ("we could put the key here but there is no way to force a re-loading of link keys to the kernel from here") — these two functions are exactly that missing path. |
| `btd_adapter_set_connectable()` | Explicit page-scan control from the plugin (the Switch and phones stay connectable always; a cable-paired controller waking by paging must be heard even with the GUI's discoverable off). |
| `property_set_mode()` (DISCOVERABLE case) | With kernel conn control, turning discoverable off normally *also* clears connectable. We skip that swap while cable-paired devices exist, so page scan survives a GUI "discoverable off". |
| `adapter_start()` | The kernel clears the accept list on power-off; re-add non-temporary cable-paired BR/EDR devices on every power-on so the wake-page is heard even after a bluetoothd restart, with no GUI. |

### `Makefile.plugins` — build wiring

The `if SIXAXIS` block gains `profiles/input/procon.c` + `procon.h`.
`Makefile.am:300` includes `Makefile.plugins`, so this is the correct
upstream-able location (`Makefile.in` in the tree is the regenerated
artifact; upstream regenerates it with `autoreconf`).

## 7. Configuration notes

### Page-scan tuning (the battery trade-off)

```ini
PageScanType=0x01        # interlaced (INTEGER 0x01, not the word "interlaced")
PageScanInterval=0x0012
PageScanWindow=0x0012    # window == interval ⇒ ~100% listen duty
```

The controller pages the host only **briefly** on wake. The default host page
scan listens 11.25 ms per 1.28 s (≈ 0.9% duty), so the brief page is usually
missed; window == interval makes the host listen essentially always, so the
first wake-page is caught. The ~100% duty comes at a small radio cost (the
screen dominates a laptop's battery; a modern BT radio listening for pages is
nearly free compared to active traffic).

Two gentler options:

- **Accept it** — the simplest, and what this repo ships.
- **Dynamic (planned)** — call `btd_adapter_set_fast_connectable(adapter,
  true)` from the plugin only while a cable-paired controller exists, false
  when it disconnects. The BlueZ patch already exports the helper; only the
  plugin call sites are missing.

`PageScanType` must be an **integer** (`0x01`), not the word `interlaced` —
BlueZ's `parse_config_int()` rejects the word (seen live).

### Link supervision timeout (the "zombie" window)

After the controller sleeps, the host holds the idle ACL "Connected" for the
default ~20 s link-supervision timeout, and page scan is off during that
window (the controller's accept-list entry looks satisfied). In practice:
wait ~20 s after the controller sleeps, then one button press connects
instantly. Reconnects aren't frequent enough to matter, so this repo accepts
the stock 20 s. If you want it snappier, a per-Pro-Controller
`HCI_Write_Link_Supervision_Timeout` (2–3 s) from the procon plugin at
connect would shrink the window without touching the global default for other
devices — deliberately left as a follow-up.

## 8. The firmware's hostname quirk (Bluetooth power profile)

<details>
<summary><b>The controller checks the host's Bluetooth name — and changes its power/connect behavior on a non-"Nintendo" host.</b> (workaround: verified by many users 2021–2026; firmware mechanism: second-hand RE) — click to expand</summary>

The Pro Controller's firmware switches Bluetooth behavior based on the name of
the host it connects to. A host whose name does **not** start with `Nintendo`
gets the controller's **sniff mode**: the device listens at reduced intervals,
sends no keepalive traffic, and if it does not hear from the host it assumes
the connection is dead and **disconnects**. Rumble + IMU traffic can
overwhelm that reduced-bandwidth mode — which is the classic "random
disconnect with rumble enabled" bug.

The workaround is well-established and still active in 2026: name the
Bluetooth adapter `Nintendo`, `Nintendo Switch`, or anything starting with
`Nintendo` (e.g. `Nintendo PC`, `NintendoDeck`):

```bash
bluetoothctl system-alias "Nintendo Switch"     # BlueZ way (adapter alias)
# or legacy:  sudo hciconfig hci0 name 'Nintendo'
# or:         PRETTY_HOSTNAME=Nintendo in /etc/machine-info, then restart bluetooth
```

**Why it matters here:** the ~2.2 s "host silence kills the link" behavior
captured in `docs/maki_memories.md` §6 (btmon 2026-08-13) is consistent with
sniff mode — the controller dropped the link when we sent nothing, and kept
it alive while host subcommand traffic flowed. ProBlue's per-connection
serialized setup therefore works *despite* the quirk. Renaming the adapter
may make the controller hold the link with zero host traffic and page more
aggressively on wake; this is **not yet A/B-tested** in ProBlue's setup — the
obvious next experiment is `bluetoothctl system-alias "Nintendo Switch"` vs.
the default name. If it holds, the plugin could set the adapter alias on
cable-pair (alongside `btd_adapter_set_connectable()`) to make the fix
automatic.

**Sources** (primary, 2021–2026):

- ArchWiki — *Gamepad*: "Pro Controller disconnects over Bluetooth with
  rumble enabled… rename the Bluetooth adapter hostname to `Nintendo` or
  anything that begins with that substring" —
  https://wiki.archlinux.org/title/Gamepad
- `DanielOgorchock/linux` issue #33 — the original hid-nintendo disconnect
  report; includes the second-hand firmware-RE claim (`memcmp` on
  `Nintendo Switch` / `NintendoRobson` / `Nintendo`; sniff-mode mechanics)
  and many user confirmations of the rename fix —
  https://github.com/DanielOgorchock/linux/issues/33
- `bluez/bluez` issue #1797 — feature request to auto-spoof the host name
  for Switch controllers: "generic mode… leads to dropped packets and
  controller disconnects… prefixed with the string `Nintendo `… all of these
  problems clear up" — https://github.com/bluez/bluez/issues/1797
- `ValveSoftware/SteamOS` issue #1262 — Steam Deck users: rename the adapter
  to something containing "Nintendo" —
  https://github.com/ValveSoftware/SteamOS/issues/1262
- `ValveSoftware/steam-for-linux` issue #7631 — the rumble-disconnect
  reports + the rename fix —
  https://github.com/ValveSoftware/steam-for-linux/issues/7631
- `darthcloud/BlueRetro` issue #146 — an independent project that hit the
  same issue and the same fix — https://github.com/darthcloud/BlueRetro/issues/146

⚠ **Confidence split:** the *mechanism* (hostname `memcmp`, sniff vs
full-power profile) comes from a second-hand RE summary in issue #33 and is
**not independently verified**; the *workaround* is verified by many
independent users across 2021–2026. Also unverified: whether the controller
switches report format on the Nintendo path (ProBlue's serialized `0x03 30`
forces full mode regardless, so the setup should win either way — confirm in
the A/B traces).
</details>

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
  documented in §7; a dynamic "only while a controller is present" toggle is
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
  nothing ProBlue-specific to rebuild on kernel updates (§1).

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
included), the fact that connectable is never turned back off (§7/§9), and
the Pro Controller VID/PID special-casing in the generic input profile
(`profiles/input/device.c`).

Things an upstream reviewer will ask about, answered:

| Question | Answer |
|---|---|
| Why not just use over-the-air SSP? | The controller does not SSP on connect; it connects only to the stored-MAC+key host. Wired pairing is the vendor mechanism. |
| Why re-pair on every dock? | The Switch does (c2j capture: host-record push at every connect), there is no "read stored central" subcommand, and the controller's single slot can be silently overwritten by another host. Fresh key per dock is the robust model. |
| Why the hardcoded SDP record? | The stock SDP seed is malformed on 5.84 for this device; the record is captured verbatim from a genuine pairing. Mirrors the existing `SIXAXIS_HID_SDP_RECORD`. |
| Why the global page-scan config? | The controller pages only briefly on wake; the stock ~0.9% duty misses it. A per-plugin `set_fast_connectable` dynamic toggle is the planned gentler alternative. |
| Does the stock kernel driver conflict? | No — verified on stock. Over USB it creates the hidraw node the plugin talks to (`HID_CONNECT_HIDRAW`) and its init is runtime-only (writes no SPI pairing records, not re-run on radio events); `procon_usb_session_init()` re-establishes the session regardless. Over BT the controller's input is served by bluetoothd over uhid, so `hid-nintendo` never creates a competing BT input device in this configuration (§5). |

**Porting to a newer BlueZ:** the changed symbols (`setup_device`,
`agent_auth_cb`, `input_device_connected`, `property_set_mode`,
`adapter_start`, `store_link_key`) are stable across 5.8x; the patch's own
comments call out every anchor. After porting, regenerate the patch from
`src/bluez/` vs. pristine:

```bash
# fetch pristine 5.84 via tools/get_pristine_bluez.sh, then:
diff -u --label a/bluez-5.84 --label b/problue \
    <pristine-tree> <patched-tree> > patches/bluez-5.84-procon.patch
```

## 11. Reverse-engineering sources

The protocol implementation is based on public reverse-engineering work, plus
our own live captures. Key sources:

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
    GUI pairing (the LTK cross-check; **link key redacted** in the shipped
    copy).
  - `docs/golden/procon_sdp_cache` — the controller's SDP service record,
    source of `PROCON_HID_SDP_RECORD`.

Working protocol notes (semi-RE, flags unverified claims) were kept internal
during development; the verified conclusions are in §2 above. The patch and
source comments cite a few of those internal notes by name
(`Bluez_switch_cable_plan.md`, "c2j", "evidence 21", `bt_pairer.cpp:1427`).
They are not shipped in this repo — they were development scratch — and the
conclusions they reference are stated in full in §2/§6.

## 12. License

- The `LICENSE` file at the repo root is the GNU GPL **version 2** reference
  text (upstream GPLv2, June 1991).
- BlueZ patch and the historical kernel fork: **GPL-2.0-or-later** (matches
  BlueZ; new files carry the BlueZ SPDX header + `Copyright (C) 2026 Tim Van
  Dyke <tim.vandyke123@gmail.com>`).
- This documentation: **CC-BY-4.0** unless noted otherwise — the canonical
  text is at <https://creativecommons.org/licenses/by/4.0/>.

This project is not affiliated with or endorsed by Nintendo. "Nintendo",
"Switch", and "Pro Controller" are trademarks of Nintendo. The RE sources in
§11 are the work of their respective authors, attributed above.

---

*ProBlue — Nintendo Switch Pro Controller, properly.*
