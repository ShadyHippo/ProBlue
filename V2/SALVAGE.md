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
| _(none yet)_ | | | | | |

## Expected salvage candidates (do not copy early — only at the named stage)

| Stage | V1 path | What to take | What to do differently |
|---|---|---|---|
| 1b | `V1/src/bluez/profiles/input/device.c` | ~~the arm state machine~~ — **CANCELLED 2026-09-11: R3 falsified, do NOT copy.** BT-side arm (0x08 00) proven unneeded (x5000 already 0xFF, wake works unarmed). | — |
| 3 | `V1/patches/hid-nintendo-6.8.0-137-generic-usb-passive.patch` | the passive shape (`joycon_is_passive`, probe/resume guards, the 13-line `joycon_init` early-return). NOTE: `V1/src/kernel` is empty — this .patch is the only kernel reference. | apply to 6.18.46 `hid-nintendo.c`; hunk-3 port required (3-Mbit non-fatal upstream); 6.18 rename `joycon_hid_resume`→`nintendo_hid_resume` |
| 5 | `V1/src/bluez/profiles/input/procon.c` + `procon.h` | the wired protocol code (framing verified byte-exact in `KEY_CONTEXT.md` §3) | add read-then-decide via `0x05`/`0x10`; drop re-pair-every-dock (P8, now doc-backed: GET_LTK returns *stored* key, §3.6) |
| 5 | `V1/src/bluez/src/adapter.c` | `btd_adapter_store_link_key` + `reload_link_keys` pattern | adapt to 5.86 anchors; check whether 5.86 has a runtime add-key path instead of full reload |
| 5 | `V1/src/bluez/plugins/sixaxis.c` | the PROCON wiring pattern + trust-before-auth | adapt to 5.86; only the pieces the ledger proves |

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