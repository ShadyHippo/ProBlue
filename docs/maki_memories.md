# ProBlue — Evidence Log & Protocol Reference

> **Truth policy (2026-08-24):** status claims live in `PLAN.md` and must carry
> a [PROVEN]/[ASSUMED]/[DEAD] tag. This file keeps the raw debugging evidence
> trail (mostly `btmon` captures from 2026-08-13) and the protocol reference.
> Where an earlier section contradicts PLAN.md, PLAN.md wins. The old title
> "Session Memories" and the old "IT WORKS" claims described **pairing only**,
> never input — see §1 correction.

---

## 1. Status correction (2026-08-24)

The original §1 claimed "IT WORKS" (2026-08-13). What was actually verified
that day: dock pairing + button-press BT reconnection. **Input was not part of
the verification.** Journal forensics across all eras (see PLAN.md) show input
over BT has never been clean or reliable on the test machine — including under
a pure stock GUI pairing before any project code existed. Do not cite this
file's earlier claims as proof of a working controller.

## 2. What the BlueZ patch is

A BlueZ 5.84 patch implementing the Switch Pro Controller's **wired
cable-pairing** inside the Bluetooth stack: plugging the controller into USB
*is* the pairing — no GUI, no agent, no over-the-air SSP (the controller does
not SSP on connect; it only connects to the host whose MAC + link key are
stored in its SPI flash, written over USB).

| File (relative to bluez-5.84) | What it does |
|---|---|
| `profiles/input/procon.c` / `.h` (new) | Wired protocol: USB session init (`0x80 02/03/02`), device-info probe (`subcmd 0x02`), arm (`0x08 00`), 3-step pairing (`subcmd 0x01`: x01 host-MAC → x02 GET_LTK (XOR-0xAA, byte-reversed) → x03 save). Nintendo device table + hardcoded HID SDP record (captured from a genuine GUI pairing — kills the malformed-SDP-seed problem). |
| `plugins/sixaxis.c` | Wires `CABLE_PAIRING_PROCON` into the existing PS3/PS4 cable-pairing machinery: on plug-in run session init + probe + arm; PROCON exempt from the "already known, skipping" early-return when known-but-disconnected (re-pairs every dock, like the Switch); cable pairing = authorization → trusted, no agent prompt; keeps the host connectable. |
| `profiles/input/device.c` | Per-connection **serialized setup** over the BT interrupt channel: `0x02 probe → 0x08 00 arm → 0x03 30 report-mode full → 0x30 01 player-1 LED`, each waiting for its `0x21` ack (500 ms cap). Runs on EVERY connection. Sole owner of BT-side subcommands (PLAN.md single-owner rule). |
| `profiles/input/server.c` | `dev_is_cable_pairing()` (renamed from `dev_is_sixaxis`) covers Nintendo too; cable-paired devices skip bonded/encryption checks and get the SDP-browse deferral on inbound connect. |
| `profiles/input/sixaxis.h` | Adds `CABLE_PAIRING_PROCON` to the shared enum. |
| `src/adapter.c` / `.h` | `btd_adapter_store_link_key()` (persist wired LTK + reload kernel keys via `MGMT_OP_LOAD_LINK_KEYS` — the only runtime key-add path in 5.84), `btd_adapter_set_connectable()`, keep page-scan on while cable-paired devices exist, re-add accept-list entries on power-on. |
| `Makefile.plugins` | Builds `procon.c` + `procon.h` with the sixaxis plugin. |

## 3. Kernel side (corrected 2026-08-24)

This file previously claimed "the kernel patch is NOT required" and "the
machine has never had a patched/forked hid-nintendo kernel installed." Both
statements were wrong:

- `/lib/modules/<kver>/updates/hid-nintendo.ko` WAS built from this project's
  experimental fork and installed 2026-08-16 16:20.
- Stock `hid-nintendo` on USB sends `0x80 04` (pin-to-USB) and config
  subcommands: BT radio idles while docked, and the driver's init collides
  with the plugin over the same hidraw (journal-proven 2026-08-14 23:43).
