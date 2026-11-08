<!--
  SPDX-FileCopyrightText: 2026 Sugo contributors
  SPDX-License-Identifier: CC-BY-SA-4.0
-->

## What changes for the user

<!-- The behaviour that is different, in one or two sentences. "Nothing visible" is a fine answer. -->

## Why

<!-- The problem this solves, or the constraint that made this the right change. -->

## How it was checked

<!-- The tests you ran, and what you did by hand. `ctest --test-dir build --output-on-failure` is the baseline. -->

## Checklist

- [ ] The build has no warnings and the test suite passes
- [ ] New files carry an SPDX header
- [ ] A new test covers the change, when the change is testable
- [ ] A new dependency is also in `.github/ci-image/Dockerfile`
- [ ] A file only one platform compiles is in the lists of `.github/scripts/ci-changes.sh`
- [ ] The user guide (`docs/USAGE.md`) or the development guide (`docs/DEVELOPMENT.md`) is updated, when the change is visible
