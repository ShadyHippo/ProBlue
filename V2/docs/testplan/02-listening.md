# Test 02 — listening (stage 1a)

**Stage:** 1a · **Change:** bluez `adapter.c` connectable keep (+ R2/R5 only if
needed) · **Status:** in progress

## Hypothesis

With the arm held constant (present, or proven unneeded by 01), the controller
wake page is not *heard* (or not *accepted*) by the host when the GUI is closed
/ discoverable is off: page scan off (R1), accept list empty after reboot (R2),
or default page-scan duty missing the brief page (R5).

## A/B — arm held constant

| Trial | Host state | Expected if R1 needed |
|---|---|---|
| A | discoverable OFF, GUI closed | btmon: page **arrives from controller**, host sends nothing / rejects |
| B | same + connectable keep | page accepted → connect |

Trial A/B decides R1. Add R2 only if the failure reproduces right after a
reboot/bluetoothd restart. Add R5 only if btmon shows the page being repeatedly
missed by duty cycle (page arrives but host responds too late / never).

Note: btmon distinguishes "controller never pages" (arm problem — go back to
01) from "controller pages, host ignores" (listening problem — this stage).

## Result

2026-09-10, stock BlueZ 5.86, arm held constant (proven unneeded by 01).
Raw traces in `V2/logs/stage0/`.

- Trial A (discoverable OFF; host auto page-scan `0x02` after disconnect;
  `1a-nodisc.btmon.log`): controller `Connect Request` 15:47:47.55 → connect
  15:47:47.67. **Page heard and accepted with page-scan-only.**
- Trial B (discoverable ON, `0x03`; `1a-scan.btmon.log`): same — controller
  `Connect Request` 15:31:52.96 → connect 15:31:53.09; link key reused.
- SYNC-click sleep (`syncsleep.btmon.log`): controller `Connect Request`
  15:49:23 → connect. x5000 after wake = `0xFF` (sleep does not re-flag
  shipment).
- 14:01 session (`wake-press.btmon.log`): page scan `0x02` on, repeated
  normal presses over 40 s → **no page at all**. The failing case is
  controller-side (nothing emitted), not host-side.

### 2026-09-10 later — canonical successful reconnect (`wake-a.btmon.log`)

Re-paired OTA first (sync-button flow; new link key `25 31 28 85 04 4c fc 66 …`),
then: host idle (no UI, no discovery session), one short press of **A** →
reconnect. Raw trace in `V2/logs/stage0/`. Annotated sequence (times 17:11:xx,
connection handle 256):

