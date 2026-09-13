# V2 — ProBlue, rebuilt one proven step at a time

Clean-room successor to `V1/` (the original ProBlue). Same goal, opposite
process: **code enters only when a test proves it is needed.**

`V1/` is a frozen reference archive — read it to compare or copy specific
pieces, never edit it.

**Read `docs/KEY_CONTEXT.md` first** — it is the machine + protocol briefing
every piece of work in this repo is based on. `V2/PLAN.md` is the gated stage
list and necessity ledger.

## Fetching the pristine sources (reproducible, upgrade-proof)

The port targets are **exactly what this machine runs**. Nothing is cloned from
upstream guesswork: the sources are materialized from the nix store, resolved
from the same nixpkgs rev your system flake pins. `MANIFEST.md` in `src/`
records the pin.

**First fetch (and after every system upgrade):**

```zsh
cd ~/Programming/ProBlue/V2
tools/fetch-pristine.zsh            # fails if src/ already has content
```

This unpacks:

- `src/bluez/` — the pristine BlueZ tree (5.86)
- `src/kernel/hid-nintendo.c` — the pristine driver for your exact kernel
- `src/MANIFEST.md` — pinned rev, versions, store paths, sha256s

**Overwrite (only when re-fetching after an upgrade, or to restore pristine):**

```zsh
tools/fetch-pristine.zsh --force    # DESTROYS any edits in src/
```

**Custom nixpkgs pin** (flake.lock not at the default path):

```zsh
NIXOS_CONFIG=/path/to/config tools/fetch-pristine.zsh
```

The script refuses to overwrite a non-empty `src/` unless `--force`. After a
`nix flake update` + `nixos-rebuild switch`, re-run `--force` and re-apply the
patches in `stages/` per `docs/KEY_CONTEXT.md` §5.

## Known issue: the reconnect wedge, and the one command that fixes it

The Pro Controller intermittently "won't reconnect" because the **Intel
Wireless-AC 9260 radio silently stops page-scanning** — the host believes
it's listening (`hciconfig` shows PSCAN) but the controller's own scan-enable
register has reverted to off, so the controller's pages are never heard.

**Rescue (works every time; same as BlueJay's "Toggle Bluetooth", which is
an rfkill cycle):**

```sh
sudo rfkill block bluetooth; sleep 1; sudo rfkill unblock bluetooth
```

`bluetoothctl power off/on`, `btmgmt power off/on`, and `hcitool` HCI Reset
do **not** clear it (mgmt power-off is deferred/raced; the kernel skips
re-asserting page-scan when its cached flag already matches). Full record:
`docs/wedge-investigation.md`.

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
| `docs/results/` | committed summaries per test |
| `logs/` | raw measurement logs and captures (gitignored except `.gitkeep`) |
| `external_docs/` | vendored reference repos (RE notes, nxbt), gitignored |

## Anti-goals

- Rebuilding the Linux input stack from scratch.
- A general "Switch controller support" project. Scope is: cable pairing +
  reliable reconnect for the Pro Controller.
- Changing the Bluetooth host name (unverifiable mechanism; `KEY_CONTEXT.md`
  §3.6/§6).