# PLAN.md — Pro Controller Switch-parity pairing for Linux (SOURCE OF TRUTH)

Last updated: 2026-08-24. This file supersedes every status claim made anywhere
else in this repo or in the archived `~/Programming/slop/joycond` repo.

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
| BT transport | **100% stock upstream driver.** Full init, parses 0x30 reports → real `/dev/input`, LEDs/rumble/battery. Untouched by the patch. | Serialized post-connect setup queue over PSM 19 on EVERY connection: `0x02 probe → 0x08 00 arm → 0x03 30 full mode → 0x30 01 LED`, ack-waited, 500 ms cap per subcmd. Sole owner of BT-side subcommands. |

**Single-owner rule:** exactly one component ever sends BT subcommands (the
BlueZ queue). The kernel sends BT subcommands only as its stock, unmodified
self during its own init. The arm does NOT move into the kernel — a kernel
probe arm fires seconds after connect (behind module autoload + calibration
init); the queue fires within milliseconds. Kernel-probe arm placement was
falsified live on 2026-08-16 16:26 (`arm: subcmd 0x08 00 failed; ret=-110`
into an already-dead session).

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

### Layer 2: input over BT — NEVER PROVEN in any configuration
The pipeline CAN assemble (kernel driver bound over uhid reached READ state
and streamed 0x30 reports — IMU-compensation lines exist in every era,
including ~20 min continuous on 2026-08-16 16:21–16:43 under the current
binaries). But it has never been clean or reliable:

- Calibration reads fail on essentially EVERY session (333 `using factory
  cal` fallbacks across 84 BT binds, 2026-08-10→16). Healthy links show zero.
- Multi-second report stalls with burst catch-up (`delta=1589ms`,
  avg_delta 15→25 ms), rate-limiter starvation.
- Sporadic probe deaths (`probe of 0005:057E:2009 failed with error -110`).
- Session churn without a fast life-signal (controller drops un-fed sessions;
  historical estimates range 150 ms–2.2 s).

**Critical baseline fact [PROVEN]:** the degradation predates ALL project code.
2026-08-09/10 — pure stock GUI pairing, no patched daemon, fork-era modules
loaded but BT binds still showed 93 factory-cal failures across 19 binds.
No truly pristine (zero custom modules) baseline has ever been measured on
this machine.

Ruled out as causes [PROVEN]: adapter hostname quirk (alias already
"Nintendo Switch", verified live), link-key type (golden GUI capture also
stores Type=4), PageScan tuning (live in main.conf, correct), which daemon
variant runs (degradation identical across all three).

Prime suspects for layer 2 (unranked, untested): Intel combo-card BR/EDR
behavior with this controller (sniff interval / EDR policy);
`hid-nintendo`'s rate limiter vs this adapter's report-cadence jitter;
uhid/GLib forwarding latency. Decisive cheap test: a different BT adapter
(USB dongle) or another Linux machine.

## Next steps (ordered)

1. **Virgin-stack control test** (~15 min, no code): uninstall
   `/lib/modules/$(uname -r)/updates/hid-nintendo.ko` (+ depmod), reboot,
   forget controller, stock GUI pair, `evtest` the nintendo-created node,
   `journalctl -kf | grep -E 'factory cal|delta=|timeout waiting'`.
   - Still degraded → platform problem; test a USB BT dongle / other machine
     before touching either patch again.
   - Clean → cable-path-specific; isolate from here.
2. Assemble target stack in this repo:
   - BlueZ side = committed `patches/bluez-5.84-procon.patch` (queue present
     at HEAD; working-tree strip reverted 2026-08-24).
   - Kernel side = port slim passive-USB-only patch from joycond repo git
     history (commit-era keep-bt-radio.patch MINUS the BT-passive hunk MINUS
     `joycon_procon_arm`). Target file lives at `patches/hid-nintendo-*-usb-passive.patch`.
3. Acceptance tests (input-level, written down so testing can't lie):
   - evtest streams events within ~2 s of button-wake.
   - Zero `using factory cal` lines during init.
   - Zero report stalls >100 ms over a ≥10 min session.
   - ≥10 consecutive dock→unplug→button→input cycles without manual recovery.
   - While docked: button press connects (mash-to-connect) and USB carries
     no input (no USB evdev node created).
4. Rewrite user-facing install docs around the two-patch flow (install.sh
   gains the kernel-module step; README quickstart updated).

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
- Blaming/swapping daemon variants for input quality (fault predates all
  three variants; see baseline fact above).

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
- Working agreements carried over: user commits only (agents never commit);
  never unload/unbind drivers at runtime in the shipped design; logs go to
  a gitignored results dir; frozen root tooling in the joycond repo remains
  valid for diagnostics on this machine.
