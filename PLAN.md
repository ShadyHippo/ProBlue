# PLAN.md — Pro Controller Switch-parity pairing for Linux (SOURCE OF TRUTH)

Last updated: 2026-08-24 (evening). This file supersedes every status claim
made anywhere else in this repo or in the archived `~/Programming/slop/joycond`
repo.

## STATE AT HANDOFF (fresh sessions start here)

The complete two-patch stack is **assembled, installed and working on the
live machine**: dock → magic pairing works end-to-end, button-wake
reconnect works (arm acked), input streams into real applications (Dolphin)
over Bluetooth without sudo. What remains is small: formal acceptance
tallies (see Progress & remaining work) and the README rewrite (step 4).
Raw evidence: `docs/virgin-stack-control-test.md` +
`docs/results/2026-08-24-virgin-stack/`.

## Doc truth policy (read first)

- Every status claim in this repo must carry one of: **[PROVEN]** (journal/btmon
  evidence exists and is cited), **[ASSUMED]** (theory, untested), or
  **[DEAD]** (tried, falsified — do not retry).
- A btmon/journal proof of *pairing* is NOT a proof of *input*. They are
  separate layers. This distinction was the cause of months of false "IT
  WORKS" claims. Do not repeat it.
- The protocol documentation (subcommands, framing, SPI layout) in
  `docs/maki_memories.md` §5 is correct and battle-tested. The *status*
  sections of old docs were not.

## Goal experience (the product)

1. Plug controller into USB → magic pairing completes (no GUI, no agent).
2. User presses any button → controller pages host → reconnects (already paired).
3. Input works over BT via the kernel's `hid-nintendo` driver: proper evdev
   gamepad, full support in any application.
4. Mashing from the instant of plug-in should connect (BT radio stays alive
   while docked; accept-list lets the page through).
5. The wire does exactly three things: UART session handshake + pairing
   subcommands, and charging. **Never input over USB.** Switch parity.

## Architecture — DECIDED 2026-08-24: two patches

| | Kernel patch (`hid-nintendo`) | BlueZ patch (this repo) |
|---|---|---|
| USB transport | Passive probe: bind + expose hidraw ONLY. No `0x80 02/03/02`, no `0x80 04` (no-timeout/pin-to-USB), no config subcmds, no USB input/leds/battery. | Owns the wired session over that hidraw: init, arm (`0x08 00`), 3-step pairing, LTK storage, hardcoded SDP record, `-ENOENT` deferral. |
| BT transport | **100% stock upstream driver.** Full init, parses 0x30 reports → real `/dev/input`, LEDs/rumble/battery. Untouched by the patch. The kernel sends **no** arm — stock never uses subcmd 0x08 (verified against pristine source). | Serialized per-connect queue over PSM 19 on EVERY connection, Pro Controllers only: **one subcommand — `0x08 00` arm**, first attempt T+1s, retry ×3 (500ms ack cap each). Sole owner of deliberate BT-side subcommands. |

**Single-owner rule:** exactly one component ever sends deliberate BT
subcommands (the BlueZ arm queue). The kernel sends BT subcommands only as
its stock, unmodified self during its own init — which covers report mode,
calibration, IMU/rumble and player LEDs on its own (proven by the virgin
control test reaching full 0x30 mode with zero BlueZ writes).

**Arm is required for wake [PROVEN 2026-08-24]:** with no arm ever sent, a
slept controller never wake-reconnects on button press (user test, virgin
stack); re-pairing revives it. `0x08 00` clears shipment low-power state
(SPI x5000), re-enabling Broadcom Fast Connect scan-on-button-press; the
Switch sends it after every connection too (RE doc). Timing constraint:
early connect-time subcmds hit a dead window (~300 ms+; factory-cal reads
time out every session) while an unfed wake session dies within ~2 s — so
first attempt at T+1s, retries to ~T+2.5s. Dock-time wired arm
(`procon_arm_wired`) additionally guarantees the flag across undocks.
Docked LED feedback: none (Switch parity decision, 2026-08-24).

**Why the kernel patch is mandatory [PROVEN]:**
- Stock USB init sends `0x80 04` (pin-to-USB/no-timeout): BT radio idles while
  docked → "push button to reconnect" dies. Falsifies the earlier
  "BlueZ-only / stock driver doesn't conflict" claim (old README Q&A,
  maki_memories §3 — both written from pairing-level evidence only).
