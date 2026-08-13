# ProBlue — Session Memories

> **Origin.** These notes are the project's working memory, previously stored in
> `pro_blue/.maki/` under the dead `pro_blue` tree (the original Python MVP:
> `pair.py` + systemd + udev, which never worked reliably and is now deleted).
> Relocated here 2026-08-13 so nothing useful is lost with that tree.
>
> They are the raw debugging evidence trail (mostly from `btmon` captures on
> 2026-08-13) plus the current state. Where a later finding superseded an
> earlier conclusion, the later note is marked **VERIFIED** and the earlier one
> **SUPERSEDED** — trust the newest section.

---

## 1. Current status (2026-08-13) — IT WORKS

- **Stack:** stock Ubuntu kernel `6.8.0-137-generic` + patched BlueZ 5.84
  (this repo's `patches/bluez-5.84-procon.patch`). **No kernel patch.**
- **Flow:** dock (wired 3-step pairing, link key registered) → unplug →
  button press → BT connect + input, reliably.
- The change that made it reliable: the **serialized post-connect subcommand
  setup** (section 6). Before that, the controller cycled
  connect → ~2.2s → drop → re-page and never left simple report mode.

## 2. What the BlueZ patch is (summary)

A BlueZ 5.84 patch that implements the Switch Pro Controller's **wired
cable-pairing** inside the Bluetooth stack, so plugging the controller into USB
*is* the pairing — no GUI, no agent, no over-the-air SSP (the controller does
not SSP on connect; it only connects to the host whose MAC + link key are
stored in its SPI flash, written over USB).

| File (relative to bluez-5.84) | What it does |
|---|---|
| `profiles/input/procon.c` / `.h` (new) | The wired protocol: USB session init (`0x80 02/03/02`), device-info probe (`subcmd 0x02`), arm (`0x08 00`), and the 3-step pairing (`subcmd 0x01` x01 host-MAC → x02 GET_LTK (XOR-0xAA, byte-reversed) → x03 save). Plus the Nintendo device table and the hardcoded HID SDP record (captured from a genuine GUI pairing — kills the malformed-SDP-seed problem). |
| `plugins/sixaxis.c` | Wires `CABLE_PAIRING_PROCON` into the existing PS3/PS4 cable-pairing machinery: on plug-in run session init + probe + arm; PROCON is exempt from the "already known, skipping" early-return when known-but-disconnected (re-pairs every dock, like the Switch); cable pairing = authorization → trusted, no agent prompt; keeps the host connectable. |
| `profiles/input/device.c` | The per-connection **serialized setup** over the BT interrupt channel: `0x02` probe → `0x08 00` arm → `0x03 30` report-mode full → `0x30 01` player-1 LED, each waiting for its `0x21` ack. This is the reconnect-keeper: it arms the controller (clears shipment, LPM-to-sleep) and moves it out of simple/search mode on **every** connection. |
| `profiles/input/server.c` | `dev_is_cable_pairing()` (renamed from `dev_is_sixaxis`) now covers Nintendo too; cable-paired devices skip bonded/encryption checks and get the SDP-browse deferral on inbound connect. |
| `profiles/input/sixaxis.h` | Adds `CABLE_PAIRING_PROCON` to the shared enum. |
| `src/adapter.c` / `.h` | `btd_adapter_store_link_key()` (persist the wired LTK + reload kernel keys via `MGMT_OP_LOAD_LINK_KEYS` — the only runtime key-add path in 5.84), `btd_adapter_set_connectable()`, keep page-scan on while cable-paired devices exist (discoverable-off no longer clears connectable), re-add accept-list entries on power-on. |
| `Makefile.plugins` | Builds `procon.c` + `procon.h` with the sixaxis plugin. |

## 3. The kernel patch is NOT required

**State:** the machine has **never** had the patched/forked `hid-nintendo`
kernel installed. Every observation in the evidence trail below — wired
pairing, BT reconnect, input streaming, LED behavior — was made on the **stock
Ubuntu 6.8.0-137** module. `hid-nintendo.ko` is the stock one; `/usr/src` has
only stock headers; no custom bzImage/module anywhere.

**Why it was thought to be needed, and why it isn't:**

- The original concern: the stock driver's USB init sends `0x80 04` (USB-only
  lock) and config subcommands, which could knock the controller out of pair
  mode or fight our wired subcommands.
