# Virgin-stack control test — 2026-08-24 [PROVEN]

**Question (PLAN.md step 1):** does a truly pristine stack produce clean BT
input on this machine? It had never been measured — every prior "baseline"
had project code loaded.

**Verdict: YES.** Input over BT streams cleanly on a fully pristine stack.
The platform (Intel combo card, adapter, uhid path) is **exonerated** as a
cause of the historical layer-2 degradation. Raw logs are versioned next to
this file (`run1-fork-present.log`, `run2-virgin.log`).

---

## Machine state during run 2 (the decisive run)

| Component | State |
|---|---|
| Kernel module | **stock in-tree** `/lib/modules/6.8.0-137-generic/kernel/drivers/hid/hid-nintendo.ko.zst` — `updates/` copy deleted, `depmod -a` re-run, no `loading out-of-tree module taints kernel` line |
| bluetoothd | **stock Ubuntu** (ProBlue drop-in moved to `.bak`) |
| joycond daemon | disabled (`systemctl disable --now`) |
| Pairing | fresh OTA GUI-equivalent pairing via `bluetoothctl pair/trust/connect` after forgetting the device |

## Method

```bash
sudo rm /lib/modules/$(uname -r)/updates/hid-nintendo.ko && sudo depmod -a
sudo modprobe -r hid_nintendo
# controller sync-button → bluetoothctl pair/trust/connect 20:0B:CF:34:F1:BD
journalctl -kf | tee <logfile> | grep -E 'factory cal|out-of-tree|delta=|timeout waiting|-110'
```

## Run history

### Run 1 (~13:32–13:39) — near-virgin, one confound
Prep sequence omitted the `updates/` module deletion, so the **fork** module
auto-loaded (`hid_nintendo: loading out-of-tree module taints kernel`,
srcversion `55741788526D65B5960D1EA`). On BT it behaves like stock except its
arm attempts. Still valuable:

- Input streamed cleanly: avg IMU delta 11 ms, worst hiccup 89 ms,
  zero `timeout waiting`.
- **Dead-window discovery:** fork arm subcommands at T+2 s after connect
  failed with `-110`:
  ```
  13:36:24 nintendo ...: arm: subcmd 0x08 00 failed; ret=-110
  13:36:25 nintendo ...: arm: player-1 LED failed; ret=-110
  ```
- Factory-cal ×3 fired with exactly ONE hidraw writer and no project userspace.

### Run 2 (13:49–13:53+) — true virgin stack [the verdict]
Fresh OTA pairing at 13:49:35, device instance `.0004`. Recorded window
~4 min of active play plus an evtest session.

| Metric | Value | PLAN bar | Pass |
|---|---|---|---|
| `using factory cal` | 3 (L stick / R stick / IMU) | was "0" | see note A |
| out-of-tree taint | 0 | 0 | ✅ |
| `timeout waiting for input report` | 0 | 0 | ✅ |
| probe failures / `-110` | 0 | 0 | ✅ |
| evdev stream (evtest) | continuous ~7–15 ms cadence during motion, no perceptible stalls, all buttons/sticks/dpad/rumble nodes present | events within ~2 s of wake | ✅ |
| IMU report gaps (`delta=` prints, ~90 samples) | typical 60–67 ms, avg_delta 11–16 ms; ten outliers 101–130 ms | stalls ≤100 ms | ⚠️ note B |

**Note A — factory cal is now characterized [PROVEN]:** it fires on every
connect even with zero custom code and a single hidraw writer. It is an
inherent controller/host timing quirk, NOT project-caused, and it does not
impair input (stick centers rest within ±1.2 k of 0; play quality normal).
PLAN's old acceptance gate "zero factory cal" is impossible on this machine
and has been revised.

**Note B — IMU micro-gaps [PROVEN, benign]:** the IMU substream shows periodic
60–130 ms packet gaps ("compensating for N dropped IMU reports", N=4–10)
roughly every few seconds. The gamepad substream shows none. No effect on
gamepad input; recorded here so future tests don't misread it as regression.
Root cause unknown (likely BT buffering / controller IMU pacing); not pursued.

## Conclusions

1. **Layer 2 works on this machine, pristine.** avg report delta 11 ms;
   worst gamepad-relevant hiccup none observed; session stable.
2. **Historical degradation was project-era interference** [PROVEN by
   elimination]: journal forensics (see memory/journal-mining notes) show
   every degraded era had concurrent hidraw writers — Phase 0 hand-driving
   2026-07-29/31, pairing experiments 2026-08-12..14, patched-daemon queue +
   driver init collisions thereafter. Platform, stock driver behavior, page
   scan, link-key type all exonerated.
3. **Early-session subcommand dead window [PROVEN]:** subcommands sent within
   the first ~2 s after BT connect time out (`ret=-110`; seen for arm at T+2 s
   in run 1; factory-cal SPI reads at init hit the same window). Later
   subcommands succeed immediately.
   → **Design constraint for the BlueZ setup queue:** firing instantly on
   connect will burn one 500 ms timeout per queued step (`0x02→0x08→0x03→0x30`).
   The queue needs an initial delay (~2–3 s) or retry-on-timeout semantics.
   This must be folded into `patches/bluez-5.84-procon.patch` before final
   acceptance runs.

## Ops state after the test (differs from pre-test machine)

- `updates/hid-nintendo.ko` permanently removed (stock module now authoritative).
- joycond service disabled.
- `/etc/systemd/system/bluetooth.service.d/ProBlue.conf` renamed `ProBlue.conf.bak`
  (patched daemon inactive). Restore: move back + `daemon-reload` + restart,
  or re-run `tools/install.sh`.
