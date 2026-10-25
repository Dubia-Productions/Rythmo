#!/usr/bin/env sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
BUILD_DIR=${BUILD_DIR:-"$ROOT/build/release-linux-arm64"}
HOST_ARCH=$(uname -m)
[ "$(uname -s)" = Linux ] || { echo "Ce script doit être lancé sous Linux." >&2; exit 1; }
case "$HOST_ARCH" in aarch64|arm64) ;; *) echo "Ce script cible Linux ARM64, architecture détectée : $HOST_ARCH." >&2; exit 1;; esac

install_deps() {
  if command -v cmake >/dev/null 2>&1 && command -v pkg-config >/dev/null 2>&1 && command -v curl >/dev/null 2>&1 && pkg-config --exists expat Qt6Widgets Qt6Multimedia Qt6MultimediaWidgets; then
    CMAKE_VERSION=$(cmake --version | awk 'NR == 1 { print $3 }')
    CMAKE_MAJOR=$(printf '%s' "$CMAKE_VERSION" | cut -d. -f1)
    CMAKE_MINOR=$(printf '%s' "$CMAKE_VERSION" | cut -d. -f2)
    if [ "${CMAKE_MAJOR:-0}" -gt 3 ] || { [ "${CMAKE_MAJOR:-0}" -eq 3 ] && [ "${CMAKE_MINOR:-0}" -ge 25 ]; }; then
      echo "Dépendances déjà présentes (CMake $CMAKE_VERSION)."
      return 0
    fi
  fi
  if command -v apt-get >/dev/null 2>&1; then
    SUDO=""; [ "$(id -u)" -eq 0 ] || SUDO=sudo
    $SUDO apt-get update
    $SUDO apt-get install -y build-essential cmake ninja-build pkg-config curl libexpat1-dev libgl1-mesa-dev libegl1-mesa-dev libopengl-dev libxkbcommon-dev libxkbcommon-x11-dev libxcb-cursor-dev qt6-base-dev qt6-multimedia-dev ffmpeg
  elif command -v dnf >/dev/null 2>&1; then
    SUDO=""; [ "$(id -u)" -eq 0 ] || SUDO=sudo
    $SUDO dnf install -y gcc-c++ make cmake ninja-build pkgconf-pkg-config curl expat-devel qt6-qtbase-devel qt6-qtmultimedia-devel ffmpeg
  elif command -v pacman >/dev/null 2>&1; then
    SUDO=""; [ "$(id -u)" -eq 0 ] || SUDO=sudo
    $SUDO pacman -Sy --needed --noconfirm base-devel cmake ninja pkgconf curl expat qt6-base qt6-multimedia ffmpeg
  elif command -v zypper >/dev/null 2>&1; then
    SUDO=""; [ "$(id -u)" -eq 0 ] || SUDO=sudo
    $SUDO zypper --non-interactive install gcc-c++ make cmake ninja pkg-config curl expat-devel libqt6-qtbase-devel libqt6-qtmultimedia-devel ffmpeg
  else
    echo "Gestionnaire pris en charge introuvable (apt, dnf, pacman, zypper). Installez CMake 3.25+, Qt 6.5 Multimedia, Expat et un compilateur C++17." >&2
    return 1
  fi
}
install_deps
sh "$ROOT/prepare-python-runtime.sh" linux arm64
TOOLENV="$ROOT/build/tools/build-env"
CMAKE_CMD=cmake
CMAKE_VERSION=$(cmake --version | awk 'NR == 1 { print $3 }')
CMAKE_MAJOR=$(printf '%s' "$CMAKE_VERSION" | cut -d. -f1)
CMAKE_MINOR=$(printf '%s' "$CMAKE_VERSION" | cut -d. -f2)
if [ "${CMAKE_MAJOR:-0}" -lt 3 ] || { [ "${CMAKE_MAJOR:-0}" -eq 3 ] && [ "${CMAKE_MINOR:-0}" -lt 25 ]; }; then
  mkdir -p "$(dirname "$TOOLENV")"
  python3 -m venv "$TOOLENV"
  "$TOOLENV/bin/python" -m pip install --upgrade cmake
  CMAKE_CMD="$TOOLENV/bin/cmake"
fi
QT_VERSION=$(pkg-config --modversion Qt6Widgets 2>/dev/null || true)
QT_MAJOR=$(printf '%s' "$QT_VERSION" | cut -d. -f1)
QT_MINOR=$(printf '%s' "$QT_VERSION" | cut -d. -f2)
QT_PREFIX=
if [ "${QT_MAJOR:-0}" -lt 6 ] || { [ "${QT_MAJOR:-0}" -eq 6 ] && [ "${QT_MINOR:-0}" -lt 5 ]; }; then
  mkdir -p "$(dirname "$TOOLENV")"
  if [ ! -x "$TOOLENV/bin/aqt" ]; then
    python3 -m venv "$TOOLENV"
    "$TOOLENV/bin/python" -m pip install --upgrade aqtinstall
  fi
  "$TOOLENV/bin/aqt" install-qt linux_arm64 desktop 6.8.3 linux_gcc_arm64 --outputdir "$ROOT/build/tools/Qt" --modules qtmultimedia
  QT_CONFIG=$(find "$ROOT/build/tools/Qt/6.8.3" -path '*/lib/cmake/Qt6/Qt6Config.cmake' -print -quit)
  [ -n "$QT_CONFIG" ] || { echo "Échec de la préparation du SDK Qt Linux ARM64." >&2; exit 1; }
  QT_PREFIX=${QT_CONFIG%/lib/cmake/Qt6/Qt6Config.cmake}
fi
if [ -n "$QT_PREFIX" ]; then
  "$CMAKE_CMD" -S "$ROOT" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON "-DRYTHMO_VERSION=${RYTHMO_VERSION:-0.4.1}" "-DCMAKE_PREFIX_PATH=$QT_PREFIX" \
    "-DRYTHMO_PYTHON_RUNTIME_ARCHIVE=$ROOT/build/tools/python-runtime/linux-arm64.tar.gz"
else
  "$CMAKE_CMD" -S "$ROOT" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON "-DRYTHMO_VERSION=${RYTHMO_VERSION:-0.4.1}" \
    "-DRYTHMO_PYTHON_RUNTIME_ARCHIVE=$ROOT/build/tools/python-runtime/linux-arm64.tar.gz"
fi
"$CMAKE_CMD" --build "$BUILD_DIR" --parallel
ctest --test-dir "$BUILD_DIR" --output-on-failure
echo "Build Linux ARM64 terminé : $BUILD_DIR/Rythmo"
