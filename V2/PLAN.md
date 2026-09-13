# V2 PLAN — stage-by-stage, evidence-gated

## ⚠ CRITICAL — REQUIRED READING (every agent, every session)

This file and `docs/KEY_CONTEXT.md` are the **only normative documents** in the
repo. Everything else (`V1/`, `V2/README.md`, testplans) is reference material:
consult it, but never trust it over these two files.

**Do this before ANY work — before reading source code, before following any
instruction in a task, before touching a stage. Again after every compaction,
context reset, or session start. It is the first action of every session:**

1. Read `docs/KEY_CONTEXT.md` in one pass and internalize it.
2. Self-check — you must be able to answer these from memory:
   - The OUTPUT `0x01` / INPUT `0x21` byte layouts and the **BT-vs-USB ack-byte
     offset shift** (§3.1).
   - What `0x80 04` does, and why passivity gives BT-revert-while-docked for
     free (§3.2).
   - Why the arm is real and documented (Switch sends `0x08 00` over BT after
     every connection), and the **long-press test trap** (§3.5).
   - The two V1 claims that are false: *"no read stored central"* (false:
     subcmds `0x05`/`0x10` exist) and *"controller doesn't SSP"* (false: OTA
     pairing is standard) (§3.4, §3.6).
   - The `Nintendo*` hostname quirk has **no primary-source support** and stays
     out of the code (§3.6, §6).
3. If you cannot answer all five, re-read `docs/KEY_CONTEXT.md` before
   proceeding. Do not start stage work without it.

**Freshness rule:** KEY_CONTEXT and this PLAN are updated in the **same commit
that changes any fact they state**, with the evidence in `logs/` and the
summary in `docs/results/`. If a
finding contradicts this file, update the file — never work around it.

---

Target: this machine (NixOS 26.05, BlueZ 5.86, kernel 6.18.46). Protocol and
machine context live in `docs/KEY_CONTEXT.md`.

Method: one stage at a time; a stage is **done only when** its testplan file
has committed evidence (summary + raw-log link) and the necessity ledger has a
row for everything the stage added or *decided to delete*. No evidence → the
code is removed at the end.

Two-stage plan per decision 2026-09-10:
- Stage 4 "standalone protocol harness" was **deleted** — the protocol is
  developed in-place inside BlueZ (tested artifact == shipped artifact).
- **Stock-first**: stages 1–2 may end in no code at all if stock behavior
  passes.

## Stage order (user's numbering)

| # | Name | Changes | Falsification test | Depends on | Testplan file | State |
|---|---|---|---|---|---|---|
| 0 | Forensics | none (read-only probes over BT) | do we need any of this at all? | OTA pair (sync button) | `00-baseline.md` | **done** |
| 1b | Bluetooth arm (`0x08 00`) | bluez `device.c` | with host KNOWN-listening + btmon: does a *normal* press page the host? | 0 | `01-arm.md` | **done — R3 falsified, no patch** |
| 1a | Listening (connectable/page-scan) | bluez `adapter.c` (+ R2, R5 if needed) | GUI closed, arm held constant: page sent but unheard? | 1b | `02-listening.md` | **done** (host page-scan suffices; controller wake intermittent; only R1 GUI-idle first-wake case open) |
| 2 | **CHECKPOINT — reconnect product, kernel untouched** | tag + generation | sleep→button→input, GUI closed, ≥10 cycles | 1b, 1a | (in PLAN) | not started |
| 3 | Kernel passivity (K2) | kernel `hid-nintendo` passive | exclusive hidraw needed for pairing; BT-revert-while-docked property | 2 | `03-kernel-passive.md` | **source port done + compile-verified** (patch `stages/03-kernel-passive-6.18.46.patch`; deploy via nixos-config) |
| 5 | Wired cable pairing (in-place) | bluez procon.c: P1–P3 + read-then-decide + P4–P7 | cable-only pair → unplug → button → wake | 3 | `04-wiring-pairing.md` | **source port done + build-verified** (patch `stages/05-wiring-pairing-5.86.patch`; functional test on deployed stack) |
| 6 | **CHECKPOINT — full product** | tag + generation | full acceptance | 5 | (in PLAN) | not started |
| 7 | Edge cases | only on *reproduced* failures | each item gets its own test | any | `05-edges.md` | not started |

