# V2 testplans — method and ordering

**Reference reading:** [`PLAN.md`](../../PLAN.md) (method + necessity ledger) and
[`../KEY_CONTEXT.md`](../KEY_CONTEXT.md) (protocol facts). Testplans are
reference material for a single test — they never override the plan or
KEY_CONTEXT.

Each stage has one file describing its falsification test. A stage is done
when its file carries a **result** and a **verdict**, the raw log is linked
under `logs/`, the summary in `docs/results/<date>/`, and the `PLAN.md` ledger
rows are filled.

## The controlled A/B method

Change **one variable**; pin the other side; capture `btmon` every time.
See `../KEY_CONTEXT.md` §2.5 for the long-press trap and §2.1 for the BT/USB
offset conventions.

| Stage file | Tests |
|---|---|
| `00-baseline.md` | stock-only forensics: OTA pair, controller-state reads, smoking tests |
| `01-arm.md` | arm (1b): controller wake-side, host known-listening |
| `02-listening.md` | page-scan/connectable (1a): host listen-side, arm held constant |
| `03-kernel-passive.md` | USB passivity: exclusive hidraw + BT-revert-while-docked |
| `04-wiring-pairing.md` | in-place wired pairing (protocol + integration) |
| `05-edges.md` | reproduced-failure only edge cases |

Checkpoints (stage 2 reconnect product, stage 6 full product) are acceptance
lists in `PLAN.md`, not separate testplan files.

Raw logs go under `V2/logs/` and are gitignored; summaries stay in
`docs/results/<date>/` and the testplan files hold summaries + verdicts
(committed).