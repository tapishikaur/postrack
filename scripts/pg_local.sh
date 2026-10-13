#!/usr/bin/env bash
# A throwaway PostgreSQL cluster for tests and demos: port 54329, user "postrack", no
# password, accepting connections from this machine only, data under the build root.
# It never touches any other PostgreSQL server on the machine. To remove it, stop it and
# delete the pgdata folder under the build root.
#
# Usage: scripts/pg_local.sh start | stop | status
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "$ROOT/scripts/common.sh"
find_pg_bin

pgctl() { "$PG_BIN/pg_ctl$EXE" -D "$POSTRACK_PG_DATA" "$@"; }

case "${1:-status}" in
  start)
    if [ ! -f "$POSTRACK_PG_DATA/PG_VERSION" ]; then
      mkdir -p "$POSTRACK_PG_DATA"
      "$PG_BIN/initdb$EXE" -D "$POSTRACK_PG_DATA" -U postrack -A trust -E UTF8 --no-locale > "$POSTRACK_BUILD_ROOT/pg_initdb.log"
    fi
    if pgctl status > /dev/null 2>&1; then
      echo "PostgreSQL already running on port $POSTRACK_PG_PORT"
    else
      pgctl -w -l "$POSTRACK_BUILD_ROOT/pg_server.log" \
        -o "-p $POSTRACK_PG_PORT -c listen_addresses=localhost" start > /dev/null
      echo "PostgreSQL started on port $POSTRACK_PG_PORT"
    fi
    # The database, created once.
    "$PG_BIN/psql$EXE" -h localhost -p "$POSTRACK_PG_PORT" -U postrack -d postgres -tAc \
      "SELECT 1 FROM pg_database WHERE datname='postrack'" | grep -q 1 ||
      "$PG_BIN/createdb$EXE" -h localhost -p "$POSTRACK_PG_PORT" -U postrack postrack
    ;;
  stop)
    if pgctl status > /dev/null 2>&1; then pgctl -w -m fast stop > /dev/null && echo "PostgreSQL stopped"; else echo "PostgreSQL not running"; fi
    ;;
  status)
    pgctl status || true
    ;;
  *)
    echo "usage: $0 start | stop | status" >&2
    exit 2
    ;;
esac
