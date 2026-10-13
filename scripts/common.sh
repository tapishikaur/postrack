# Shared settings for the scripts. Sourced, not run; the caller sets ROOT.

# Build output goes in build/ beside the sources, unless POSTRACK_BUILD_ROOT is set.
POSTRACK_DEFAULT_BUILD_ROOT="$ROOT/build"
EXE=""

case "$(uname -s)" in
  MINGW* | MSYS* | CYGWIN*)
    EXE=".exe"
    # The MSYS2 MinGW-w64 toolchain, Boost and libpq (pacman -S mingw-w64-x86_64-{gcc,cmake,ninja,boost,postgresql}).
    [ -d /c/msys64/mingw64/bin ] && export PATH="/c/msys64/mingw64/bin:$PATH"
    # Inside a OneDrive folder, OneDrive syncs build output and turns folders into read-only
    # cloud placeholders. There, default to a local folder that isn't synced.
    case "$ROOT" in
      */OneDrive*) POSTRACK_DEFAULT_BUILD_ROOT="$(cygpath -u "$LOCALAPPDATA")/postrack/build" ;;
    esac
    ;;
esac

POSTRACK_BUILD_ROOT="${POSTRACK_BUILD_ROOT:-$POSTRACK_DEFAULT_BUILD_ROOT}"
BIN="$POSTRACK_BUILD_ROOT/release"

# PostgreSQL server programs (initdb, pg_ctl), for the throwaway test cluster: an installed
# PostgreSQL on Windows, the distribution's packages on Linux, or whatever is on PATH.
find_pg_bin() {
  local d
  for d in /c/Program\ Files/PostgreSQL/*/bin /usr/lib/postgresql/*/bin /c/msys64/mingw64/bin; do
    [ -x "$d/initdb$EXE" ] && [ -x "$d/pg_ctl$EXE" ] && { PG_BIN="$d"; }
  done
  if [ -z "${PG_BIN:-}" ] && command -v initdb > /dev/null; then PG_BIN="$(dirname "$(command -v initdb)")"; fi
  [ -n "${PG_BIN:-}" ] || { echo "PostgreSQL server programs (initdb, pg_ctl) not found" >&2; return 1; }
}

# The throwaway cluster: port 54329, user postrack, no password, data under the build root.
POSTRACK_PG_PORT="${POSTRACK_PG_PORT:-54329}"
POSTRACK_PG_DATA="$POSTRACK_BUILD_ROOT/pgdata"
POSTRACK_LOCAL_DB="host=localhost port=$POSTRACK_PG_PORT dbname=postrack user=postrack"
