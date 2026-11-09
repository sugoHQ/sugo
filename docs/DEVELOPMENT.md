<!--
  SPDX-FileCopyrightText: 2026 Sugo contributors
  SPDX-License-Identifier: CC-BY-SA-4.0
-->

# Sugo development guide

How to build Sugo, run its tests and publish a release. For using the application see the [user guide](USAGE.md).

**Contents**

1. [Building from source](#building-from-source)
2. [Building the Flatpak](#building-the-flatpak)
3. [Building the AppImage](#building-the-appimage)
4. [Building the macOS app](#building-the-macos-app)
5. [Tests](#tests)
6. [Project layout](#project-layout)
7. [Translating](#translating)
8. [Releasing](#releasing)

---

## Building from source

### Requirements

- **C++17** compiler (GCC 11+ or Clang 14+)
- **CMake** 3.20+ and **Ninja**
- **Qt 6.8+** (Core, Gui, Widgets, Qml, Quick, QuickControls2, Sql, Network, Multimedia; DBus on Linux only: Sugo does not link it on macOS, though the app bundle still carries QtDBus.framework because qt.io's QtGui links it)
- **extra-cmake-modules 6.8+** (build tool only: no KDE Frameworks library is used)
- **libsodium 1.0.18+** and **pkg-config** (Sugo Sync's encryption, the same on Linux and macOS; the Flatpak builds it as a module, the AppImage bundles it)
- **Gettext** (`xgettext`), only to extract the strings

### Build dependencies

#### Fedora
```sh
sudo dnf install cmake ninja-build gcc-c++ \
    qt6-qtbase-devel qt6-qtdeclarative-devel qt6-qtsvg qt6-qtmultimedia-devel kf6-extra-cmake-modules \
    libsodium-devel pkgconf-pkg-config
```

#### Ubuntu 26.04 or newer
```sh
sudo apt install cmake ninja-build g++ \
    qt6-base-dev qt6-declarative-dev qt6-svg-plugins qt6-multimedia-dev libqt6sql6-sqlite extra-cmake-modules \
    libsodium-dev pkgconf
```

#### Arch Linux
```sh
sudo pacman -S cmake ninja gcc qt6-base qt6-declarative qt6-svg qt6-multimedia qt6-multimedia-ffmpeg extra-cmake-modules \
    libsodium pkgconf
```

#### openSUSE Tumbleweed
```sh
sudo zypper install cmake ninja gcc-c++ \
    qt6-base-devel qt6-declarative-devel qt6-svg-devel qt6-multimedia-devel qt6-sql-sqlite extra-cmake-modules \
    libsodium-devel pkgconf
```

#### macOS 13 or newer

With the Xcode command line tools (`xcode-select --install`) and [Homebrew](https://brew.sh):

```sh
brew install qt cmake ninja extra-cmake-modules libsodium pkgconf
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON \
    -DCMAKE_PREFIX_PATH="$(brew --prefix qt)"
cmake --build build
open build/bin/Sugo.app
```

libsodium (the sync's encryption) is found through pkg-config, so `pkgconf` is needed too. Without Homebrew, or to build with the same library as the release (universal and static), build it from source and point pkg-config at it:

```sh
bash packaging/macos/build-libsodium.sh "$HOME/sugo-deps/sodium"
export PKG_CONFIG_PATH="$HOME/sugo-deps/sodium/lib/pkgconfig"
```

The program is the app bundle `build/bin/Sugo.app` (its binary is `Sugo.app/Contents/MacOS/Sugo`); there is nothing to install. The Linux files (desktop entry, KRunner plugin, D-Bus service, metainfo) are not installed on macOS. Homebrew's Qt is enough to develop; the app published in the releases is built with Qt from qt.io, see [Building the macOS app](#building-the-macos-app).

For a system-wide install use `-DCMAKE_INSTALL_PREFIX=/usr` and run the install step with `sudo`.

The install step also puts the KRunner plugin, and the D-Bus activation file in place. A running Sugo keeps the old program in memory: quit it and start it again after installing, and restart KRunner (`kquitapp6 krunner`) when the runner definition changed.

---

## Building the Flatpak

```sh
flatpak-builder --user --install --install-deps-from=flathub --force-clean \
    "$HOME/.cache/sugo-flatpak-build" stream.sugo.app.yml
```

The sandbox permissions are in the `finish-args` of `stream.sugo.app.yml`. The manifest has nothing tied to one architecture: `release.yml` builds the bundle for x86_64 and aarch64, each on a runner of its own (`ubuntu-24.04`, `ubuntu-24.04-arm`) in the same multi-architecture image of flatpak-github-actions. On an ARM machine the command above builds the aarch64 bundle.

---

## Building the AppImage

There are two AppImages, `Sugo-x86_64.AppImage` and `Sugo-aarch64.AppImage` (64-bit ARM). `packaging/appimage/build.sh` builds the one of the machine it runs on (`uname -m`), with Qt 6.10 from qt.io (installed with aqtinstall), extra-cmake-modules built from source and linuxdeploy, all pinned in the script (linuxdeploy's SHA-256 once per architecture). Each is built in a container of the oldest system it runs on:

- **x86_64**: Ubuntu 22.04 (glibc 2.35).
- **aarch64**: Ubuntu 24.04 (glibc 2.39). qt.io builds its arm64 Qt on Ubuntu 24.04, and that Qt needs glibc 2.38 and the libstdc++ of GCC 13, which Ubuntu 22.04 does not have. Building Qt from source on an older system would lower that, at the cost of hours per Qt update; a distribution's Qt 6.8+ (Debian 13) would raise it.

```sh
podman run --rm -v "$PWD:/src:ro,z" -v "$PWD/out:/out:z" docker.io/library/ubuntu:22.04 \
    bash /src/packaging/appimage/build.sh 1.0.1-dev /out
# on an ARM machine:
podman run --rm -v "$PWD:/src:ro,z" -v "$PWD/out:/out:z" docker.io/library/ubuntu:24.04 \
    bash /src/packaging/appimage/build.sh 1.0.1-dev /out
```

The first argument is the version the program shows and compares when it looks for updates. In CI, `appimage.yml` builds both, each on a runner of its architecture (`ubuntu-24.04`, `ubuntu-24.04-arm`), and starts each without a display; the aarch64 build is not blocking yet (see [Releasing](#releasing)). To move to a newer linuxdeploy, take the SHA-256 of both files (`linuxdeploy-x86_64.AppImage`, `linuxdeploy-aarch64.AppImage`, and the same for linuxdeploy-plugin-qt) at the new tag. To try the result without installing it, unpack it (`./out/Sugo-x86_64.AppImage --appimage-extract`) and run `squashfs-root/AppRun`, or pass that to `tests/smoke_app.sh`. If AppImageLauncher is installed, starting the AppImage itself makes it offer to move the file into `~/Applications`.

---

## Building the macOS app

The release disk image is built on a Mac by `packaging/macos/build.sh` with Qt 6.10 from qt.io (installed with aqtinstall) and extra-cmake-modules from source, at the same versions as the AppImage, and libsodium from source. It needs the Xcode command line tools, CMake, Python 3 and git (Ninja and pkg-config are installed with Homebrew if missing):

```sh
bash packaging/macos/build.sh 1.0.1-dev out
```

The arguments are the version, as for the AppImage, and the folder that receives `Sugo-macOS-universal.dmg`. The script builds one universal app (`arm64` and `x86_64`, macOS 13 or later), copies Qt into it with `macdeployqt` (SQLite is the only SQL driver kept; the `offscreen` platform plugin is added for the smoke tests), and fails if the bundle is incomplete: a missing platform, SQL, SVG, multimedia (FFmpeg) or TLS plugin or QML module, a library loaded from outside the bundle (other than macOS's own), or a file without both architectures. It then signs the app ad hoc (`codesign --sign -`, required on Apple Silicon), checks the signature and `--version`, and makes the disk image with a link to `/Applications`. The downloads and the build are kept in `WORK` (`/tmp/sugo-macos` by default) and reused by the next run.

**libsodium.** Homebrew's libsodium is a dylib for one architecture, so `packaging/macos/build-libsodium.sh` builds the release pinned in it (`LIBSODIUM_VERSION`, with the tarball's `LIBSODIUM_SHA256`) once for `arm64` and once for `x86_64` (macOS 13 or later), joins the two static libraries with `lipo`, and installs them with a `libsodium.pc` in `WORK/sodium`. `build.sh` puts that folder first in `PKG_CONFIG_PATH`, so CMake's `pkg_check_modules` finds it before any other copy, and fails if the program links a libsodium dylib or, once the sources include `sodium.h`, lacks its code in either architecture: linked statically, it adds nothing to the bundle. To move to a newer libsodium, check the tarball's minisign signature against libsodium's key (`RWQf6LRCGA9i53mlYecO4IzT51TGPpvWucNSCh1CBM0QTaLn73Y7GFO3`, on [libsodium's site](https://doc.libsodium.org/installation)), then change the version and the SHA-256 in the script; the CI caches follow the version.

The app is not signed with a Developer ID nor notarized, so Gatekeeper asks users to confirm the first launch (see the [user guide](USAGE.md#macos)). `SIGN_IDENTITY="Developer ID Application: …"` signs with that identity and the hardened runtime instead, with the entitlements of `packaging/macos/entitlements.plist` (`com.apple.security.cs.allow-jit`, without which the hardened runtime refuses the QML engine's JIT); notarization is not done by the script. The script's `--version` check loads no QML, so after such a build open a page of the real interface (or run a smoke page) before publishing it.

### The Focus shortcuts

On macOS Sugo silences notifications by running two shortcuts of the Shortcuts app, "Sugo Focus On" (Set Focus: Do Not Disturb on, until turned off) and "Sugo Focus Off" (Do Not Disturb off). The app ships them, signed by Apple, in `data/shortcuts/` (bundled into `Sugo.app/Contents/Resources/shortcuts/`); "Install Shortcuts" (settings, welcome tour) opens one, waits until the user has added it (`shortcuts list`), then opens the other. Opening a signed file needs no iCloud on the user's Mac, but signing it does, so the files are made once by a maintainer:

1. On a Mac signed into iCloud, create the two shortcuts in Shortcuts, each with the single action "Set Focus" ("Imposta Full immersion" in the Italian interface): "Sugo Focus On" turns Do Not Disturb on until turned off, "Sugo Focus Off" turns it off. The names must be exactly these: the file name becomes the shortcut's name, and Sugo looks for them.
2. Select each and choose File → Export, then "Anyone" (not "People Who Know Me"), and save it as `Sugo Focus On.shortcut` / `Sugo Focus Off.shortcut`. Such a file starts with `AEA1` (an Apple Encrypted Archive carrying Apple's signature); `tst_shortcutsfocusinstaller` checks that.
3. Replace the files in `data/shortcuts/` (keep their `.license` files), and check on a Mac without iCloud, or another user account, that opening each shows the "Add Shortcut" sheet.

To try the result, open the disk image, or run `tests/smoke_app.sh` on `Sugo.app/Contents/MacOS/Sugo` (it needs GNU `timeout`: `brew install coreutils` and put `$(brew --prefix coreutils)/libexec/gnubin` first in `PATH`). In CI, `macos.yml` builds it (keeping Qt, extra-cmake-modules and libsodium in its cache) and starts it without a display; `nightly.yml` and `release.yml` publish it when that succeeds, and publish the Linux packages without it when it fails.

---

## Tests

```sh
ctest --test-dir build --output-on-failure
```

The suite covers the timer engine, the database and statistics, the platform services (D-Bus, inhibitors, autostart, KRunner), the calendar client against an in-process fake server, and smoke runs of every QML page.

The continuous-integration server has no display and no session bus. To get the same conditions locally, run the tests in a clean environment:

```sh
env -i PATH=/usr/bin:/bin HOME=/tmp/sugo-test-home \
    ctest --test-dir build --output-on-failure
```

A test that builds a GUI application sets `QT_QPA_PLATFORM=offscreen` for itself (see `tests/CMakeLists.txt`); tests that need D-Bus start a private daemon and are written to work without a session bus. The build is expected to be free of compiler warnings.

CI builds on Linux with GCC (and runs the suite) and with Clang (its warnings are close to those of Apple's compiler), both with warnings as errors, and on macOS (arm64, Qt from qt.io, warnings as errors, the whole suite; not blocking yet).

CI runs only what a change can affect: its first job, Changes, reads the changed files and `.github/scripts/ci-changes.sh` decides which builds they need. Documentation runs nothing, a macOS-only file (`packaging/macos/`, the `.mm` sources, the `Mac*` sources built under `if(APPLE)`) only the macOS job, a Linux-only one (the D-Bus services, the desktop entry and the metainfo) only the Linux jobs, and the test scripts skip the Clang build; anything else, or a change it cannot read, runs everything. A pull request is compared with its base; a push with the commit of the newest CI run on its branch that passed, not with the commit before it, so the changes of a run that failed or was cancelled stay in the comparison until a run passes with them (otherwise a push of `po/sugo.pot` alone after a failed one would pass, and on `testing` publish a nightly no test ran on); with no such run, or one whose commit is not an ancestor of the push, everything runs. A file added for one platform only goes in that script's lists. The Linux jobs run in a container with every build dependency already installed (`.github/ci-image/Dockerfile`, on ghcr.io as `sugo-ci`, tagged with a hash of the Dockerfile): the CI image job builds it only when the Dockerfile changes, so a new dependency goes in the Dockerfile. The jobs keep the compiler cache (ccache) between runs, and a newer push cancels the run of the older one.

**On macOS** the same `ctest` command works (the smoke runs need GNU `timeout`, see [Building the macOS app](#building-the-macos-app)). What is tested only on Linux: the freedesktop services over D-Bus (notifications, notification and power inhibitors, KRunner, the `/Timer` service, the single-instance bus name, the Secret Service, the Background portal), which the macOS build does not compile at all: QtDBus is not linked there (`SUGO_DBUS` in the top-level `CMakeLists.txt`), so `tst_powerinhibitor`, `tst_dbustimerservice`, `tst_singleinstance`, `tst_freedesktopnotifier`, `tst_freedesktopinhibitor`, `tst_krunnerservice` and `tst_secretstore` are not built, and the D-Bus cases of `tst_screenlockwatcher` and `tst_autostart` skip. The AppImage's self-update is not used on macOS, where the app bundle updates itself instead (`MacBundleInstaller`); `tst_updatechecker` tests both installers with stand-in tools on either system. `tst_mackeychainstore` writes to the real login Keychain, so it runs only with `SUGO_TEST_KEYCHAIN=1` from a terminal of the logged-in desktop session; over SSH there is no login session (every Keychain call fails with OSStatus -25308) and it skips. What happens to the Keychain items across updates of an ad hoc signed app is checked by hand (`tests/manual-review-checklist.md`, "macOS: Keychain").

### Linux and macOS parity

`tst_platform_parity` makes sure Sugo offers the same on both systems. It starts the program in the smoke mode `inventory` (`src/qml/components/FeatureInventory.qml`), which opens every screen (the focus page, a timer's page, a new timer, statistics, each settings category from General to About, each step of the welcome tour, the search) and lists what the user can use there: rows, switches (by the setting they change), buttons, choices, fields, sliders and the search's commands. `tests/check_platform_parity.py` compares the list with `tests/platform-parity.json`:

- `common`: on both systems; one missing here fails the test.
- `linux` / `macos`: on that system only, each with the reason (the Dock, the Focus shortcuts, keeping running in the tray...). Anything else found on one system only fails the test.
- `updateInstall`: only in builds that install updates (AppImage, the disk image's `Sugo.app`).
- `macosNames`: the Mac wording of the same control ("menu bar" for "tray", "Show in Finder" for "Open folder").
- `tests`: for each platform feature, the tests that must exist on each system (notifications, silencing, screen awake, autostart, keyring, single instance, external control, quitting...).

So a control added on one system only, or lost on one, shows up in CI; if the difference is intended, list it with its reason.

To compare the two by eye, build the same commit on Linux and on a Mac, then:

```sh
SUGO_MAC_SSH="ssh user@mac" SUGO_MAC_APP='~/sugo/build/bin/Sugo.app/Contents/MacOS/Sugo' \
    tests/compare_platforms.sh build/bin/sugo /tmp/sugo-compare
```

It takes the same screenshots of every screen on both and writes `/tmp/sugo-compare/index.html`: each screen side by side, with the declared differences and their reasons.

### Environment variables for development

| Variable | Effect |
|---|---|
| `SUGO_START_PAGE` | Opens on another page: `presets` (the timer in use), `stats`, `settings`, `welcome`; or on a part of one: `settings/<category>` (`general`, `timers`, `sounds`, `focus`, `calendar`, `sync`, `advanced`, `about`; `about` alone is `settings/about`), `welcome/<step>` (0-3) |
| `SUGO_SMOKE_PAGE`, `SUGO_STATS_SMOKE_TEST=1` | Smoke runs (`tests/smoke_app.sh`): check a page, then quit |
| `SUGO_SMOKE_PAGE=inventory`, `SUGO_INVENTORY=file.json` | Writes what every screen offers to the file, then quits ([parity](#linux-and-macos-parity)) |
| `SUGO_SCREENSHOT=file.png` | Saves a picture of the window after `SUGO_SCREENSHOT_DELAY` ms (1500 by default), then quits |
| `SUGO_WINDOW_STYLE` | Forces the title bar style (`kde`, `gnome`, `macos`, `windows`, `generic`, ...); `macos` also shows the wording and shortcuts of the Mac version, to preview it on Linux |
| `SUGO_DATA_DIR` | The folder that holds `sugo.db` and `sugo.conf`, instead of the usual ones: keeps test runs (and the macOS smoke runs, where `XDG_*` has no effect) away from your data |
| `SUGO_OVERRIDE_VERSION` | The version the update check compares, to try it against the published feeds |
| `SUGO_SYNC_URL` | Another server for Sugo Sync and the usage statistics (`POST /v1/telemetry`), e.g. `http://localhost:8787` (`wrangler dev` of sugo-sync) |

---

## Project layout

| Path | Contents |
|:---|:---|
| `src/core/` | Timer engine, presets, settings, notifications, tray, calendar sync, update checker, diagnostics |
| `src/db/` | SQLite access: schema and migrations, presets, sessions, the recorder that stores finished phases |
| `src/stats/` | Statistics aggregation, the model behind the Statistics page |
| `src/platform/` | Desktop services: D-Bus API, KRunner, inhibitors, autostart, screen lock, single instance, signals |
| `src/qml/` | The interface (Qt Quick, our own components and `theme/Theme.qml`): `Main.qml`, `pages/`, `components/` |
| `data/` | Desktop entries, KRunner and D-Bus activation files, AppStream metadata, icon |
| `po/` | String template (`sugo.pot`) and the script that extracts it |
| `tests/` | The test suite |
| `.github/workflows/` | CI and release workflows |

All the C++ code but `src/main.cpp` is built once, as the static library `sugo_core` (also the QML module), which the program and every test link.

Usage statistics (`src/core/Telemetry.h`, opt-in, off by default): the events and their values are listed in `docs/USAGE.md` ("Anonymous usage statistics") and must be exactly what the Worker accepts (sugo-sync, `worker/src/telemetry.ts`, `EVENTS`). To add an event or a property, or a distribution to `TelemetryInfo::distroFrom()`, change the Worker first and deploy it, then Sugo, then `docs/USAGE.md`: the Worker silently drops what it does not know. Smoke runs never send anything.

Conventions worth knowing: user-visible strings go through `i18n()` (never `tr()`), with `i18np()` for anything that can be plural; QML uses only the colours and sizes of `Theme` (`src/qml/theme/Theme.qml`).

---

## Translating

For now the program is in English only. The texts still go through the KI18n-style `i18n()`, `i18nc()`, `i18np()` and `i18ncp()` functions of `src/core/Localization.h` (in C++ and QML), so translations can come back without touching the sources: `bash po/Messages.sh` still extracts every string to `po/sugo.pot`.

---

## Branches

| Branch | For | A push runs |
|:---|:---|:---|
| `dev` | Daily work: every commit goes here | CI (build and tests) |
| `testing` | What is ready to try: `dev` brought over when you decide | CI, then the nightly build (`nightly.yml`) |
| `main` | What has been tried: `testing` brought over once it works | CI; releases are tagged here |

Each step is a fast-forward push:

```sh
git push origin dev:testing     # publish a nightly build of dev
git push origin testing:main    # mark the tested build as the base of the next release
```

---

## Releasing

A release is a version bump, a commit, an annotated tag on `main` and a push. The release workflow (`release.yml`) builds the AppImages and the Flatpak bundles (x86_64 and aarch64) and publishes the GitHub release when it sees a `v*` tag, with `stable.json`: the feed the AppImages and the macOS apps on the stable channel read to update themselves (`releases/latest/download/stable.json`, so the release must be the latest one). Every file of a release or a nightly is named after its version, the same way for each package: `Sugo-X.Y.Z-x86_64.AppImage`, `Sugo-X.Y.Z-aarch64.AppImage`, `Sugo-X.Y.Z-x86_64.flatpak`, `Sugo-X.Y.Z-aarch64.flatpak`, `Sugo-X.Y.Z-macOS-universal.dmg` (the build scripts write them without the version; the publish jobs rename them). So no link can point straight at the latest file: the README and the user guide link the page of the latest release (`releases/latest`), and the feeds carry each file's full address.

**Nightly builds.** Every push to `testing` that passes CI is built by `nightly.yml` and published as a new pre-release tagged `vX.Y.Z-nightly.YYYYMMDD.N` (`release.yml` ignores these tags): the AppImages and `nightly.json`. Started by hand (Run workflow on `testing`), it publishes only when the newest CI run of that commit on `testing` passed. The pre-release `nightly` stays as the fixed address of the feed of the nightly channel (it holds only the latest `nightly.json`; its tag follows `testing`, the branch can't be called `nightly` like the tag) and as the starting point of the next release notes. Old nightlies are not deleted. Its version is a pre-release of the version in `CMakeLists.txt`, `X.Y.Z-nightly.YYYYMMDD.N` (of the next patch if `vX.Y.Z` is already released), so a nightly is newer than the last release and older than the next one. `YYYYMMDD` is the day the workflow run was created (UTC), not the date of the commit, and `N` its run number, so each nightly is newer than the ones before it even when `testing` moves to an older commit. The feed and the tag `nightly` only move forward: a publish that finds `nightly.json` already announcing a newer version (an older run re-run, or one that reached the publish job late) keeps its own pre-release and leaves them as they are. Bump `CMakeLists.txt` after a release to name the next one. The build commands of both are in `appimage.yml`, which also starts the AppImage without a display before anything is published.

**The feeds and the architectures.** The top-level keys of `stable.json` and `nightly.json` (`version`, `url`, `sha256`, `size`, `page`) describe the x86_64 AppImage, as they always have: the AppImages of Sugo 1.0.x read only those, and keep updating from them. Every other package reads its own object, with the `url`, `sha256` and `size` of its file: `linux-aarch64` for the arm64 AppImage, `macos` for the disk image (below). Which object an AppImage reads is decided by the architecture it was built for (`AppImageInstaller::feedKeyFor()`, `QSysInfo::buildCpuArchitecture()`), so an arm64 AppImage never downloads the x86_64 file nor the other way round; a feed without its object only reports the new version. `tst_updatechecker` checks both architectures on any machine.

**aarch64.** Both workflows build the aarch64 AppImage (and `release.yml` the aarch64 Flatpak bundle) next to the x86_64 one. For now a failure there does not stop the release or the nightly: they are published without the aarch64 files (the publish job leaves a warning), and the feed then has no `linux-aarch64` entry, so the arm64 AppImages only report the update. By hand, as for the disk image below: build it on an ARM machine, upload `Sugo-X.Y.Z-aarch64.AppImage`, then add `"linux-aarch64": {url, sha256, size}` to `stable.json` with the same `jq` command (`sha256sum` and `stat -c %s` on Linux) and upload the feed again.

**macOS.** Both workflows also build `Sugo-macOS-universal.dmg` with `macos.yml` (on a macOS runner, see [Building the macOS app](#building-the-macos-app)). For now a failure there does not stop the release: the Linux packages and the feed are published without the disk image. When it is there it is uploaded with the other files, the first launch (Gatekeeper) is explained in the [user guide](USAGE.md#macos), not in the notes, and the feed gets a `macos` entry (`url`, `sha256`, `size` of the disk image) next to the AppImage's keys, which the AppImages ignore. The macOS app reads that entry to update itself, like the AppImage, when its bundle is in a folder it can write to (`/Applications` for an administrator; `MacBundle::path()`, `MacBundleInstaller` in `src/core/UpdateInstaller.h`): it downloads the disk image next to the bundle, checks its size and SHA-256, mounts it (`hdiutil attach -nobrowse -readonly`), checks that its `Sugo.app` has the bundle identifier `stream.sugo.app` and a valid signature (`codesign --verify --deep --strict`), copies it next to the running app as `.Sugo.app.new`, hidden so that Spotlight and launchers do not list it (`ditto`, then `xattr -cr`) and detaches the image; when Sugo quits, the new bundle takes the old one's place, and "Restart to update" opens it (`open -n`). Any failure leaves the installed app as it was. Only the disk image's app does this: `packaging/macos/build.sh` configures with `-DSUGO_INSTALLS_UPDATES=ON`, which puts `SugoInstallsUpdates` = true in its `Info.plist`; a `Sugo.app` built from source has it false and, like a build from source on Linux, only reports new versions (otherwise "Install update" would replace the build being worked on with the release). A feed without `macos`, or an app in a folder it cannot write to, also only reports the new version, with a link to the release page.

**Checklist** for version `X.Y.Z`:

1. `CMakeLists.txt`: `project(Sugo VERSION X.Y.Z …)`.
2. `data/stream.sugo.app.metainfo.xml`: add a new `<release version="X.Y.Z" date="YYYY-MM-DD">` **above** the previous ones, with a short list of what changed. `release.yml` puts its lead and list at the top of the GitHub release as "Highlights", ahead of the generated list of pull requests.
3. Build and run the whole test suite in a clean environment.
4. Validate the metadata:
   ```sh
   appstreamcli validate --no-net data/stream.sugo.app.metainfo.xml
   desktop-file-validate data/stream.sugo.app.desktop \
       build/stream.sugo.app.runner.desktop
   ```
5. Commit (`Release vX.Y.Z: …`), then tag and push:
   ```sh
   git tag -a vX.Y.Z -m "Sugo X.Y.Z: …"
   git push origin main vX.Y.Z
   ```
6. Check the aarch64 part of the run: both aarch64 jobs passed (`Build AppImage (aarch64)`, `Build Flatpak bundle (aarch64)`), the release has `Sugo-X.Y.Z-aarch64.AppImage` and `Sugo-X.Y.Z-aarch64.flatpak`, and `stable.json` has a `linux-aarch64` entry. If they failed, see "aarch64" above.
7. Check the macOS part of the run: the job of `macos.yml` passed, the release has `Sugo-X.Y.Z-macOS-universal.dmg`, and `stable.json` has a `macos` entry. If the macOS job failed, the release went out without it: fix the build and attach the disk image by hand, or leave it for the next release. By hand, the feed needs the disk image's `macos` entry too, as `release.yml` writes it (without it the Macs see the update but never install it): build with `bash packaging/macos/build.sh X.Y.Z out` on a Mac, then
   ```sh
   cd out
   dmg=Sugo-X.Y.Z-macOS-universal.dmg
   mv Sugo-macOS-universal.dmg "$dmg"
   gh release upload vX.Y.Z "$dmg" --repo sugoHQ/sugo
   gh release download vX.Y.Z --repo sugoHQ/sugo --pattern stable.json --clobber
   jq --arg url "https://github.com/MineraleYT/sugo/releases/download/vX.Y.Z/$dmg" \
      --arg sha256 "$(shasum -a 256 "$dmg" | cut -d' ' -f1)" \
      --argjson size "$(wc -c < "$dmg" | tr -d ' ')" \
      '. + {macos: {url: $url, sha256: $sha256, size: $size}}' stable.json > stable.json.new
   mv stable.json.new stable.json
   gh release upload vX.Y.Z stable.json --repo sugoHQ/sugo --clobber
   ```
   The address keeps the path from before the move to sugoHQ, as `release.yml` writes it: GitHub redirects it, and Sugo 1.0.2 and older download only from there. The disk image goes first: the feed must never announce a file that is not there. On a Mac, open the disk image, drag Sugo to Applications, and check the first launch and `Sugo.app/Contents/MacOS/Sugo --version`.

The workflow refuses to publish unless the tag, the version in `CMakeLists.txt` and the newest `<release>` in the metainfo all agree. Every `v*` tag is built, tested and published, whatever the tagged commit changed. Pushes that only change Markdown files or `docs/` skip CI. Third-party actions and the build image are pinned to a commit or digest; Dependabot proposes updates.
