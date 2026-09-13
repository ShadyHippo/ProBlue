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

**2026-09-11 — ported to BlueZ 5.86, patch generated, bluetoothd builds clean.**

**2026-09-12 — FUNCTIONAL PASS (deployed, user-verified "flawlessly", committed).**
Live-deploy fixes folded into the patch (see KEY_CONTEXT §3.7): device is
marked Paired+Bonded after cable-pairing (`device_set_paired` +
`device_set_bonded`, sixaxis.c PROCON completion — UI no longer shows
"Pair"; power-on accept-list re-add now includes it), and the extra
`procon_usb_session_init()` in `procon_acquire_ltk` was removed so the flow
matches V1's proven sequence (3-step no longer stalls at save).
Open (non-blocking): read-then-decide ("not paired to us") never hit even
after completed saves — x2000 compare unverified (needs SPI dump);
non-blocking hidraw fd hardening not done (latent).

- Patch: `V2/stages/05-wiring-pairing-5.86.patch` (899 lines, 8 files:
  `Makefile.plugins`, `plugins/sixaxis.c`, new `profiles/input/procon.{c,h}`,
  `profiles/input/server.c`, `profiles/input/sixaxis.h`, `src/adapter.{c,h}`).
  Applies clean `patch -p1 --dry-run` on pristine 5.86; round-trip verified
  byte-identical on all 8 files.
- Build: `autoreconf -fi` + `./configure --enable-sixaxis …` + `make -j`
  per KEY_CONTEXT §2 recipe → `src/bluetoothd` (7.5 MB), zero warnings/errors.
- The binary contains the procon protocol (verified via `strings`): session
  framing, 3-step markers, `link key stored`.
- Read-then-decide implemented in `procon_acquire_ltk()`: reads x2000 (subcmd
  0x10); magic 0x95 + stored MAC == ours → LTK straight from flash (byte-
  reversed), **no 3-step**; else full 3-step. Belt-and-braces check applies:
  GET_LTK == OTA-stored key (`25 31 28 85 …`).
- Not ported (decided): BT-side arm in `profiles/input/device.c` (R3
  falsified), DISCOVERABLE-keeps-connectable hunk + accept-list re-add
  (R1/R2), always-re-pair (P8 replaced). `btd_adapter_set_connectable` ported
  (first-wake-after-dock, R1-adjacent — verify at functional test).

## Verdict

**Stage-5 source port complete and build-verified.** Functional testing
(cable-only pair → unplug → button → wake → input; re-dock no-op; GET_LTK ==
stored) needs the deployed patched bluetoothd + passive kernel — deploy via
nixos-config, then run this testplan's acceptance list.