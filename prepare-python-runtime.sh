#!/usr/bin/env sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
PLATFORM=${1:?Usage: prepare-python-runtime.sh macos|linux arm64|x64}
ARCH=${2:?Usage: prepare-python-runtime.sh macos|linux arm64|x64}
RELEASE=20261021
PYTHON_VERSION=3.12.15

case "$PLATFORM-$ARCH" in
  macos-arm64)
    TARGET=aarch64-apple-darwin
    SHA256=7b8d00b751db21cea87dc551a8f8165147c5fbc5251d5537d7e5497dde00a219
    ;;
  macos-x64)
    TARGET=x86_64-apple-darwin
    SHA256=f12e0504397683d6be0745ffd4c39ef45a455cec169adf35c628840d06133773
    ;;
  linux-arm64)
    TARGET=aarch64-unknown-linux-gnu
    SHA256=b4567aa5a11f51b540c4dfd9e082ebf23e554f8ac17b85caa6cea23e83693394
    ;;
  linux-x64)
    TARGET=x86_64-unknown-linux-gnu
    SHA256=6d7521138fd33afddc1ccd08abb007d9a2829e38117c2576f9efdc0ec73ce772
    ;;
  *) echo "Plateforme ou architecture inconnue : $PLATFORM-$ARCH" >&2; exit 2 ;;
esac

ASSET="cpython-${PYTHON_VERSION}+${RELEASE}-${TARGET}-install_only_stripped.tar.gz"
URL="https://github.com/astral-sh/python-build-standalone/releases/download/${RELEASE}/${ASSET}"
OUT_DIR="$ROOT/build/tools/python-runtime"
OUT="$OUT_DIR/$PLATFORM-$ARCH.tar.gz"
mkdir -p "$OUT_DIR"

sha256_file() {
  if command -v sha256sum >/dev/null 2>&1; then sha256sum "$1" | awk '{print $1}'
  elif command -v shasum >/dev/null 2>&1; then shasum -a 256 "$1" | awk '{print $1}'
  else echo "sha256sum ou shasum est requis pour vérifier le runtime." >&2; return 1
  fi
}

if [ -f "$OUT" ] && [ "$(sha256_file "$OUT")" = "$SHA256" ]; then
  echo "Runtime Python natif déjà vérifié : $OUT"
  exit 0
fi

TMP="$OUT.part"
rm -f "$TMP"
echo "Téléchargement du runtime Python ${PYTHON_VERSION} natif ($PLATFORM / $ARCH)…"
curl --fail --location --proto '=https' --proto-redir '=https' --retry 3 --connect-timeout 30 --output "$TMP" "$URL"
ACTUAL=$(sha256_file "$TMP")
if [ "$ACTUAL" != "$SHA256" ]; then
  rm -f "$TMP"
  echo "Échec de vérification SHA-256 du runtime Python." >&2
  exit 1
fi
mv "$TMP" "$OUT"
echo "Runtime Python vérifié et prêt : $OUT"
