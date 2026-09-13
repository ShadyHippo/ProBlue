# KEY_CONTEXT — protocol and reference digest

The facts the code is built on. Read this with [`../README.md`](../README.md)
(what/why) and `../src/` (the code). Protocol details here are grounded in the
reverse-engineering references and verified on the target machine on 2026-09-10;
re-verify against those references before changing protocol code.

---

## 1. Target and machine facts

- Reference target: NixOS 26.05, kernel **6.18.46**, BlueZ **5.86** (nixpkgs pin
  in [`../src/MANIFEST.md`](../src/MANIFEST.md)).
- Build BlueZ without autotools on PATH:
  ```bash
  nix-shell -p autoconf automake libtool gettext glib dbus systemd.dev pkg-config readline \
    --run 'cd <bluez-src> && autoreconf -fi && ./configure --enable-sixaxis \
      --disable-obex --disable-mesh --disable-midi --disable-nfc --disable-health \
      --disable-test --disable-btpclient --disable-manpages && make -j"$(nproc)"'
  ```
  This built a clean bluetoothd 5.86 with the ported tree (zero warnings).
- NixOS kernel changes are a full kernel rebuild — the scarcest resource here.
- The sixaxis plugin is what handles USB cable pairing; BlueZ must be built with
  it enabled (`--enable-sixaxis`; nixpkgs enables it by default).

---

## 2. Protocol digest

### 2.1 Report framing (byte-exact; matches V1's code)

OUTPUT report `0x01` (host → controller; every subcommand; same over USB hidraw
and BT PSM 19):
```
[0]=0x01 report id, [1]=packet counter (low nibble, 0x0-0xF),
[2..9]=rumble data (zero), [10]=subcmd id, [11..]=subcmd data
```
INPUT report `0x21` (controller → host; subcommand reply):
```
[13]=ack (MSB=1 ack, MSB=0 NACK; 0x80 = simple ack, 0x00 = NACK),
[14]=subcmd id echo, [15..49]=reply data
```
**The BT/USB offset shift**: over Bluetooth the HIDP header precedes the report
and `hidp_recv_intr_data()` hands the hook the report *without* it, so the ack is
at `data[14]`/subcmd at `data[15]` (with `data[1]==0x21`). Over USB hidraw there
is no header: ack at `buf[13]`/subcmd at `buf[14]` (with `buf[0]==0x21`). Both are
correct in their own context; do not "fix" one to match the other.

### 2.2 USB session commands (2-byte writes, answered by `0x81 <cmd>`)

| cmd | meaning |
|---|---|
| `80 02` | UART handshake with the controller's Broadcom BT chip. Once per session. |
| `80 03` | switch to 3 Mbit; must follow `80 02`, and another `80 02` is then required for the baud switch to take effect. |
| `80 04` | **pin to USB**: force USB-only, no timeout. Sent by the stock kernel driver. This is what stops the controller reverting to Bluetooth while docked — the reason ProBlue makes the driver passive instead of sending `80 04`. |
| `80 05` | restore the timeout so the controller reverts to BT. Unnecessary under passivity: default behaviour without `80 04` is already timeout-and-revert. |
| `80 01` | status query → `81 01 00 <type> <MAC>` (type 2 = right Joy-Con, 3 = Pro Controller). |

### 2.3 Subcommands used by this project

| subcmd | purpose | notes |
|---|---|---|
| `0x01` | manual pairing (3-step) | also works over BT; used to rewrite pairing info |
| `0x02` | device info | reply `[0..1]` fw, `[2]` type (1=L, 2=R, **3=Pro**), `[4..9]` MAC big-endian (raw `buf[19..24]`) |
| `0x03` | set input report mode | `0x30` standard full (120 Hz Pro), `0x3F` simple HID |
| `0x05` | get page list state | `0x01` if a host pairing record is in memory — the quick "already paired?" probe |
| `0x06` | set HCI state | `x00` sleep/page-scan, `x01`/`x04` reboot-and-reconnect, `x02` reboot to pair mode |
| `0x07` | reset pairing info | zeroes the x2000 section |
| `0x08` | set shipment low-power state | see 2.5 |
| `0x10` | SPI flash read | LE address + length (max 0x1D); reply `90 xx` ack + data |
| `0x11`/`0x12` | SPI write / sector erase | write-protection status byte available |

### 2.4 SPI flash layout (the parts that matter)

**x2000 pairing record** (repeated sections; magic marks the current one):
```
x2000      magic: 0x95 = used, 0x00 = unused (then the next section is current)
x2001      size (0x22)
x2004-09   host BT address, BIG-ENDIAN
x200A-19   128-bit LTK, LITTLE-ENDIAN
x2024      host capability: Switch = 0x68, PC = 0x08
x2026..    next section (same layout; 0xFF when unused)
```
**x5000 shipment**: `0x01` = shipment on, `0xFF` = normal (factory = `0x01`).
**x6000**: factory calibration/config (not pairing). Reset pairing: subcmd `0x07`.
Reads exist (`0x05`, `0x10`) — V1's "no read stored central" claim was false.

### 2.5 Power / wake model

- `0x08 0x01` writes `0x01` at x5000: disables Triggered Broadcom Fast Connect,
  LPM→HID OFF, button wake disabled (a long press can still wake a paired
  controller — a test-design trap).
