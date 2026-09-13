# Stage 0 — controller state as read 2026-09-10

> Historical record. The "what that means for the plan" reading below was
> superseded by later measurements — see
> [`../../hardware/intel-9260-reconnect-wedge.md`](../../hardware/intel-9260-reconnect-wedge.md).

Decoded from the read-only probes in `logs/stage0/probe-*.txt`. No writes were
made to the controller. This is the Q1 + Q2 half of stage 0.

## Device (subcmd `0x02`)

| Field | Value |
|---|---|
| firmware | `0x0421` |
| controller type | `0x03` = Pro Controller |
| MAC | `20:0B:CF:34:F1:BD` (big-endian at `data[4..9]`) |

## Pairing state

- `0x05` page-list state = **`0x01`** → a host pairing record is in memory.
- SPI x2000 section 1 (magic `0x00` = unused / previous):
  - host MAC `50:28:4A:0D:54:7A` (our adapter, big-endian at x2004-09)
  - LTK x200A-19 = `e9 80 92 a9 ae 14 c0 2e 6c 1f 9f c8 07 49 7a 5f`
  - capability x2024 = **`0x08`** (docs: `0x68` Switch, `0x08` PC)
- SPI x2026 section 2 (magic `0x95` = used / current):
  - host MAC `50:28:4A:0D:54:7A`, checksum `0xeaf4`
  - LTK x2030-3F = `3d 6f 15 02 52 a5 e7 6d b9 98 58 64 47 3e 69 11`
  - capability x204A = **`0x08`** (PC)
  - x204C onward = `0xFF` (end of pairing area)

Conclusion: the controller is genuinely paired to this adapter, and it records
the host capability as **PC (`0x08`), not Switch (`0x68`)**.

## Shipment / low-power (SPI x5000)

`x5000` = **`0xFF`** (all of x5000-x500F is `0xFF`).

Per `bluetooth_hid_subcommands_notes.md` §0x08 and `spi_flash_notes.md`, `0xFF`
means shipment is **off / normal**: Triggered Broadcom Fast Connect
(scan-on-button-press) is **enabled**, and LPM is not forced to HID OFF.

## What that means for the plan

- Ledger **R3 (BT-side arm `0x08 00`)**: **unresolved**, not falsified. x5000 read
  `0xFF` only while connected; whether that value holds through the
  sleep/disconnected state is **unverified** (the docs do not claim it
  persists). Resolved inside stage 1b: arm, host pinned listening, normal
  press, observe with btmon.
- **Confirmed baseline (this is stage 1's falsification test): there is no
  working reconnect in any direction.** Host-initiated fails
  (`Create Connection` → `Page Timeout`); controller-initiated wake has never
  been observed in any capture (even with the host page-scanning); the only
  path that works is forget + re-pair via SYNC. This matches the plan's
  decision that stages 1b/1a are needed, in that order.

## Evidence files

- `logs/stage0/pair.btmon.log` — full OTA pairing + SSP (IO Capability Request/
  Response, User Confirmation, Simple Pairing Complete, Link Key Notification).
- `logs/stage0/probe-02-devinfo.txt`, `probe-05-pagelist.txt`,
  `probe-10-2000.txt`, `probe-10-2018.txt`, `probe-10-5000.txt`,
  `probe-10-6000.txt`.
- `logs/stage0/wake-press.btmon.log` — host had page scan on after the
  disconnect; a normal button press produced no HCI events.