- **Decision (PLAN.md): two patches are required.** Kernel = passive-USB-only;
  BT side stays stock upstream. The BlueZ queue owns all deliberate BT
  subcommands.

## 4. Environment

- Canonical repo: `~/Programming/slop/ProBlue`. The `slop/joycond` repo is the
  archived research workspace (protocol docs, dev tools, dead ends); its
  `driver/` trees are superseded.
- Live machine state (verified 2026-08-24): patched bluetoothd installed at
  `/usr/local/libexec/bluetooth/bluetoothd` via systemd drop-in
  (`/etc/systemd/system/bluetooth.service.d/ProBlue.conf`), running since boot
  2026-08-21; PageScan tuning live in `/etc/bluetooth/main.conf`
  (`PageScanType=0x01`, interval/window `0x0012`, integer form required);
  adapter alias already `Nintendo Switch`; stock joycond daemon still enabled
  (should be disabled); experimental fork module still in `updates/` (should
  be removed per PLAN.md step 1/3).
- Controller BT MAC `20:0B:CF:34:F1:BD`; host BT MAC `50:28:4A:0D:54:7A`.
  Ubuntu 24.04, kernel 6.8.0-137-generic, BlueZ 5.84.
- No git commits without explicit permission.

## 5. Protocol reference (CORRECT — trusted, do not edit without RE sources)

### Output report (subcommand send)
```c
struct joycon_subcmd_request {
    u8 output_id;       // [0] must be 0x01 for subcommand
    u8 packet_num;      // [1] incremented every send
    u8 rumble_data[8];  // [2-9]
    u8 subcmd_id;       // [10]
    u8 data[];          // [11+]
};
```

### Input report (subcommand reply)
```c
struct joycon_input_report {
    u8 id;              // [0] 0x21 = subcmd reply
    u8 timer;           // [1]
    u8 bat_con;         // [2]
    u8 button_status[3];// [3-5]
    u8 left_stick[3];   // [6-8]
    u8 right_stick[3];  // [9-11]
    u8 vibrator_report; // [12]
    struct joycon_subcmd_reply {
        u8 ack;         // [13] 0x80 = ACK, 0x00 = NACK
        u8 id;          // [14] subcmd echo
        u8 data[];      // [15+] subcmd-specific
    };
};
```
Over the BT interrupt channel the raw report is `[HIDP hdr 0xa1, 0x21, timer,
bat, buttons(3), L-stick(3), R-stick(3), vibrator, ack, subcmd-echo, data...]`
— i.e. ack is raw byte 14, subcmd echo raw byte 15 (what
`hidp_recv_intr_data()` sees as `data[14]` / `data[15]`).

### Subcommand 0x01 — manual BT pairing (the wired 3-step)
- **Step 1** (`0x01 0x01` + host MAC LE): reply echoes `0x01`, controller MAC
  at `[16:22]`.
- **Step 2** (`0x01 0x02`): reply echoes `0x02`, 16-byte LTK XORed with `0xAA`,
  in flash (little-endian) order — **byte-reverse** before use as a BR/EDR
  link key (loading un-reversed fails auth with HCI error 0x05).
- **Step 3** (`0x01 0x03`): commit to the controller's SPI flash; simple ACK.
- Historical Python-era bugs (fixed long ago): extra `0x01` byte in step-1
  packet; wrong MAC offset `[3:9]` → `[16:22]`; single `read()` consuming
  0x30 input reports instead of waiting for the 0x21 reply.

### Other key subcommands
| ID | Name | Data |
|---|---|---|
| `0x02` | Request device info | none; MAC at reply `[19:25]` (big-endian → baswap) |
| `0x03` | Set report mode | `0x30` standard full, `0x31` NFC, `0x3F` simple |
| `0x06` | Set HCI state | `0x00` disconnect/sleep, `0x01` reconnect, `0x02` pairing, `0x04` home |
| `0x08` | Set shipment state | `0x00` clear shipment (the arm) |
| `0x30` | Set player LEDs | `(flash << 4) \| on`; `0x01` = player 1 |
| `0x40` / `0x48` | Enable IMU / vibration | `0x01` |

