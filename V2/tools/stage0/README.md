# Stage 0 — stock forensics (runbook)

Read `docs/KEY_CONTEXT.md` first; `PLAN.md` and `docs/KEY_CONTEXT.md` are the
only normative documents. This file is the procedure for stage 0 and nothing
else.

Goal: record the stock system's baseline and answer four questions.

1. Can the controller pair OTA (does it SSP)?
2. What state is the controller actually in — shipment byte (x5000), stored
   pairing (x2000), capability byte (x2024)?
3. What does the stock link actually do — cadence, and does a normal button
   press wake/reconnect it?
4. Does the adapter alias `Nintendo Switch` change anything? (expected: no)

Rules:
- **Read-only.** Nothing in this stage writes to the controller. Probes are
  `0x02`, `0x05`, `0x10` only. The arm (`0x08 00`) is stage 1b.
- One variable at a time; capture `btmon` whenever a connection event matters.
- Never long-press during a wake test — a long press can wake a shipment-state
  controller and be a false positive (KEY_CONTEXT §2.5).

## Run dir — set once per shell

```zsh
cd ~/Programming/ProBlue/V2
R=$PWD/logs/stage0
mkdir -p "$R"; echo "$R"
```

Raw artifacts and captures land in `logs/` (the only other writable place is
`/tmp`). `.gitignore` ignores `V2/logs/*`, so raw logs stay out of git. The
committed summary goes in `docs/testplan/00-baseline.md`.

Automation: `tools/stage0/stage0.zsh <phase>` does the tedious parts only
(`preflight`, `probes`, `cadence`, `alias`, `map`, `monitor`, `stop`,
`collect`). Pairing, connecting and button-press tests are manual, below.

## Steps

Run in order. Each step lists the state it needs, what to run, and what it
proves.

### 1. Preflight — automated

*State:* any (controller may be off).

```zsh
tools/stage0/stage0.zsh preflight
```

Writes `$R/00-preflight.log`. Proves: adapter MAC/alias, `powerOnBoot`, current
bond provenance (`Paired/Bonded/Trusted/CablePairing/LegacyPairing`), hidraw
nodes, whether a Bluetooth UI is running.

### 2. Clean OTA pair — manual *(optional, but it is the only proof for Q1)*

*State:* controller off at the start. This forgets the host-side bond.

```zsh
pkill -f blueman-applet; pkill -f blueman-tray
tools/stage0/stage0.zsh monitor 180 pairing
bluetoothctl remove 20:0B:CF:34:F1:BD </dev/null
```

Now **hold the small SYNC button** on top of the controller until the LEDs
cycle. Then run bluetoothctl interactively:

```zsh
bluetoothctl
```

Inside it, in order (wait for the controller to appear after `scan on`):

```
power on
agent on
default-agent
scan on
pair 20:0B:CF:34:F1:BD
trust 20:0B:CF:34:F1:BD
connect 20:0B:CF:34:F1:BD
info 20:0B:CF:34:F1:BD
quit
```

Then stop the capture:

```zsh
tools/stage0/stage0.zsh stop
```

Evidence: `$R/pairing.btmon.log`. Grep it for
`IO Capability`, `User Confirmation`, `Simple Pairing Complete`,
`Link Key Notification` — that is SSP happening. Skip this step if you are not
willing to re-pair; Q1 then stays unproven in this run.

### 3. Connect — manual

*State:* controller awake (one short button press).

```zsh
bluetoothctl connect 20:0B:CF:34:F1:BD </dev/null
sleep 3
bluetoothctl info 20:0B:CF:34:F1:BD </dev/null | tee "$R/info-connected.txt"
bluetoothctl trust 20:0B:CF:34:F1:BD </dev/null
```

Evidence: `$R/info-connected.txt` (`Connected: yes`, `Trusted: yes`).

### 4. Hidraw map — automated

*State:* controller connected.

```zsh
tools/stage0/stage0.zsh map
```

Writes `$R/hidraw-map.txt`. Identifies the controller's `/dev/hidrawN`.

### 5. Controller state probes — automated (read-only)

*State:* controller connected, idle.

```zsh
tools/stage0/stage0.zsh probes
```

Writes `$R/probe-*.txt`. Answers Q2. If everything reports `# NO REPLY`, the
kernel driver is eating raw output writes:

