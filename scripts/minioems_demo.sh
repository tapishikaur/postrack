#!/usr/bin/env bash
# PosTrack fed live by MiniOEMS (github.com/tapishikaur/mini-oems): the real FIX gateway
# runs with its drop copy on, its FIX load client sends orders that trade, and PosTrack
# consumes the drop copy. Then: the number of fills PosTrack logged must equal the number
# of trade reports the FIX client received, and the Python reference must agree with
# PosTrack's positions exactly.
#
# Usage:  scripts/minioems_demo.sh [ORDERS=20000] [results file]
# Env:    MINI_OEMS_DIR  the mini-oems checkout (default: ../mini-oems), already built
#                        with its scripts/build.sh
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "$ROOT/scripts/common.sh"
source "$ROOT/scripts/run_lib.sh"
ORDERS="${1:-20000}"
OUT="${2:-$ROOT/results/minioems-drop-copy.md}"
OEMS="${MINI_OEMS_DIR:-$ROOT/../mini-oems}"
OEMS="$(cd "$OEMS" && pwd)"
OEMS_BIN="$OEMS/build/release"
case "$OEMS" in */OneDrive*) [ -n "${LOCALAPPDATA:-}" ] && OEMS_BIN="$(cygpath -u "$LOCALAPPDATA")/mini-oems/build/release" ;; esac
DROP_PORT="${DROP_PORT:-9201}"
trap stop_children EXIT

pick_db
new_run_dir "minioems-$ORDERS"
SCHEMA="minioems_$(date +%s)"
echo "run files: $RUN"

# The gateway, with its drop copy on DROP_PORT; its own files go in the run folder.
(cd "$RUN" && exec "$OEMS_BIN/mini_oems$EXE" "$OEMS/config/bench_acceptor.cfg" "$OEMS/config/instruments.csv" \
  "$OEMS/config/accounts.csv" "$DROP_PORT" > "$RUN/mini_oems.out" 2>&1) &
CHILDREN+=($!)
wait_for_line "$RUN/mini_oems.out" "listening"

# Orders over FIX: buys and sells at each symbol's reference price, so every sell trades.
"$OEMS_BIN/bench/oems_load_client$EXE" "$OEMS/config/load_client.cfg" "$OEMS/config/instruments.csv" ACC1 \
  "$ORDERS" 100 | tee "$RUN/load_client.out"
TRADES=$(grep "trade reports:" "$RUN/load_client.out" | awk '{print $3}')
echo "the FIX client received $TRADES trade reports"

# PosTrack on the drop copy, stopping once it has every fill.
"$BIN/postrack$EXE" --upstream "127.0.0.1:$DROP_PORT" --log "$RUN/fills.log" --db "$DB" --schema "$SCHEMA" \
  --keep 0 --snapshot-every 5000 --stop-after-seq "$TRADES" --report "$RUN/report" | tee "$RUN/postrack.out"
"$BIN/postrack_export$EXE" --db "$DB" --schema "$SCHEMA" --out "$RUN/snapshots"

# The drop copy's own record of what it sent is PosTrack's input here, so the log is checked
# against itself for delivery (seq 1..N, no gaps, count = trade reports) and the reference
# recomputes every position from it independently.
echo "ts_ns,symbol,price" > "$RUN/no-marks.csv"
status=0
"$PYTHON" "$ROOT/reference/reconcile.py" --sent "$RUN/fills.log" --log "$RUN/fills.log" --prices "$RUN/no-marks.csv" \
  --report "$RUN/report" --snapshots "$RUN/snapshots" --out "$RUN/reconcile.md" \
  --title "PosTrack vs the reference on MiniOEMS's drop copy" || status=$?
# (Paths go as arguments, not inside code: Git Bash converts arguments for Windows Python.)
LOGGED=$("$PYTHON" "$ROOT/reference/postrack_ref.py" --fills "$RUN/fills.log" | awk 'NR==1 {print $1}')
[ "$LOGGED" = "$TRADES" ] || status=1

mkdir -p "$(dirname "$OUT")"
{
  echo "# PosTrack on a live MiniOEMS drop copy"
  echo
  echo "The real MiniOEMS FIX 4.4 gateway ran with its drop copy on; its FIX load client sent $ORDERS orders"
  echo "(account ACC1: a buy then a sell of 100 at each symbol's reference price, so every sell trades with the buy"
  echo "before it); PosTrack consumed the drop copy over TCP and snapshotted to PostgreSQL every 5,000 fills."
  echo
  echo "| Check | Result |"
  echo "|---|---|"
  echo "| Trade reports the FIX client received | $TRADES |"
  echo "| Fills PosTrack logged from the drop copy (seq 1..N, no gaps or repeats) | $LOGGED |"
  echo "| Match | $([ "$LOGGED" = "$TRADES" ] && echo "**yes**" || echo "**NO**") |"
  echo
  echo "Every buy here is matched by a sell at the same price, so every position should end flat with zero realized"
  echo "P&L; the report below shows what PosTrack and the reference computed."
  echo
  echo "PosTrack: $(grep 'stopped at seq' "$RUN/postrack.out" | sed 's/^postrack: //')"
  echo
  echo "Machine: $(machine | paste -sd ';' - | sed 's/;/; /g')"
  echo
  sed 's/^# /## /' "$RUN/reconcile.md"
  echo
  echo "### Positions (from PosTrack's report)"
  echo
  echo '```'
  cat "$RUN/report/summary.txt"
  echo '```'
} > "$OUT"
echo "report: $OUT"
exit $status
