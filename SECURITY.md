<!--
  SPDX-FileCopyrightText: 2026 Sugo contributors
  SPDX-License-Identifier: CC-BY-SA-4.0
-->

# Security policy

Sugo is a local application: your timers, statistics and notes stay on your computer. The only things that leave it are the ones you turn on yourself: calendar sync with your own Nextcloud or CalDAV server, Sugo Sync, the update check, and the anonymous usage statistics (off unless you enable them).

## Reporting a vulnerability

**Please do not open a public issue for a security problem.** Use GitHub's private reporting instead: [Security → Report a vulnerability](https://github.com/sugoHQ/sugo/security/advisories/new). Only the maintainers can read the report, and the advisory stays private until a fix is out.

A useful report says what an attacker gains, the version of Sugo (`Settings → About`), how it was installed, and the steps to reproduce it. A proof of concept helps but is not required.

What to expect:

- an acknowledgement within a week;
- an assessment, and a fix or a mitigation plan, as soon as we can;
- credit in the advisory and the release notes, unless you prefer to stay anonymous.

Please give us the time to publish a fix before writing about the problem in public.

## Supported versions

Fixes go into the latest release and the latest nightly build. Older releases are not patched: the AppImage and the macOS app update themselves, so the fix reaches you with the next release.

## What is in scope

- **The application** (`sugoHQ/sugo`): the timer engine, the database, the QML interface, the platform services (D-Bus, inhibitors, autostart, KRunner, the Secret Service, the macOS Keychain), the calendar client, the update installer, and the parsing of everything that comes from outside (calendar responses, update feeds, sync blobs).
- **Sugo Sync** (`sugoHQ/sugo-sync`): the service at `sugo.stream` — the account and device endpoints, the encrypted blobs, the key handover between devices.
- **The release pipeline**: the workflows that build and publish the AppImage, the Flatpak bundle, the macOS disk image and the update feeds (`stable.json`, `nightly.json`).

## What is not in scope

- Builds of Sugo that come from anywhere but this repository's releases.
- Problems that need an attacker who already controls your user account, your desktop session, or your keyring.
- Denial of service against the free sync service, and reports produced only by a scanner without a working exploit.
- Vulnerabilities in Qt, libsodium or another dependency: report them upstream. We still want to hear about them if Sugo uses the affected code in a way that makes them worse.

## How Sugo protects your data

- **Secrets in the system keyring.** Calendar passwords, the sync token and the sync data key live in the Secret Service (KWallet, GNOME Keyring) on Linux and in the login Keychain on macOS. Without a keyring Sugo Sync does not start at all, rather than keeping the key in a file.
- **Sync is end-to-end encrypted.** The data key is 32 random bytes created by the first device; blobs are sealed with XChaCha20-Poly1305 and the key reaches a new device sealed with `crypto_box` for that device's key pair. The service stores ciphertext it cannot read.
- **No telemetry by default.** The anonymous usage statistics are off until you turn them on, and the settings row says what they send.
- **Pinned builds.** The workflows pin every third-party action to a commit and the build image to a digest, so a moved tag cannot change what runs near the release token.