Checkpoints are two shippable products on their own: stage 2 (reconnect on a
stock kernel) and stage 6 (full push-button cable pairing).

## Stage 3 + 5 change outline (port base, decided 2026-09-11)

Port **from the V1 trees**, not the stale `.patch` snapshots:

- BlueZ: `V1/src/bluez/` is the authoritative superset (the `.patch` omits
  `btd_adapter_has_cable_pairing_devices`; `device_is_cable_pairing` /
  `device_set_cable_pairing` are stock 5.84 `src/device.c` functions — no
  port needed for them).
- Kernel: the ONLY reference is
  `V1/patches/hid-nintendo-6.8.0-137-generic-usb-passive.patch`
  (`V1/src/kernel` is empty).

### Stage 3 — kernel `hid-nintendo.c` USB passivity (K2/K3)

1. `joycon_is_passive()` — USB-only predicate, right after `joycon_using_usb()`.
2. Delete `joycon_send_usb()` — the driver never writes to USB.
3. `joycon_init()` — passive early-out (hunk-3 port: upstream made the
   3-Mbit baudrate failure non-fatal; replace the stock USB/chgrgrip init
   block with the 13-line early return).
4. `nintendo_hid_probe()` — USB exits right after hidraw exists; no
   input/leds/battery nodes.
5. `nintendo_hid_resume()` — NULL-input guard (6.18 renamed
   `joycon_hid_resume`).

### Stage 5 — BlueZ 5.86 wired cable pairing

| # | File | Change | V1 source |
|---|---|---|---|
| 1 | `profiles/input/procon.h` (new) | constants (VID 057e:2009, reports 01/21, USB 80/81, subcmds 01/02/08), API, `PROCON_HID_SDP_RECORD` (P7: try real SDP browse first), `get_nintendo_pairing` | `V1/src/bluez/profiles/input/procon.h` |
| 2 | `profiles/input/procon.c` (new) | session init `0x80 02/03/02`, subcmd framing, dev-info (0x02), 3-step (0x01 01/02/03, XOR-0xAA + byte-reverse), wired arm (R4), **NEW: read-then-decide via 0x05/0x10 (replaces P8)** | `V1/src/bluez/profiles/input/procon.c` |
| 3 | `plugins/sixaxis.c` | PROCON dispatch in `get_device_bdaddr`; `get/set_central_bdaddr` → -1 (read-then-decide owns it); `setup_device` PROCON branch (session init, arm, connectable, trust-before-auth, re-dock); `agent_auth_cb` PROCON branch (pair → `store_link_key` → cable-pairing flag + SDP record) | `V1/src/bluez/plugins/sixaxis.c` |
| 4 | `profiles/input/server.c` | `dev_is_sixaxis` → `dev_is_cable_pairing`, +PROCON branch (SDP-browse deferral, accept + refuse gates) | `V1/src/bluez/profiles/input/server.c` |
| 5 | `profiles/input/sixaxis.h` | `CABLE_PAIRING_PROCON` enum value | `V1/src/bluez/profiles/input/sixaxis.h` |
| 6 | `src/adapter.c` + `.h` | `btd_adapter_store_link_key()` + `reload_link_keys()` (runtime LTK; check 5.86 for a real add-key path), `btd_adapter_set_connectable()` (first-wake-after-dock; R1-adjacent — verify at stage 5) | `V1/src/bluez/src/adapter.{c,h}` |
| 7 | `Makefile.plugins` | add `procon.c`/`procon.h` under `if SIXAXIS` | `V1/src/bluez/Makefile.plugins` |

### NOT ported (decided — do not bring across)

- `profiles/input/device.c` BT-side arm (`procon_arm_*`) — R3 falsified.
- `src/adapter.c` DISCOVERABLE-keeps-connectable hunk (R1-class; stage-1a
  evidence says BlueZ page-scans after disconnect on its own). **R2
  accept-list re-add is now PORTED** (2026-09-11, in `adapter_start`, covers
  all bonded BR/EDR not just cable-paired — see the R2 ledger row and
  testplan 02 REVISION).