- Journal 2026-08-14 23:43–44: with a stock-like module loaded, the USB-bound
  driver ran full active init while the plugin drove the same hidraw for the
  dock sequence — two writers, one pipe; every cal read failed.

## Current status

### Layer 1: wired pairing + BT reconnect — SOLID [PROVEN]
Dock → 3-step → link key stored → later button press → page → accept →
encryption → PSM 17/19 up. Verified repeatedly across all configurations
(e.g. journal 2026-08-15 23:51, 2026-08-16 15:25 & 16:28: complete dock
sequence every time).

**2026-08-24 live, final stack installed:** dock at 15:13:56 → session init
×2 → dock arm → pre-3-step re-arm → 3-step steps 1–3 → `link key stored`,
everything acked, no daemon restart. Re-dock idempotent.

### Layer 2: input over BT — CLEAN ON A VIRGIN STACK [PROVEN 2026-08-24]
Control test executed per step 1 below (full evidence + raw logs:
`docs/virgin-stack-control-test.md`). True-pristine configuration — stock
in-tree `hid-nintendo`, stock bluetoothd, joycond disabled, fresh OTA pairing:

- avg IMU report delta 11 ms; gamepad evdev stream continuous at ~7–15 ms
cadence with no perceptible stalls over the recorded play session.
- Zero `timeout waiting`, zero probe failures/`-110`, stable session.
- **Platform exonerated:** Intel adapter, stock driver behavior, page-scan
and link-key type are not causes of the historical degradation. That
degradation is attributed to project-era concurrent hidraw writers (Phase 0
hand-driving 07-29..31, pairing experiments 08-12..14, patched-daemon queue
vs driver init collisions) — see the journal forensics summary in the
control-test doc.

Two persistent quirks [PROVEN], folded into all future testing:
1. **Factory-cal fallback ×3 on every connect** — inherent controller/host
quirk; fires even with one writer and zero project code. Cosmetic only;
sticks center fine, input unaffected.
2. **Early-session subcommand dead window** — subcommands sent in the first
~300 ms–1 s after BT connect time out (`ret=-110`; factory-cal reads hit it
every connect; a fork arm at T+2 s failed once under load). HANDLED: the
BlueZ arm fires at T+1 s and retries ×3 (Architecture below). Tuning knobs
if ever needed: `PROCON_ARM_DELAY_SEC` / `PROCON_ARM_MAX_TRIES` at the top
of the queue block in `patches/bluez-5.84-procon.patch`.
3. IMU micro-gaps (60–130 ms, "compensating for N dropped IMU reports") every
few seconds — gamepad substream unaffected; do not misread as regression.

## Progress & remaining work (state at handoff, 2026-08-24 evening)

| Plan step | State |
|---|---|
| 1. Virgin-stack control test | ✅ DONE — clean; platform exonerated |
| 2. Assemble two-patch stack | ✅ DONE — written, applied-clean verified, built, **installed & active**, live-proven same day |
| 3. Acceptance tests | ◐ PARTIAL — core proven live, tallies outstanding (below) |
| 4. Docs rewrite (README quickstart) | ⬜ NOT STARTED |

### Already banked as live proofs [PROVEN 2026-08-24]
- Wired pairing end-to-end with the final stack (Layer 1 log above).
- Wake-reconnect works WITH the arm; armless wake fails completely.
- Arm acks (~T+2 s): bluetoothd log `procon: connection established,
  arming in 1s` → `subcmd 0x08 00 sent` → `subcmd 0x08 acked`.
- BT input consumed by a real application: Dolphin shows
  `evdev/0/Pro Controller`; sticks/buttons stream.
- Desktop access fixed by removing stale joycond udev rules
  (`72-/89-joycond.rules` forced MODE=0600 on the pad node while exempting
  the IMU — exactly the observed asymmetry). `tools/install.sh` removes
  them automatically now.

### Acceptance checklist — the only testing left (step 3)
Run and write tallies here:
1. ≥10 consecutive sleep → button-wake → input cycles, zero manual recovery.
2. While docked: `grep -B4 'Pro Controller' /proc/bus/input/devices` shows
   ONLY `bus=0x0005` entries (wire never carries input).
3. While docked: button press pages + connects over BT (mash-to-connect).
4. ≥10 min play soak, then `journalctl -k --since '-15 min' | grep -c 'timeout
   waiting'` (expect 0) plus max `delta=` value. Watch item: one session saw
   IMU avg delta drift 11→37 ms (likely link quality; unexplained).

