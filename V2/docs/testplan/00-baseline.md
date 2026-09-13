# Test 00 — forensics / baseline (stock system)

**Stage:** 0 · **Artifact under test:** none (stock NixOS 26.05, BlueZ 5.86, kernel 6.18.46) · **Status:** not started

## Questions

1. Can this controller be paired OTA (sync button) at all?
2. What is the controller's *actual* state — report it, don't assume it:
   shipment byte, stored pairing, capability byte.
3. Does this machine have the problems V1 was built for — measured, not felt?
4. Does renaming the adapter (alias `Nintendo*`) change cadence at all? (Expect
   no-op per KEY_CONTEXT §2.6; this closes the hostname thread with evidence.)

## Procedure

The ordered, state-by-state runbook is `tools/stage0/README.md`. It automates
only capture/summarise (`tools/stage0/stage0.zsh preflight|probes|cadence|alias|map|monitor|stop|collect`)
and leaves pairing, connecting and button-press tests to the operator.

Read-only controller probes over a BT connection (no code changes, no writes):

- subcmd `0x05` page-list state → `0x01` = pairing info in memory.
- SPI read `0x10` @ x2000 (two 0x18-byte reads): magic (`0x95`?), stored host
  MAC (x2004-09), LTK presence, capability x2024.
- SPI read @ x5000: `0x01` (shipment ON → arm will be needed) vs `0xFF`.
- subcmd `0x02` device info: fw, type, MAC.

## Measurements

1. **Input cadence**: per-report `delta=` values over 5 min; min/avg/max;
   count `timeout waiting`. Clean link ≈ 7–17 ms.
2. **Wake, stock, GUI open**: sleep → normal button press → reconnect? Record.
3. **Wake, stock, GUI closed**: repeat. Note `powerOnBoot` value first.
4. **UI-reconnect smoke**: host-initiated `bluetoothctl connect <mac>` — proves
   link key only, not wake/listen.
5. **Alias A/B**: same cadence with adapter alias `Nintendo Switch` vs default.
   No-op expected; that closes O1.

## Result

Full record: `docs/results/2026-09-10-stage0/SUMMARY.md`; raw evidence under
`V2/logs/stage0/`.

- Q1: OTA pairing works; full SSP traced (`pair.btmon.log`).
- Q2: Pro Controller, fw `0x0421`; pairing record matches the RE docs
  byte-for-byte; capability `0x08` (PC); **x5000 = `0xFF`** (shipment normal),
  persistent through pairs, disconnects and SYNC-sleeps.
- Q3: stock host-initiated connect to a sleeping controller fails (page
  timeout). A normal button press makes the controller emit a `Connect Request`
  and reconnect **iff the host is page-scanning**; BlueZ enables page scan on
  its own after a disconnect. The controller's wake page is intermittent.
- Q4 (alias): not measured this day (deferred; no signal to chase).

## Verdict

Stages 1b/1a are needed, with the arm test now **decided**: R3 is falsified
(x5000 already `0xFF`, wake works unarmed when the host listens) and the work
is in 1a: host listening state (R1) + controller wake intermittency.
Ledger deltas are in `PLAN.md`.