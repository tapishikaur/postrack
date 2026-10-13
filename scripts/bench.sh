#!/usr/bin/env bash
# Runs postrack_bench (apply, fill log under each fsync policy, recovery, snapshots in
# PostgreSQL) and writes the results, with the machine described but not named.
#
# Usage:  scripts/bench.sh [results file]      (after scripts/build.sh)
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "$ROOT/scripts/common.sh"
source "$ROOT/scripts/run_lib.sh"
OUT="${1:-$ROOT/results/bench-$(uname -s | cut -d_ -f1 | tr '[:upper:]' '[:lower:]').txt}"
pick_db
new_run_dir "bench"

{
  echo "== postrack_bench, $(date -u +%Y-%m-%d)"
  machine
  echo
  "$BIN/postrack_bench$EXE" --fills 1000000 --dir "$RUN" --db "$DB" --schema bench
} | tee "$RUN/bench.txt"
mkdir -p "$(dirname "$OUT")"
cp "$RUN/bench.txt" "$OUT"
echo "results: $OUT"