### Step 4 scope (for whoever picks this up)
README quickstart needs: kernel-module build commands, `tools/install.sh
<builddir> [<hid-nintendo.ko>]` usage, joycond-rule cleanup note, and the
permission symptom (non-root `evtest` silently hides unreadable nodes —
try `sudo evtest` when a device "disappears"). Where README and PLAN.md
disagree, PLAN.md wins.

## Dead ends — do not retry [DEAD]

From Project_History (joycond repo) plus this repo's findings:

- mvp/`--bt-host` Python tooling era (deleted 2026-08-10).
- SDP cache seeding (malformed on 5.84; solved permanently by hardcoded record).
- Raw HCI auth tricks, `MGMT PAIR_DEVICE` on existing connection,
  `L2CAP_LM_SECURE/HIGH`, raw `Authentication Requested`.
- Restarting bluetoothd to load keys; `bluetoothctl remove/trust` as fixes.
- Underscore-style storage naming (colons only, proven).
- `0x80 05` idle-revert as the arming mechanism (not console behavior).
- Arm inside kernel probe (timing-falsified 2026-08-16).
- Kernel fork passive on ALL transports (committed joycond version):
  structurally cannot produce BT input — no evdev is ever created.
- Blaming/swapping daemon variants for input quality (superseded by the
  2026-08-24 control test).
- Blaming the Intel combo card / platform for input quality (exonerated by
  the virgin-stack control test, 2026-08-24).
- Requiring zero `using factory cal` as an init gate (impossible on this
  machine/controller pair; benign quirk — see control-test doc Note A).
- Instant 4-step BT setup queue (probe/arm/mode/LED fired on every connect):
  duplicated stock init from a second writer → reply cross-delivery on shared
  subcmd ids; superseded 2026-08-24 by the gated single delayed arm.
- Leaving joycond's udev rules installed beside ProBlue: they force MODE=0600
  on the pad node (daemon-exclusive design) and break desktop access.
  `tools/install.sh` removes them; don't reintroduce.

## Maintenance & porting policy (LLM-friendly)

- No upstreaming goal. Personal forks, published as-is.
- Both patches carry anchor comments naming the function/context they modify
  so a fresh LLM session can re-locate them in newer BlueZ/kernel sources.
- Stable anchors, BlueZ side: `setup_device`, `agent_auth_cb`,
  `input_device_connected`, `hidp_recv_intr_data`, `property_set_mode`,
  `adapter_start`, `btd_adapter_store_link_key`. Kernel side: `joycon_is_passive`,
  `nintendo_hid_probe`, `joycon_init`.
- Porting procedure stays: fetch pristine source of target version, apply
  patch, resolve conflicts against anchors, rebuild, run acceptance tests.
- Working agreements: local commits are fine (personal vibe-coded repo);
  never push or force-push without being asked. Never unload/unbind drivers
  at runtime in the shipped design. Logs go to a gitignored results dir.

## Appendix A — machine & ops context (no other repo needs opening)

### The joycond directory is an ARCHIVE — never delete it
`~/Programming/slop/joycond` holds (a) root-owned, sha256-pinned diagnostic
tools wired into `/etc/sudoers.d/joycond-tools` — moving/deleting them breaks
the pins by design — and (b) gitignored vendored reference repos. All
development happens in ProBlue only.

### Frozen diagnostics (exact paths, exact args)
| command | use |
|---|---|
| `sudo ~/Programming/slop/joycond/dev_tools/btmon_ctl.sh start\|stop` | HCI capture → `dev_tools/results/btmon.log` |
| `sudo ~/Programming/slop/joycond/dev_tools/btstate.sh` | radio/controller snapshot |
| `sudo ~/Programming/slop/joycond/dev_tools/fix_bad_bluetooth.sh` | reboot-equivalent clean state (Already-Paired 0x13 loops, pre-test reset) |
| `sudo ~/Programming/slop/joycond/dev_tools/SPI_DUMP.py` | controller flash pairing records |
| `sudo ~/Programming/slop/joycond/dev_tools/joycond_ctl.sh stop\|start` | joycond daemon control |

Editing any frozen tool breaks its digest pin (intended tripwire; a human must
re-run `dev_tools/not_for_bots/grant_root_access.sh`).

