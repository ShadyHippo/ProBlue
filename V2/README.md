# ProBlue

Linux support for **Nintendo Switch Pro Controller cable pairing** — plug the
controller into a PC with its USB-C cable, let it pair over the wire exactly like
it does on a Switch, then unplug and use it as a normal Bluetooth controller.

This is a clean-room rebuild (V2) of an earlier two-patch attempt (the frozen
`V1/` archive). Every change here exists because a test showed the stock stack
needs it; the [necessity ledger](#why-each-piece-is-needed) below is the record.

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

The protocol is the interesting part; the full byte-level digest lives in
[`docs/KEY_CONTEXT.md`](docs/KEY_CONTEXT.md) §2. The short version:

- **Report framing.** All commands are HID OUTPUT report `0x01` (packet counter
  + zeroed rumble + subcommand id + payload). Replies are INPUT report `0x21`,
  whose ack byte sits at a *different offset over USB vs Bluetooth* because the
  HIDP header is stripped by bluetoothd before the hook sees it. Both offset
  conventions are correct in their own context.
- **UART session.** Two-byte writes `0x80 02` (handshake), `0x80 03` (3 Mbit),
  `0x80 02` again (re-handshake), answered by `0x81 <cmd>`. This opens the
  controller's internal command channel; subcommands need it.
- **Read-then-decide.** The controller stores its pairing record in SPI flash at
  `x2000` (magic `0x95`, host MAC big-endian, LTK little-endian, capability byte).
  Reading it tells us whether the controller is already paired to this host, which
  avoids blindly re-pairing on every dock.
- **The wired 3-step** (`0x01`): step 1 registers the host address, step 2 returns
  the stored LTK (XOR-`0xAA`, flash byte order — the BR/EDR key must be
  byte-reversed), step 3 commits.

The reconnect side is ordinary Bluetooth: once the key is in bluetoothd and the
device is bonded, the controller pages the host on a button press and the host
accepts with the stored key.

---

## Source layout

| Path | Contents |
|---|---|
| `src/kernel/drivers/hid/hid-nintendo.c` | full **patched** driver (vs pristine 6.18.46) |
| `src/bluez/…` | full **patched** BlueZ files (vs pristine 5.86) |
| `src/patches/` | generated patches — what you actually apply |
| `src/MANIFEST.md` | pinned nixpkgs rev, versions, store paths, sha256s |
| `docs/KEY_CONTEXT.md` | protocol digest, machine facts, V1 autopsy, review concerns |
| `PLAN.md` | stage method + necessity ledger |
| `SALVAGE.md` | provenance of everything reused from V1 |
| `docs/testplan/` | the controlled test per stage |
| `docs/results/` | committed evidence summaries (raw logs are gitignored) |
| `docs/hardware/` | secondary: an unrelated Intel-radio quirk + research notes |
| `tools/` | fetch pristine sources, regenerate patches, build helpers |

**Rule:** `src/` holds the reviewed full files and is the source of truth;
`src/patches/` is generated from it (`tools/make-patches.zsh`). One direction
only — never hand-edit a generated patch.

---

## Applying it

```sh
# 1. Get pristine upstream sources for the exact versions you run
V2/tools/fetch-pristine.zsh            # → V2/build/pristine/ (gitignored)

# 2. Apply the patches to those trees
cd build/pristine/kernel && patch -p1 < ../../../src/patches/kernel-hid-nintendo-usb-passive-6.18.46.patch
cd ../bluez               && patch -p1 < ../../../src/patches/bluez-procon-cable-pairing-5.86.patch

# 3. Build BlueZ with the sixaxis plugin enabled (example, autotools)
autoreconf -fi && ./configure --enable-sixaxis --disable-obex --disable-mesh \
  --disable-midi --disable-nfc --disable-health --disable-test --disable-manual-pages
make -j"$(nproc)"
```

The kernel side is a source patch for `drivers/hid/hid-nintendo.c`: rebuild the
module or pass it through your distro's kernel-patch mechanism. The author
deploys both through a NixOS module (`kernelPatches` + a BlueZ package override)
kept outside this repo, since that is machine/OS plumbing rather than the
change itself.

Regenerating the patches after editing `src/`:

```sh
V2/tools/make-patches.zsh
```

---

## Why each piece is needed

No unit below survives without evidence; this is the project's core rule.
Full rows and the exact falsification tests are in [`PLAN.md`](PLAN.md).

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
  primary-source support (community folklore only); `KEY_CONTEXT.md` §2.6/§4.

### A note on an unrelated hardware quirk

The author's Intel Wireless-AC 9260 occasionally stops delivering pages from an
already-paired controller until the host scan register is re-armed. It is a radio
firmware issue, **not** caused by these patches and not fixable by them. It is
documented separately in
[`docs/hardware/intel-9260-reconnect-wedge.md`](docs/hardware/intel-9260-reconnect-wedge.md)
so it does not distract from the feature above.
