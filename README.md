# ProBlue

Linux support for **Nintendo Switch Pro Controller cable pairing** — plug the
controller into a PC with its USB-C cable, let it pair over the wire exactly like
it does on a Switch, then unplug and use it as a normal Bluetooth controller.

Every change here exists because a test showed the stock stack needs it; the
[why each piece is needed](#why-each-piece-is-needed) section below is the record.

---

## The problem

The Pro Controller can pair to a host over its USB-C cable (that is how the
Switch pairs it: plug in, done). On Linux two things get in the way.

1. **The kernel driver fights the pairing.** `hid-nintendo` binds to the USB
   interface, runs its own init, and creates a `/dev/input` device. Two problems
   with that:
   - It sends command `0x80 04` ("pin to USB"), which makes the controller stop
     timing out and *never revert to Bluetooth* while plugged in. This is what
     prevents "use it wirelessly while charging".
   - Its init/session writes race the userspace code that needs the same
     `hidraw` to perform pairing. The wire carries **no input** at all (input is
     Bluetooth-only, same as on the Switch), so the `/dev/input` node it creates
     is dead weight — but it still contends for the interface.
2. **No userspace implements the wired pairing handshake.** The Pro Controller's
   cable-pairing protocol (Broadcom UART session + HID subcommands that read and
   rewrite the controller's stored pairing record) exists only in the console and
   in a handful of reverse-engineering notes. Without it, a cable-paired
   controller's link key never reaches the Bluetooth daemon, so the controller
   cannot reconnect over radio afterwards.

Net effect on stock Linux: plugging the controller in does nothing useful, and
if you do get it paired, the `0x80 04` from the kernel driver holds it on USB.

---

## The solution

Two focused changes. They are independent in purpose but designed to be used
together: the kernel change frees the USB interface, and the BlueZ change uses it.

### 1. Kernel — make `hid-nintendo` passive on USB
`src/kernel/drivers/hid/hid-nintendo.c`, patch
`src/patches/kernel-hid-nintendo-usb-passive-6.18.46.patch`

On the USB transport only, the driver now binds, exposes `hidraw`, and sends
**nothing**: no `0x80 02/03/04` session commands, no subcommands, no LED or
battery init, no `/dev/input` device. Bluetooth is untouched and 100% stock.

Because nothing pins the controller to USB, it times out and reverts to
Bluetooth on its own — which is exactly the desired "wireless while charging"
behaviour, obtained by *not writing* rather than by adding a command.

### 2. BlueZ — implement wired (cable) pairing
`src/bluez/…`, patch `src/patches/bluez-procon-cable-pairing-5.86.patch`

The `sixaxis` input plugin (which already handles USB cable pairing for Sony
controllers) grows a Nintendo Pro Controller path. When the controller is
plugged in it:

- opens the controller's `hidraw` and runs the BT-side UART session
  (`0x80 02/03/02`),
- reads the controller's identity (`0x02` device info) and current pairing
  record (SPI `x2000` via `0x10`, with `0x05` as the "is a host stored?" probe),
- decides whether to pair: if the stored record already belongs to us, it reuses
  it; otherwise it runs the wired 3-step pairing (`0x01 01/02/03`),
- stores the resulting BR/EDR link key in bluetoothd's storage, reloads keys,
  marks the device **Paired + Bonded** and trusted, and accepts the inbound
  connection.

After that, unplugging the cable makes the controller revert to Bluetooth, and a
button press reconnects it using the stored key.

---

## How it works

The full byte-level details live in the code comments: `procon.h`/`procon.c`
for the wired side, and the kernel patch for the passivity. The shape is:

- **Report framing.** Every command is HID OUTPUT report `0x01` (packet counter
  + zeroed rumble + subcommand id + payload); replies are INPUT report `0x21`.
  The ack/subcommand bytes sit at a *different offset over USB vs Bluetooth*
  because bluetoothd strips the HIDP header before the hook sees the report —
  both conventions are correct in their own context.
- **UART session.** Two-byte writes `0x80 02` (handshake), `0x80 03` (3 Mbit),
  `0x80 02` again (re-handshake), answered by `0x81 <cmd>`. Subcommands are not
  answered until this session is open, and the passive kernel never opens it.
- **Stored pairing.** The controller keeps one host record in SPI flash at
  `x2000`: magic `0x95`, host MAC big-endian, 128-bit LTK little-endian, and a
  host-capability byte (`0x68` Switch / `0x08` PC). **Read-then-decide** reads it
  (subcmd `0x10`, with `0x05` as a quick "is a host stored?" probe) and reuses
  the record when it belongs to us, instead of re-pairing on every dock.
- **The wired 3-step** (`0x01`): step 1 registers the host address, step 2
  returns the **stored** LTK (XOR-`0xAA`, flash order; byte-reversed for the
  BR/EDR link key), step 3 commits.
- **Wake / shipment state.** The controller's flash byte `x5000` selects the
  power profile: `0x01` = shipment (button wake disabled), `0xFF` = normal
  (scan-on-button-press). The Switch sends subcmd `0x08 00` to clear shipment
  after every connection; the port does the same at dock (a no-op on units
  already at `0xFF`). A sleeping controller only wakes on a button press and
  pages the host — the host never pages it. (A *long* press can wake even a
  shipment-state controller, so tests must use normal presses.)
- **Reconnect** is then ordinary Bluetooth: with the key in bluetoothd and the
  device bonded, a button press makes the controller page the host and the host
  accepts with the stored key.

---

## Source layout

| Path | Contents |
|---|---|
| `src/kernel/drivers/hid/hid-nintendo.c` | full **patched** driver (vs pristine 6.18.46) |
| `src/bluez/…` | full **patched** BlueZ files (vs pristine 5.86) |
| `src/patches/` | generated patches — what you actually apply |
| `src/MANIFEST.md` | pinned nixpkgs rev, versions, store paths, sha256s |
| `docs/hardware/` | secondary: an unrelated Intel-radio quirk + research notes |
| `tools/` | fetch pristine sources, regenerate patches, build helpers |

**Rule:** `src/` holds the reviewed full files and is the source of truth;
`src/patches/` is generated from it (`tools/make-patches.zsh`). One direction
only — never hand-edit a generated patch.

---

## Applying it

```sh
# 1. Get pristine upstream sources for the exact versions you run
tools/fetch-pristine.zsh               # → build/pristine/ (gitignored)

# 2. Apply the patches to those trees
cd build/pristine/kernel && patch -p1 < ../../../src/patches/kernel-hid-nintendo-usb-passive-6.18.46.patch
cd ../bluez               && patch -p1 < ../../../src/patches/bluez-procon-cable-pairing-5.86.patch

# 3. Build BlueZ with the sixaxis plugin enabled (example, autotools)
autoreconf -fi && ./configure --enable-sixaxis --disable-obex --disable-mesh \
  --disable-midi --disable-nfc --disable-health --disable-test --disable-manual-pages
make -j"$(nproc)"
```

If autotools is not on `PATH`, the author builds in a shell with the toolchain
and BlueZ's dependencies:
`nix-shell -p autoconf automake libtool gettext glib dbus systemd.dev pkg-config readline`.

The kernel side is a source patch for `drivers/hid/hid-nintendo.c`: rebuild the
module or pass it through your distro's kernel-patch mechanism. The author
deploys both through a NixOS module (`kernelPatches` + a BlueZ package override)
kept outside this repo, since that is machine/OS plumbing rather than the
change itself.

Regenerating and verifying the patches after editing `src/` (the patches are the
artifact to submit; `src/` holds the same change as full files for review):

```sh
tools/make-patches.zsh                         # regenerate src/patches/ from src/
# pristine + patch must reproduce src/ byte-for-byte:
cp -a build/pristine /tmp/rt
patch -p1 -d /tmp/rt/bluez  < src/patches/bluez-procon-cable-pairing-5.86.patch
patch -p1 -d /tmp/rt/kernel < src/patches/kernel-hid-nintendo-usb-passive-6.18.46.patch
diff -r /tmp/rt/bluez src/bluez
diff /tmp/rt/kernel/drivers/hid/hid-nintendo.c src/kernel/drivers/hid/hid-nintendo.c
```

---

## Why each piece is needed

No unit below survives without evidence; that is the project's core rule.

### Kernel

| Unit | Why it exists |
|---|---|
| `joycon_is_passive()` | single USB-only predicate; keeps the Bluetooth path stock |
| delete `joycon_send_usb()` | the driver must never write to the USB hidraw (bluetoothd owns it; `0x80 04` is what pins the controller to USB) |
| `joycon_init()` early-out | skip the UART session/baud handshake on USB |
| `nintendo_hid_probe()` exit after hidraw | no input/LED/battery nodes for a transport that carries no input |
| `nintendo_hid_resume()` NULL-input guard | required *by* the above: there is no input device to flip to READ |

### BlueZ (all on the Pro Controller path only)

| Unit | Why it exists |
|---|---|
| `procon.h` (new) | USB IDs, report/subcommand constants, the 3-step API, HID SDP record |
| `procon.c` (new) | session init + framing + device-info + read-then-decide + 3-step |
| `plugins/sixaxis.c` | dispatch by USB id; on completion store the link key, mark Paired+Bonded, trust, and set the cable-pairing flag |
| `profiles/input/server.c` | gate inbound connections for a not-yet-bonded cable-pairing device; defer the SDP browse |
| `profiles/input/sixaxis.h` | `CABLE_PAIRING_PROCON` device kind |
| `src/adapter.{c,h}` | runtime link-key storage (`btd_adapter_store_link_key` + `reload_link_keys`); first-wake connectable re-assert |
| `Makefile.plugins` | build `procon.c` under the existing `--enable-sixaxis` switch |

### Tried, rejected, or demoted

The rebuild tested and dropped several units that earlier attempts carried. They
are recorded here so they are not re-added:

| Unit | Outcome |
|---|---|
| BT-side arm `0x08 00` after every connection | **dropped** — the controller wakes unarmed on this unit (x5000 already `0xFF`); the wired arm at dock is enough |
| Page-scan window == interval tuning | **dropped** — the default page-scan duty hears the wake page |
| Always re-pair on dock | **replaced** by read-then-decide (the stored record already says whether it is paired to us) |
| Connectable / discoverable keep-alive patch | **not needed** — BlueZ page-scans after a disconnect on its own; the only connectable call kept is the first-wake re-assert at dock |
| `adapter_start` accept-list re-add | **demoted to hardening** — it keeps the `disconnected_accept_list_entries` page-scan path populated, but measured traces show it is *not* the reconnect fix (during a failure, page scan was on and zero Connect Requests reached HCI) |
| Host-name alias `Nintendo*` | **kept out** — no primary-source support, and it is a workaround for a different symptom (rumble-induced disconnects) |
| `bluetooth.disable_ertm=1` + `UserspaceHID=true` | **not a fix** — addresses a different failure (connect-then-terminate) |

---

## Known limitations and review concerns

- **`procon.c` blocks bluetoothd's mainloop** with synchronous select/read (up
  to ~2 s per read, several per dock). Fine today, but it stalls the whole
  daemon during a dock — the first thing to revisit if another device misbehaves
  while a controller is being paired.
- **The `DISCOVERABLE` handling is global.** Turning discoverable off no longer
  clears connectable while *any* cable-paired device exists (Sony controllers
  included), not just the Pro Controller.
- **Page scan stays on indefinitely** once a cable-paired device has been
  plugged in (`btd_adapter_set_connectable(adapter, true)`, with nothing
  reverting it). A dynamic "only while a controller is present" toggle is the
  obvious refinement; the reachability/battery trade-off was accepted for now.
- **~20 s "zombie" window.** After the controller sleeps, the host holds the
  idle ACL for the ~20 s link-supervision timeout and page scan may be off; wait
  it out, then one press connects. The stock timeout is deliberately kept.
- **Authorization before the radio link.** Cable pairing trusts/authorizes the
  device while it is still on USB, before the BT link exists, so bluetoothd's
  authorization gate can fire for a not-yet-connected device. Observed once; it
  did not block pairing.
- `jc_type_is_chrggrip` becomes unused after the kernel change (harmless).

---

## How it was tested

Every unit above earned its place from a measurement. The method:

- Change **one variable** at a time, pin the other side, and capture with `btmon`.
- Use **normal button presses, never long presses** (see the wake note above).
- Wake is controller-initiated: the host reconnects only if it is page-scanning
  when the controller presses. Host-initiated paging never works.

Acceptance for the full product:

1. ≥10 consecutive sleep → normal-button wake → input cycles with no manual
   recovery.
2. While docked, `/proc/bus/input/devices` shows only `bus=0x0005` (BT) Pro
   Controller entries — no `bus=0x0003` (USB) instance.
3. While docked, a button press connects over BT.
4. ≥10 min soak with no `timeout waiting` in the kernel log; a clean link shows
   report deltas of ≈ 7–17 ms.

---

## Scope and anti-goals

- **Targets the Pro Controller only.** Joy-Cons, charging grips and third-party
  clones are out of scope until the Pro Controller path is solid.
- **Not a general "Switch controller support" project.** The scope is cable
  pairing + reliable wireless reconnect.
- **Portability is a non-goal for now.** The reference target is the author's
  machine (NixOS 26.05 / kernel 6.18.46 / BlueZ 5.86). The code is written to be
  portable (kernel + BlueZ upstream only, no OS-specific coupling) but is only
  proven there.
- **The Bluetooth host-name/alias workaround stays out.** It has no
  primary-source support (community folklore only); it is a workaround for a
  *different* symptom (rumble-induced disconnects on some hosts), not part of
  this project's scope.

### A note on an unrelated hardware quirk

The author's Intel Wireless-AC 9260 occasionally stops delivering pages from an
already-paired controller until the host scan register is re-armed. It is a radio
firmware issue, **not** caused by these patches and not fixable by them. It is
documented separately in
[`docs/hardware/intel-9260-reconnect-wedge.md`](docs/hardware/intel-9260-reconnect-wedge.md)
so it does not distract from the feature above.

---

## Credits and references

- **Reverse engineering:** dekuNukem,
  [Nintendo_Switch_Reverse_Engineering](https://github.com/dekuNukem/Nintendo_Switch_Reverse_Engineering)
  — `bluetooth_hid_subcommands_notes.md`, `spi_flash_notes.md`,
  `USB-HID-Notes.md`.
- **Pairing capture:** [nxbt](https://github.com/Brikwerk/nxbt) ("Example
  Pairing Session").
- **Upstream:** the kernel `hid-nintendo` driver (Daniel J. Ogorchock and
  contributors) and the BlueZ `sixaxis` input plugin.

Copyright (C) 2026 Tim Van Dyke <tim.vandyke123@gmail.com>.

Licensed under the GNU General Public License v2.0 or later
(GPL-2.0-or-later, matching both upstream projects) — see [`LICENSE`](LICENSE).
The patches under `src/patches/` carry this copyright and SPDX identifier in
their header; the new BlueZ files (`profiles/input/procon.{c,h}`) carry them as
SPDX file headers. The kernel file is upstream `hid-nintendo.c` and keeps its
upstream header.