### Live machine state (post-install, 2026-08-24 evening)
- Patched bluetoothd **active** via drop-in
  `/etc/systemd/system/bluetooth.service.d/ProBlue.conf` →
  `/usr/local/libexec/bluetooth/bluetoothd` (an old `ProBlue.conf.bak` may
  sit beside it — harmless).
- Passive module installed at
  `/lib/modules/6.8.0-137-generic/updates/hid-nintendo.ko` and loaded.
  Verify: `modinfo hid_nintendo | grep srcversion` → `B126DE5C5AC7E0F799C78D5`.
- joycond daemon disabled; its udev rules (72-/89-) removed.
- PageScan tuning live in `/etc/bluetooth/main.conf` under `[BR]`.
- Controller BT MAC `20:0B:CF:34:F1:BD`; host `50:28:4A:0D:54:7A`; bonded,
  trusted — do NOT re-pair to "fix" things.

### Rebuilding both artifacts (the 2026-08-24 builds lived in /tmp — volatile)
```bash
# BlueZ daemon — the script applies patches/bluez-5.84-procon.patch itself:
bash tools/get_pristine_bluez.sh ~/ProBlue-build
cd ~/ProBlue-build/bluez-5.84 && autoreconf -fi && ./configure --enable-sixaxis \
  --disable-obex --disable-mesh --disable-midi --disable-nfc --disable-health \
  --disable-test --disable-btpclient --disable-manpages && make -j"$(nproc)"
# sanity: strings src/bluetoothd | grep -ci procon   (nonzero = patched)

# Kernel module — pristine base is the archive reference copy:
mkdir -p /tmp/km/drivers/hid
cp ~/Programming/slop/joycond/reference_docs/hid-nintendo.c /tmp/km/drivers/hid/
cp ~/Programming/slop/joycond/reference_docs/hid-ids.h     /tmp/km/drivers/hid/
cd /tmp/km && patch -p1 < ~/Programming/slop/ProBlue/patches/hid-nintendo-*-usb-passive.patch
printf 'obj-m := hid-nintendo.o\n' > Makefile
make -C /lib/modules/$(uname -r)/build M=$PWD modules   # → hid-nintendo.ko
```
Install either/both with `bash tools/install.sh <bluez-builddir> [<hid-nintendo.ko>]`.
Reboot (or undock + modprobe cycle) if a controller is bound during install.

### Gotchas that cost hours (distilled from the archive's Project_History)
- `bluetooth.service` is D-Bus-activatable: testing a self-built bluetoothd
  requires `systemctl mask` → foreground `-n -f` run → Ctrl-C → unmask.
- `btmgmt` can hang forever while bluetoothd runs — always wrap in `timeout`.
- Storage naming is COLONS ONLY: `/var/lib/bluetooth/<host>/<ctlr>/info`.
- Kernel link-key state clears only via reboot / btusb reload, never daemon
  restarts (`fix_bad_bluetooth.sh` is the tool).
- `insmod` does not resolve deps — use `modprobe` (fork needs `ff_memless`).
- Hand-editing `.patch` text changes hunk line counts — RECOUNT every `@@`
  header afterwards or the patch misapplies (bit us twice on 2026-08-24;
  awk recount one-liners in git history of `patches/bluez-5.84-procon.patch`).
- Non-root `evtest` silently HIDES input nodes it can't open — a "missing"
  gamepad usually means permissions, not a missing driver. Confirm with
  `/proc/bus/input/devices` before debugging anything else.

### Reference material (read-only, lives in the archive)
- Protocol RE: `~/Programming/slop/joycond/reference_docs/Nintendo_Switch_Reverse_Engineering/`
  (`bluetooth_hid_subcommands_notes.md`, `USB-HID-Notes.md`,
  `spi_flash_notes.md`, `packet_parse/c2j.txt`)
- BlueZ source: `~/Programming/slop/joycond/reference_docs/bluez/`
- Pristine kernel driver + verification procedure:
  `~/Programming/slop/joycond/reference_docs/{hid-nintendo.c,hid-ids.h}` and
  `pristine_kernel_reference.md`
- Complete dead-end history: `~/Programming/slop/joycond/reference_docs/Project_History.md`
  (its banner explains what to distrust)

### Fresh-session bootstrap
Read this file top to bottom. Then: `docs/maki_memories.md` §5 when touching
protocol code; `docs/virgin-stack-control-test.md` for layer-2 evidence and
journal-forensics method. The README still contains pre-step-4 quickstart
text — where it disagrees with this file, this file wins.