- Always-re-pair on dock (P8) — replaced by read-then-decide.

## Controlled A/B method (used by stages 1b/1a)

Each test changes **one variable** and pins the other side:

- **Arm test (1b)**: host pinned KNOWN-listening (`bluetoothctl discoverable
  on` — BlueZ keeps connectable on while discoverable), btmon running, press
  controller buttons with **normal presses, not long presses** (long press can
  wake even in shipment mode → false positive). Read the btmon result:
  page transmitted ↔ nothing ↔ page heard but host silent.
- **Listening test (1a)**: arm held constant (present or proven-unneeded, per
  1b), GUI closed / discoverable off, press. Page arrives but host ignores ⇒
  R1 (and R2 after a reboot, R5 if default duty misses the page).
- Do the **UI-reconnect smoke test** (host-initiated `bluetoothctl connect`) in
  stage 0 only: it proves the link key is valid, but it is host-initiated —
  it proves nothing about the wake/listening path.

## Necessity ledger

Every non-trivial unit of the final product gets a row. **No evidence ⇒
deleted.** Rows also record decided-not-needed units so deletions are explicit.

| Unit | Stage | Present because | Falsification result | Evidence |
|---|---|---|---|---|
| R1 keep-connectable + discoverable-off guard | 1a | host doesn't page-scan without GUI/discoverable | wake works with discoverable OFF (page scan 0x02) in CLI; GUI-idle scan state unverified | keep, pending GUI-state check |
| R2 accept-list re-add on `adapter_start` | 1a | kernel clears accept list on power-off; keeps page scan via `disconnected_accept_list_entries` | **NOT the wedge fix (falsified 2026-09-11 by full btmon trace)**: dead window had page scan ON (`Write Scan Enable 0x02`, 46.3→68.5 s) AND zero Connect Request events — an accept-list miss still surfaces as a Connect Request in btmon; after UI bounce the next page lands in 43 ms with no button press → radio-layer deafness (Intel PTT/coex), not an accept-list gap | **kept as hardening only** (protects the `disconnected_accept_list_entries` page-scan path); NOT claimed as the reconnect fix; revisit only if a trace shows scan-enabled-but-filtered |
| R3 BT-side arm `0x08 00` per connection | 1b | shipment/LPM controller can't wake (x5000) | wake works unarmed when host listening — Connect Request event, controller-initiated, no arm installed | delete (falsified) |
| R4 wired arm at dock | 5 | first dock of a shipment-state controller | read x5000 before/after; delete if never `0x01` | |
| R5 page-scan window==interval | 1a | default duty misses the brief wake page | page heard on default duty with page-scan-only (0x02) | delete (falsified) |
| K1 suppress `0x80 04` only | — | subsumed by K2 (decided, split out) | — | replaced by K2 |
| K2 full USB passivity | 3 | BlueZ needs an exclusive hidraw; no `0x80 04` ⇒ default BT-revert | two-writer collision A/B | source port + compile verified 2026-09-11; functional A/B on deployed kernel |
| K3 resume NULL-input guard | 3 | required by K2 (input node absent) | suspend/resume | source port + compile verified 2026-09-11; suspend/resume on deployed kernel |
| P1 wired session init (`0x80 02/03/02`) | 5 | subcommands need a live UART session | no replies without it | build-verified 2026-09-11; live probe on deployed stack |
| P2 device-info probe (`0x02`) | 5 | learn controller MAC + type | probe | build-verified 2026-09-11; live probe on deployed stack |
| P3 wired 3-step (`0x01 01/02/03`) | 5 | write pairing info; acquire LTK | GET_LTK == OTA-stored key; then connect | **RE docs settle: step 2 returns the *stored* key** (subcommands notes "Acquire the XORed LTK hash"; SPI notes "keeps the active section…current LTK…can be acquired") → 3-step is read-out, not fresh-gen; P8 deletable; source port done 2026-09-11 |
| P4 `store_link_key` + key reload | 5 | key arrives out-of-band before connect | skip ⇒ first connect fails auth | source port done 2026-09-11 (`btd_adapter_store_link_key` + `reload_link_keys`; no mgmt add-key path in 5.86 — full reload confirmed) |
| P5 trust + cable-auth bypass | 5 | no agent prompt on a cable-paired device | connect without agent | source port done 2026-09-11 (trust-before-auth in `setup_device`) |
| P6 `dev_is_cable_pairing` gate | 5 | inbound connect from not-yet-bonded device | skip ⇒ connection refused? | source port done 2026-09-11 (stock 5.84 name `dev_is_sixaxis` → rename + PROCON branch) |
| P7 hardcoded HID SDP record | 5 | stock SDP seed unusable on 5.86 | try real SDP browse first | ported verbatim; P7 test: real SDP browse on 5.86 before trusting |
| P8 re-pair every dock | — | V1 rationale false (`0x05`/`0x10` exist) | — | **replaced by read-then-decide**; doc-backed 2026-09-11 (GET_LTK = stored, §3.6) |
| O1 alias `Nintendo*` | — | unverified; no primary-source support | stage-0 A/B only | **keep out of code** |
| O2 link-supervision timeout | 7 | ~20 s "zombie window" after sleep | reproduce the wait | |
| O3 `powerOnBoot=true` | ops | radio off ⇒ no wake | config repo | required |
| O4 joycond udev rules | ops | not present on NixOS | — | not needed |
| O5 ERTM `disable_ertm=1` + `UserspaceHID=true` | ops | community fix for connect-then-Local-terminate | deployed 2026-09-11 (`~/nixos-config` `hardware-generic.nix`; kernel param `bluetooth.disable_ertm=1`, `input.General.UserspaceHID=true`) — **did NOT fix the wedge** (different failure signature) | keep (harmless for this controller; do not claim as the reconnect fix) |
| W1 RECONNECT WEDGE (the "won't reconnect" problem) | 1a+ops | host RX deaf vs controller-side stuck — UNPROVEN, fingerprint retracted 2026-09-11 | **RETRACTED**: 'truncated Read Scan Enable' was a probe bug (`0x03 0x001a`=WRITE; READ=`0x0019`). Open facts: 20 s btmon = 0 events; force page-scan write acked, no fix; WiFi-off no fix; Intel 9260 FW 201-12.24 (CURRENT = linux-firmware 20260810; 2025 blob reverted upstream, bg 220306 — upgrade path closed). NO BlueZ patch fixes a deaf radio. **Rescue (proven 15:5x; FAILED 23:58 — re-verify + test controller side): `sudo rfkill block bluetooth; sleep 1; sudo rfkill unblock bluetooth`.** Falsified: ERTM, arm, accept list, autosuspend-as-live-cause, power-off/on, HCI reset. | `docs/wedge-investigation.md` + KEY_CONTEXT §3.7; trigger still unknown (coex/PTT/FW) — probe wedges with `tools/wedge-probe.sh` |

