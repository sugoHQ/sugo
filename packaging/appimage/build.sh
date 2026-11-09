#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Builds Sugo as an AppImage for the machine's architecture, x86_64 or aarch64. Runs as root in
# a container of the oldest system the AppImage is meant for, with Qt from qt.io (neither system
# has Qt 6.8):
#
# - x86_64: Ubuntu 22.04 (glibc 2.35);
# - aarch64: Ubuntu 24.04 (glibc 2.39), because qt.io builds its arm64 Qt there: it needs glibc
#   2.38 and the libstdc++ of GCC 13.
#
#   podman run --rm -v "$PWD:/src:ro,z" -v "$PWD/out:/out:z" ubuntu:22.04 \
#       bash /src/packaging/appimage/build.sh 1.0.0 /out
#
# Arguments: the version the program shows and compares when it looks for updates (see
# UpdateChecker), and the directory that receives Sugo-<architecture>.AppImage.
set -euo pipefail

VERSION=${1:?version}
OUT=${2:?output directory}
SRC=$(cd "$(dirname "$0")/../.." && pwd)
WORK=${WORK:-/tmp/sugo-appimage}

# 6.9 or later: older Qt sends no window icon on Wayland (xdg-toplevel-icon).
QT_VERSION=6.10.3
AQT_VERSION=3.3.0
ECM_TAG=v6.8.0
# The commit of that tag (tags can move): the clone is refused when it is another.
ECM_COMMIT=02e39e9538d3699e470194c989f7047efe44d2cc
# linuxdeploy publishes no checksums: these are of the files at the pinned tags.
LINUXDEPLOY_TAG=1-alpha-20251107-1
LINUXDEPLOY_QT_TAG=1-alpha-20250213-1

# The name of the architecture in the AppImage's name and linuxdeploy's, and what qt.io calls it.
ARCH=$(uname -m)
case $ARCH in
    x86_64)
        QT_HOST=linux QT_ARCH=linux_gcc_64 QT_DIR=gcc_64
        LINUXDEPLOY_SHA256=c20cd71e3a4e3b80c3483cef793cda3f4e990aca14014d23c544ca3ce1270b4d
        LINUXDEPLOY_QT_SHA256=15106be885c1c48a021198e7e1e9a48ce9d02a86dd0a1848f00bdbf3c1c92724
        ;;
    aarch64)
        QT_HOST=linux_arm64 QT_ARCH=linux_gcc_arm64 QT_DIR=gcc_arm64
        LINUXDEPLOY_SHA256=620095110d693282b8ebeb244a95b5e911cf8f65f76c88b4b47d16ae6346fcff
        LINUXDEPLOY_QT_SHA256=bf1c24aff6d749b5cf423afad6f15abd4440f81dec1aab95706b25f6667cdcf1
        ;;
    *)
        echo "No AppImage for $ARCH: only x86_64 and aarch64" >&2
        exit 1
        ;;
esac

export DEBIAN_FRONTEND=noninteractive
# Renamed in Ubuntu 24.04 (the 64-bit time_t transition).
GLIB=libglib2.0-0
if grep -q '^VERSION_ID="24\.04"' /etc/os-release; then
    GLIB=libglib2.0-0t64
fi
apt-get update -qq
apt-get install -y -qq --no-install-recommends \
    ca-certificates wget git file xz-utils python3-venv g++ cmake ninja-build pkg-config libsodium-dev \
    libgl1-mesa-dev libegl1-mesa-dev libvulkan-dev libxkbcommon-dev libxkbcommon-x11-0 \
    libfontconfig1 libfreetype6 libdbus-1-3 "$GLIB" \
    libxcb-cursor0 libxcb-icccm4 libxcb-image0 libxcb-keysyms1 libxcb-randr0 \
    libxcb-render-util0 libxcb-shape0 libxcb-xinerama0 libxcb-xkb1 \
    libwayland-client0 libwayland-cursor0 libwayland-egl1 libpulse0 libxrandr2 \
    libsm6 libice6 libx11-xcb1 libxcb-glx0 libxcb-xfixes0 libxcb-shm0 libxcb-sync1 libxcb-util1 \
    libxi6 libxrender1 libxext6 >/dev/null

mkdir -p "$WORK" "$OUT"
cd "$WORK"

# --- Qt and extra-cmake-modules ------------------------------------------------------------------
QT="$WORK/qt/$QT_VERSION/$QT_DIR"
if [ ! -d "$QT" ]; then
    # In a virtual environment: Ubuntu 24.04's pip installs nothing system-wide.
    python3 -m venv "$WORK/aqt-venv"
    "$WORK/aqt-venv/bin/pip" install -q "aqtinstall==$AQT_VERSION"
    "$WORK/aqt-venv/bin/python" -m aqt install-qt "$QT_HOST" desktop "$QT_VERSION" "$QT_ARCH" -m qtmultimedia -O "$WORK/qt" >/dev/null
fi
if [ ! -e "$QT/plugins/multimedia/libffmpegmediaplugin.so" ]; then
    echo "error: this Qt has no FFmpeg backend for Qt Multimedia" >&2
    exit 1
