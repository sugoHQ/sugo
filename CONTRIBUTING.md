<!--
  SPDX-FileCopyrightText: 2026 Sugo contributors
  SPDX-License-Identifier: CC-BY-SA-4.0
-->

# Contributing to Sugo

Thanks for wanting to help. Bug reports, ideas, translations and code are all welcome.

By taking part you agree to the [code of conduct](CODE_OF_CONDUCT.md).

## Reporting a bug or asking for something

Open an [issue](https://github.com/sugoHQ/sugo/issues): the templates ask for the version, how you installed Sugo, and a diagnostic report. `Settings → Advanced → Troubleshooting & Diagnostics → Copy to Clipboard` produces that report with usernames and personal paths already removed.

A security problem does not go in an issue: see the [security policy](SECURITY.md).

## Building and testing

The [development guide](docs/DEVELOPMENT.md) has the dependencies for each distribution and for macOS, the build commands, and how to run the suite:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

CI runs the same suite in a clean environment, with GCC and with Clang, warnings as errors. A change is ready when the build has no warnings and the tests pass on your machine too.

## Branches and pull requests

| Branch | For |
|:---|:---|
| `dev` | Daily work: every change goes here |
| `testing` | What is ready to try: `dev` brought over when the maintainer decides |
| `main` | What has been tried: `testing` brought over once it works, and where releases are tagged |

Open your pull request against **`dev`**, from a branch named after what it does (`fix/…`, `feat/…`, `docs/…`). Keep it to one change: a fix and a refactor in the same pull request are two pull requests. CI must pass before it is merged, and the branch is deleted once it is.

A pull request says what changes for the user and why, and how you checked it. If it changes something visible, a screenshot helps.

## Commits

The subject is a type, a colon, and a sentence in the imperative that says what the commit does, without a capital letter and without a full stop:

```
fix: the wait for the key reads the device list again after a minute
feat: the welcome tour tells the usage statistics where it ended
docs: the new usage statistics events
test: loginFlowCancel allows the poll already on its way on a slow machine
chore(i18n): update the template for the usage statistics row
```

The body, when there is one, says why rather than what: the constraint that made this the right change, or what it replaces.

## Code

- **C++17 and Qt 6.8+**, built with CMake and Ninja. Match the style of the file you are in: the project has no formatter, and a diff that reformats a file hides the change.
- **Comments explain a decision**, not what the line does. The ones worth writing say why something is done this way, or what would break otherwise.
- **Every new file starts with an SPDX header**, like the ones already there: `GPL-3.0-or-later` for code, `CC-BY-SA-4.0` for documentation and media, `CC0-1.0` for AppStream metadata.
- **A new test comes with the change** it protects, in `tests/`. A file that only one platform compiles goes in the lists of `.github/scripts/ci-changes.sh`, so CI runs the right jobs.
- **A new dependency** goes in `.github/ci-image/Dockerfile` as well, or CI cannot build the change.

## Translating

The program is in English only for now, but every string goes through the `i18n()` family of `src/core/Localization.h`, so translations can come back without touching the sources. `bash po/Messages.sh` extracts them to `po/sugo.pot`.

## License

Sugo is free software under the [GNU General Public License v3.0 or later](LICENSE); documentation and media are under [CC-BY-SA-4.0](https://creativecommons.org/licenses/by-sa/4.0/). By contributing you agree to publish your work under the same terms. There is no copyright assignment and no contributor license agreement.
