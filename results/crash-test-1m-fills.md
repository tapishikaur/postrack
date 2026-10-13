# Crash-recovery test: 1000000 fills, 10 hard kills

PosTrack was killed hard 10 times at random moments (seed 2026) and restarted each time; once, its fill log's
last record was torn while it was down. Then it ran to the end. The upstream sent 15000 fills a second, like a
live drop copy, and injected duplicates (rate 0.001) and gaps (rate 0.0001) throughout.
Snapshots went to PostgreSQL every 10,000 fills. Each round's recovery line shows what the previous kill left behind.

| Round | Recovery at the start of the round | How it ended | Fill log bytes at the kill | Damage while down |
|---|---|---|---|---|
| 1 | no snapshot in 4.4298 ms; fill log has 0 records, replayed 0 in 1.3033 ms; resuming after seq 0 | killed after running ~6695 ms | 4120480 |  |
| 2 | snapshot at seq 61856 in 20.6027 ms; fill log has 63392 records, replayed 1536 in 14.4815 ms; resuming after seq 63392 | killed after running ~6601 ms | 8193705 | cut the last 2 bytes |
| 3 | snapshot at seq 123497 in 16.9861 ms; fill log has 126056 records, replayed 2559 in 35.9817 ms; cut a torn tail of 63 bytes; resuming after seq 126056 | killed after running ~7224 ms | 12754755 |  |
| 4 | snapshot at seq 195153 in 23.7013 ms; fill log has 196227 records, replayed 1074 in 46.3762 ms; resuming after seq 196227 | killed after running ~6474 ms | 16693040 |  |
| 5 | snapshot at seq 246637 in 23.5372 ms; fill log has 256816 records, replayed 10179 in 63.6863 ms; resuming after seq 256816 | killed after running ~6014 ms | 20296380 |  |
| 6 | snapshot at seq 309029 in 24.6448 ms; fill log has 312252 records, replayed 3223 in 81.8397 ms; resuming after seq 312252 | killed after running ~6432 ms | 24040900 |  |
| 7 | snapshot at seq 360388 in 25.7217 ms; fill log has 369860 records, replayed 9472 in 101.51 ms; resuming after seq 369860 | killed after running ~6622 ms | 28017015 |  |
| 8 | snapshot at seq 422327 in 23.845 ms; fill log has 431031 records, replayed 8704 in 101.632 ms; resuming after seq 431031 | killed after running ~6778 ms | 32010160 |  |
| 9 | snapshot at seq 483886 in 34.9493 ms; fill log has 492464 records, replayed 8578 in 158.713 ms; resuming after seq 492464 | killed after running ~6517 ms | 35965670 |  |
| 10 | snapshot at seq 545745 in 41.5071 ms; fill log has 553318 records, replayed 7573 in 174.253 ms; resuming after seq 553318 | killed after running ~7909 ms | 40818960 |  |
| final | snapshot at seq 617744 in 34.631 ms; fill log has 627984 records, replayed 10240 in 174.07 ms; resuming after seq 627984 | ran to seq 1000000, stopped cleanly | | |

Final run: stopped at seq 1000000; this run applied 372016, ignored 357 duplicates, recovered 43 gaps, connected 44 times, wrote 38 snapshots (0 failed), 4040 marks

Machine: OS: MINGW64_NT-10.0-26200 3.6.7-fb42d713.x86_64 x86_64; CPU: Intel(R) Core(TM) Ultra 7 165U; logical CPUs: 14

## Reconciliation after the crashes

**Result: PASS, 0 breaks**

| Check | Compared | Breaks |
|---|---|---|
| Fill log = fills sent (every field) | 1,000,000 fills | 0 |
| Final positions: net, realized, cash, mark, unrealized, every open lot | 160 positions, 12,665 lots | 0 |
| Snapshots in PostgreSQL vs reference at the same seq | 3 snapshots | 0 |

Totals over all positions (reference): realized -1,049,997.1800, unrealized at final marks 183,056.3500.

Torn bytes at the end of the fill log: 0.

Reference: Python 3.14.6, standard library only; reconciled in 30.3 s.
