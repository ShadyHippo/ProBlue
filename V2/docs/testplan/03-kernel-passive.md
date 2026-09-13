# Test 03 — kernel USB passivity (stage 3)

**Stage:** 3 · **Change:** kernel `hid-nintendo` passive (K2 + K3) · **Status:** not started

## Why this stage exists

Stage 5's wired pairing needs bluetoothd to own the controller's hidraw
exclusively; the stock driver is a second writer at bind time (it runs its own
init including `0x80 04`, which also pins the radio to USB). The port of V1's
patch to 6.18.46: apply the existing passive patches, port hunk 3
(`joycon_init`) — upstream made the 3-Mbit baudrate failure non-fatal; replace
the stock USB/charggrip init block with the 13-line early-return (see
KEY_CONTEXT §3).

**Port source (decided 2026-09-11):** `V1/patches/hid-nintendo-6.8.0-137-generic-usb-passive.patch`
is the ONLY kernel reference (`V1/src/kernel` is empty). The 6.18 tree also
renamed `joycon_hid_resume` → `nintendo_hid_resume`. Anchors to hit:
`joycon_is_passive()` (after `joycon_using_usb()`), delete `joycon_send_usb()`,
`joycon_init()` early-out, `nintendo_hid_probe()` exit-after-hidraw,
`nintendo_hid_resume()` NULL guard.

## Properties to verify

1. **Exclusive hidraw**: hidraw node exists; the driver sends nothing on USB
   (btmon/`hidraw` monitor: no `0x80 02/03/04`, no subcommands, no calibration
   reads).
2. **No USB input**: `ls /dev/input/by-id` and `/proc/bus/input/devices` show
   no `bus=0x0003 (USB)` Pro Controller entry; only `bus=0x0005` (BT) exists.
3. **BT-revert-while-docked** (the `0x80 04` insight): with the controller
   docked and no USB traffic, it times out and reverts to BT — press buttons
   and confirm wake/paging works while still plugged (mash-to-connect). No
   `0x80 05` anywhere.
4. **A/B of necessity (two-writer)**: with the stock driver bound, run a probe
   that races a session/subcmd against the driver's bind-time init and count
   failures; repeat against passive. This is the evidence row for K2.

## Result

**2026-09-11 — ported to 6.18.46, patch generated, module compiles.**

- Patch: `src/patches/kernel-hid-nintendo-usb-passive-6.18.46.patch` (5 hunks,
  applies clean `patch -p1 --dry-run`; round-trip verified pristine→apply→
  byte-identical to `src/kernel/drivers/hid/hid-nintendo.c`).
- Compile: out-of-tree build vs the machine's `linux-6.18.46-dev` kbuild tree
  (nix store path from `linuxPackages.kernel.dev`):
  - pristine `hid-nintendo.c` → `.ko` clean (baseline rc=0)
  - patched → `.ko` clean (rc=0), zero warnings
- Module contains all three markers (verified via `strings`):
  `passive probe (ProBlue fork): hidraw only`, `probe - success (passive)`,
  `no-op resume for passive ctlr`.
- Anchor check (KEY_CONTEXT §5): `joycon_is_passive` used exactly 4×
  (def @781, `joycon_init` @2473, `nintendo_hid_probe` @2682,
  `nintendo_hid_resume` @2752). DEFAULT `joycon_hid_resume` rename confirmed
  on 6.18.46 (it was already `nintendo_hid_resume` in pristine).
- No residual `joycon_send_usb` refs (definition deleted; 0 grep hits).
- `jc_type_is_chrggrip` macro now unused (harmless, `#define`).

Not yet done (needs nixos-config, the scarce kernel rebuild): boot the
patched kernel, then the functional checks below (exclusive hidraw, no USB
input node, BT-revert-while-docked) on real hardware.

## Verdict

**K2/K3 port complete and compile-verified at source level.** Patch ready for
`kernelPatches` in nixos-config per KEY_CONTEXT §1/§5. Functional A/B
(two-writer collision, BT-revert-while-docked) deferred to the deployed
kernel — record evidence there, then finalize the ledger rows K2/K3.