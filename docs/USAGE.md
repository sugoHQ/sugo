<!--
  SPDX-FileCopyrightText: 2026 Sugo contributors
  SPDX-License-Identifier: CC-BY-SA-4.0
-->

# Sugo user guide

This guide describes how to install Sugo and use it day to day, on Linux and on macOS. To build it yourself or contribute, see the [development guide](DEVELOPMENT.md).

**Contents**

1. [Installing](#installing)
2. [First start](#first-start)
3. [Timers](#timers)
4. [Staying focused](#staying-focused)
5. [The tray icon and its menu](#the-tray-icon-and-its-menu)
6. [Keyboard shortcuts](#keyboard-shortcuts)
7. [Statistics](#statistics)
8. [Task notes](#task-notes)
9. [Calendar events](#calendar-events)
10. [KRunner](#krunner)
11. [Scripts and automation](#scripts-and-automation)
12. [Data, backup and privacy](#data-backup-and-privacy)
13. [Updates and diagnostics](#updates-and-diagnostics)

---

## Installing

Every [release](https://github.com/sugoHQ/sugo/releases/latest) offers three packages:

| Package | For | Updates |
|:---|:---|:---|
| [AppImage](#appimage) | any recent x86_64 Linux | installs them itself, when you say so |
| [Flatpak](#flatpak) | x86_64 Linux with Flatpak | through Flatpak, by installing the new bundle |
| [macOS disk image](#macos) | macOS 13 Ventura or later, Apple Silicon and Intel | installs them itself, when you say so |

### AppImage

One file that runs on any recent x86_64 Linux and keeps itself up to date (the recommended package on Linux):

1. Download `Sugo-<version>-x86_64.AppImage` from the [latest release](https://github.com/sugoHQ/sugo/releases/latest).
2. Make it executable and start it:
   ```sh
   chmod +x Sugo-*-x86_64.AppImage
   ./Sugo-*-x86_64.AppImage
   ```

Keep the file in a folder you can write to (for example `~/Applications`): Sugo puts its updates in place of it. If AppImageLauncher is installed, starting the AppImage makes it offer to move the file there and add Sugo to the application menu. How updates work is described in [Updates and diagnostics](#updates-and-diagnostics).

### Flatpak

The Flatpak bundle is attached to each release as `Sugo-<version>-x86_64.flatpak`. It is not on Flathub yet, so it is installed from the file:

1. Download `Sugo-<version>-x86_64.flatpak` from the [latest release](https://github.com/sugoHQ/sugo/releases/latest).
2. Install it for your user, then start Sugo from the application menu, or with:
   ```sh
   flatpak install --user ./Sugo-*-x86_64.flatpak
   flatpak run stream.sugo.app
   ```

The first install also downloads the KDE runtime from Flathub, if it is not installed yet.

- **Updating:** the Flatpak does not update itself. **Settings → About → Check for Updates** tells you when a new release is out: download its bundle and install it the same way, over the old one. Your timers, statistics and settings are kept.
- **Removing:** `flatpak uninstall --user stream.sugo.app` removes the app; add `--delete-data` to remove your data too.
- **Permissions:** the sandbox has network access (for the update check and calendar sync), the notification and screen-saver services (to silence notifications and keep the screen awake), the system tray, and audio. *Start at login* goes through the desktop's background portal, which may ask for your permission the first time.
- **Your data** lives under `~/.var/app/stream.sugo.app/` instead of the usual folders (see [Data, backup and privacy](#data-backup-and-privacy)).
- **From Sugo 1.0.2 or older:** the app ID changed from `io.github.mineraleyt.sugo` to `stream.sugo.app`, so 1.0.3 installs as a new app next to the old one. At its first start it copies your timers, statistics and settings from the old app (it may read `~/.var/app/io.github.mineraleyt.sugo/` for that); then remove the old one with `flatpak uninstall --user io.github.mineraleyt.sugo`. If Sugo started at login, turn **Start at login** on again.

### macOS

One app for both Apple Silicon and Intel Macs, for **macOS 13 Ventura or later**:

1. Download `Sugo-<version>-macOS-universal.dmg` from the [latest release](https://github.com/sugoHQ/sugo/releases/latest), open it and drag **Sugo** to **Applications**.
2. Start it. Sugo is not notarized by Apple yet, so macOS blocks the first launch:
   - on macOS 15 Sequoia or later, open **System Settings → Privacy & Security**, scroll to the message about Sugo and click **Open Anyway**;
   - on macOS 13 and 14, Control-click Sugo in Applications, choose **Open**, then **Open** again;
   - or, in Terminal, remove the quarantine mark: `xattr -dr com.apple.quarantine /Applications/Sugo.app`.

   macOS remembers the choice: later launches open normally.

Sugo updates itself like the AppImage when it is in a folder you can write to, such as **Applications** for an administrator account. Installed elsewhere, it only tells you that a new release is out: download the new disk image and replace the app.

On macOS Sugo lives in the **menu bar** and the **Dock** rather than a system tray, keeps the Mac's own title bar, and uses the **Keychain** for the calendar password. Everything else works the same as on Linux; where it differs, this guide says so.

### From source

Sugo can also be built from source on Linux and macOS: dependencies for the common distributions, build and install steps, and a local Flatpak build are in the [development guide](DEVELOPMENT.md#building-from-source).

---

## First start

The first time you open Sugo, a short animated tour shows how timers work and sets up a few things: the tray icon (the menu bar icon on macOS), starting at login and, on macOS, the shortcuts that silence notifications while you work. Everything it sets can be changed later in **Settings**.

The window has three tabs at the top: **Focus** (the timer), **Statistics** and **Settings**; the search button next to them (`Ctrl` + `K`) finds pages, timers and commands. To pick a timer, click its name under the controls on the Focus tab, or search for it, then press **Start**. That is all there is to it: Sugo counts down the work phase, tells you when to take a break, and records the session in your [statistics](#statistics).

---

## Timers

A *timer* (preset) bundles the durations of a work phase, a short break and a long break, how many work sessions come before a long break, and a few options. Sugo starts with one timer, **Pomodoro** (25/5, a 15-minute break after four sessions), which silences desktop notifications while you work; edit it, or add timers of your own with **New timer** (in the list of timers on the Focus tab, in **Settings → Timers**, or in the search). **Settings → Timers** lists every timer, grouped by category: click one to open its page, change it and press **Save** at the top.

- **Name, category and icon** appear in the timer list, the tray menu and the statistics. The category is also used as the label of the phase chip while you work.
- **Auto-start** decides what happens when a phase ends: *Manual* (nothing starts by itself), *Breaks only*, or *All phases*.
- **A long break of 0 minutes** means no long break: the set still ends there, with a short break instead. Set both breaks to 0 to go straight from one work session to the next.
- **Silence notifications while working**, **play a sound** and **show a notification** when a phase ends can be set per timer.
- The timer you pick stays selected the next time you start Sugo. You cannot switch timers while one is running.

Pausing keeps the remaining time; stopping resets the phase. A phase stopped early is recorded as *interrupted* in the statistics if it ran for at least five seconds.

Skipping a running work session counts it towards the long break. Skipping a work session that has not started yet (for example with the notification's **Skip** button) moves on to the short break without counting it, so the long break is only reached by working.

When the computer goes to sleep, a running phase is paused, and it stays paused after the computer wakes up until you resume it: the time asleep never counts as work or as a break.

---

## Staying focused

While you work, Sugo can keep distractions away. These options are in **Settings → Focus** (the focus sound in **Settings → Sounds**), and the first one is set per timer:

- **Silence notifications while working** (per timer): other apps' notifications are held back during the work phase and come back for the break.
  - On **Linux** Sugo asks the desktop's notification service directly; there is nothing to set up.
  - On **macOS** Sugo turns *Do Not Disturb* on and off through two shortcuts of the Shortcuts app, "Sugo Focus On" and "Sugo Focus Off". Press **Install Shortcuts** in the welcome tour or in **Settings → Focus → Focus while working** and add each one when Shortcuts asks. Until both are installed, a timer offers **Set Up…** instead of the switch.
- **Keep the screen awake during work sessions**, so the screen does not dim or lock in the middle of one.
- **Pause timer when screen is locked**: locking the screen pauses the timer, and when you unlock it Sugo asks whether to resume.
- **Focus sound**: an optional ambient sound that plays while you work, at the volume you choose.

---

## The tray icon and its menu

On macOS the same icon and menu are in the **menu bar**.

The tray icon shows a ring with the minutes left (red while working, green during breaks). Its context menu contains, from the top:

1. **A status line** (not clickable): the phase, the time left and the name of the timer, for example `Work · 12:30 · Pomodoro`.
2. **Start / Pause / Resume**, and **Stop** and **Skip**, which appear only while a phase is running or paused.
3. **Timer**: a submenu with all your timers; pick one to switch to it. It is disabled while a timer is running.
4. **Statistics** and **Settings**, which open the window on that page.
5. **Show Window / Hide Window** and **Quit**.

Closing the window keeps Sugo running in the tray if *Settings → System tray → Keep running in the tray when the window is closed* is on. If your desktop has no system tray, the tray options are disabled and closing the window quits the app.

On macOS, closing the window always leaves Sugo running in the Dock and the menu bar; **Quit** (`⌘` + `Q`) ends it. **Settings → General → Dock** can show the minutes left as a badge on the Dock icon, and hide the Dock icon while the window is hidden.

---

## Keyboard shortcuts

On macOS, use `⌘` (Command) wherever this table says `Ctrl`.

| Shortcut | Action |
|:---|:---|
| `Space` | Start or pause the timer (on the Focus page, when nothing else has the focus) |
| `S` | Skip to the next phase (on the Focus page, while a phase runs) |
| `Ctrl` + `1` … `4` | Go to Focus, Statistics, Settings, Settings → About |
| `Ctrl` + `K` | Search pages, timers and commands |
| `Ctrl` + `N` | New timer |
| `Ctrl` + `,` | Open Settings |
| `Ctrl` + `S` / `Esc` | Save / drop the changes on a timer's page |
| `Ctrl` + `Q` | Quit |

For shortcuts that work from anywhere, bind the [D-Bus commands](#linux-d-bus) in Plasma's **System Settings → Shortcuts → Add Command**, or, on macOS, give a [sugo:// link](#macos-sugo-links) a keyboard shortcut in the Shortcuts app.

---

## Statistics

The **Statistics** page offers day, week, month and year views with:

- work and break time, completed and interrupted sessions;
- how the work time compares with the same stretch of the period before;
- your streak of days in a row; with *weekend streak protection* an inactive Saturday or Sunday does not break the streak;
- a chart of the period and a distribution by timer, category or task.

Sessions are counted on the day they started, in your time zone. The page follows the clock: after midnight it moves on to the new day.

---

## Task notes

When a work session ends, Sugo can ask what you worked on (**Settings → Focus → Streak and notes → Prompt for a task note when a work session finishes**). Type it in the notification's reply field or in the in-app dialog, which also suggests recent notes. Notes are stored with the session and shown in the "Task" view of *Where the time went* in the statistics.

---

## Calendar events

Sugo can add an event to your calendar every time you **finish a work session**, for example "Pomodoro, 25 minutes". Breaks and interrupted phases are not sent. It is **off by default** and set up in **Settings → Calendar**.

### With Nextcloud

1. Choose **Nextcloud** as the calendar service and type your server address (for example `https://cloud.example.org`).
2. Press **Log in** and authorize Sugo in the browser tab that opens. Sugo receives an *app password* by itself; you do not copy anything.
3. If you have several calendars, pick one in the **Calendar** list. The refresh button next to it reloads the list from the server.
4. Press **Test** to check the connection: a green check on the button means it works.

In Nextcloud (*Settings → Security → Devices & sessions*) the new entry is named **Sugo**. Delete it there to revoke Sugo's access, and press **Disconnect** in Sugo to forget the login.

### With another CalDAV calendar

Choose **Custom CalDAV** and enter the calendar's CalDAV address, your user name and a password (use an *app password* if your provider offers them). Press the refresh button to list the calendars at that address and pick one. Nextcloud and Custom CalDAV keep **separate settings**, so switching between them does not mix up addresses or passwords.

### Good to know

- **Subscription links do not work.** A calendar link ending in `.ics` is read-only: it lets you *see* events, not add them. Use the calendar's CalDAV address.
- **What is sent:** the timer name, its category and the real start and end times. The task note goes in the event description only if you turn on *Include the task note in the event description*.
- **Secure connections only:** addresses must be `https://` (plain `http://` is accepted only for `localhost`). Certificate errors are never ignored.
- **If the server is unreachable**, events wait in a queue (up to 100) and Sugo retries every few minutes while it is running. The queue is kept when you quit and sent at the next start. Each waiting event belongs to the account that was set up when it was recorded: if you sign in with another account, the old account's waiting events are dropped instead of being sent to the new one. Work sessions that end after you turn the calendar on but before you finish signing in also wait, and go to the account you then set up (unless it is another user name than the one you had entered).
- **Where the password is kept:** on macOS, in the Keychain. On Linux, in your desktop keyring (KWallet on Plasma, or any other Secret Service keyring); only when no keyring is available is it kept in `~/.config/sugo/sugo.conf`, a file readable only by you; it moves to the keyring by itself the next time one is available. It is never written to logs or to the diagnostics report.
- **Not supported:** Proton Calendar (to our knowledge it offers no way for other apps to add events), and calendars that only accept an OAuth sign-in, such as Google Calendar or Microsoft 365.

---

## KRunner

On KDE Plasma, Sugo adds a runner to the launcher. Press `Alt` + `Space` (or `Meta`) and type:

| Query | What it does |
|:---|:---|
| `pomodoro`, `sugo`, `tomato` or `timer` | Shows the current status, the time left and quick actions |
| `pomodoro start` | Start or resume the timer |
| `pomodoro pause` / `pomodoro resume` | Pause or continue the countdown |
| `pomodoro stop` | Stop and reset |
| `pomodoro skip` | Go to the next phase |
| `pomodoro <timer name>` (or part of it) | Switch to that timer and start it |
| `pomodoro 25` (any number of minutes, 1–180) | Start a one-off work session of that length; your timer's own duration is not changed |

While a timer runs, the status result also offers buttons (select it, or press `Shift` + `Enter`): **Pause/Resume**, **Skip** and **Stop**.

Opening KRunner does **not** start Sugo; it starts (hidden in the tray) only when you type one of the words above. After installing or updating, restart KRunner once (`kquitapp6 krunner`) so that it reads the new plugin.

---

## Scripts and automation

Other programs can control the timer: on Linux through D-Bus (and [KRunner](#krunner)), on macOS by opening `sugo://` links. Both can be turned off in **Settings → Advanced** (*Global Shortcuts and Automation* on Linux, *Shortcuts and Automation* on macOS) with **Allow other programs to control the timer**; a refused command then changes nothing.

### Linux: D-Bus

Sugo exposes a D-Bus interface:

- **Service**: `stream.sugo.app`
- **Path**: `/Timer`
- **Interface**: `stream.sugo.app.Timer`

Up to Sugo 1.0.2 the service and the interface were `io.github.mineraleyt.sugo`: scripts and shortcuts that call it need the new name.

| Command | Action |
|:---|:---|
| `qdbus stream.sugo.app /Timer toggle` | Start or pause |
| `qdbus stream.sugo.app /Timer start` | Start or resume the active timer |
| `qdbus stream.sugo.app /Timer pause` | Pause |
| `qdbus stream.sugo.app /Timer stop` | Stop and reset |
| `qdbus stream.sugo.app /Timer skip` | Skip to the next phase |
| `qdbus stream.sugo.app /Timer status` | Print `state:phase:seconds` |
| `qdbus stream.sugo.app /Timer presets` | List the names of the timers, one per line |
| `qdbus stream.sugo.app /Timer selectPreset "Deep work"` | Choose a timer by name (not case-sensitive) or uuid |
| `qdbus stream.sugo.app /Timer nextPreset` | Choose the next timer in the list (wraps around) |
| `qdbus stream.sugo.app /Timer previousPreset` | Choose the previous timer (wraps around) |
| `qdbus stream.sugo.app /Timer startPreset "Deep work"` | Choose a timer and start it |

`status` returns three colon-separated fields, for example `running:work:1342`:

- **state**: `idle`, `running` or `paused`;
- **phase**: `work`, `short_break` or `long_break` (when idle, the phase that starts next);
- **seconds**: time left in the phase, in whole seconds.

These commands, and the KRunner ones, can be turned off in **Settings → Global Shortcuts and Automation → Allow other programs to control the timer**. They then answer with the error `stream.sugo.app.Error.ControlDisabled` and change nothing; `status` and `presets` keep working.

Changing the timer resets it, so while a phase is running or paused `selectPreset`, `nextPreset`, `previousPreset` and `startPreset` refuse with the error `stream.sugo.app.Error.TimerActive` and change nothing (stop the timer first). Choosing the timer that is already selected is always allowed, and `startPreset` then starts or resumes it. An unknown name or uuid gives `stream.sugo.app.Error.UnknownPreset`.

Depending on your distribution, `qdbus` may be called `qdbus6` or `qdbus-qt6`. If Sugo is not running, calling the service starts it hidden in the system tray (D-Bus activation). You can also start it that way yourself with `sugo --background`.

### macOS: sugo links

On macOS, opening a `sugo://` link sends Sugo a command. Open it from Terminal or a script with `open`, or from the Shortcuts app with the **Open URLs** action, which you can give a keyboard shortcut there:

| Link | Action |
|:---|:---|
| `open sugo://toggle` | Start or pause |
| `open sugo://start` | Start or resume the active timer |
| `open sugo://pause` / `open sugo://resume` | Pause or continue the countdown |
| `open sugo://stop` | Stop and reset |
| `open sugo://skip` | Skip to the next phase |
| `open sugo://show` | Show the window |
| `open sugo://preset/Deep%20work` | Choose a timer by name (spaces written as `%20`) or uuid |
| `open sugo://start/Deep%20work` | Choose a timer and start it |

As over D-Bus, choosing another timer is refused while a phase is running or paused. `sugo://show` works even when control by other programs is turned off.

---

## Data, backup and privacy

| What | Linux | macOS |
|:---|:---|:---|
| Timers, sessions, notes | `~/.local/share/sugo/sugo.db` (SQLite) | `~/Library/Application Support/sugo/sugo.db` |
| Settings | `~/.config/sugo/sugo.conf` | `~/Library/Application Support/sugo/sugo.conf` |

Inside the Flatpak the Linux files live under `~/.var/app/stream.sugo.app/`.

**Settings → Advanced → Data management** can **back up** the database to a file, **restore** a backup (including backups made by older versions), and **clear the session history**. Restoring replaces your current timers and sessions, and is not possible while a timer is running. If the database file is damaged, Sugo moves it aside (`sugo.db.corrupt-…`) and starts a new one.

**What leaves your computer.** Nothing about you:

- **Updates**: the AppImage and the macOS app download a small file from GitHub every few hours to see whether a newer Sugo exists (see [Updates](#updates-and-diagnostics); you can turn this off), and the new version itself only when you click **Install update**. The other packages only look when you press **Check for Updates**. Either way the request carries only the program version.
- **Calendar sync**, if you turn it on, sends the information described [above](#good-to-know) to the server you chose.
- **Anonymous usage statistics**, only if you turn them on (see below).

### Anonymous usage statistics

Sugo can tell us, anonymously, how it is used, so we know what to improve. It is **off** unless you turn it on: the welcome tour asks once, and **Settings → Advanced → Privacy → Share anonymous usage statistics** turns it on or off at any time.

While it is on, Sugo sends these events, and nothing else:

| Event | When | What it says |
|:---|:---|:---|
| `app_started` | Once each time Sugo starts (a minute after), and when you turn the statistics on | The Sugo version and channel (stable or nightly); Linux or macOS; the package (AppImage, Flatpak, macOS app, or built from source / a distribution's package); the Qt version; the desktop (KDE, GNOME or other); the Linux distribution, if it is one of a [fixed list](#which-distributions) (otherwise just "other"); how many timers you have (1, 2-3 or 4 or more); and whether these are on: calendar sync, focus sound, tray icon, start at login, Sugo Sync, keeping the screen awake |
| `session_ended` | Each time a phase ends | Work, short break or long break; its planned length in minutes; whether it ran to the end or was stopped or skipped |

Each event carries a random identifier, made when you turn the statistics on and forgotten when you turn them off (turning them on again makes a new one), and the hour it happened (not the minute). Sugo never sends your name, your computer's name, file paths, task notes, the names of your timers, calendar data, your Sugo Sync account, or anything you type.

The events go in small batches to Sugo's own server (`api.sugo.stream`, the one of Sugo Sync), which keeps only the events and values listed above, drops your IP address, and passes them on to [PostHog](https://posthog.com) in the EU, where we look at them in aggregate. Sugo itself never contacts PostHog. Events that cannot be sent (offline) wait, at most 200 of them, and are sent later; turning the statistics off deletes them unsent.

<a id="which-distributions"></a>The distributions named by their own name: Ubuntu, Kubuntu, Debian, Fedora, Arch, Manjaro, EndeavourOS, CachyOS, openSUSE Tumbleweed and Leap, Linux Mint, Pop!_OS, KDE neon, elementary OS, Zorin OS, NixOS, Gentoo, Void, Alpine, RHEL, CentOS, Rocky, AlmaLinux, SteamOS, Bazzite and Fedora Kinoite.

---

## Updates and diagnostics

- **The AppImage and the macOS app update themselves, when you say so.** A minute after Sugo starts, and every six hours, it looks for a newer version on its channel; when there is one, the title bar shows **Install update** (only its icon in a narrow window). Nothing is downloaded before you click it. Sugo then downloads the new version (the AppImage next to itself, the macOS app its disk image), checks that the file is complete and intact (its size and SHA-256 checksum), and only then prepares it. The same button then shows **Restart to update**; the new version starts when you click it (Sugo asks first if a timer is running) or the next time you open Sugo (on macOS, quitting also puts it in place). A failed or damaged download changes nothing. This needs the AppImage or the app to be in a folder you can write to; otherwise Sugo only tells you that a new release is out.
- **Settings → General → Updates** (AppImage and macOS app) has **Check for updates automatically**: turn it off and Sugo only looks when you ask, with **Settings → About → Check for Updates**.
- **Settings → General → Updates → Update channel** (AppImage and macOS app) chooses the update channel: **Stable**, the tested releases, or **Nightly**, test builds of what is coming, newer and sometimes with new bugs, published as [pre-releases](https://github.com/sugoHQ/sugo/releases). Going back from Nightly to Stable keeps the nightly build until a newer release comes out.
- **Settings → About → Check for Updates** tells you whether a newer version exists; the AppImage and the macOS app can install it from there, the other packages link to the release (a Flatpak or a distribution's package is updated the way it was installed). A successful check with nothing new shows a green check on the button for a few seconds.
- **Settings → Advanced → Troubleshooting & Diagnostics** copies a report with the version, your environment and the latest log lines. Your home directory, user name, passwords and addresses with credentials are replaced with placeholders. Attach it to a bug report.
