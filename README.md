# ProBlue — Nintendo Switch Pro Controller Switch-parity pairing on Linux

> **STATUS (2026-08-24).** Architecture decision: this is a **two-patch**
> project — the BlueZ patch below PLUS a slim passive-USB kernel patch for
> `hid-nintendo`. An earlier "BlueZ-only, no kernel patch" claim in this repo
> was based on pairing-level evidence and is retracted: stock `hid-nintendo`
> pins the controller to USB (`0x80 04`) and its init collides with the plugin
> over hidraw. Input over BT has never been validated end-to-end on any
> configuration; see `PLAN.md` for proven status, evidence anchors, and next
> steps before trusting anything here about "working".

ProBlue implements the pairing the Switch does on Linux: a **BlueZ patch**
(wired cable-pairing) plus a small **kernel patch** (keep the BT radio alive
over USB; wire never carries input).
## Table of contents

1. [Quickstart](#1-quickstart)
2. [Why this exists](#2-why-this-exists)
3. [The BlueZ patch — file by file](#3-the-bluez-patch--file-by-file)
4. [Configuration notes](#4-configuration-notes)
5. [The firmware's hostname quirk (Bluetooth power profile)](#5-the-firmwares-hostname-quirk-bluetooth-power-profile)
6. [Known limitations & open items](#6-known-limitations--open-items)
7. [Upstreaming notes](#7-upstreaming-notes)
8. [Reverse-engineering sources](#8-reverse-engineering-sources)
9. [License](#9-license)

---

## 1. Quickstart

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

# 4. Install — one shot, idempotent (make install + systemd drop-in + page scan):
bash tools/install.sh ~/ProBlue-build
```

> `install.sh` is idempotent (safe to re-run) and handles the fiddly bits:
> it locates the installed daemon wherever `configure` put it (default
> `/usr/local/libexec/bluetooth/bluetoothd`, **not** `/usr/local/sbin`),
> unmasks a previously-masked `bluetooth.service`, writes the drop-in, and
> sets the page-scan keys in the `[BR]` section — appending them at the end
> of the file lands them in the wrong section and BlueZ ignores them.
> `PageScanType` must be the **integer** `0x01`, not the word `interlaced`
> (BlueZ's parser rejects the word). The empty `ExecStart=` in the drop-in
> clears the stock unit's path (Debian/Ubuntu use `/usr/libexec/...`); the
> second line installs yours — `make install` also copies BlueZ's own
> `bluetooth.service` over the distro unit, and the drop-in keeps your
> `ExecStart` whichever unit file wins. Check with
> `systemctl cat bluetooth | grep -A2 ExecStart`.

**How to use:**

1. Plug the controller into USB. Within a second or two you should see:
   ```bash
   journalctl -u bluetooth -f
   # procon: usb 0x80 0x02 ack
   # procon: armed (0x08 00 shipment cleared)
   # procon: 3-step step 1/2/3 done
   # procon: link key stored
   # procon: cable pairing complete
   ```
2. Unplug it, press the face buttons or the L/R buttons for the Pro Controller to "reconnect" to your device.

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

The wired protocol (public RE work, §8):

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
  reconnects. The Switch sends it after every connection; so does ProBlue, plus
  `0x30 01` (player-1 LED) for parity.
- **The device-info probe** (`subcmd 0x02`): returns the controller's
  firmware and its own BT MAC (big-endian; byte-swapped for Linux). This is
  how the host learns which BT device the plugged-in controller *is*, so
  pairing attaches to the right address.

## 3. The BlueZ patch — file by file

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
record, captured verbatim from bluetoothd's SDP cache during a real GUI
pairing (`docs/golden/procon_sdp_cache`). Setting it kills the "malformed SDP
seed" problem — services resolve from the hardcoded record with no SDP
seed/cache needed, mirroring the existing `SIXAXIS_HID_SDP_RECORD` mechanism.

### `profiles/input/procon.c` (new) — the wired protocol

Four public functions, all running over the USB hidraw fd:

| Function | What it does |
|---|---|
| `procon_usb_session_init()` | `0x80 02` → `0x80 03` (3 Mbit) → `0x80 02` — the wired UART session handshake with the controller's BT chip. **Must run before any subcommand** (the plugin establishes the session itself) or the chip never answers. |
| `procon_get_device_bdaddr()` | `subcmd 0x02` device-info probe; returns the controller's BT MAC (byte-swapped from display order). |
| `procon_arm_wired()` | `subcmd 0x08 00` — clear shipment mode so the controller can wake from button presses (the wired-side arm; the BT-side keeper is in `device.c`). |
| `procon_pair()` | The full wired 3-step (host MAC → GET_LTK → save) with the LTK byte-reverse + XOR-0xAA decode; re-inits the session first (a gap between init and the 3-step can let the chip drop the session) and re-arms before pairing, matching the Switch's order. |

Subcommand framing: 64-byte output report `0x01` (`[0]=0x01, [1]=counter,
[2..9]=rumble(0), [10]=subcmd, [11+]=data`), reply report `0x21`
(`[13]=ACK (0x80+ ok), [14]=subcmd, [15+]=data`).

### `plugins/sixaxis.c` — wiring the Pro Controller into the cable-pairing flow

The sixaxis plugin already watches udev for cable-paired devices (PS3/PS4).
The patch extends the same machinery:

- `get_pairing_type_for_device()` — falls back to `get_nintendo_pairing()`
  when no PS entry matches.
- `device_added()` / `setup_device()` — accept `CABLE_PAIRING_PROCON`; on
  plug-in: run `procon_usb_session_init()` (the plugin starts the wired UART
  session itself), probe the device MAC (`subcmd 0x02`), and **arm on every
  plug-in** (`0x08 00`, non-fatal on failure — the Switch re-arms after
  every connection).
- `setup_device()` — **PROCON is exempt from the "already known, skipping"
  early-return when the device is known but disconnected.** The Switch
  re-pairs on *every* dock, and there is no "read stored central" subcommand
  to detect whether the controller still holds the previously stored key (it
  can be lost by pairing to another host since the last dock). A
  trusted-but-disconnected Pro Controller therefore re-runs the full pairing
  flow on every dock —
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

Why ack-waited, and why this sequence:

- The controller processes subcommands slowly (~0.31 s per ack) and drops an
  un-acked batch — a fire-and-forget burst leaves it stuck in its search
  state, cycling connect → drop → re-page.
- `0x08 00` clears shipment / LPM-to-sleep — an un-armed controller does a
  connect-and-self-terminate dance and can give up entirely; `0x03 30`
  switches to standard full report mode; `0x30 01` lights player-1.
- Every connect path funnels through `input_device_connected()` (inbound and
  outbound/auto-reconnect), so the setup runs on **every** connection, not
  just the first — the Switch's documented "after every connection" behavior.

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
| `btd_adapter_store_link_key()` + `reload_link_keys()` | Runtime BR/EDR link-key registration: persist the LTK bluetoothd-style (info file `[LinkKey]` section) and reload the whole kernel key list via `MGMT_OP_LOAD_LINK_KEYS` (the *only* runtime key-add path in 5.84 — there is no `MGMT_OP_ADD_LINK_KEY`). Required because the controller connects with the freshly written key, before any over-the-air key exchange could happen. This also resolves the stock TODO in `ds4_set_central_bdaddr()` — its comment notes the key could be stored there, but with no way to force a kernel link-key reload; these two functions are exactly that missing path. |
| `btd_adapter_set_connectable()` | Explicit page-scan control from the plugin (the Switch and phones stay connectable always; a cable-paired controller waking by paging must be heard even with the GUI's discoverable off). |
| `property_set_mode()` (DISCOVERABLE case) | With kernel conn control, turning discoverable off normally *also* clears connectable. The swap is skipped while cable-paired devices exist, so page scan survives a GUI "discoverable off". |
| `adapter_start()` | The kernel clears the accept list on power-off; re-add non-temporary cable-paired BR/EDR devices on every power-on so the wake-page is heard even after a bluetoothd restart, with no GUI. |

### `Makefile.plugins` — build wiring

The `if SIXAXIS` block gains `profiles/input/procon.c` + `procon.h`.
`Makefile.am:300` includes `Makefile.plugins`, so this is the correct
upstream-able location (`Makefile.in` in the tree is the regenerated
artifact; upstream regenerates it with `autoreconf`).

## 4. Configuration notes

### Page-scan tuning (the battery trade-off)

```ini
# In /etc/bluetooth/main.conf, under the [BR] section (the keys ship there,
# commented out — don't append them to the end of the file, BlueZ only parses
# keys inside their section):
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
- **Dynamic (Requires further work)** — call `btd_adapter_set_fast_connectable(adapter,
  true)` from the plugin only while a cable-paired controller exists, false
  when it disconnects. The BlueZ patch already exports the helper; only the
  plugin call sites are missing.

`PageScanType` must be an **integer** (`0x01`), not the word `interlaced` —
BlueZ's `parse_config_int()` rejects the word.

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

## 5. The firmware's hostname quirk (Bluetooth power profile)

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
forces full mode regardless, so the setup wins either way).
</details>

## 6. Known limitations & open items

- **Wired USB carries no input (by design):** with the required kernel patch,
  the USB-bound `hid-nintendo` instance is passive (bind + hidraw only), so no
  USB input device exists. With the STOCK module this was merely an observed
  side effect of the wired session claiming the controller first — and the
  stock module must not be used anyway (see the kernel-conflict answer in §7).
  Input is always over BT; USB serves pairing + charging only.
- **Input over BT has never been validated end-to-end [PROVEN].** Journal
  forensics (see PLAN.md): calibration reads fail on essentially every session
  (333 fallbacks / 84 binds across all eras), report streams stall in bursts,
  and probes intermittently die (-110). The same degradation appears under a
  pure stock GUI pairing from before this project existed, so it is not caused
  by these patches — but no configuration has yet delivered reliable input on
  the test machine. Do not treat the pairing flow working as the product
  working.
- **Page scan is kept on (connectable) indefinitely once a cable-paired
  device has been plugged in.** `setup_device()` calls
  `btd_adapter_set_connectable(adapter, true)` and nothing reverts it, and
  turning the GUI's discoverable off no longer clears connectable while
  cable-paired devices exist. This is the ~100% page-scan duty trade-off
  documented in §4; a dynamic "only while a controller is present" toggle is
  planned but not implemented.
- **Hostname-quirk automation (untested)** — renaming the adapter to
  `Nintendo…` may make the controller hold the link with zero host traffic
  (§5); the plugin could set the adapter alias on cable-pair, alongside
  `btd_adapter_set_connectable()`, to make that fix automatic. Not tested.
- **`Authorization request for non-connected device!?`** — observed once
  during reconnects and worked around: cable pairing authorizes (and
  automatically trusts) the device while it is still physically on USB, i.e. before the BT
  link exists, so bluetoothd's authorization gate can fire for a device that
  is not (yet) connected. It did not block pairing or reconnects and has not
  recurred; if it appears in normal use, capture `journalctl -u bluetooth`
  around it and report it.
- **BlueZ 5.84 only** — the patch is against 5.84; porting notes for newer
  versions are in the file comments (symbols are stable across 5.8x).

## 7. Upstreaming notes

**BlueZ patch — review concerns.**

- global `property_set_mode` change (discoverable-off no longer clears connectable whenever *any* cable-paired device exists — PS3/PS4 included).
- the Pro Controller VID/PID special-casing in the generic input profile (`profiles/input/device.c`).

| Question | Answer |
|---|---|
| Why not just use over-the-air SSP? | The controller does not SSP on connect; it connects only to the stored-MAC+key host. Wired pairing is the vendor mechanism. |
| Why re-pair on every dock? | The Switch re-pairs at every dock, there is no "read stored central" subcommand, and the controller's single slot can be silently overwritten by another host. Fresh key per dock is the robust model. |
| Why the hardcoded SDP record? | The stock SDP seed is malformed on 5.84 for this device; the record is captured verbatim from a real pairing. Mirrors the existing `SIXAXIS_HID_SDP_RECORD`. |
| Why the global page-scan config? | The controller pages only briefly on wake; the stock ~0.9% duty misses it. A per-plugin `set_fast_connectable` dynamic toggle is the planned gentler alternative. |
| Does the stock kernel driver conflict? | **Yes — a kernel patch is required [PROVEN].** Stock USB init sends `0x80 04` (pin-to-USB/no-timeout), which idles the BT radio while docked and breaks button-wake reconnect. Its active init also collides with the plugin over the same hidraw during docks (journal 2026-08-14 23:43: every cal read failed). The kernel patch makes USB binding passive (hidraw only); BT stays stock upstream. |

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

## 8. Reverse-engineering sources

The protocol implementation is based on public reverse-engineering work and
captured session artifacts. Key sources:

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
- **Captured session artifacts** (`docs/golden/`):
  - `docs/golden/procon_info` — bluetoothd's stored device info for a real
    pairing (**link key redacted** in the shipped copy).
  - `docs/golden/procon_sdp_cache` — the controller's SDP service record,
    source of `PROCON_HID_SDP_RECORD`.

## 9. License

- The `LICENSE` file at the repo root is the GNU GPL **version 2** reference
  text (upstream GPLv2, June 1991).
- The BlueZ patch: **GPL-2.0-or-later** (matches BlueZ; new files carry the
  BlueZ SPDX header + `Copyright (C) 2026 Tim Van Dyke
  <tim.vandyke123@gmail.com>`).
- This documentation: **CC-BY-4.0** unless noted otherwise — the canonical
  text is at <https://creativecommons.org/licenses/by/4.0/>.

This project is not affiliated with or endorsed by Nintendo. "Nintendo",
"Switch", and "Pro Controller" are trademarks of Nintendo. The RE sources in
§8 are the work of their respective authors, attributed above.

---

*ProBlue — Nintendo Switch Pro Controller, properly.*
