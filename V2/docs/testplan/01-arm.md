# Test 01 — the arm (stage 1b)

**Stage:** 1b · **Change:** bluez `device.c` BT-side `0x08 00` arm · **Status:** done

## Hypothesis

A sleeping controller won't page the host on a *normal* button press if it is
in shipment/LPM state (x5000=0x01). Sending `0x08 00` over the BT channel after
every connection enables Triggered Broadcom Fast Connect (the Switch does this
after every connection — nxbt capture).

## Setup — host pinned KNOWN-listening (do not skip)

- `bluetoothctl discoverable on` (BlueZ keeps connectable on while
  discoverable → the host *is* listening by construction).
- `btmon` running for every trial.
- Press with **normal presses only**. Long presses can wake even in shipment
  mode (KEY_CONTEXT §2.5) and would produce a false "arm not needed".

## A/B

| Trial | Arm installed | Host state | Expected if hypothesis is true |
|---|---|---|---|
| A | no | listening | no page from controller (btmon shows nothing) |
| B | yes | listening | page transmitted → connect → input |

Record per trial: btmon trace (page transmitted?), time to connect, whether a
second press was needed.

Prior from stage 0: if x5000 read `0xFF`, expect trial A to pass and the arm to
be **unneeded** (delete R3); if `0x01`, expect trial A to fail → arm needed.

## Result

_(summary + raw-log links)_

## Verdict

_(needed / not needed — and whether the flag persists across re-sleeps, i.e.
whether the arm must be sent per-connection or once suffices)_