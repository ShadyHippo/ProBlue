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
| 1b | `V1/src/bluez/profiles/input/device.c` | the arm state machine (post-cleanup: `procon_arm_*`, `PROCON_ARM_*` knobs) | port to 5.86; keep delayed fire + retries |
| 3 | `V1/patches/hid-nintendo-6.8.0-137-generic-usb-passive.patch` | the passive shape (`joycon_is_passive`, probe/resume guards, the 13-line `joycon_init` early-return) | apply to 6.18.46 `hid-nintendo.c`; hunk-3 port required |
| 5 | `V1/src/bluez/profiles/input/procon.c` + `procon.h` | the wired protocol code (framing verified byte-exact in `KEY_CONTEXT.md` §3) | add read-then-decide via `0x05`/`0x10`; drop re-pair-every-dock; test GET_LTK-vs-stored |
| 5 | `V1/src/bluez/src/adapter.c` | `btd_adapter_store_link_key` + `reload_link_keys` pattern | adapt to 5.86 anchors |
| 5 | `V1/src/bluez/plugins/sixaxis.c` | the PROCON wiring pattern + trust-before-auth | adapt to 5.86; only the pieces the ledger proves |

Rules:

- Copy only what the current stage needs. A file that "was there anyway" is a
  row that must name the test that needs it.
- No `#include`/path references into `V1/`. Code lives in `V2/src/`.
- Reference-material knowledge lives in `docs/KEY_CONTEXT.md`, not in copies.