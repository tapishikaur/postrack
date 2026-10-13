#!/usr/bin/env bash
# One command to build and test PosTrack.
#
# Usage:  scripts/build.sh [Release|Debug]      (default: Release)
# Env:    JOBS=N               parallel build jobs (default 4)
#         POSTRACK_TEST_DB     PostgreSQL for the database tests. If unset, the throwaway
#                              local cluster (scripts/pg_local.sh) is started and used.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "$ROOT/scripts/common.sh"
CONFIG="${1:-Release}"
BUILD_DIR="$POSTRACK_BUILD_ROOT/$(echo "$CONFIG" | tr '[:upper:]' '[:lower:]')"
echo "Build output: $BUILD_DIR"

EXTRA=()
# On MSYS2, find Boost and libpq in the MinGW prefix first (not a Windows PostgreSQL install).
[ -d /c/msys64/mingw64 ] && EXTRA+=(-DCMAKE_PREFIX_PATH=C:/msys64/mingw64)

cmake -S "$ROOT" -B "$BUILD_DIR" -G Ninja -DCMAKE_BUILD_TYPE="$CONFIG" "${EXTRA[@]}"
cmake --build "$BUILD_DIR" -j "${JOBS:-4}"

if [ -z "${POSTRACK_TEST_DB:-}" ]; then
  bash "$ROOT/scripts/pg_local.sh" start
  export POSTRACK_TEST_DB="$POSTRACK_LOCAL_DB"
fi
ctest --test-dir "$BUILD_DIR" --output-on-failure
