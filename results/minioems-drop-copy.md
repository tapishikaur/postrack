# PosTrack on a live MiniOEMS drop copy

The real MiniOEMS FIX 4.4 gateway ran with its drop copy on; its FIX load client sent 20000 orders
(account ACC1: a buy then a sell of 100 at each symbol's reference price, so every sell trades with the buy
before it); PosTrack consumed the drop copy over TCP and snapshotted to PostgreSQL every 5,000 fills.

| Check | Result |
|---|---|
| Trade reports the FIX client received | 20000 |
| Fills PosTrack logged from the drop copy (seq 1..N, no gaps or repeats) | 20000 |
| Match | **yes** |

Every buy here is matched by a sell at the same price, so every position should end flat with zero realized
P&L; the report below shows what PosTrack and the reference computed.

PosTrack: stopped at seq 20000; this run applied 20000, ignored 0 duplicates, recovered 0 gaps, connected 1 times, wrote 4 snapshots (0 failed), 0 marks

Machine: OS: MINGW64_NT-10.0-26200 3.6.7-fb42d713.x86_64 x86_64; CPU: Intel(R) Core(TM) Ultra 7 165U; logical CPUs: 14

## PosTrack vs the reference on MiniOEMS's drop copy

**Result: PASS, 0 breaks**

| Check | Compared | Breaks |
|---|---|---|
| Fill log = fills sent (every field) | 20,000 fills | 0 |
| Final positions: net, realized, cash, mark, unrealized, every open lot | 4 positions, 0 lots | 0 |
| Snapshots in PostgreSQL vs reference at the same seq | 4 snapshots | 0 |

Totals over all positions (reference): realized 0.0000, unrealized at final marks 0.0000.

Torn bytes at the end of the fill log: 0.

Reference: Python 3.14.6, standard library only; reconciled in 0.5 s.

### Positions (from PosTrack's report)

```
PosTrack report: positions and P&L per strategy
last applied seq: 20000

ACC1: 4 positions, realized 0, unrealized 0, total 0

ALL: 4 positions, realized 0, unrealized 0, total 0
```
