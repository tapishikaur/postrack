# Helpers for the end-to-end scripts (reconcile.sh, crash_test.sh). Sourced, not run.

PYTHON="$(command -v python3 || command -v python)"
SIM_PORT="${SIM_PORT:-9101}"
PRICE_PORT="${PRICE_PORT:-9102}"
CHILDREN=()

# The database to use: POSTRACK_DB if set, otherwise the throwaway local cluster.
pick_db() {
  if [ -n "${POSTRACK_DB:-}" ]; then
    DB="$POSTRACK_DB"
  else
    bash "$ROOT/scripts/pg_local.sh" start > /dev/null
    DB="$POSTRACK_LOCAL_DB"
  fi
}

# A new folder for this run's files, outside the repo.
new_run_dir() {
  RUN="$POSTRACK_BUILD_ROOT/runs/$1-$(date +%Y%m%d-%H%M%S)"
  mkdir -p "$RUN"
}

# Waits until a file contains a line, or fails after 60 s.
wait_for_line() {
  local file="$1" pattern="$2"
  for _ in $(seq 1 600); do
    grep -q "$pattern" "$file" 2> /dev/null && return 0
    sleep 0.1
  done
  echo "timed out waiting for '$pattern' in $file" >&2
  return 1
}

# Starts the fill simulator and the price replayer for N fills; writes what they send.
start_upstreams() {
  local fills="$1"
  "$BIN/fill_simulator$EXE" --fills "$fills" --port "$SIM_PORT" --dump "$RUN/sent.bin" --prices-out "$RUN/prices.csv" \
    --duplicate-rate "${DUP_RATE:-0.001}" --gap-rate "${GAP_RATE:-0.0001}" --rate "${SIM_RATE:-0}" \
    > "$RUN/simulator.out" 2>&1 &
  CHILDREN+=($!)
  wait_for_line "$RUN/simulator.out" "listening"
  "$BIN/price_replayer$EXE" --prices "$RUN/prices.csv" --port "$PRICE_PORT" > "$RUN/replayer.out" 2>&1 &
  CHILDREN+=($!)
  wait_for_line "$RUN/replayer.out" "listening"
}

stop_children() {
  local pid
  for pid in "${CHILDREN[@]:-}"; do
    [ -n "$pid" ] && kill "$pid" 2> /dev/null || true
  done
  for pid in "${CHILDREN[@]:-}"; do
    [ -n "$pid" ] && wait "$pid" 2> /dev/null || true
  done
}

# Kills a process with no chance to clean up: no final flush, no snapshot, no report.
kill_hard() {
  local pid="$1"
  if [ -r "/proc/$pid/winpid" ]; then
    taskkill //F //PID "$(cat "/proc/$pid/winpid")" > /dev/null 2>&1 || true # Windows: TerminateProcess
  else
    kill -9 "$pid" 2> /dev/null || true
  fi
  wait "$pid" 2> /dev/null || true
}

# The machine, without its name.
machine() {
  echo "OS: $(uname -srm)"
  if [ -r /proc/cpuinfo ]; then
    grep -m1 "model name" /proc/cpuinfo | sed 's/.*: /CPU: /'
  elif command -v powershell.exe > /dev/null; then
    echo "CPU: $(powershell.exe -NoProfile -Command '(Get-CimInstance Win32_Processor | Select-Object -First 1).Name' | tr -d '\r')"
  fi
  command -v nproc > /dev/null && echo "logical CPUs: $(nproc)"
}