- `0x08 0x00` resets x5000 to `0xFF`: enables Triggered Fast Connect
  (scan-on-button-press), LPM→SLEEP. The Switch sends this after every
  connection, over the BT interrupt channel.
- On this unit x5000 read `0xFF` from the first read and stayed `0xFF` through
  fresh OTA pairs, disconnects and SYNC-click sleeps; `0x08 00` was a no-op.
  **The arm is not the reconnect mechanism here** (ledger R3, deleted).
- Wake is **controller-initiated only**: a normal button press makes the
  controller emit an HCI `Connect Request`; the host reconnects iff it is
  page-scanning. BlueZ enables page scan after a disconnect on its own;
  discoverable is not required. Host-initiated paging into a sleeping controller
  page-timeouts — the controller never page-scans.

### 2.6 Pairing model

- First pairing is ordinary OTA: sync button → pair mode (`0x06 0x02`) → standard
  SSP → sockets. The controller does SSP fine (V1's "it doesn't SSP" is false); it
  simply reuses the stored key on reconnect, which is why the host must hold the
  key/accept-list state.
- The `0x01` 3-step rewrites pairing info (and is how docking works):
  1. `0x01 0x01 [+6 B host MAC LE]` → echoes type; Joy-Con MAC in reply
  2. `0x01 0x02` → returns the **stored** LTK (each byte XOR `0xAA`, flash LE
     order; the BR/EDR key is byte-reversed). "Acquire", not "generate" — so
     re-pairing every dock (V1 P8) is unnecessary.
  3. `0x01 0x03` → commit
- **Read-then-decide is therefore possible**: `0x05` + SPI read of x2000 — if
  magic `0x95` and the stored host MAC is ours, reuse the record and skip the
  3-step. This is what the port implements.

---

## 3. V1 autopsy — what to trust

Trust (verified against pristine sources or by build):

- `V1/src/bluez/profiles/input/procon.c` framing/offsets are byte-exact per §2.
- The BT-side arm design (delayed T+1s, retry ×3) is Switch-parity, but unneeded
  here (§2.5).
- `btd_adapter_store_link_key` + a full `MGMT_OP_LOAD_LINK_KEYS` reload is the
  only runtime key-add path in this era of BlueZ.
- The passive-USB kernel shape (early return in `joycon_init`, no USB writes,
  probe exit before input/leds/battery, resume NULL guard).

Do **not** trust:

- `V1/README.md` and V1's PLAN prose — AI-generated against the Ubuntu build,
  disavowed; only `V1/docs/results/` + the RE references count.
- `V1/patches/bluez-5.84-procon.patch` — a stale snapshot; the `V1/src/bluez/`
  tree is the superset. (`device_is_cable_pairing()` / `device_set_cable_pairing()`
  are stock `src/device.c` functions, not project additions.)
- "No read stored central" (false: `0x05`/`0x10`).
- "Controller doesn't SSP" (false, §2.6).
- "Re-pair every dock required" (false, §2.4/§2.6).
- The kernel patch as shipped targets Ubuntu 6.8.0-137; on 6.18.46 its
  `joycon_init` hunk fails because upstream made the 3-Mbit baudrate failure
  non-fatal, and `joycon_hid_resume` was renamed `nintendo_hid_resume`.

---

## 4. Review concerns (do not regress)

- `procon.c` does **synchronous blocking select/read in bluetoothd's mainloop**
  (up to ~2 s per read, several per dock). Fine for now, but it blocks the whole
  daemon during a dock — revisit if another device misbehaves while pairing.
- The `property_set_mode()` DISCOVERABLE change affects **all** cable-paired
  devices (Sony included), not just Pro Controllers.
- `jc_type_is_chrggrip` becomes unused after the kernel hunk (harmless `#define`).
- Session re-init (`0x80 02` is "once per session"): the port runs the V1-proven
  sequence; verify empirically if a second init after a quiet gap is used.
- Report cadence health: deltas ≈ 7–17 ms clean, 30–120 ms sniff-ish. Measure
  with numbers, not feel.
- The Bluetooth-name/"Nintendo" workaround has no primary-source support and stays
  out of the code. Capability byte x2024 (`0x68` Switch vs `0x08` PC) is the
  alternative mechanism to probe if a power/sniff difference ever reproduces.

---

## 5. Workflow

- `src/kernel/` and `src/bluez/` are the source of truth (full patched files);
  `src/patches/` is generated from them by `tools/make-patches.zsh`. Never
  hand-edit a patch; regenerate instead.
- Pristine upstream is not committed: `tools/fetch-pristine.zsh` materializes it
  under `build/pristine/` (gitignored).
- Verify after edits: apply each patch to pristine with `patch -p1`, then confirm
  the result is byte-identical to `src/` (see `src/README.md`).
- Kernel anchor check: `joycon_is_passive` must appear exactly 4× (definition,
  `joycon_init`, `nintendo_hid_probe`, `nintendo_hid_resume`) and no
  `joycon_send_usb` references may remain.

## 6. Known hardware interaction

An unrelated Intel 9260 radio-firmware wedge can stop reconnects and therefore
false-fail ProBlue's acceptance tests. Single document:
[`hardware/intel-9260-reconnect-wedge.md`](hardware/intel-9260-reconnect-wedge.md).
