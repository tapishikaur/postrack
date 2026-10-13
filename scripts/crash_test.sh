#!/usr/bin/env bash
# Crash-recovery proof. The fill simulator serves N fills; PosTrack is killed hard (no
# flush, no final snapshot, no clean shutdown: TerminateProcess on Windows, SIGKILL on
# Linux) at random moments, KILLS times, and restarted each time. Once, while it is down,
# the last record of its fill log is torn in half, as a crash in the middle of a write
# would leave it. Finally PosTrack runs to the end and everything is reconciled against
# the Python reference: zero breaks means every fill was applied exactly once and every
# recovery was exact.
#
# Usage:  scripts/crash_test.sh [N=1000000] [KILLS=10] [SEED=2026] [results file]
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "$ROOT/scripts/common.sh"
source "$ROOT/scripts/run_lib.sh"
N="${1:-1000000}"
KILLS="${2:-10}"
SEED="${3:-2026}"
OUT="${4:-$ROOT/results/crash-test-$N.md}"
trap stop_children EXIT

pick_db
new_run_dir "crash-$N"
SCHEMA="crash_$(date +%s)"
echo "run files: $RUN"
# A paced feed, like a live drop copy, so the kills land all through the run.
export SIM_RATE="${SIM_RATE:-100000}"
start_upstreams "$N"

RANDOM=$SEED # bash's RANDOM is repeatable from a seed
TEAR_ROUND=2 # early, so it always happens
MIN_MS="${MIN_MS:-300}"
SPREAD_MS="${SPREAD_MS:-1000}" # wait MIN_MS .. MIN_MS + SPREAD_MS after start-up, then kill
now_ms() { date +%s%3N; }

start_postrack() { # $1: output file
  "$BIN/postrack$EXE" --upstream "127.0.0.1:$SIM_PORT" --prices "127.0.0.1:$PRICE_PORT" --log "$RUN/fills.log" \
    --db "$DB" --schema "$SCHEMA" --keep 3 --snapshot-every 10000 --stop-after-seq "$N" --report "$RUN/report" \
    > "$1" 2>&1 &
  PT=$!
}
recovery_line() { grep -m1 "recovered" "$1" | sed 's/^postrack: recovered //'; }

rows=()
kills=0
for round in $(seq 1 "$KILLS"); do
  out="$RUN/round-$round.out"
  t0=$(now_ms)
  start_postrack "$out"
  wait_for_line "$out" "recovered"
  delay_ms=$((MIN_MS + RANDOM % SPREAD_MS))
  sleep "$((delay_ms / 1000)).$(printf '%03d' $((delay_ms % 1000)))"
  if ! kill -0 "$PT" 2> /dev/null; then
    rows+=("| $round | $(recovery_line "$out") | finished before the kill | | |")
    break
  fi
  kill_hard "$PT"
  ran_ms=$(($(now_ms) - t0)) # measured: includes process start-up and the kill itself
  kills=$((kills + 1))
  size=$(stat -c %s "$RUN/fills.log")
  torn=""
  if [ "$round" -eq "$TEAR_ROUND" ]; then
    cut=$((1 + RANDOM % 40))
    truncate -s "-$cut" "$RUN/fills.log"
    torn="cut the last $cut bytes"
  fi
  rows+=("| $round | $(recovery_line "$out") | killed after running ~$ran_ms ms | $size | $torn |")
  echo "round $round: killed after ~$ran_ms ms, log $size bytes ${torn:+($torn)}"
done

out="$RUN/final.out"
start_postrack "$out"
wait "$PT"
final_recovery="$(recovery_line "$out")"
echo "final run: recovered $final_recovery"

"$BIN/postrack_export$EXE" --db "$DB" --schema "$SCHEMA" --out "$RUN/snapshots"
status=0
"$PYTHON" "$ROOT/reference/reconcile.py" --sent "$RUN/sent.bin" --log "$RUN/fills.log" --prices "$RUN/prices.csv" \
  --report "$RUN/report" --snapshots "$RUN/snapshots" --out "$RUN/reconcile.md" \
  --title "Reconciliation after the crashes" || status=$?

mkdir -p "$(dirname "$OUT")"
{
  echo "# Crash-recovery test: $N fills, $kills hard kills"
  echo
  echo "PosTrack was killed hard $kills times at random moments (seed $SEED) and restarted each time; once, its fill log's"
  echo "last record was torn while it was down. Then it ran to the end. The upstream sent $SIM_RATE fills a second, like a"
  echo "live drop copy, and injected duplicates (rate ${DUP_RATE:-0.001}) and gaps (rate ${GAP_RATE:-0.0001}) throughout."
  echo "Snapshots went to PostgreSQL every 10,000 fills. Each round's recovery line shows what the previous kill left behind."
  echo
  echo "| Round | Recovery at the start of the round | How it ended | Fill log bytes at the kill | Damage while down |"
  echo "|---|---|---|---|---|"
  printf '%s\n' "${rows[@]}"
  echo "| final | $final_recovery | ran to seq $N, stopped cleanly | | |"
  echo
  echo "Final run: $(grep 'stopped at seq' "$out" | sed 's/^postrack: //')"
  echo
  echo "Machine: $(machine | paste -sd ';' - | sed 's/;/; /g')"
  echo
  sed 's/^# /## /' "$RUN/reconcile.md"
} > "$OUT"
echo "report: $OUT"
exit $status