fi
if [ ! -d "$WORK/ecm" ]; then
    git clone -q --depth 1 --branch "$ECM_TAG" https://invent.kde.org/frameworks/extra-cmake-modules.git ecm-src
    [ "$(git -C ecm-src rev-parse HEAD)" = "$ECM_COMMIT" ] || { echo "error: extra-cmake-modules $ECM_TAG is not commit $ECM_COMMIT" >&2; exit 1; }
    cmake -S ecm-src -B ecm-build -G Ninja -DBUILD_TESTING=OFF -DBUILD_HTML_DOCS=OFF \
        -DBUILD_MAN_DOCS=OFF -DBUILD_QTHELP_DOCS=OFF -DCMAKE_INSTALL_PREFIX="$WORK/ecm" >/dev/null
    cmake --install ecm-build >/dev/null
fi

fetch() { # url sha256 file
    [ -f "$3" ] || wget -q -O "$3" "$1"
    echo "$2  $3" | sha256sum -c --quiet
    chmod +x "$3"
}
fetch "https://github.com/linuxdeploy/linuxdeploy/releases/download/$LINUXDEPLOY_TAG/linuxdeploy-$ARCH.AppImage" \
    "$LINUXDEPLOY_SHA256" "linuxdeploy-$ARCH.AppImage"
fetch "https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/$LINUXDEPLOY_QT_TAG/linuxdeploy-plugin-qt-$ARCH.AppImage" \
    "$LINUXDEPLOY_QT_SHA256" "linuxdeploy-plugin-qt-$ARCH.AppImage"

# --- Sugo ----------------------------------------------------------------------------------------
cmake -S "$SRC" -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF \
    -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_PREFIX_PATH="$QT;$WORK/ecm" \
    -DSUGO_VERSION="$VERSION" >/dev/null
cmake --build build
rm -rf AppDir
DESTDIR="$WORK/AppDir" cmake --install build >/dev/null
# KRunner and D-Bus activation would point at /usr/bin/sugo, which an AppImage does not have.
rm -rf AppDir/usr/share/krunner AppDir/usr/share/dbus-1

# Wayland: linuxdeploy-plugin-qt brings the platform plugins, not the shell and EGL integration
# a window needs under Wayland.
for plugin in wayland-shell-integration/libxdg-shell.so \
              wayland-graphics-integration-client/libqt-plugin-wayland-egl.so \
              wayland-decoration-client/libbradient.so; do
    install -D -m 755 "$QT/plugins/$plugin" "AppDir/usr/plugins/$plugin"
done

# --- The AppImage --------------------------------------------------------------------------------
# Only SQLite is used; linuxdeploy would bring every SQL driver, and fail on those whose client
# libraries are missing. Set aside while it runs (Qt's CMake files still refer to them).
mkdir -p sqldrivers-unused
find "$QT/plugins/sqldrivers" -name '*.so' ! -name libqsqlite.so -exec mv -t sqldrivers-unused {} +
# Likewise the GStreamer backend of Qt Multimedia, which qt.io's arm64 Qt has next to the FFmpeg one
# (linuxdeploy would try to bundle GStreamer with it): the AppImage plays sound with FFmpeg, as
# the x86_64 one.
mkdir -p multimedia-unused
mv -f "$QT/plugins/multimedia/libgstreamermediaplugin.so" multimedia-unused/ 2>/dev/null || true
trap 'mv -f "$WORK"/sqldrivers-unused/*.so "$QT/plugins/sqldrivers/" 2>/dev/null || true
      mv -f "$WORK"/multimedia-unused/*.so "$QT/plugins/multimedia/" 2>/dev/null || true' EXIT

# Unpacked rather than run: the build container has no FUSE.
export APPIMAGE_EXTRACT_AND_RUN=1
export QMAKE="$QT/bin/qmake"
export LD_LIBRARY_PATH="$QT/lib"
export QML_SOURCES_PATHS="$SRC/src/qml"
export EXTRA_QT_MODULES="svg;waylandclient"
# offscreen: the smoke tests run the AppImage without a display before it is published.
# Qt 6.10 has one Wayland platform plugin (libqwayland.so) where older versions had two.
export EXTRA_PLATFORM_PLUGINS="libqwayland.so;libqoffscreen.so"
export OUTPUT="$OUT/Sugo-$ARCH.AppImage"
# The AppImage runtime of this architecture (linuxdeploy-plugin-appimage reads it).
export ARCH
rm -f "$OUTPUT"
"./linuxdeploy-$ARCH.AppImage" --appdir AppDir --plugin qt --output appimage \
    --deploy-deps-only AppDir/usr/plugins/wayland-shell-integration \
    --deploy-deps-only AppDir/usr/plugins/wayland-graphics-integration-client \
    --deploy-deps-only AppDir/usr/plugins/wayland-decoration-client \
    -d AppDir/usr/share/applications/stream.sugo.app.desktop \
    -i AppDir/usr/share/icons/hicolor/scalable/apps/stream.sugo.app.svg \
    > linuxdeploy.log 2>&1 || { tail -40 linuxdeploy.log; exit 1; }
# Not worth reporting: missing licence files of system libraries, unstripped Qt plugins, the
# styles of other systems, and our own QML module (compiled into the program).
grep -E 'ERROR|WARNING' linuxdeploy.log \
    | grep -vE 'Could not find copyright files|Not calling strip|Missing qml module: +(QtQuick\.Controls\.(Windows|macOS|iOS)|stream\.sugo\.app) *$' || true
# Sugo Sync's encryption: not part of the systems the AppImage runs on, so it must travel with it.
compgen -G "AppDir/usr/lib/libsodium.so.*" >/dev/null || { echo "libsodium is missing from the AppImage" >&2; exit 1; }
echo "Built $OUTPUT ($VERSION)"
