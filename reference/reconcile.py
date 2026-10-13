"""Reconciles PosTrack (C++) against the independent Python reference.

Checks, and counts every difference as a break:

  1. the fill log PosTrack wrote holds exactly the fills the upstream sent (same seqs,
     every field equal): no fill lost, duplicated or changed;
  2. PosTrack's final positions equal the reference's, computed from the upstream's own
     record of what it sent: net quantity, realized P&L, cash, every open lot, the mark,
     and unrealized P&L at the final marks;
  3. every snapshot PosTrack wrote to PostgreSQL equals the reference's state at that
     snapshot's seq;
  4. the accounting identity holds for every final position.

Writes a Markdown report and exits 1 if there is any break.

    python reconcile.py --sent fills.bin --log postrack-fills.log --prices prices.csv
                        --report report_dir [--snapshots export_dir] --out reconcile.md
"""

from __future__ import annotations

import argparse
import csv
import os
import platform
import sys
import time
from collections import defaultdict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import postrack_ref as ref  # noqa: E402


def money(raw: int) -> str:
    sign = "-" if raw < 0 else ""
    raw = abs(raw)
    return f"{sign}{raw // ref.PRICE_SCALE:,}.{raw % ref.PRICE_SCALE:04d}"


def load_report(report_dir: str):
    positions = {}
    with open(os.path.join(report_dir, "positions.csv"), newline="", encoding="utf-8") as f:
        for row in csv.DictReader(f):
            positions[(row["strategy"], row["symbol"])] = row
    lots = defaultdict(list)
    with open(os.path.join(report_dir, "lots.csv"), newline="", encoding="utf-8") as f:
        for row in csv.DictReader(f):
            lots[(row["strategy"], row["symbol"])].append((int(row["queue_pos"]), int(row["qty"]), int(row["price_raw"])))
    return positions, {k: tuple((q, p) for _, q, p in sorted(v)) for k, v in lots.items()}


def load_snapshots(export_dir: str):
    seqs = []
    with open(os.path.join(export_dir, "snapshots.csv"), newline="", encoding="utf-8") as f:
        for row in csv.DictReader(f):
            seqs.append(int(row["last_seq"]))
    states = {seq: {} for seq in seqs}
    with open(os.path.join(export_dir, "snapshot_positions.csv"), newline="", encoding="utf-8") as f:
        for row in csv.DictReader(f):
            states[int(row["last_seq"])][(row["strategy"], row["symbol"])] = [
                int(row["net_qty"]), int(row["realized_raw"]), int(row["cash_raw"]), []]
    with open(os.path.join(export_dir, "snapshot_lots.csv"), newline="", encoding="utf-8") as f:
        for row in csv.DictReader(f):
            states[int(row["last_seq"])][(row["strategy"], row["symbol"])][3].append(
                (int(row["queue_pos"]), int(row["qty"]), int(row["price_raw"])))
    return {seq: {k: (v[0], v[1], v[2], tuple((q, p) for _, q, p in sorted(v[3]))) for k, v in s.items()}
            for seq, s in states.items()}


