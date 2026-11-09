#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Builds Sugo for macOS 13 or later as a universal (arm64 + x86_64) app in a disk image, with Qt
# from qt.io (the same version as the AppImage) and libsodium built from source
# (build-libsodium.sh). Runs on a Mac with the Xcode command line tools, CMake, Python 3 and git
# (a GitHub macos runner has them all; Ninja and pkg-config come from Homebrew if missing):
#
#   bash packaging/macos/build.sh 1.0.0 out
#
# Arguments: the version the program shows and compares when it looks for updates (see
# UpdateChecker), and the directory that receives Sugo-macOS-universal.dmg.
#
# The app is signed ad hoc (Apple Silicon runs no unsigned code, and macdeployqt's changes break
# the linker's signature), so Gatekeeper asks the user to confirm the first launch. SIGN_IDENTITY
# set to a "Developer ID Application" identity signs it for distribution instead, with the hardened
# runtime and the entitlements in entitlements.plist (notarization is a separate step, not done here).
set -euo pipefail
# With pipefail, "producer | grep -q" fails when grep stops at the first match and the producer,
# still writing, dies of SIGPIPE: searched output is read into a variable (or a here-string) first.

VERSION=${1:?version}
OUT=${2:?output directory}
SRC=$(cd "$(dirname "$0")/../.." && pwd)
WORK=${WORK:-/tmp/sugo-macos}
SIGN_IDENTITY=${SIGN_IDENTITY:--}

# Pinned like packaging/appimage/build.sh: keep the two in step.
QT_VERSION=6.10.3
AQT_VERSION=3.3.0
ECM_TAG=v6.8.0
# The commit of that tag (tags can move): the clone is refused when it is another.
ECM_COMMIT=02e39e9538d3699e470194c989f7047efe44d2cc
DEPLOYMENT_TARGET=13.0
ARCHS="arm64;x86_64"
DMG_NAME=Sugo-macOS-universal.dmg

fail() { echo "error: $*" >&2; exit 1; }

mkdir -p "$WORK" "$OUT"
# Absolute: the script works in WORK, and APP and the exit trap name paths under it.
WORK=$(cd "$WORK" && pwd)
OUT=$(cd "$OUT" && pwd)
cd "$WORK"

if ! command -v ninja >/dev/null 2>&1; then
    command -v brew >/dev/null 2>&1 || fail "ninja is missing (brew install ninja)"
    brew install ninja >/dev/null
fi

# --- Qt and extra-cmake-modules ------------------------------------------------------------------
# aqt names the folder "macos" from Qt 6.7 on, "clang_64" before: accept either.
find_qt() {
    for dir in "$WORK/qt/$QT_VERSION/macos" "$WORK/qt/$QT_VERSION/clang_64"; do
        if [ -x "$dir/bin/macdeployqt" ]; then
            echo "$dir"
            return 0
        fi
    done
    return 1
}
if ! QT=$(find_qt); then
    # A virtual environment: Homebrew's Python refuses packages installed next to its own.
    python3 -m venv "$WORK/aqt-venv"
    "$WORK/aqt-venv/bin/pip" install -q "aqtinstall==$AQT_VERSION"
    "$WORK/aqt-venv/bin/python" -m aqt install-qt mac desktop "$QT_VERSION" clang_64 -m qtmultimedia -O "$WORK/qt" >/dev/null
    QT=$(find_qt) || fail "aqt installed no Qt in $WORK/qt/$QT_VERSION"
fi
if [ ! -d "$WORK/ecm" ]; then
    rm -rf ecm-src ecm-build
    git clone -q --depth 1 --branch "$ECM_TAG" https://invent.kde.org/frameworks/extra-cmake-modules.git ecm-src
    [ "$(git -C ecm-src rev-parse HEAD)" = "$ECM_COMMIT" ] || { echo "error: extra-cmake-modules $ECM_TAG is not commit $ECM_COMMIT" >&2; exit 1; }
    cmake -S ecm-src -B ecm-build -G Ninja -DBUILD_TESTING=OFF -DBUILD_HTML_DOCS=OFF \
        -DBUILD_MAN_DOCS=OFF -DBUILD_QTHELP_DOCS=OFF -DCMAKE_INSTALL_PREFIX="$WORK/ecm" >/dev/null
    cmake --install ecm-build >/dev/null
fi

