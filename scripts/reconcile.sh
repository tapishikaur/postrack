#!/usr/bin/env bash
# End to end, then proof: the fill simulator sends N fills (with some duplicates and gaps
# injected), PosTrack processes them with snapshots in PostgreSQL every 10,000 fills, and
# reconcile.py checks every fill, every final position and every snapshot against the
# independent Python reference.
#
# Usage:  scripts/reconcile.sh [N=1000000] [results file]     (after scripts/build.sh)
# Env:    POSTRACK_DB  a libpq connection string; default: the throwaway local cluster
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "$ROOT/scripts/common.sh"
source "$ROOT/scripts/run_lib.sh"
N="${1:-1000000}"
OUT="${2:-$ROOT/results/reconcile-$N.md}"
trap stop_children EXIT

pick_db
new_run_dir "reconcile-$N"
SCHEMA="recon_$(date +%s)"
echo "run files: $RUN"
start_upstreams "$N"

start=$(date +%s)
"$BIN/postrack$EXE" --upstream "127.0.0.1:$SIM_PORT" --prices "127.0.0.1:$PRICE_PORT" --log "$RUN/fills.log" \
  --db "$DB" --schema "$SCHEMA" --keep 0 --snapshot-every 10000 --stop-after-seq "$N" --report "$RUN/report" \
  | tee "$RUN/postrack.out"
elapsed=$(( $(date +%s) - start ))

"$BIN/postrack_export$EXE" --db "$DB" --schema "$SCHEMA" --out "$RUN/snapshots"
status=0
"$PYTHON" "$ROOT/reference/reconcile.py" --sent "$RUN/sent.bin" --log "$RUN/fills.log" --prices "$RUN/prices.csv" \
  --report "$RUN/report" --snapshots "$RUN/snapshots" --out "$RUN/reconcile.md" \
  --title "Reconciliation: $N fills" \
  --note "Upstream: fill_simulator, seed 42, 8 strategies x 20 symbols, duplicate rate ${DUP_RATE:-0.001}, gap rate ${GAP_RATE:-0.0001}. PosTrack: --sync batch, a PostgreSQL snapshot every 10,000 fills, all kept. PosTrack took $elapsed s from start to its final snapshot." \
  --note "PosTrack's own account of the run: $(grep 'stopped at seq' "$RUN/postrack.out" | sed 's/^postrack: //')" \
  --note "Machine: $(machine | paste -sd ';' - | sed 's/;/; /g')" || status=$?
mkdir -p "$(dirname "$OUT")"
cp "$RUN/reconcile.md" "$OUT"
echo "report: $OUT"
exit $status
