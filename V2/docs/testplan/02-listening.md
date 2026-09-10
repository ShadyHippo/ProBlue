# Test 02 — listening (stage 1a)

**Stage:** 1a · **Change:** bluez `adapter.c` connectable keep (+ R2/R5 only if
needed) · **Status:** not started

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

_(summary + raw-log links)_

## Verdict

_(R1 needed? R2 needed? R5 needed? — each gets a ledger row with its own fate)_