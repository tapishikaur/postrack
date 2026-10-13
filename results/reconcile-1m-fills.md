# Reconciliation: 1000000 fills

**Result: PASS, 0 breaks**

| Check | Compared | Breaks |
|---|---|---|
| Fill log = fills sent (every field) | 1,000,000 fills | 0 |
| Final positions: net, realized, cash, mark, unrealized, every open lot | 160 positions, 12,665 lots | 0 |
| Snapshots in PostgreSQL vs reference at the same seq | 95 snapshots | 0 |

Totals over all positions (reference): realized -1,049,997.1800, unrealized at final marks 183,056.3500.

Torn bytes at the end of the fill log: 0.

Upstream: fill_simulator, seed 42, 8 strategies x 20 symbols, duplicate rate 0.001, gap rate 0.0001. PosTrack: --sync batch, a PostgreSQL snapshot every 10,000 fills, all kept. PosTrack took 30 s from start to its final snapshot.

PosTrack's own account of the run: stopped at seq 1000000; this run applied 1000000, ignored 997 duplicates, recovered 109 gaps, connected 110 times, wrote 95 snapshots (0 failed), 4040 marks

Machine: OS: MINGW64_NT-10.0-26200 3.6.7-fb42d713.x86_64 x86_64; CPU: Intel(R) Core(TM) Ultra 7 165U; logical CPUs: 14

Reference: Python 3.14.6, standard library only; reconciled in 71.3 s.