def main() -> int:
    parser = argparse.ArgumentParser(description="Reconcile PosTrack against the Python reference")
    parser.add_argument("--sent", required=True, help="simulator dump: every fill the upstream sent")
    parser.add_argument("--log", required=True, help="the fill log PosTrack wrote")
    parser.add_argument("--prices", required=True, help="the price file the price feed played")
    parser.add_argument("--report", required=True, help="PosTrack's report directory (positions.csv, lots.csv)")
    parser.add_argument("--snapshots", help="postrack_export output (snapshots in PostgreSQL)")
    parser.add_argument("--out", required=True, help="Markdown report to write")
    parser.add_argument("--title", default="Reconciliation")
    parser.add_argument("--note", action="append", default=[], help="extra line for the report (repeatable)")
    args = parser.parse_args()
    started = time.time()
    breaks: list[str] = []

    counts = defaultdict(int)

    def brk(category: str, text: str) -> None:
        if len(breaks) < 50:
            breaks.append(text)
        counts[category] += 1
        counts["breaks"] += 1

    # 1. Log vs sent, fill by fill.
    sent = list(ref.read_fills(args.sent))
    log_stats = ref.ReadStats()
    logged = list(ref.read_fills(args.log, log_stats))
    if len(logged) != len(sent):
        brk("log_breaks", f"fill log has {len(logged)} fills, the upstream sent {len(sent)}")
    for a, b in zip(sent, logged):
        if a != b:
            brk("log_breaks", f"fill seq {a.seq}: logged {b} but sent {a}")
    counts["fills"] = len(sent)

    # Reference state, from what was sent (not from PosTrack's log), with every snapshot point.
    snapshots = load_snapshots(args.snapshots) if args.snapshots else {}
    wanted = set(snapshots)
    book = ref.Book()
    ref_at = {}
    for fill in sent:
        book.apply(fill)
        if fill.seq in wanted:
            ref_at[fill.seq] = book.frozen()
    final = book.frozen()
    marks = ref.read_final_marks(args.prices)

    # 2. Final positions.
    positions, lots = load_report(args.report)
    if set(positions) != set(final):
        missing = sorted(set(final) - set(positions))[:5]
        extra = sorted(set(positions) - set(final))[:5]
        brk("final_breaks", f"position keys differ: missing {missing}, extra {extra}")
    total_realized = total_unrealized = 0
    for key, (net, realized, cash, ref_lots) in sorted(final.items()):
        row = positions.get(key)
        if row is None:
            continue
        counts["positions"] += 1
        counts["open_lots"] += len(ref_lots)
        mark = marks.get(key[1])
        unrealized = book.positions[key].unrealized(mark)
        total_realized += realized
        total_unrealized += unrealized
        checks = [
            ("net_qty", int(row["net_qty"]), net),
            ("realized", int(row["realized_raw"]), realized),
            ("cash", int(row["cash_raw"]), cash),
            ("mark", int(row["mark_raw"]) if row["mark_raw"] else None, mark),
            ("unrealized", int(row["unrealized_raw"]), unrealized),
            ("lots", lots.get(key, ()), ref_lots),
        ]
        for name, got, want in checks:
            if got != want:
                brk("final_breaks", f"{key} {name}: PosTrack {got}, reference {want}")
        # 4. The identity (docs/accounting.md), on the reference's own numbers.
        if mark is not None and realized + unrealized != cash + net * mark:
            brk("final_breaks", f"{key}: accounting identity fails")

    # 3. Every snapshot.
    for seq in sorted(snapshots):
        counts["snapshots"] += 1
        if snapshots[seq] != ref_at.get(seq):
            differing = [k for k in set(snapshots[seq]) | set(ref_at.get(seq, {}))
                         if snapshots[seq].get(k) != ref_at.get(seq, {}).get(k)]
            brk("snapshot_breaks", f"snapshot at seq {seq}: {len(differing)} positions differ, e.g. {sorted(differing)[:3]}")

    ok = counts["breaks"] == 0
    elapsed = time.time() - started
    lines = [
        f"# {args.title}",
        "",
        f"**Result: {'PASS, 0 breaks' if ok else 'FAIL, ' + str(counts['breaks']) + ' breaks'}**",
        "",
        "| Check | Compared | Breaks |",
        "|---|---|---|",
        f"| Fill log = fills sent (every field) | {counts['fills']:,} fills | {counts['log_breaks']} |",
        f"| Final positions: net, realized, cash, mark, unrealized, every open lot | {counts['positions']:,} positions, {counts['open_lots']:,} lots | {counts['final_breaks']} |",
        f"| Snapshots in PostgreSQL vs reference at the same seq | {counts['snapshots']:,} snapshots | {counts['snapshot_breaks']} |",
        "",
        f"Totals over all positions (reference): realized {money(total_realized)}, "
        f"unrealized at final marks {money(total_unrealized)}.",
        "",
        f"Torn bytes at the end of the fill log: {log_stats.torn_bytes}.",
    ]
    for note in args.note:
        lines.append("")
        lines.append(note)
    lines += ["", f"Reference: Python {platform.python_version()}, standard library only; "
              f"reconciled in {elapsed:.1f} s."]
    if breaks:
        lines += ["", "## Breaks (first 50)", ""] + [f"- {b}" for b in breaks]
    with open(args.out, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines) + "\n")
    print("\n".join(lines[:9]))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
