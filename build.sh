#!/usr/bin/env sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
case "$(uname -s)" in
  Darwin)
    if [ "$(sysctl -n hw.optional.arm64 2>/dev/null || echo 0)" = 1 ]; then
      if [ "$(uname -m)" != arm64 ]; then exec arch -arm64 sh "$ROOT/build-macos-silicon.sh" "$@"; fi
      sh "$ROOT/build-macos-silicon.sh" "$@"; exit $?
    fi
    if [ "$(uname -m)" = x86_64 ]; then sh "$ROOT/build-macos-intel.sh" "$@"; exit $?; fi
    echo "Architecture macOS non prise en charge: $arch" >&2; exit 1
    ;;
  Linux)
    arch=$(uname -m)
    case "$arch" in
      x86_64|amd64) sh "$ROOT/build-linux-x64.sh" "$@"; exit $? ;;
      aarch64|arm64) sh "$ROOT/build-linux-arm64.sh" "$@"; exit $? ;;
      *) echo "Architecture Linux non prise en charge: $arch" >&2; exit 1 ;;
    esac
    ;;
  *) echo "Utilisez build.bat sous Windows." >&2; exit 1 ;;
esac
