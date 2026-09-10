# Test 04 — wired cable pairing, in place (stage 5)

**Stage:** 5 · **Change:** bluez `profiles/input/procon.{c,h}`, sixaxis wiring,
adapter key storage, server gate, SDP decision · **Status:** not started

## Protocol work (developed in-place in bluez — no standalone harness)

Salvage `V1/src/bluez/profiles/input/procon.c` at this stage (SALVAGE.md), with
these changes mandated by KEY_CONTEXT §3:

1. **Session init** — verify (with logging) that a second `0x80 02/03/02` init
   after a quiet gap is acked (V1 re-inits before the 3-step).
2. **Device info** — confirm type byte == 3 (Pro Controller) and capture MAC.
3. **Read-then-decide pairing** (replaces P8): subcmd `0x05` or SPI read of
   x2000 — if magic `0x95` and stored host MAC == ours, **skip the 3-step**.
4. **GET_LTK experiment**: run `0x01 0x02` alone against an OTA-paired
   controller and compare to the stored key in bluetoothd storage. Stored ==
   returned ⇒ the 3-step is only needed when the stored MAC isn't ours.
5. **The 3-step itself** — per KEY_CONTEXT §3.6 (XOR 0xAA, flash LE order,
   byte-reversed for the BR/EDR key).
6. **Wired arm (R4)**: arm at dock only if the x5000 prior says the controller
   is in shipment (`0x01`); re-read x5000 after pairing as the state readback.
7. **P7 decision**: try real SDP browse on 5.86 before carrying the hardcoded
   record.

## Integration work

P4 link-key storage/reload, P5 trust before cable authorization, P6 server
gate — each with its ledger row. Order matters: trust first, then key storage,
then gate, then SDP.

## Acceptance (this stage)

- Cable-only pairing: plug → (read-then-decide) 3-step if needed → key stored →
  unplug → button → wake → input.
- Re-docks are no-op-pairing when nothing changed (read-then-decide proof).
- GET_LTK == stored-key result recorded (decides whether P3 is ever needed on
  an already-OTA-paired controller).

## Result

_(summary + raw-log links)_

## Verdict