| # | log line | time | event |
|---|---|---|---|
| 1 | 8 | 33.375793 | `Connect Request (0x04)` from `20:0B:CF:34:F1:BD` — **controller pages host** (class 0x000508, ACL) |
| 2 | 14 | 33.375898 | Host `Accept Connection Request`, Role **Central** (host stays central, no role switch) |
| 3 | 20 | 33.496786 | Role Change → Central |
| 4 | 28 | 33.502761 | **Connect Complete: Success** (127 ms after request); encryption off pending auth |
| 5 | 34–112 | 33.503+ | Feature reads, page 0/1 (standard set: Interlaced Page Scan, SSP, Simultaneous LE+BR/EDR) |
| 6 | 41 | 33.503851 | `Write Scan Enable: No Scans` — host stops scanning once connected |
| 7 | 124 | 33.518755 | `Link Key Request` → Reply with **stored key** (no pairing happened; key is today's re-pair key) |
| 8 | 148 | 33.528851 | `MGMT Device Connected` (`Pro Controller`, class 0x000508) |
| 9 | 159 | 33.546767 | `Encryption Change: Enabled with E0` |
| 10 | 287–597 | 33.8+ | bluetoothd input handshake (`a2 01 00`…`0b`, 12 subcommands — byte-identical to stage-0 traces) |
| 11 | 298+ | 33.8+ | Controller streams: short `a1 3f` status, then standard-mode input `a1 21`/`a1 30` |

Counts (entire file): 1 page, 1 accept + complete (Success), 1 link-key reply,
1 encryption change, **0 Discovery/Inquiry events**, 0 disconnects, 0 timeouts;
input reports at ~67 Hz cadence (~15 ms apart) for the whole capture. File ends
mid-stream (btmon killed after report #1029 — no close marker, expected).

What this trace shows:

- Wake = **controller pages, host accepts**. The host's only requirement is
  page scan `0x02`; the capture ran with no GUI, no discoverable, no discovery
  session, and the page was caught 3.7 s after capture start, page-scan-only.
- Stored link key reused end-to-end; nothing re-pairs during wake.
- Host role stays **Central**; the host never pages the controller. This is the
  canonical shape — compare failed sessions against it (cf. session 2: identical
  until the 96.80 s `Connection Timeout` death).

### Companion tests same day (`testa-burst.btmon.log`, `testb-awake.btmon.log`)

- Disconnect → 12 s idle, no input → **zero** `Connect Request`s. The 0.65 s
  cadence seen in session 2 was button-mashing, not automatic retry.
  **Auto-retry falsified.**
- Host `Create Connection` into an idle controller → `Page Timeout`
  (bluetoothctl surfaces `br-connection-create-socket`; HCI log shows Page
  Timeout). Host paging cannot wake/attach an idle controller.
  **Host-initiated wake falsified** — and now permanently closed: per user
  experience + RE (`x5000 0xFF` = *Triggered* fast connect, scan-on-button-
  press), host paging has NEVER connected this controller, on this PC or on a
  Switch. The controller is **initiator-only**: radio off while asleep, it
  never page-scans, it only pages on button press. The persistent-paging test
  (test 2) is therefore dropped — no host-side page attempt can ever wake it.

## Verdict

- **R1: NOT closed by this data; kept.** BlueZ page-scans after a disconnect in
  CLI runs, so host listening works — but the daily GUI (BluJay) idle scan
  state is unverified and remains the prime suspect for daily failures.
- **R5: deleted (falsified)** — default duty heard the page (`0x02`).
- **R2: PROMOTED to prime suspect (2026-09-11)** — see revision below.
- New open item: controller wake-page **intermittency** (characterize press
  window/cooldown; long-press fallback confirmed).
- 2026-09-10 later suite: no auto-retry burst (12 s idle, 0 pages) and host
  paging into an idle controller page-timeouts → wake is strictly
  controller-initiated; when it fails the controller emits nothing, which BlueZ
  cannot provoke or fix. No BlueZ reconnect-patch direction survives.
- **Recovery observation (same day,~hours later): after a dead episode (presses
  → zero pages, host listening), the controller revived with ZERO intervention
  after waiting a few hours** (no sync, no re-pair, no USB). The dead state is a
  transient firmware wedge with a long self-clear TTL (somewhere between ~5 min
  — the full-sleep press test gap — and hours; exact bound unmeasured). Re-
  arm happens on its own after a long enough cold sleep; pairing is never lost.
  Practical recovery order when dead: wait out the LED-full-sleep → sync re-init
  → USB (battery check, if even sync emits nothing).

### REVISION 2026-09-11 — the wedge is HOST-side; R2 is the prime suspect

The controller-side-wedge reading above is **withdrawn** on new evidence:

1. **Reset fixes it, reproducibly and instantly.** Every Linux host reset —
   reboot or a BlueJay "Toggle Bluetooth" adapter power cycle — reliably
   restores reconnects. The controller had been working "every single time"
   right after a host reset, drifting into failing periods afterwards (onset
   *entirely random*, not time/cycle counted).
2. **The controller is demonstrably paging during failure.** While the host
   is in a failing period, the controller's LEDs strobe (auto-reconnect page
   mode) — and resetting the Linux Bluetooth side *during the strobe*
   reconnects **with no new button press**. That proves the page was being
   emitted all along.
3. **btmon shows NOTHING during failure.** No Connect Request ever reaches
   HCI. A deaf *radio* (page-scan accept list silently dropped / scan state
   lost / radio autosuspend hang) produces exactly this: the link layer
   filters or never hears the page, so the host stack sees nothing.

This re-reads the 14:01 "dead zone" too: we recorded "host scanning, zero
pages" — but a setting-level scan with a deaf radio yields zero Connect
Requests regardless of what the controller does. The controller may never
have been the failure.

**Root cause: R2 — the BR/EDR page-scan accept list (PATCHED 2026-09-11).** The
kernel clears it on power events; BlueZ only re-adds at the bonding
transition (`device.c:6880`), so a bonded device whose entry was dropped is
silently filtered while "Paired" appears healthy. Random onset matches
arbitrary power/suspend events. **Fixed**: `adapter_start` now re-adds all
bonded BR/EDR devices to the accept list on every power-on (broader than
V1's cable-pairing-only filter — covers OTA-paired controllers). Ported into
`V2/src/bluez/src/adapter.c`; in `stages/05-wiring-pairing-5.86.patch`;
compile-verified. **Functional confirm pending**: the fix must make a power
toggle NOT kill reconnects, and a reconnect must survive a toggle at the
next failing period.

Other candidates (in order): connectable scan-state silently off (would show
in `bluetoothctl show`); Intel USB-autosuspend radio hang (random; only a
radio reset clears). If the R2 patch does not hold, these are next.

R-note: ERTM (`bluetooth.disable_ertm=1` + `UserspaceHID=true`) was deployed
independently 2026-09-11 and did **not** fix this — it addresses a different
failure (connect-then-Local-terminate within seconds), which we do not see.

**Discriminating probes (still worth running at a failing period, to confirm
R2 was the layer):**
1. `bluetoothctl show` → is `Connectable` / `Discoverable` still listed in
   Current settings?
2. `journalctl -k --since -20min | grep -iE 'hci0|bluetooth|usb.*suspend|runtime'`
   → any radio reset / autosuspend around onset?
3. Live rescue without full reset: `sudo btmgmt connectable on` — if that
   rescues instantly → scan-state loss; else `systemctl restart bluetooth`
   → daemon state; else adapter toggle/reboot → radio-level hang.
The instant-rescue result plus (1)/(2) confirms the layer; record trace
`dead-period.<ts>.btmon.log` + the probe outputs.

### REVISION 2 (2026-09-11, full failing-period btmon) — radio-layer deafness; R2 falsified as the fix

A full trace of a real failure (dead window → UI bounce → instant connect)
was captured. It **falsifies R2 as the wedge fix** and points to Intel
radio-layer deafness:

- **Page scan was ON during the entire dead window** (`Write Scan Enable:
  Page Scan (0x02)` at 46.35 s, unchanged through 68.5 s) — so this is not a
  host-stopped-scanning failure. The kernel keeps page scan on via
  `HCI_CONNECTABLE || disconnected_accept_list_entries`
  (`hci_update_scan_sync()` in 6.18 `net/bluetooth/hci_sync.c`).
- **Zero `Connect Request` events in the dead window.** The accept list is
  checked in software in `hci_conn_request_evt()` (6.18
  `net/bluetooth/hci_event.c`): a miss with connectable off would call
  `hci_reject_conn()`, but the CONNECT REQUEST event itself would still
  appear in btmon first. None did — so this is not an accept-list filter
  decision; the page never reached the HCI layer.
- **The controller was paging the whole time**: after the UI bounce (`Reset`
  + full re-init ≈70.9 s), the next `Connect Request` from the controller
  arrived at 71.009 s — 43 ms, no button press. The power cycle only touches
  the host.
- The same trace shows an `Intel PTT Switch Notification` (vendor event
  0x26) — Intel WiFi/BT coexistence, a known source of silent radio
  deafness. Suspect: the radio stops delivering pages while reporting
  healthy; only a controller reset restores it.

**Actions:** R2 is demoted to hardening-only (protects the
`disconnected_accept_list_entries` page-scan path, which this failure does
not exercise). The open investigation is Intel-side: (a) correlates the PTT
notification timing with the wedge onset; (b) test `dmesg`/USB-autosuspend
(`/sys/bus/usb/devices/*/power/control`) for the Intel BT device; (c) check
`journalctl -k` for hci0 vendor/firmware errors at onset; (d) consider
rfkill/USB-autosuspend workaround or a firmware update before any further
BlueZ patch.

### REVISION 3 (2026-09-11, live failing-period session) — CONFIRMED: Intel radio-firmware wedge; ALL userspace theories dead

Live data taken DURING a real failing period (14:0x, before any rescue):

- `hciconfig hci0` → `UP RUNNING PSCAN` — the host **was** page-scanning
  during the failure. Scan-state-loss theory dead.
- `btmgmt connectable on` → **did NOT reconnect on button push** — the last
  userspace theory dead. (Likely no-op at HCI level: connectable was already
  implied by accept-list-kept page scan, so `hci_update_scan_sync()` early-
  returned; and even a real re-assert changed nothing.)
- `sudo systemctl restart bluetooth` → adapter comes back OFF
  (`powerOnBoot=false` machine) — daemon restart is NOT a rescue on this
  config (BlueJay "bluetooth off" = expected).
- `bluetoothctl power on` → **instant `Connected: yes`, no button press** —
  the controller had been paging the whole time; only the power-on's
  `HCI_OP_RESET` + radio re-init restored hearing.
- `dmesg` silent at onset: no hci0/PTT/firmware/coex messages near the
  failure. The radio fails silently while reporting healthy.

**Corrected rescue ladder (this machine):** daemon restart ≠ rescue.
Rescue = adapter power cycle only (`power off && power on`, or the UI
toggle). Anything short of `HCI_OP_RESET` cannot help.

**Verdict: the wedge lives in the Intel radio firmware** (stops delivering
pages; HCI state stays "healthy"; only a controller reset clears it). Not
fixable by any BlueZ/userspace change. Remaining work is Intel-side:
firmware version/update, USB-autosuspend as trigger, WiFi/BT coexistence
(PTT) correlation. R2 stays hardening-only, unproven as a fix.
### REVISION 4 (2026-09-11, final) — wedge fingerprint measured; rfkill is the rescue; trigger still open

- **Measured fingerprint (wedge-probe-live, still wedged):** `Read Scan
  Enable` (HCI `0x03 0x001a`) → reply `plen 4` `02 1A 0C 00`, **missing the
  scan_enable byte** — the controller cannot even report its own scan state.
  `Read Page Scan Activity` (0x1b) still returns full valid params, so the
  controller knows how to scan but is not enabled. 20 s passive btmon:
  **zero events** (btsnoop 278 B = housekeeping only).
- **Rescue ladder verified live:** rung 1 `hcitool cmd 0x03 0x0003` (HCI
  Reset) → still truncated, no fix. Rung 2 `btmgmt power off; sleep 1;
  power on` → still truncated, no fix. **Rung 3 `rfkill block;
  sleep 1; rfkill unblock` → FIXED** (reconnected, no button press).
  Reason (kernel 6.18.46): rfkill closes the HCI device synchronously and
  reopens via full init that rewrites scan enable; mgmt power-off is
  deferred to `power_off` work (raced/BUSY) and `hci_update_scan_sync()`
  skips re-asserting page scan when cached `HCI_PSCAN` already matches.
- **Stage closed pending trigger hunt:** `tools/wedge-probe.sh` run at every
  future wedge; preceding sequence 01-arm / 02-listening evidence stands;
  see `docs/wedge-investigation.md` (normative) + KEY_CONTEXT §3.7.

### REVISION 5 (2026-09-12) — fingerprint RETRACTED; mechanism confirmed; rfkill downgraded

Supersedes the fingerprint claim in REVISION 4 and the "rfkill is the rescue"
wording. The `Read Scan Enable` prints above (0x001a) were **Write Scan
Enable with no parameter** — a probe bug (`hcitool cmd 0x03 0x0019` is the
read; docs/wedge-investigation.md banner + logs/wedge-fix-20260912.txt).

- Confirmed mechanism: while wedged, `0x03 0x0019` → `02 19 0C 00 00`
  (scan_enable genuinely 0x00, well-formed reply). Rescue = re-arm
  `0x03 0x001a 0x02` → re-read 0x02 → button presses
  (`tools/unwedge.sh`). rfkill reinit FAILED 2026-09-11 23:58 (controller
  had stopped paging).
- Import for this stage: R1/R2 page-scan/accept-list theories were never
  the wedge; the final wedge state is host scan-register 0x00 + kernel
  cached-flag blind spot. This stage's listening conclusions (host CAN
  listen post-disconnect when scan is armed) remain valid.
