# Test 00 — forensics / baseline (stock system)

**Stage:** 0 · **Artifact under test:** none (stock NixOS 26.05, BlueZ 5.86, kernel 6.18.46) · **Status:** not started

## Questions

1. Can this controller be paired OTA (sync button) at all?
2. What is the controller's *actual* state — report it, don't assume it:
   shipment byte, stored pairing, capability byte.
3. Does this machine have the problems V1 was built for — measured, not felt?
4. Does renaming the adapter (alias `Nintendo*`) change cadence at all? (Expect
   no-op per KEY_CONTEXT §3.6; this closes the hostname thread with evidence.)

## Read-only controller probes (over a BT connection; no code changes)

Use a disposable probe (bluetoothctl + a short L2CAP/subcmd read via an
existing tool, or temporary debug output in a scratch daemon build — no
committed tooling yet):

- subcmd `0x05` page-list state → `0x01` = pairing info in memory.
- SPI read `0x10` @ x2000 (two 0x18-byte reads): magic (`0x95`?), stored host
  MAC (x2004-09), LTK presence, capability x2024.
- SPI read @ x5000: `0x01` (shipment ON → arm will be needed) vs `0xFF`.

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

_(summary + raw log link under docs/results/)_

## Verdict

_(which of stages 1b/1a are even needed; the x5000 prior for the arm test)_