```zsh
sudo modprobe -r hid_nintendo
tools/stage0/stage0.zsh probes
sudo modprobe hid_nintendo
```

### 6. Input cadence — automated (5 min)

*State:* controller connected and left alone for the whole capture.

```zsh
tools/stage0/stage0.zsh cadence
```

Writes `$R/cadence-stock.log` and prints a summary. Clean link ≈ 7–17 ms;
30–120 ms = sniff-ish / link quality. Answers the Q3 half.

### 7. Wake, host connectable — manual

*State:* controller connected.

```zsh
bluetoothctl discoverable on </dev/null >/dev/null
tools/stage0/stage0.zsh monitor 90 wake-on
bluetoothctl disconnect 20:0B:CF:34:F1:BD </dev/null
```

Press **A once, short** within the 90 s, then:

```zsh
sleep 15
bluetoothctl info 20:0B:CF:34:F1:BD </dev/null | tee "$R/wake-on.info.txt"
tools/stage0/stage0.zsh stop
```

Evidence: `$R/wake-on.info.txt` plus `$R/wake-on.btmon.log` (does a page even
appear?). NOTE: because the device is trusted, the host may reconnect on its
own — record that too, it is data.

### 8. Wake, no UI — manual

*State:* blueman killed.

```zsh
pkill -f blueman-applet; pkill -f blueman-tray
bluetoothctl discoverable off </dev/null >/dev/null
tools/stage0/stage0.zsh monitor 90 wake-off
bluetoothctl disconnect 20:0B:CF:34:F1:BD </dev/null
```

Press **A once, short**, then:

```zsh
sleep 15
bluetoothctl info 20:0B:CF:34:F1:BD </dev/null | tee "$R/wake-off.info.txt"
tools/stage0/stage0.zsh stop
```

Together, steps 7–8 answer the Q3 wake half. If neither wakes on a normal
press, that is the x5000 prior for stage 1b — not a failure of this stage.

### 9. UI reconnect smoke — manual

*State:* controller connected or awake. Host-initiated only.

```zsh
bluetoothctl disconnect 20:0B:CF:34:F1:BD </dev/null
sleep 2
bluetoothctl connect 20:0B:CF:34:F1:BD </dev/null
sleep 3
bluetoothctl info 20:0B:CF:34:F1:BD </dev/null | tee "$R/smoke-connect.txt"
```

Proves the stored link key is valid; proves nothing about wake/page.

### 10. Alias A/B — automated (5 min)

*State:* controller connected and idle.

```zsh
tools/stage0/stage0.zsh alias
```

Writes `$R/cadence-nintendo.log` and prints a summary; restores the alias.
Compare with step 6 — expected no-op, which closes Q4/O1.

### 11. Bundle — automated

```zsh
tools/stage0/stage0.zsh collect
```

Writes `$R/collect.txt` (all artifacts except the raw btmon traces, plus
greps). Send that file, or just say "done" and the files can be read in place.

## Reading the probe output

Each reply line is `attempt=N ack=0x.. subcmd=0x.. data=<hex>`.

| Probe | Meaning |
|---|---|
| `02` | device info: `data[0..1]` fw, `data[2]` type (`03` = Pro), MAC `data[4..9]` |
| `05` | page-list state: `data=01` means a host pairing record is in memory |
| `10 0020000018` | SPI x2000, 24 B (section 1: magic, host MAC x2004-09 BE, LTK x200A, capability x2024) |
| `10 1820000018` | SPI x2018, 24 B (tail of section 1; section 2 magic at x2026) |
| `10 0050000010` | SPI x5000, 16 B: byte 0 is shipment — `01` = ON (button wake disabled), `FF` = normal |

Note the SPI read format: the subcommand data is a **32-bit little-endian
address + 1 length byte** (not 16-bit), and the reply payload is
`addr[4] + len[1] + data[len]`. The probe prints the decoded `spi @0x.. len=..`
line for `0x10`.

## Notes / caveats to carry into the report

- `hardware.bluetooth.powerOnBoot = false` today. It only affects wake across a
  cold boot; with the adapter powered manually, steps 7–8 still exercise the
  wake path. A true cold-boot wake test needs `true` + a reboot (stage 1).
- The `delta=` and `timeout waiting` strings come from the stock 6.18.46
  `hid_nintendo` module, exposed via dynamic debug — no debug patch.
- The adapter alias is restored after step 10.
