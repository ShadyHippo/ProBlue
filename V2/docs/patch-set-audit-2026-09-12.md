# Patch-set audit vs new wedge knowledge (2026-09-12)

Audits every staged patch that is about to be deployed into nixos-config
against what changed in the investigation after the patches were authored
(2026-09-11): the retracted `0x001a`=Write fingerprint, the confirmed
scan_enable-0x00 mechanism, the unreliable rfkill rescue, and the upstream
btusb remote-wake findings. KEY_CONTEXT §3.7 is authoritative for the wedge.

## What changed since these patches were written

- "Read Scan Enable truncated plen 4" fingerprint = **probe bug**, retracted.
- While wedged, the truthful read `0x03 0x0019` returns **0x00** (scans off)
  — the register genuinely drops; kernel cached `HCI_PSCAN` may or may not be
  set (15:5x: PSCAN on; 23:58: PSCAN off) — so "firmware dropped it" vs
  "kernel cleared it and forgot" is **still open**.
- rfkill reinit **failed** to reconnect on 2026-09-11 23:58 (controller had
  stopped paging; needed re-arm + button presses).
- Rescue that works: `hcitool cmd 0x03 0x001a 0x02` + button presses
  (`tools/unwedge.sh`).
- Upstream: btusb "clear remote wake on idle Intel ACPI" series UNMERGED
  (v2 2026-05-13, rework 2026-08-04); dependency usb-core commit not in
  master. `V2/stages/btusb-remote-wake-v2.patch` backports with offsets.

## Audit per item

### 1) Kernel hid-nintendo USB passivity (stage 3) — VERDICT: SHIP, no changes

Assumptions in patch: bluetoothd's procon owns the USB hidraw exclusively
(driver sends nothing on USB — no `0x80 02/03/04`, no subcommands, no
calibration); USB carries no input (Switch parity); BT path is 100% stock.

vs new info: **orthogonal.** The wedge lives in the host HCI scan state;
stage 3 touches only USB transport ownership and the USB probe/resume paths.
Nothing in it writes scan-enable, page-scan, connectable, or the BT reconnect
path. No contradiction.

Notes / gaps the audit surfaced:
- The patch deletes `joycon_send_usb()` and its `else if (jc_type_is_chrggrip)`
  branch: BT charging-grip controllers now take the stock (non-USB) path.
  Only matters if a charging grip is ever used — add to testplan 05 if so.
- **Product consequence (new):** with USB passive, "plug in to play" is no
  longer available as a wedge workaround. During a wedge the cable gives
  pairing only, not input. Rescue remains `unwedge.sh`. Already tracked as a
  testplan-05 edge (charging while playing), now has a real-world reason.
- The wedge can still strike while docked; stage 3 neither causes nor fixes it.

### 2) Bluetooth stability power + patch — VERDICT: CARRY AS EXPERIMENTS, low expectations

- `btusb-remote-wake-v2.patch`: premise = let the USB port power off when the
  BT device is idle (gate on page/inquiry scan). **Our measurements show
  `power/control=on`, `runtime_status=active` during wedges** — the port is
  not being power-managed, so this path likely never engages on this machine.
  Keep it (cheap, aligned with the only active upstream work) but do **not**
  expect it to fix the wedge. Documented in
  docs/intel-bt-remote-wake-research.md.
- `iwlwifi.bt_coex_active=0`: untested lever for the PTT/coex trigger
  hypothesis (a wedge *cause* experiment, not a wedge fix). WiFi-off did not
  cure an *active* wedge (23:58) — consistent with coex being a trigger, not
  the mechanism.
- `btusb.enable_autosuspend=0`: belt-and-braces only; device is already
  `control=on`. Keep.
- `iwlwifi power_save` already N (off). No action.
- Falsified-as-cause list stands: autosuspend-as-live-cause, ERTM, BT-side
  arm, accept-list re-add — none retried.

### 3) BlueZ procon cable pairing (stage 5) — VERDICT: SHIP AS DESIGNED

Assumptions (session init `0x80 02/03/02`; read-then-decide x2000 magic 0x95
+ stored host MAC; GET_LTK `0x01 0x02` == stored, XOR 0xAA, byte-reverse;
3-step `0x01/0x02/0x03`; wired arm `0x08 00`; sixaxis/server gates confined
to the input profile): **none contradicted** by the new wedge knowledge.
§3.6 (GET_LTK == stored-key read-out) is settled and unchanged.

Interplay found by the audit (methodological, not code):
- Stage 5 does **not** fix the wedge. Cable-pairing gives a correct stored
  LTK; the host still drops scan-enable to 0x00 on its own schedule.
- Stage 5 acceptance (cable-only pair → unplug → button → wake → input) and
  the stage-2 checkpoint (≥10 sleep→press→input cycles) will **false-fail**
  whenever the wedge strikes.
- **Required practice for both:** run with `unwedge.sh` available; record
  every wedge occurrence in the result log; count only wedge-free cycles
  toward acceptance, and separately report wedge frequency. Do not interpret
  a non-reconnect as a pairing/key failure until `0x0019` has been read.

### 4) Housekeeping found by the audit

- testplan/02 REVISION 4 contains the retracted fingerprint (0x001a-based)
  and calls rfkill "the rescue". Appended REVISION 5 correction below.
- No other testplan/stage file cites the retracted fingerprint as evidence.

## Pre-deploy checklist (next step, not done)

1. kernelPatches: stage-3 passive + btusb-remote-wake-v2 (+ existing
   kernelParams additions: btusb.enable_autosuspend=0, iwlwifi.bt_coex_active=0)
2. BlueZ: build 5.86 with 05-wiring-pairing patch via package override,
   keep UserspaceHID=true + disable_ertm=1; wire `hardware.bluetooth`
3. unwedge.sh on PATH + (optionally) hotkey with NOPASSWD rule
4. Testplans 03/04 run with the wedge-aware method above; no commits.