# --- libsodium (the sync's encryption) -----------------------------------------------------------
# A universal static library, found by CMake through pkg-config before any other libsodium
# (Homebrew's is a dylib for one architecture). Pinned in build-libsodium.sh.
if ! command -v pkg-config >/dev/null 2>&1; then
    command -v brew >/dev/null 2>&1 || fail "pkg-config is missing (brew install pkgconf)"
    brew install pkgconf >/dev/null
fi
MACOSX_DEPLOYMENT_TARGET=$DEPLOYMENT_TARGET bash "$SRC/packaging/macos/build-libsodium.sh" "$WORK/sodium"
export PKG_CONFIG_PATH="$WORK/sodium/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
[ "$(pkg-config --variable=libdir libsodium)" = "$WORK/sodium/lib" ] \
    || fail "pkg-config finds a libsodium other than the one in $WORK/sodium"

# --- Sugo ----------------------------------------------------------------------------------------
cmake -S "$SRC" -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF \
    -DCMAKE_PREFIX_PATH="$QT;$WORK/ecm" \
    -DCMAKE_OSX_ARCHITECTURES="$ARCHS" -DCMAKE_OSX_DEPLOYMENT_TARGET="$DEPLOYMENT_TARGET" \
    -DSUGO_VERSION="$VERSION" -DSUGO_INSTALLS_UPDATES=ON >/dev/null
cmake --build build
[ -x build/bin/Sugo.app/Contents/MacOS/Sugo ] || fail "build/bin/Sugo.app was not built"
[ "$(plutil -extract SugoInstallsUpdates raw build/bin/Sugo.app/Contents/Info.plist 2>/dev/null)" = true ] \
    || fail "Sugo.app does not install its updates (SugoInstallsUpdates in Info.plist)"

# A copy, so that a rebuild never sees a bundle macdeployqt has already changed.
rm -rf app
mkdir app
ditto build/bin/Sugo.app app/Sugo.app
APP="$WORK/app/Sugo.app"
CONTENTS="$APP/Contents"

# The libraries a Mach-O file loads, once each (a universal file lists them for each architecture,
# under a header line per architecture; the dependencies are the indented lines).
deps() {
    otool -arch all -L "$1" | awk '/^[[:space:]]/ { print $1 }' | sort -u
}

# --- Qt in the bundle ----------------------------------------------------------------------------
# Only SQLite is used: the other SQL drivers need client libraries that are not there. Set aside
# while macdeployqt runs (Qt's CMake files still refer to them), put back on exit.
mkdir -p sqldrivers-unused
find "$QT/plugins/sqldrivers" -name '*.dylib' ! -name libqsqlite.dylib -exec mv {} sqldrivers-unused/ \;
trap 'mv -f "$WORK"/sqldrivers-unused/*.dylib "$QT/plugins/sqldrivers/" 2>/dev/null || true' EXIT

"$QT/bin/macdeployqt" "$APP" -qmldir="$SRC/src/qml" -verbose=1 > macdeployqt.log 2>&1 \
    || { tail -40 macdeployqt.log; exit 1; }
# Not worth reporting: our own QML module (compiled into the program) and the styles of other systems.
grep -iE 'error|warning' macdeployqt.log \
    | grep -viE 'stream\.sugo\.app|QtQuick/Controls/(Windows|Fusion|Imagine|Material|Universal|FluentWinUI3|iOS)' || true
find "$CONTENTS/PlugIns/sqldrivers" -name '*.dylib' ! -name libqsqlite.dylib -delete 2>/dev/null || true