- Reality: the BlueZ patch drives the entire wired flow itself over the
  plugin's own hidraw fd — `procon_usb_session_init()` re-establishes the USB
  session (`0x80 02/03/02`) and every subcommand runs over that fd, which the
  stock driver also creates (`HID_CONNECT_HIDRAW`). The stock driver's USB
  init is runtime-only: it writes no SPI pairing records and is not re-run on
  radio events, so it does not interfere. Verified end-to-end on the stock
  kernel (section 1).
- The fork (`patches/hid-nintendo-keep-bt-radio.patch`, `src/kernel/`) is kept
  only as historical reference. Do not install it. **ProBlue is BlueZ-only.**

## 4. Direction & environment constraints

- **Canonical repo:** `~/Programming/slop/ProBlue`. The `slop/joycond` bluez
  tree is **deprecated dev work — do not use it.**
- The patch must stay **slim and upstreamable**, BlueZ-only, no fallback paths
  in the code — one setup sequence, working first time.
- **Live setup:** patched `bluetoothd` built at `~/ProBlue-build/bluez-5.84`
  (from this repo's `src/bluez/`), run manually as root:
  ```bash
  sudo ~/ProBlue-build/bluez-5.84/src/bluetoothd \
       -n -f ~/ProBlue-build/bluez-5.84/src/main.conf \
       -d 'plugins/*:profiles/input/*' 2>&1 | grep -v 'Endpoint registered: sender='
  ```
  - systemd `bluetooth.service` is **masked**; the manual run dies with the
    terminal (proper install = systemd drop-in per README §7.2 B5).
  - `-f` points at the **tree's** `main.conf` (has `PageScanType=0x01`,
    `PageScanInterval/Window=0x0012` — the ~100% page-scan duty that catches
    the controller's brief wake-page), *not* `/etc/bluetooth/main.conf`.
- `joycond.service` runs as root (player-slot daemon; it uses the kernel LED
  class, which does not exist over uhid — see section 8).
- Controller BT MAC `20:0B:CF:34:F1:BD`; host BT MAC `50:28:4A:0D:54:7A`.
- Environment: Ubuntu 24.04, Python 3.12+, BlueZ 5.84, systemd 255+.
  `hidraw` perms are `MODE="0600"` (joycond udev rules) — service runs as root.
- No git commits without explicit permission.

## 5. Protocol reference

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
- Bug history (Python MVP era): extra `0x01` byte in step-1 packet; wrong MAC
  offset `[3:9]` → `[16:22]`; single `read()` consuming 0x30 input reports
  instead of waiting for the 0x21 reply.

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
`0x03` switch to 3 Mbit · `0x04` no-timeout (keep on USB) · `0x05` enable timeout.

### Sources
- Kernel driver: `drivers/hid/hid-nintendo.c` (the real protocol source of truth).
- dekuNukem RE: https://github.com/dekuNukem/Nintendo_Switch_Reverse_Engineering
- nxbt pairing session example: https://github.com/Brikwerk/nxbt/blob/master/docs/Example%20Pairing%20Session.md

## 6. Reconnect root cause & the fix that works (VERIFIED)

### The controller's connect/drop cycle
`btmon` 2026-08-13 (run 2): the controller cycles **connect → ~2.2 s → drop
(0x08 link timeout) → re-page**, continuously. In that run 8/9 sessions never
opened **PSM 19** (the interrupt channel) → the host sent nothing → the
controller's "host dead?" timer fired. The 1 session that opened PSM 19 and
received host subcommands **survived** (still streaming at capture end). Host
traffic keeps the link alive; silence kills it at ~2.2 s.

### Why the setup never completed (the real bug)
We sent `[0x08 00, 0x03 30, 0x30 01]` **back-to-back in <1 ms** (fire-and-forget).
The controller processes subcommands slowly (its ack arrived ~0.31 s later) and
**dropped everything but the first**: only `0x08` was ever acked
(`a1 21 ... 80 08`); `0x03`/`0x30` never were, and input stayed `0x3F` simple.
So the controller never left its search state.

### The fix: serialize, wait for each ack
`profiles/input/device.c` now runs a **setup queue** over PSM 19 on every
connection — the same discipline as the kernel driver's
`joycon_hid_send_sync()`:

```
0x02 probe → 0x08 00 arm → 0x03 30 report-mode full → 0x30 01 player-1 LED
```
each sent only after the previous subcommand's `0x21` ack (500 ms cap,
timeout continues the queue). Ack matching hooks `hidp_recv_intr_data()`
(raw `data[1] == 0x21`, ack `data[14]`, echo `data[15]`). Queue state lives in
`struct input_device` (`procon_setup_pos/source/last`), cleaned up in
`input_device_free()`, re-entrant across reconnects. The old fire-and-forget
`procon_arm()` / `procon_set_report_mode_full()` / `procon_set_player_led()`
and their wrappers were **deleted** — one sequence, no fallbacks.