### Input report IDs
`0x21` subcmd reply · `0x30` standard full input (49 B, 60–120 Hz) ·
`0x3F` simple input (12 B) · `0x81` USB command response.

### USB command IDs (via output report 0x80)
`0x01` connection status · `0x02` handshake (start UART session) ·
`0x03` switch to 3 Mbit · `0x04` no-timeout (pin to USB; why stock driver
kills the BT radio while docked) · `0x05` enable timeout.

### Sources
- Kernel driver: `drivers/hid/hid-nintendo.c` (the real protocol source of truth).
- dekuNukem RE: https://github.com/dekuNukem/Nintendo_Switch_Reverse_Engineering
- nxbt pairing session example: https://github.com/Brikwerk/nxbt/blob/master/docs/Example%20Pairing%20Session.md

## 6. Reconnect root cause & the serialized-setup fix [PROVEN — pairing level]

> **Update 2026-08-24:** the 4-step queue described below is SUPERSEDED —
> probe/report-mode/LED steps belong to stock hid-nintendo's own init; BlueZ
> now sends ONLY the arm (`0x08 00`), delayed to T+1s with retries (PLAN.md
> Architecture). The watchdog analysis and ack-wait discipline below remain
> valid history and motivated the original design.

### The controller's connect/drop cycle
`btmon` 2026-08-13 (run 2): the controller cycles **connect → ~2.2 s → drop
(0x08 link timeout) → re-page**, continuously. In that run 8/9 sessions never
opened **PSM 19** (the interrupt channel) → the host sent nothing → the
controller's "host dead?" timer fired. The 1 session that opened PSM 19 and
received host subcommands **survived**. Host traffic keeps the link alive;
silence kills it at ~2.2 s.

### Why fire-and-forget failed
Sending `[0x08 00, 0x03 30, 0x30 01]` back-to-back in <1 ms: the controller
processes subcommands slowly (~0.31 s per ack) and **dropped everything but
the first**: only `0x08` was ever acked (`a1 21 ... 80 08`); `0x03`/`0x30`
never were, and input stayed `0x3F` simple.

### The fix: serialize, wait for each ack
`profiles/input/device.c` runs the setup queue (§2 table). Ack matching hooks
`hidp_recv_intr_data()`. Queue state lives in `struct input_device`
(`procon_setup_pos/source/last`), cleaned up in `input_device_free()`,
re-entrant across reconnects. Reconnect itself (page → accept → encrypt →
PSM 17/19) verified working by the user, 2026-08-13.

## 7. Earlier (superseded) steps in the trail

1. **`0x03 30` sent pre-connect in a batch (v1)** — superseded by the
   serialized queue (§6).
2. **"Controller ignores output reports over uhid"** — wrong; the `0x08` arm
   demonstrably gets acked over BT. Failures were the fire-and-forget drop +
   simple mode. **Superseded.**
3. **"LEDs need the kernel LED class path"** — was concluded while the mode
   handshake was missing; over uhid there IS no kernel LED class, but the
   queue's `0x30 01` sets player LEDs directly. Final LED behavior still
   unvalidated (input layer blocks end-to-end testing). **Open.**

## 8. Open items

- Input-layer reliability: see PLAN.md (control test first; factory-cal
  storms, stalls, -110 deaths documented there).
- LEDs: confirm `0x30 01` acks in full mode once input layer works.
- Multi-controller support; dynamic page-scan (battery); optional 2–3 s link
  supervision timeout (snappier reconnect after sleep) — all deferred until
  input works.

## 9. Dead-code note (pro_blue)

The old `pro_blue` tree (Python `pair.py` MVP, `deploy/`, `tests/`) was an
abandoned approach that never worked reliably (joycond interference, missing
BlueZ device object, systemd path-activation complexity). Deleted 2026-08-13.
Its one reusable trick: cycling USB plug/unplug via
`/sys/bus/usb/devices/<dev>/port/disable` (write `1` = unplug, `0` = replug)
for fast iteration — recreate here if needed.