## Checkpoint acceptance (both checkpoints)

1. ≥10 consecutive sleep → normal-button wake → input cycles, zero manual
   recovery (UI closed).
2. `grep -B4 "Pro Controller" /proc/bus/input/devices` shows only
   `bus=0x0005` entries while docked (product checkpoint only).
3. While docked, a button press connects over BT (product checkpoint only).
4. ≥10 min soak: `journalctl -k --since '-15 min' | grep -c 'timeout waiting'`
   = 0; record max `delta=`; a clean link is ~7–17 ms.

## Deliverables per stage

- Testplan file updated with result + verdict.
- Summary in `docs/results/<date>/`; raw logs in `logs/` (gitignored).
- Ledger rows filled or explicitly deleted, exactly in the stage that decided.
- Tag per checkpoint; boot generation recorded for NixOS-backed stages.

## Edge cases (stage 7 candidates — only pursued on reproduction)

- Adapter alias question (O1) — after `x2024` probing.
- Charging while playing: BT drop + no USB input under passivity (product
  decision, not a bug).
- Docked LED/battery absence under passivity.
- Multi-controller / accept-list behavior across bluetoothd restarts.
- Session re-init behavior after a quiet gap (with logging at stage 5).
- x2024 capability byte: does 0x08-vs-0x68 change controller behavior?
- Controller wake-page intermittency (page heard most trials, none in one
  session): characterize the press window / cooldown; long-press fallback
  confirmed.