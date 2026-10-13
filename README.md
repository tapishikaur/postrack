# PosTrack

[![CI](https://github.com/tapishikaur/postrack/actions/workflows/ci.yml/badge.svg)](https://github.com/tapishikaur/postrack/actions/workflows/ci.yml)

A position and P&L service in C++20. It receives fills over TCP, keeps each strategy's
position, realized and unrealized P&L per symbol with exact integer money, and survives
being killed at any moment without losing a fill or counting one twice.

- **FIFO tax lots**, exact to the last 1/10,000: 128-bit integers, never floating point.
- **Write-ahead fill log** on disk, checksummed (CRC-32C), with torn writes from a crash detected and repaired.
- **Snapshots in PostgreSQL** (libpqxx, one transaction each, bulk `COPY`) every N fills.
- **Crash recovery** = latest snapshot + replay of the log after it, then resume the feed from the last logged seq.
- **Boost.Asio**, one thread, a length-prefixed little-endian binary protocol.
- **Proved** by an independent Python reference over 1,000,000 fills, and by killing it hard over and over.

## Results

Every record below is in [`results/`](results), produced by the scripts in this repo. CI repeats smaller versions
of both proofs on every push (Linux, PostgreSQL 18), and puts the reports on the run's summary page.

| Proof | Result | Record |
|---|---|---|
| 1,000,000 fills vs the Python reference: every fill in the log, every final position and open lot, every one of 95 PostgreSQL snapshots | **0 breaks** | [reconcile-1m-fills.md](results/reconcile-1m-fills.md) |
| 10 hard kills at random moments (no flush, no clean shutdown), a torn log record, then reconciliation of all 1,000,000 fills | **0 breaks**; final P&L identical to the uninterrupted run, to the last 1/10,000 | [crash-test-1m-fills.md](results/crash-test-1m-fills.md) |
| Fed live by the [MiniOEMS](https://github.com/tapishikaur/mini-oems) FIX gateway's drop copy: 20,000 orders over FIX | 20,000 fills logged = 20,000 trade reports; **0 breaks** | [minioems-drop-copy.md](results/minioems-drop-copy.md) |
| A real bug planted on purpose (LIFO instead of FIFO): can the proof fail? | yes: **431 breaks**, and a unit test fails | [planted-bug-check.md](results/planted-bug-check.md) |
| Unit, property and end-to-end tests, Release and Debug | 47 C++ + 8 Python, all passing | [CI](https://github.com/tapishikaur/postrack/actions/workflows/ci.yml) |

The upstream injected duplicates and sequence gaps on purpose in every run (rates 0.1% and 0.01%): about a
thousand duplicates ignored and a hundred gaps recovered per million fills.

**Speed** ([bench-windows-laptop.txt](results/bench-windows-laptop.txt): Windows 11 laptop, Intel Core Ultra 7
165U, PostgreSQL 18):

| Measure | Result |
|---|---|
| Applying fills to positions, in memory | 5.0 million fills/s |
| Log + apply, one fsync per 500 fills (the default: one per socket read) | 140,000 fills/s |
| Log + apply, one fsync per fill | 274 fills/s: why group commit matters |
| Recovery: check every checksum and replay a 1,000,000-record log | 0.42 s |
| Snapshot of the final state (160 positions, 12,665 open lots): save / load | 119 ms / 23 ms |

On this machine one fsync costs about 3.6 ms, so durability, not accounting, sets the pace: the fsync policy
is the trade-off between speed and how much a power cut could take back from the disk (never more than the
upstream can resend).

## How it works

```
 fill_simulator / drop copy ── TCP ──► decoder ──► seq check ──► fill log (append + CRC) ──► position book
   keeps every fill sent              bytes to     ≤ last: duplicate, ignore    fsync once per       FIFO lots,
   resends from any seq   ◄── Resume  messages     > last+1: gap, reconnect     socket read          realized P&L
                                                                                                       │ every N fills
 price_replayer ── TCP ── marks ──────────────────────────────────────────► unrealized P&L            ▼
                                                                                              PostgreSQL snapshot
 start-up (and after any crash): load the latest snapshot (seq S) ─► replay log records after S ─► Resume(last logged seq)
```

What a crash at each point means:

| Crash happens… | After restart |
|---|---|
| before the fill reached the log | not logged, so never applied; Resume asks for it again |
| after the log write, before applying | replayed from the log |
| after applying, before the snapshot | the older snapshot plus the log: applied exactly once |
| in the middle of a snapshot | the transaction never committed: the previous snapshot is used |
| in the middle of a log record | its checksum fails, it's cut off, the upstream resends it |
| damage *before* the end of the log | not a crash pattern: PosTrack refuses to start, a person must look |

The rules are written down before either implementation: [docs/accounting.md](docs/accounting.md) (FIFO,
realized/unrealized, the identity *realized + unrealized = cash + position × mark*) and
[docs/protocol.md](docs/protocol.md) (frames, messages, the fill-log record format, CRC-32C).

## Project layout

```
include/postrack/   types (exact money), position (FIFO lots), protocol, crc32c, fill_log,
                    snapshot (PostgreSQL + in-memory), service, sim (test upstreams), cli
src/                the library and the postrack executable
tools/              fill_simulator, price_replayer, postrack_export (snapshots → CSV), postrack_bench
reference/          postrack_ref.py (independent reference), reconcile.py, their tests
tests/              GoogleTest: unit, property, PostgreSQL and end-to-end tests
scripts/            build.sh, pg_local.sh, reconcile.sh, crash_test.sh, bench.sh, minioems_demo.sh
docs/               accounting.md, protocol.md
results/            the proof records
```

## Build and run

Linux (Ubuntu 24.04) or GitHub Codespaces (the dev container installs everything):

```bash
sudo apt install -y g++ cmake ninja-build libboost-dev libpq-dev postgresql python3
bash scripts/build.sh             # Release; builds, starts a throwaway PostgreSQL, runs all tests
bash scripts/reconcile.sh         # 1,000,000 fills vs the Python reference -> results/
bash scripts/crash_test.sh        # 10 hard kills, a torn record, then reconciliation -> results/
bash scripts/bench.sh             # timings -> results/
bash scripts/minioems_demo.sh     # live from MiniOEMS's drop copy (mini-oems checked out and built beside this repo)
```

**With MiniOEMS.** `mini_oems <session.cfg> <instruments.csv> <accounts.csv> 9201` serves every fill on
127.0.0.1:9201 in this protocol (the account is the strategy); point PosTrack at it with `--upstream 127.0.0.1:9201`.

Windows: MSYS2 MinGW-w64 (`pacman -S mingw-w64-x86_64-{gcc,cmake,ninja,boost,postgresql}`) plus an installed
PostgreSQL for the server programs, then the same scripts from Git Bash. Build output goes to
`%LOCALAPPDATA%\postrack\build` when the source is inside OneDrive.

**Database.** `scripts/pg_local.sh` runs a throwaway PostgreSQL cluster: port 54329, user `postrack`, no
password, local connections only, data under the build folder. It never touches another PostgreSQL server.
To use a real server instead, set `POSTRACK_DB` (and `POSTRACK_TEST_DB` for the tests) to a libpq connection
string.

**By hand:**

```bash
fill_simulator --fills 100000 --port 9101 --dump sent.bin --prices-out prices.csv &
price_replayer --prices prices.csv --port 9102 &
postrack --upstream 127.0.0.1:9101 --prices 127.0.0.1:9102 --log fills.log \
         --db "host=localhost port=54329 dbname=postrack user=postrack" --snapshot-every 10000 \
         --stop-after-seq 100000 --report report/
```

`--sync batch|every|none` sets when the log is forced to disk (default `batch`: once per socket read);
`--db none` keeps snapshots in memory only; Ctrl+C stops cleanly (final flush, snapshot, report).

## Tests

| Area | What is checked |
|---|---|
| Money | exact decimal parsing (refuses anything that would round), 128-bit text round trips over the full range |
| Positions | FIFO closes oldest first, shorts, flips long↔short, flat, huge values, restore from a snapshot; 100,000 random fills checking the accounting identity and lot invariants after every one |
| Protocol | every message round-trips; the stream split at **every** byte and in random chunks decodes the same; zero, oversized, unknown, short and over-long frames rejected; 20,000 random garbage inputs never crash |
| Fill log | write/reopen/append; a torn last record cut at **every** byte position recovers; zero-filled tail; damage before the end is refused and nothing is cut; seq order enforced |
| PostgreSQL | save/load exact, extreme 128-bit money, keep-N pruning, a save that fails half way leaves the previous snapshot intact, schema-name checking |
| End to end | real service + real TCP servers with injected duplicates and gaps match direct application; crash mid-run then recovery is exact; torn tail after a crash; recovery from the log alone |
| Python reference | its own tests: FIFO, flips, the identity, CRC-32C check value, torn tails, damage, seq order |

## Why these choices

- **FIFO, not average cost.** Every FIFO number is an exact integer sum of `price × qty`; average cost needs a
  division, so two implementations would have to round identically. Total P&L is the same either way.
- **A local append-only log plus periodic snapshots,** not a database insert per fill. Appends are sequential and
  cheap; one fsync covers a whole socket read (group commit). Snapshots bound recovery time. This is how
  databases and trading journals work.
- **Log first, then apply.** Whatever crashed, the state can be rebuilt from the log, and the upstream resends
  whatever never reached it, so every fill is applied exactly once.
- **Sequence numbers and Resume,** like FIX resends or Kafka offsets (Kafka itself isn't used): duplicates are
  ignored, a gap triggers a reconnect from the last logged seq.
- **One thread.** All state changes happen on the Asio event loop: no locks, deterministic replays. Snapshots are
  written on the same thread.
- **An independent reference written from the docs, not ported from the C++,** so a bug isn't copied.

## Corporate actions: how they would break this

Not handled here, on purpose; this is what each would do to a position service that ignores it.

- **Split (2-for-1):** the broker shows twice the shares at half the price. Without a split event the position is
  half the broker's (a break) and the halved mark looks like a 50% loss. Fix: multiply every lot's quantity and
  divide its price by the ratio, keeping cost basis.
- **Reverse split:** the opposite; fractional shares are usually paid in cash, which adds a cash entry.
- **Dividend:** the price drops on the ex-date; without the cash entry that drop shows as a loss.
- **Symbol change or ticker reuse:** keying by ticker splits one holding in two, or merges two companies. Real
  systems key by a stable instrument ID (ISIN, FIGI).
- **Merger or spin-off:** one instrument becomes others at a ratio, sometimes with cash; cost basis must be split
  across the results.

## Limitations and next steps

- One currency; no fees or commissions.
- Corporate actions (above) aren't applied.
- Marks aren't persisted: after a restart unrealized P&L waits for the price feed (which replays on connect).
  Stale marks aren't flagged yet.
- Snapshots are written on the event-loop thread; a background writer would remove that pause.
- One upstream connection; a real deployment would take several drop-copy feeds.

## License

MIT
