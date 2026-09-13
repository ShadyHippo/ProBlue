# SALVAGE — provenance of V1 reuse

One row per piece copied from the frozen [`../V1/`](../V1) archive, added in the
same commit as the copy. V1 is reference only; nothing here imports from it by
path.

V1 reference pin: `9de3fbf` (pre-restructure HEAD) or the post-restructure
commit that `git mv`'d the tree into `V1/`.

| V2 path | V1 source | Copied verbatim? | Reason / changes |
|---|---|---|---|
| `src/kernel/drivers/hid/hid-nintendo.c` (patched) | `V1/patches/hid-nintendo-6.8.0-137-generic-usb-passive.patch` (`V1/src/kernel` is empty) | No — ported | K2 USB passivity + K3 resume guard for 6.18.46. The 5 anchors port 1:1: `joycon_is_passive` after `joycon_using_usb`; delete `joycon_send_usb`; `joycon_init` early-out; `nintendo_hid_probe` exit after hidraw; `nintendo_hid_resume` NULL guard. Base already differed: upstream made the 3-Mbit failure non-fatal (6.18 hunk-3 change), and `joycon_hid_resume` → `nintendo_hid_resume`. Patch: `src/patches/kernel-hid-nintendo-usb-passive-6.18.46.patch`. |
| `src/bluez/profiles/input/procon.{c,h}` (new) | `V1/src/bluez/profiles/input/procon.{c,h}` | No — ported + extended | Wired 3-step protocol for BlueZ 5.86. New `procon_acquire_ltk()` does read-then-decide (SPI x2000, no 3-step when already paired to us) instead of V1's always-re-pair. |
| `src/bluez/plugins/sixaxis.c`, `src/bluez/profiles/input/server.c`, `src/bluez/profiles/input/sixaxis.h`, `src/bluez/Makefile.plugins` | `V1/src/bluez/` (same paths) | No — ported | PROCON dispatch + accept/refuse gate; `dev_is_sixaxis` → `dev_is_cable_pairing`. New: after `btd_adapter_store_link_key`, mark the device Paired+Bonded (`device_set_paired` + `device_set_bonded`), mirroring `src/adapter.c`'s load-from-storage path. |
| `src/bluez/src/adapter.{c,h}` | `V1/src/bluez/src/adapter.{c,h}` | No — ported, changed | `btd_adapter_store_link_key` + `reload_link_keys` + `btd_adapter_set_connectable`. The `adapter_start` accept-list re-add filters on `!temporary && device_is_bonded(BREDR)` instead of V1's cable-pairing-only filter (the controller here is OTA-paired). Kept as hardening, not a wedge fix. |

No other V1 code is copied. Reference knowledge lives in
[`docs/KEY_CONTEXT.md`](docs/KEY_CONTEXT.md), not in copies.
