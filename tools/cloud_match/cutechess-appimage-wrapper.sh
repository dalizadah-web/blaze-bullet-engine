#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/cutechess-root" && pwd)"
runner="$(find "$root" -type f -name cutechess-cli -print -quit)"
if [[ -z "$runner" ]]; then
  echo "frozen CuteChess tree has no cutechess-cli" >&2
  exit 1
fi
export LD_LIBRARY_PATH="$root/usr/lib:$root/usr/lib/x86_64-linux-gnu:${LD_LIBRARY_PATH:-}"
exec "$runner" "$@"
