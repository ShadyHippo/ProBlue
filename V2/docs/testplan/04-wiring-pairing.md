# Test 04 — wired cable pairing, in place (stage 5)

**Stage:** 5 · **Change:** bluez `profiles/input/procon.{c,h}`, sixaxis wiring,
adapter key storage, server gate, SDP decision · **Status:** not started

## Protocol work (developed in-place in bluez — no standalone harness)

Salvage `V1/src/bluez/profiles/input/procon.c` at this stage (SALVAGE.md), with
these changes mandated by KEY_CONTEXT §3.

**Port source (decided 2026-09-11):** copy from the `V1/src/bluez/` tree, not
from `V1/patches/bluez-5.84-procon.patch` (stale snapshot — misses
`btd_adapter_has_cable_pairing_devices`; `device_is_cable_pairing()` /
`device_set_cable_pairing()` are stock 5.84 `src/device.c`, no port needed).

1. **Session init** — verify (with logging) that a second `0x80 02/03/02` init
   after a quiet gap is acked (V1 re-inits before the 3-step).
2. **Device info** — confirm type byte == 3 (Pro Controller) and capture MAC.
3. **Read-then-decide pairing** (replaces P8): subcmd `0x05` or SPI read of
   x2000 — if magic `0x95` and stored host MAC == ours, **skip the 3-step**.
4. **GET_LTK experiment (now doc-backed, run as belt-and-braces)**: RE docs
   settle that step 2 returns the *stored* key (§3.6: "Acquire the XORed LTK
   hash"; SPI x2000 "current LTK used with Switch can be acquired"). Run
   `0x01 0x02` against this OTA-paired controller and confirm it equals
   `25 31 28 85 04 4c fc 66 74 72 86 6f f1 81 38 f4` (bluetoothd storage).
   If it ever differs → stop, re-open §3.6.
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
- GET_LTK == stored-key confirmed on this unit (`25 31 28 85 …`); recorded as
  the doc-settled read-out behavior (§3.6).

## Result

_(summary + raw-log links)_

## Verdict