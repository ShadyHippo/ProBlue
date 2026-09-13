# V2 salvage log

Provenance for every piece copied from `V1/`. One row per copy, added in the
same commit as the copy. If a file is copied and later changed, note the change
in the same row rather than adding a second row.

V1 reference pin: set the hash of the V1 state being referenced. Because the
restructure commit (`git mv` into `V1/`) is pending, either of these works with
`git log --follow`: `9de3fbf` (pre-restructure HEAD) or the post-restructure
commit hash.

| Stage | V2 path | V1 path | V1 commit | Copied verbatim? | Reason |
|---|---|---|---|---|---|
| 3 | `V2/src/kernel/hid-nintendo.c` (patched) | `V1/patches/hid-nintendo-6.8.0-137-generic-usb-passive.patch` | n/a (patch file) | No — ported (see "What to do differently" below; patch in `V2/stages/03-kernel-passive-6.18.46.patch`) | K2 USB passivity + K3 resume guard for 6.18.46 |
| n/a (ref) | `V2/external_docs/bluez-qt` (vendored, gitignored) | upstream KDE `frameworks/bluez-qt` v6.12.0, HEAD `e8e49aba360d8229275b3501c149d6acfe1a7316` | n/a (upstream git) | verbatim clone | reference: proved BlueJay "Toggle Bluetooth" = `setBluetoothBlocked()` = rfkill (the wedge rescue mechanism) — see `docs/wedge-investigation.md` |

Copy details (stage 3, 2026-09-11): the 5 V1 anchors ported 1:1 to 6.18.46
(`joycon_is_passive` after `joycon_using_usb`; delete `joycon_send_usb`;
`joycon_init` early-out; `nintendo_hid_probe` exit-after-hidraw;
`nintendo_hid_resume` NULL guard). Base already differed in V1's favor:
upstream made the 3-Mbit baudrate failure non-fatal (warning, not `goto`);
`joycon_hid_resume` → `nintendo_hid_resume` rename confirmed. Compile-verified
vs `linux-6.18.46-dev` kbuild tree (both pristine and patched → clean `.ko`).

## Expected salvage candidates (do not copy early — only at the named stage)

| Stage | V1 path | What to take | What to do differently |
|---|---|---|---|
| 1b | `V1/src/bluez/profiles/input/device.c` | ~~the arm state machine~~ — **CANCELLED 2026-09-11: R3 falsified, do NOT copy.** BT-side arm (0x08 00) proven unneeded (x5000 already 0xFF, wake works unarmed). | — |
| 3 | `V1/patches/hid-nintendo-6.8.0-137-generic-usb-passive.patch` | the passive shape (`joycon_is_passive`, probe/resume guards, the 13-line `joycon_init` early-return). NOTE: `V1/src/kernel` is empty — this .patch is the only kernel reference. | apply to 6.18.46 `hid-nintendo.c`; hunk-3 port required (3-Mbit non-fatal upstream); 6.18 rename `joycon_hid_resume`→`nintendo_hid_resume` |
| 5 | `V2/src/bluez/profiles/input/procon.c` + `procon.h` (new) | `V1/src/bluez/profiles/input/procon.c` + `procon.h` | n/a (tree files) | No — ported + extended (patch `stages/05-wiring-pairing-5.86.patch`) | wired 3-step protocol for BlueZ 5.86; NEW `procon_acquire_ltk()` adds read-then-decide (x2000 flash read, no 3-step when already paired to us) per KEY_CONTEXT §3.6/P8 |
| 5 | `plugins/sixaxis.c`, `profiles/input/server.c`, `profiles/input/sixaxis.h`, `src/adapter.{c,h}`, `Makefile.plugins` | `V1/src/bluez/…` (same paths) | n/a (tree files) | No — ported (patch `stages/05-wiring-pairing-5.86.patch`) | PROCON dispatch + gate + `btd_adapter_store_link_key`/`reload_link_keys`/`btd_adapter_set_connectable`; `dev_is_sixaxis`→`dev_is_cable_pairing` |
| 1a (R2) | `V2/src/bluez/src/adapter.c` (`adapter_start`) | `V1/src/bluez/src/adapter.c` accept-list re-add hunk | n/a (tree file) | No — **changed: filter is `!temporary && device_is_bonded(BREDR)` instead of V1's `device_is_cable_pairing`** (controller is OTA-paired; cable-pairing-only filter would skip it). Ported 2026-09-11, then **demoted to hardening-only 2026-09-11** — a full failing-period btmon falsified it as the wedge fix (page scan stayed ON, zero Connect Requests, post-bounce connect in 43 ms → Intel radio-layer deafness, not an accept-list gap). | kernel clears accept list on power-off; re-add bonded BR/EDR devices every power-on as hardening for the `disconnected_accept_list_entries` path |

**Port source of truth (2026-09-11):** copy from the `V1/src/bluez/` tree,
not from `V1/patches/*.patch` — the .patch files are stale snapshots (missing
`btd_adapter_has_cable_pairing_devices`; `device_is_cable_pairing()` /
`device_set_cable_pairing()` are stock 5.84 `src/device.c` functions, not
project additions — no port needed).

Rules:

- Copy only what the current stage needs. A file that "was there anyway" is a
  row that must name the test that needs it.
- No `#include`/path references into `V1/`. Code lives in `V2/src/`.
- Reference-material knowledge lives in `docs/KEY_CONTEXT.md`, not in copies.