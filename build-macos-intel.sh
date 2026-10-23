#!/usr/bin/env sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
# Keep CMake/Qt generated files on the Mac's local home volume; external and
# network filesystems may not support the lock operation required by AUTORCC.
BUILD_DIR=${BUILD_DIR:-"$HOME/Library/Caches/Rythmo/build/release-macos-intel"}
HOST_ARCH=$(uname -m)
[ "$(uname -s)" = Darwin ] || { echo "Ce script doit être lancé sur macOS." >&2; exit 1; }
[ "$(sysctl -n hw.optional.arm64 2>/dev/null || echo 0)" != 1 ] || { echo "Ce script cible un Mac Intel; cette machine est Apple Silicon." >&2; exit 1; }
[ "$HOST_ARCH" = x86_64 ] || { echo "Ce script cible un Mac Intel (x86_64), architecture détectée : $HOST_ARCH." >&2; exit 1; }

if ! command -v xcode-select >/dev/null 2>&1 || ! xcode-select -p >/dev/null 2>&1; then
  echo "Installation des outils Apple Command Line Tools requise. Une fenêtre système va s'ouvrir."
  xcode-select --install || true
  echo "Terminez l'installation des outils Apple, puis relancez ce script." >&2
  exit 1
fi
if ! command -v brew >/dev/null 2>&1; then
  echo "Homebrew absent : installation officielle nécessaire (peut demander le mot de passe administrateur)."
  /bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"
  if [ -x /usr/local/bin/brew ]; then eval "$(/usr/local/bin/brew shellenv)"; fi
fi
command -v brew >/dev/null 2>&1 || { echo "Homebrew n'a pas pu être installé." >&2; exit 1; }
brew install qt cmake expat ffmpeg
sh "$ROOT/prepare-python-runtime.sh" macos x64
command -v ffmpeg >/dev/null 2>&1 || { echo "FFmpeg ne s'est pas installé correctement." >&2; exit 1; }
command -v ffprobe >/dev/null 2>&1 || { echo "ffprobe (fourni avec FFmpeg) est introuvable après l'installation." >&2; exit 1; }
echo "FFmpeg installé : $(command -v ffmpeg)"
QT_PREFIX=$(brew --prefix qt)
export CMAKE_PREFIX_PATH="$QT_PREFIX${CMAKE_PREFIX_PATH:+:$CMAKE_PREFIX_PATH}"
cmake -S "$ROOT" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON "-DRYTHMO_VERSION=${RYTHMO_VERSION:-0.3.0}" -DCMAKE_OSX_ARCHITECTURES=x86_64 \
  "-DRYTHMO_PYTHON_RUNTIME_ARCHIVE=$ROOT/build/tools/python-runtime/macos-x64.tar.gz"
cmake --build "$BUILD_DIR" --parallel
ctest --test-dir "$BUILD_DIR" --output-on-failure
APP="$BUILD_DIR/Rythmo.app"
[ -d "$APP" ] || { echo "Bundle macOS absent après compilation : $APP" >&2; exit 1; }
DEPLOYQT="$QT_PREFIX/bin/macdeployqt"
if [ -x "$DEPLOYQT" ]; then
  # Homebrew Qt plugins can depend on dylibs from separate formulae. Expose
  # those locations to macdeployqt so it can bundle and rewrite their paths.
  set -- "$APP" -always-overwrite
  for formula in brotli zstd md4c webp libpng jpeg-turbo libtiff freetype icu4c; do
    if dependency_prefix=$(brew --prefix "$formula" 2>/dev/null) && [ -d "$dependency_prefix/lib" ]; then
      set -- "$@" "-libpath=$dependency_prefix/lib"
    fi
  done
  "$DEPLOYQT" "$@"
else
  echo "Attention : macdeployqt absent; le bundle peut nécessiter Qt installé pour démarrer." >&2
fi
PLIST="$APP/Contents/Info.plist"
EXECUTABLE="$APP/Contents/MacOS/Rythmo"
[ -f "$PLIST" ] || { echo "Info.plist absent du bundle : $PLIST" >&2; exit 1; }
[ -x "$EXECUTABLE" ] || { echo "Exécutable macOS absent ou non exécutable : $EXECUTABLE" >&2; exit 1; }
plutil -lint "$PLIST"
BUNDLE_ID=$(plutil -extract CFBundleIdentifier raw -o - "$PLIST")
[ "$BUNDLE_ID" = "com.dubiaproductions.rythmo" ] || { echo "Identifiant de bundle inattendu : $BUNDLE_ID" >&2; exit 1; }
if ! command -v codesign >/dev/null 2>&1; then
  echo "codesign est introuvable; impossible de vérifier le bundle macOS." >&2
  exit 1
fi
# macdeployqt and install_name_tool may rewrite nested binaries. Sign only
# after all bundle changes, then verify every nested framework and plugin.
if command -v xattr >/dev/null 2>&1; then
  xattr -dr com.apple.quarantine "$APP" 2>/dev/null || true
fi
codesign --force --deep --sign - --timestamp=none "$APP"
codesign --verify --deep --strict --verbose=2 "$APP"
if [ "${CI:-}" != "true" ]; then open "$APP"; fi
echo "Build macOS Intel terminé : $APP"
