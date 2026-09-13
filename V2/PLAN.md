# PLAN — stage method and necessity ledger

Reference reading: [`README.md`](README.md) (what/why/how) and
[`docs/KEY_CONTEXT.md`](docs/KEY_CONTEXT.md) (protocol facts).

## Method

One stage at a time. **A stage is done only when** its testplan file carries
committed evidence (summary + raw-log link) and the ledger below has a row for
everything the stage added — or an explicit row that it was decided unnecessary.
No evidence → the code is removed. Stock-first: stages may end with no code if
stock behaviour passes.

Two principles from the rebuild:

- The protocol is developed **in place** inside BlueZ; the tested artifact is the
  shipped artifact. There is no separate harness.
- Every test changes **one variable** and pins the other side. Captures are
  `btmon`; tests use normal button presses, never long presses (a long press can
  wake even a shipment-state controller and give a false positive).

## Stages

| # | Name | Change | Falsification test | Testplan | State |
|---|---|---|---|---|---|
| 0 | Forensics | none (read-only probes) | do we need any of this? | `00-baseline.md` | done |
| 1b | Bluetooth arm `0x08 00` | bluez `device.c` | with host listening, does a normal press page? | `01-arm.md` | **done — R3 falsified, no patch** |
| 1a | Listening (page-scan) | bluez `adapter.c` | GUI closed, arm held: page sent but unheard? | `02-listening.md` | **done** (host page-scan suffices; R2 kept as hardening) |
| 2 | **Checkpoint — reconnect, stock kernel** | tag + generation | sleep → button → input, ≥10 cycles | (below) | not started |
| 3 | Kernel passivity (K2/K3) | `hid-nintendo` passive on USB | exclusive hidraw needed for pairing; BT-revert-while-docked | `03-kernel-passive.md` | **ported + compile-verified** (`src/patches/kernel-…`) |
| 5 | Wired cable pairing | `procon.{c,h}` + sixaxis wiring + adapter key storage | cable-only pair → unplug → button → wake | `04-wiring-pairing.md` | **ported + built + live-verified** (`src/patches/bluez-…`) |
| 6 | **Checkpoint — full product** | tag + generation | full acceptance | (below) | not started |
| 7 | Edge cases | only on reproduced failures | each item gets its own test | `05-edges.md` | not started |

Stage 2 (reconnect on a stock kernel) and stage 6 (cable pairing) are two
shippable products.

## Stage 3 — kernel passivity (what each hunk does)

1. `joycon_is_passive()` — USB-only predicate, after `joycon_using_usb()`.
2. Delete `joycon_send_usb()` — the driver never writes to USB.
3. `joycon_init()` — passive early-out (replaces the stock USB/charggrip init
   block; upstream made the 3-Mbit failure non-fatal).
4. `nintendo_hid_probe()` — USB exits right after hidraw exists; no
   input/LED/battery nodes.
5. `nintendo_hid_resume()` — NULL-input guard (6.18 renamed the function).

## Stage 5 — BlueZ wired cable pairing (what each file does)

| File | Change |
|---|---|
| `profiles/input/procon.h` (new) | constants (VID `057e:2009`, reports 01/21, USB 80/81, subcmds), API, SDP record |
| `profiles/input/procon.c` (new) | session init `0x80 02/03/02`, framing, device-info, read-then-decide (`0x05`/`0x10`), 3-step, wired arm |
| `plugins/sixaxis.c` | PROCON dispatch; setup: session init, arm, connectable, trust-before-auth; completion: store key, mark Paired+Bonded, SDP |
| `profiles/input/server.c` | `dev_is_sixaxis` → `dev_is_cable_pairing`, + PROCON accept/refuse gate and SDP-browse deferral |
| `profiles/input/sixaxis.h` | `CABLE_PAIRING_PROCON` enum value |
| `src/adapter.{c,h}` | `btd_adapter_store_link_key` + `reload_link_keys`; `btd_adapter_set_connectable` |
| `Makefile.plugins` | build `procon.c` under `if SIXAXIS` |

