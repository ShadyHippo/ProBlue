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
- **R2: untouched** — test only if wake fails after reboot.
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