# offscreen: the smoke tests start the app without a display before it is published.
# macdeployqt brings only the Cocoa platform plugin; this one points at the bundled frameworks.
offscreen="$CONTENTS/PlugIns/platforms/libqoffscreen.dylib"
cp "$QT/plugins/platforms/libqoffscreen.dylib" "$offscreen"
chmod u+w "$offscreen"
deps "$offscreen" | while read -r dep; do
    case "$dep" in
        @rpath/Qt*.framework/*) install_name_tool -change "$dep" "@executable_path/../Frameworks/${dep#@rpath/}" "$offscreen" ;;
        "$QT"/lib/Qt*.framework/*) install_name_tool -change "$dep" "@executable_path/../Frameworks/${dep#"$QT"/lib/}" "$offscreen" ;;
    esac
done

# The build's own rpaths to Qt would make the app look for Qt where it was built; the bundled
# frameworks are found through @executable_path/../Frameworks.
rpaths() {
    otool -arch all -l "$CONTENTS/MacOS/Sugo" | awk '/cmd LC_RPATH/ { getline; getline; print $2 }' | sort -u
}
rpaths | while read -r rpath; do
    case "$rpath" in
        @*) ;;
        *) install_name_tool -delete_rpath "$rpath" "$CONTENTS/MacOS/Sugo" ;;
    esac
done
if ! grep -qx '@executable_path/../Frameworks' <<<"$(rpaths)"; then
    install_name_tool -add_rpath @executable_path/../Frameworks "$CONTENTS/MacOS/Sugo"
fi

# --- Checks: fail on anything missing ------------------------------------------------------------
require() { # file under Contents, what it is for
    [ -e "$CONTENTS/$1" ] || fail "missing $1 ($2)"
}
require PlugIns/platforms/libqcocoa.dylib "the window system"
require PlugIns/platforms/libqoffscreen.dylib "the smoke tests"
require PlugIns/sqldrivers/libqsqlite.dylib "presets and sessions"
require PlugIns/imageformats/libqsvg.dylib "SVG pictures"
require PlugIns/iconengines/libqsvgicon.dylib "SVG icons"
require PlugIns/multimedia/libffmpegmediaplugin.dylib "the focus sounds"
for module in QtQuick QtQuick/Controls QtQuick/Controls/Basic QtQuick/Layouts QtQuick/Shapes QtQuick/Dialogs; do
    require "Resources/qml/$module/qmldir" "QML module $module"
done
[ "$(find "$CONTENTS/PlugIns/sqldrivers" -name '*.dylib' | wc -l | tr -d ' ')" = 1 ] \
    || fail "SQL drivers other than SQLite were deployed"

# D-Bus is Linux-only: Sugo itself must not link QtDBus on macOS. Qt from qt.io is built with
# its dbus feature, though, and QtGui links QtDBus.framework, so macdeployqt has to ship it: name
# what pulls it in instead of deleting it (QtGui would not load without it).
sugo_deps=$(deps "$CONTENTS/MacOS/Sugo")
if grep -q 'QtDBus\.framework/' <<<"$sugo_deps"; then
    fail "Sugo links QtDBus.framework: D-Bus code is built on macOS (see SUGO_DBUS in CMakeLists.txt)"
fi
if [ -e "$CONTENTS/Frameworks/QtDBus.framework" ]; then
    users=$(find "$APP" -type f \( -perm -u+x -o -name '*.dylib' \) -not -path '*/QtDBus.framework/*' -print0 \
        | while IFS= read -r -d '' f; do
              if grep -q 'QtDBus\.framework/' <<<"$(otool -arch all -L "$f" 2>/dev/null)"; then echo "${f#"$CONTENTS"/}"; fi
          done)
    [ -n "$users" ] || fail "QtDBus.framework was deployed but nothing in the bundle links it"
    echo "note: QtDBus.framework is in the bundle only for Qt itself: $(echo $users)" >&2
fi

# libsodium is linked statically: no libsodium dylib in the program (macdeployqt would copy one
# for a single architecture from wherever CMake found it), and, once the sources use it, its code
# in the program.
if grep -qi 'libsodium' <<<"$sugo_deps"; then
    fail "Sugo links a libsodium dylib: CMake did not find the static library of $WORK/sodium"
fi
if grep -rqs --include='*.cpp' --include='*.mm' '#include <sodium' "$SRC/src"; then
    for arch in arm64 x86_64; do
        symbols=$(nm -arch "$arch" -g "$CONTENTS/MacOS/Sugo") || fail "cannot list the symbols of Sugo ($arch)"
        grep -q ' _sodium_init$' <<<"$symbols" || fail "libsodium is not linked into Sugo ($arch)"
    done
fi

# HTTPS (update checks, CalDAV): a TLS backend that needs nothing outside the bundle.
if [ ! -e "$CONTENTS/PlugIns/tls/libqsecuretransportbackend.dylib" ]; then
    [ -e "$CONTENTS/PlugIns/tls/libqopensslbackend.dylib" ] || fail "no TLS backend in PlugIns/tls"
    [ -n "$(find "$CONTENTS/Frameworks" -name 'libssl*.dylib')" ] \
        || fail "only the OpenSSL TLS backend was deployed, without OpenSSL"
fi

# FFmpeg: the media plugin's libraries, when it links them dynamically, must be in the bundle.
for lib in $(deps "$CONTENTS/PlugIns/multimedia/libffmpegmediaplugin.dylib" | grep -E 'lib(av|sw)[a-z]*\.' || true); do
    [ -e "$CONTENTS/Frameworks/$(basename "$lib")" ] || fail "FFmpeg library $(basename "$lib") is not in the bundle"
done

# Every program and library: both architectures, and nothing loaded from outside the bundle but
# the system's own libraries.
macho_files() {
    find "$APP" -type f \( -perm -u+x -o -name '*.dylib' \) -print0 | while IFS= read -r -d '' f; do
        case "$(file -b "$f")" in
            *Mach-O*) printf '%s\0' "$f" ;;
        esac
    done
}
problems=0
while IFS= read -r -d '' f; do
    archs=$(lipo -archs "$f" 2>/dev/null || true)
    for arch in x86_64 arm64; do
        case " $archs " in
            *" $arch "*) ;;
            *) echo "no $arch ($archs): ${f#"$APP"/}" >&2; problems=1 ;;
        esac
    done
    dir=$(dirname "$f")
    # A library lists its own install name among its dependencies: skip it.
    own=$(otool -arch all -D "$f" 2>/dev/null | awk '!/:$/ && !found { print; found = 1 }')
    while read -r dep; do
        [ -n "$dep" ] && [ "$dep" != "$own" ] || continue
        case "$dep" in
            /System/* | /usr/lib/*) continue ;;
            @executable_path/*) target="$CONTENTS/MacOS/${dep#@executable_path/}" ;;
            @loader_path/*) target="$dir/${dep#@loader_path/}" ;;
            @rpath/*) target="$CONTENTS/Frameworks/${dep#@rpath/}" ;;
            *) echo "outside the bundle: ${f#"$APP"/} -> $dep" >&2; problems=1; continue ;;
        esac
        [ -e "$target" ] || { echo "not in the bundle: ${f#"$APP"/} -> $dep" >&2; problems=1; }
    done < <(deps "$f")
done < <(macho_files)
[ "$problems" = 0 ] || fail "the bundle is not self-contained"

# --- Signature -----------------------------------------------------------------------------------
# Extended attributes (Finder information, quarantine) are not allowed in a signed bundle.
xattr -cr "$APP"
if [ "$SIGN_IDENTITY" = - ]; then
    codesign --force --deep --sign - "$APP"
else
    # The hardened runtime: entitlements.plist lets Qt's QML engine use its JIT.
    codesign --force --deep --options runtime --timestamp --entitlements "$SRC/packaging/macos/entitlements.plist" \
        --sign "$SIGN_IDENTITY" "$APP"
fi
codesign --verify --deep --strict --verbose=2 "$APP"

actual=$(QT_QPA_PLATFORM=offscreen "$CONTENTS/MacOS/Sugo" --version 2>/dev/null || true)
[ "$actual" = "sugo $VERSION" ] || fail "--version printed \"$actual\", expected \"sugo $VERSION\""

# --- The disk image ------------------------------------------------------------------------------
rm -rf dmg
mkdir dmg
ditto "$APP" dmg/Sugo.app
ln -s /Applications dmg/Applications
OUTPUT="$OUT/$DMG_NAME"
rm -f "$OUTPUT"

# Window of the disk image (points): large, with big icons, the app on the left and the link to
# /Applications on the right. Finder keeps this in the volume's .DS_Store.
DMG_WIN_W=800
DMG_WIN_H=500
DMG_ICON_SIZE=160

# Opens the writable image in Finder and sets the window. Returns non-zero on any failure (Finder
# can be unavailable on a runner without a login session): the caller then ships the plain image.
style_dmg() {
    local rw=$1 mount=/Volumes/Sugo
    hdiutil detach "$mount" >/dev/null 2>&1 || true
    hdiutil attach "$rw" -noverify -noautoopen -mountpoint "$mount" >/dev/null || return 1
    local ok=0
    osascript <<EOF >/dev/null 2>&1 || ok=1
tell application "Finder"
    tell disk "Sugo"
        open
        set current view of container window to icon view
        set toolbar visible of container window to false
        set statusbar visible of container window to false
        set the bounds of container window to {200, 120, $((200 + DMG_WIN_W)), $((120 + DMG_WIN_H))}
        set opts to the icon view options of container window
        set arrangement of opts to not arranged
        set icon size of opts to $DMG_ICON_SIZE
        set text size of opts to 14
        set position of item "Sugo.app" of container window to {$((DMG_WIN_W / 4)), $((DMG_WIN_H / 2 - 20))}
        set position of item "Applications" of container window to {$((DMG_WIN_W * 3 / 4)), $((DMG_WIN_H / 2 - 20))}
        update without registering applications
        delay 2
        close
    end tell
end tell
EOF
    sync
    [ -f "$mount/.DS_Store" ] || ok=1
    detach_dmg "$mount" || ok=1
    return $ok
}

# Detaches the image and waits until the volume is really gone: Finder and diskimages-help can hold
# it for a moment, and converting an image that is still attached fails with "Resource temporarily
# unavailable". Plain detach a few times, then forced; non-zero if the volume is still there.
detach_dmg() {
    local mount=$1 attempt
    for attempt in 1 2 3 4 5; do
        [ -d "$mount" ] || return 0
        if [ "$attempt" -lt 4 ]; then
            hdiutil detach "$mount" >/dev/null 2>&1 || true
        else
            hdiutil detach -force "$mount" >/dev/null 2>&1 || true
        fi
        [ -d "$mount" ] || return 0
        sleep $((attempt * 2))
    done
    [ ! -d "$mount" ]
}

# Whether hdiutil lists an image of this file name (not through a pipe into grep -q: that would
# close it early and fail the script under pipefail).
dmg_attached() {
    local info
    info=$(hdiutil info) || return 1
    case $info in *"$1"*) return 0 ;; esac
    return 1
}

# Detaches whatever is still attached from the writable image, then waits until hdiutil no longer
# lists it. Unmounting /Volumes/Sugo is not enough: with an APFS image the image itself can stay
# attached (and held by diskimages-helper) with no mount point at all, and converting it then
# fails with "Resource temporarily unavailable". Non-zero if the image is still listed.
release_dmg() {
    local rw=$1 name attempt dev
    name=$(basename "$rw")
    for attempt in 1 2 3 4 5 6; do
        # The whole disks (/dev/diskN) of the image's entry in `hdiutil info`.
        for dev in $(hdiutil info | awk -v n="$name" '
                /^image-path/ { inimg = index($0, n) > 0 }
                inimg && /^\/dev\/disk[0-9]+[ \t]/ { print $1 }'); do
            if [ "$attempt" -lt 3 ]; then
                hdiutil detach "$dev" >/dev/null 2>&1 || true
            else
                hdiutil detach -force "$dev" >/dev/null 2>&1 || true
            fi
        done
        dmg_attached "$name" || return 0
        sleep $((attempt * 2))
    done
    ! dmg_attached "$name"
}

# What holds the image, for the log of a failed conversion.
dmg_diagnostics() {
    echo "--- hdiutil info" >&2
    hdiutil info >&2 || true
    echo "--- open files of the image" >&2
    lsof "$1" >&2 2>&1 || true
    echo "--- end" >&2
}

# hdiutil sometimes fails with "Resource busy" on the CI runners: try again a few times.
RW=$(mktemp -u "${TMPDIR:-/tmp}/sugo-rw.XXXXXX").dmg
size_mb=$(( $(du -sm dmg | cut -f1) + 64 ))
for attempt in 1 2 3 4 5; do
    rm -f "$RW"
    if hdiutil create -volname Sugo -srcfolder dmg -fs APFS -format UDRW -size "${size_mb}m" -ov "$RW" >/dev/null; then
        break
    fi
    [ "$attempt" = 5 ] && fail "hdiutil could not create the disk image"
    echo "hdiutil failed (attempt $attempt), trying again" >&2
    sleep $((attempt * 5))
done
style_dmg "$RW" || echo "warning: could not set the disk image's window; it opens with Finder's default size" >&2
# The image must be detached before the conversion (the volume, then the image itself), and a
# failed attempt may leave a half-written output behind: clear both, and wait longer between the
# attempts. The conversion reads a copy of the image: if diskimages-helper still holds a lock on
# the original, the copy is free of it.
detach_dmg /Volumes/Sugo || echo "warning: /Volumes/Sugo is still mounted" >&2
release_dmg "$RW" || echo "warning: the writable image is still attached" >&2
sync
RWC="${RW%.dmg}-copy.dmg"
for attempt in 1 2 3 4 5 6; do
    rm -f "$OUTPUT" "$RWC"
    cp "$RW" "$RWC"
    if hdiutil convert "$RWC" -format ULFO -o "$OUTPUT" >/dev/null; then
        break
    fi
    if [ "$attempt" = 6 ]; then
        dmg_diagnostics "$RW"
        fail "hdiutil could not convert the disk image"
    fi
    echo "hdiutil convert failed (attempt $attempt), trying again" >&2
    release_dmg "$RW" || true
    sleep $((attempt * 10))
done
rm -f "$RW" "$RWC"
hdiutil verify "$OUTPUT" >/dev/null
echo "Built $OUTPUT ($VERSION, $(shasum -a 256 "$OUTPUT" | cut -d' ' -f1))"
