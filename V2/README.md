# V2 — ProBlue, rebuilt one proven step at a time

Clean-room successor to `V1/` (the original ProBlue). Same goal, opposite
process: **code enters only when a test proves it is needed.**

`V1/` is a frozen reference archive — read it to compare or copy specific
pieces, never edit it.

**Read `docs/KEY_CONTEXT.md` first** — it is the machine + protocol briefing
every piece of work in this repo is based on. `V2/PLAN.md` is the gated stage
list and necessity ledger.

## Rules

- **No OS-specific content.** `targets/` holds plain-shell build recipes.
  Machine plumbing (NixOS overlay, kernel patches, bluetooth settings) lives in
  the NixOS configuration, not in this repo.
- **`src/` is the source of truth** (full patched files vs pristine upstream).
  `stages/` is generated. One direction only; never hand-edit a patch.
- **Nothing imports from `V1/` by path.** Reuse is an explicit copy, recorded
  in `SALVAGE.md` with the V1 commit, file and reason.
- **A stage is done only when** its testplan entry carries committed evidence
  (summary + raw-log link) and the necessity ledger in `PLAN.md` has a row
  proving what the stage added is needed — or explicitly that it isn't.
- **Target: this machine only** — NixOS 26.05 / BlueZ 5.86 / kernel 6.18.46.
  Portability is a non-goal until the stack is proven.

## Layout

| Path | Contents |
|---|---|
| `docs/KEY_CONTEXT.md` | **read first**: machine facts, protocol digest, V1 autopsy, workflow |
| `PLAN.md` | gated stages, checkpoints, necessity ledger |
| `SALVAGE.md` | provenance of every piece copied from V1 |
| `stages/` | generated, ordered patches (source of truth is `src/`) |
| `src/bluez/` | full-file BlueZ sources, vs pristine 5.86 |
| `src/kernel/` | full-file `hid-nintendo.c`, vs pristine 6.18.46 |
| `targets/` | plain-shell build/fetch recipes, one per artifact |
| `tools/` | fetch / regen / build / measure scripts |
| `docs/testplan/` | one testplan file per stage, with the controlled A/B method |
| `docs/results/` | dated evidence per test (raw logs gitignored, summaries kept) |
| `external_docs/` | vendored reference repos (RE notes, nxbt), gitignored |

## Anti-goals

- Rebuilding the Linux input stack from scratch.
- A general "Switch controller support" project. Scope is: cable pairing +
  reliable reconnect for the Pro Controller.
- Changing the Bluetooth host name (unverifiable mechanism; `KEY_CONTEXT.md`
  §3.6/§6).