# Stage 0 + 1b/1a — results 2026-09-10

Normative reading: `PLAN.md` and `docs/KEY_CONTEXT.md`. This file is the
committed summary of what the stock machine can and cannot do, and why the arm
hypothesis died. Raw evidence is under `logs/stage0/` (gitignored); every
claim below names its file.

## 1. Method

Stock NixOS 26.05, BlueZ 5.86, kernel 6.18.46. Controller: Pro Controller
`20:0B:CF:34:F1:BD`. Read-only probes via `tools/stage0/procon_probe.py`
(subcommands `0x02`, `0x05`, `0x10` SPI, plus one `0x08` disarm write in the
1b test). All connection events captured with `btmon`.

## 2. Evidence inventory

| File | Contents |
|---|---|
| `logs/stage0/pair.btmon.log` | fresh OTA pairing: full SSP (IO Capability, User Confirmation, Simple Pairing Complete, Link Key Notification) |
| `logs/stage0/probe-*.txt` | device info, page-list, SPI x2000/x2018/x5000/x6000/x2020/x203C |
| `logs/stage0/wake-press.btmon.log` | early wake test: host page-scan on, presses → no events (controller never paged) |
| `logs/stage0/1b-arm.txt`, `1b-x5000-pre.txt`, `1b-x5000-post.txt` | arm `0x08 00` acked; x5000 `0xFF` → `0xFF` |
| `logs/stage0/1b-wake.btmon.log` | armed + host connectable/discoverable, presses → no reconnect, no page |
| `logs/stage0/1a-scan.btmon.log` | **success**: controller `Connect Request` at 15:31:52.96, host accepts, linked 15:31:53.09, link key reused |
| `logs/stage0/1a-nodisc.btmon.log` | **success with discoverable OFF**: page scan `0x02` only; controller `Connect Request` at 15:47:47.55 → connect |
| `logs/stage0/syncsleep.btmon.log` | **success after SYNC-click sleep**: controller `Connect Request` 15:49:23 → connect |
| `docs/testplan/01-arm.md` | 1b result + verdict (R3 falsified) |

## 3. Findings

### 3.1 Pairing (Q1)
OTA sync-button pairing works; the controller does standard SSP. V1's
"doesn't SSP" claim is false. Link key is reused on reconnect
(`Link Key Request Reply`, no re-pair).

### 3.2 Controller state (Q2)
- Device: Pro, fw `0x0421`, MAC `20:0B:CF:34:F1:BD`.
- Pairing record x2000 matches the RE docs byte-for-byte: section stride
  `0x26`, magic `0x95` (used)/`0x00` (previous), size `0x22`, checksum, host
  MAC big-endian, LTK little-endian, capability byte at section+`0x24`.
- Capability = **`0x08` (PC)** — not Switch `0x68`.
- x5000 = **`0xFF`** (shipment normal) from the first read, and stays `0xFF`
  through: fresh OTA pairs, host-initiated disconnects, and SYNC-click sleeps.

### 3.3 Reconnect behavior (Q3) — the model
1. A **normal button press makes the controller wake and page the host**:
   `HCI Event: Connect Request (0x04)` addressed to us, observed repeatedly.
2. The host reconnects if it is **page-scanning**. BlueZ enables page scan
   (`0x02`) by itself right after a disconnect. Discoverable (`0x03`) also
   works; **discoverable is NOT required** (1a-nodisc).
3. When the host misses the page (page scan off / not listening), the result
   is total silence and the only path is re-pair — the daily symptom.
4. **The controller's wake-page is intermittent.** It connected on most trials
   (15:31, 15:47, 15:49; operator reports 2/3 after SYNC-sleep) but once
   (14:01 session) produced **no page at all across repeated presses** while
   the host was page-scanning. The failing case is controller-side, not
   host-side.

### 3.4 The arm (Q-1b) — falsified
- x5000 was already `0xFF`; `0x08 00` acked and wrote `0xFF`→`0xFF` (no-op).
- The controller woke and paged with **no arm installed** (1a-scan, 1a-nodisc,
  syncsleep all ran fresh-paired, unarmed).
- SYNC-click sleep does **not** re-flag x5000 to `0x01` (read `0xFF` after
  wake).
- Delivered `0x08 00` per connection would change nothing on this unit.

## 4. Decisions (ledger deltas)

- **R3 (arm) — deleted (falsified).** Wake works unarmed when the host
  listens.
- **R5 (page-scan window==interval) — deleted (falsified).** The page was
  heard on default duty with page-scan-only (`0x02`).
- **R1 (keep-connectable) — kept, not yet closed.** BlueZ page-scans after a
  disconnect in our CLI runs, but whether the daily GUI (BluJay) leaves the
  adapter page-scanning during idle is **unverified** — prime suspect for the
  daily "never reconnects."
- **R2 — untouched.** Only test if wake fails after a reboot/bluetoothd
  restart.

## 5. Open questions (stage 1a finish / stage 7)

1. Daily GUI scan state: does BluJay keep the adapter page-scanning when idle?
   (Experiment B was proposed but not yet run.)
2. Controller wake intermittency: press-window and cooldown characterization;
   when it fails, nothing is heard because nothing is emitted. Long-press
   fallback confirmed (it always wakes; §3.5 trap).
3. Capability `0x08` vs `0x68`: does the firmware gate fast-connect on the
   stored host capability for a Switch host? Untested; only relevant if the
   PC-host path needs the Switch behavior.
4. Why the Switch re-sends `0x08 00` on every connection (nxbt capture) when
   x5000 persists: unexplained by docs; no effect observed here.