**Verified working by the user, 2026-08-13.**

## 7. Earlier (superseded) steps in the trail

1. **`0x03 30` added to the batch (v1)** — sent `0x03 30` pre-connect next to
   the arm, kept LED post-connect. btmon showed the command going out
   (`a2 01 01 ... 03 30`) but the controller stayed in `0x3F` mode and never
   acked `0x03`/`0x30` — because the batch was fire-and-forget. **Superseded
   by the serialized queue (section 6).**
2. **"Controller ignores output reports over uhid" (LED evidence)** — wrong:
   the `0x08` arm demonstrably gets acked over BT. The LED/subcommand
   failures were the fire-and-forget drop + simple mode, not a refusal to
   process output reports. **Superseded.**
3. **"LEDs need the kernel path (uhid → kernel HIDP → LED sysfs)"** — this was
   the conclusion while the mode handshake was missing. With the serialized
   setup, the LED subcmd should now be acked in full mode; re-evaluate before
   believing any kernel involvement is needed for LEDs. **Superseded pending
   re-test.**

## 8. LED evidence (historical)

`btmon` 2026-08-13, three connect cycles with identical host writes
(`0x08 00` + `0x30 01` each connect): LED outcomes (1) none, (2) player 1,
(3) strobe — not tracking our writes; `0x30` never acked. Interpreted at the
time as "the controller is in its internal search state and ignores LED
writes". Re-interpretation after the serialized-setup fix (section 6): the
writes were being **dropped**, and the controller was stuck in simple/search
mode because the setup never completed. **Re-test LEDs with the current
build.** Note: over uhid there is no kernel LED class, so `joycond` cannot set
player LEDs via sysfs on this path; deterministic multi-controller LEDs may
still need a kernel-LED route, which is out of scope for a BlueZ-only patch.

## 9. Open items

- **PSM 19 stall:** sessions where the controller never opens PSM 19 still die
  at ~2.2 s (it opens the control channel, then waits ~2 s for host life
  before committing). If cycling reappears even with clean acks, the next
  lever is responding on **PSM 17** (control channel) the moment it opens.
- **LEDs:** confirm `0x30 01` now acks in full mode and the player-1 LED is
  steady.
- **Multi-controller** and **2–3 s link-supervision timeout** (snappier
  reconnect) remain as documented follow-ups in the README.

## 10. Dead-code note (pro_blue)

The `pro_blue` tree (Python `pair.py` MVP, `deploy/`, `tests/`,
`pro_blue_architecture_and_plan.md`) was an abandoned approach that never
worked reliably (joycond interference, missing BlueZ device object, systemd
path-activation complexity). It was deleted 2026-08-13. Its one reusable trick:
`test_loop.py` cycled USB plug/unplug via
`/sys/bus/usb/devices/<dev>/port/disable` (write `1` = unplug, `0` = replug)
for fast iteration — recreate it in this repo if needed.