Not ported (decided): the BT-side arm in `profiles/input/device.c` (R3
falsified), the DISCOVERABLE-keeps-connectable hunk (R1), always-re-pair (P8
replaced by read-then-decide). The `adapter_start` accept-list re-add (R2) is
ported as hardening only.

## Necessity ledger

| Unit | Present because | Falsification / decision | Evidence |
|---|---|---|---|
| R1 keep-connectable + discoverable-off | host may not page-scan without GUI/discoverable | wake works with discoverable OFF; GUI-idle state unverified | testplan 02; kept, low priority |
| R2 accept-list re-add on `adapter_start` | kernel clears the accept list on power-off | **not the reconnect wedge** (page scan was on, zero Connect Requests); kept as hardening for the `disconnected_accept_list_entries` path | testplan 02 revisions; `src/bluez/src/adapter.c` |
| R3 BT-side arm `0x08 00` | shipment-state controller couldn't wake | wake works unarmed (x5000 already `0xFF`) | testplan 01 / results stage0; **deleted** |
| R4 wired arm at dock | first dock of a shipment-state controller | arm only if x5000 reads `0x01`; read back after | stage 5 port |
| R5 page-scan window == interval | default duty might miss the brief page | default duty heard the page (`0x02`) | testplan 02; **deleted** |
| K2 full USB passivity | BlueZ needs an exclusive hidraw; no `0x80 04` ⇒ default BT-revert | two-writer collision A/B | kernel patch; compile-verified, functional A/B on deployed kernel |
| K3 resume NULL-input guard | required by K2 (no input node) | suspend/resume | kernel patch; compile-verified |
| P1 wired session init `0x80 02/03/02` | subcommands need a live UART session | no replies without it | `procon.c`; live-verified |
| P2 device-info probe `0x02` | learn controller MAC + type | reply type == 3 | `procon.c`; live-verified |
| P3 wired 3-step `0x01 01/02/03` | write pairing info; read stored LTK | GET_LTK == OTA-stored key | `procon.c`; §2.6 |
| P4 `store_link_key` + key reload | key arrives out-of-band before connect | skip ⇒ first connect fails auth | `adapter.c`; live-verified |
| P5 trust + cable-auth bypass | no agent prompt on a cable-paired device | connect without agent | `sixaxis.c` |
| P6 `dev_is_cable_pairing` gate | inbound connect from a not-yet-bonded device | skip ⇒ connection refused | `server.c` |
| P7 hardcoded HID SDP record | stock SDP seed unusable on 5.86 | try real SDP browse first | `procon.h` |
| P8 re-pair every dock | V1 rationale false | **replaced by read-then-decide** | §2.4/§2.6 |
| O1 alias `Nintendo*` | unverified, no primary source | keep out of code | KEY_CONTEXT §4 |
| O5 ERTM `disable_ertm=1` + `UserspaceHID=true` | community fix for connect-then-terminate | did **not** fix the wedge (different signature) | ops only; harmless |
| W1 reconnect wedge | host-radio firmware drops scan-enable | not a BlueZ bug; not fixable here | [`docs/hardware/intel-9260-reconnect-wedge.md`](docs/hardware/intel-9260-reconnect-wedge.md) |

## Checkpoint acceptance

1. ≥10 consecutive sleep → normal-button wake → input cycles, zero manual
   recovery (UI closed).
2. `grep -B4 "Pro Controller" /proc/bus/input/devices` shows only `bus=0x0005`
   entries while docked (product checkpoint).
3. While docked, a button press connects over BT (product checkpoint).
4. ≥10 min soak: `journalctl -k --since '-15 min' | grep -c 'timeout waiting'`
   = 0; record max `delta=` (clean link ≈ 7–17 ms).

## Deliverables per stage

- Testplan file updated with result + verdict.
- Summary in `docs/results/<date>/`; raw logs under `logs/` (gitignored).
- Ledger rows filled or explicitly deleted, in the stage that decided.
- Tag per checkpoint; boot generation recorded.
