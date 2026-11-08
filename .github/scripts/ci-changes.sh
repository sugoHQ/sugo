#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Which CI jobs a change needs: reads the files changed between two commits (GitHub's compare API,
# no checkout) and writes linux=, clang= and macos= (true/false) to $GITHUB_OUTPUT.
#   linux  the GCC build with the tests and the metadata checks (ubuntu)
#   clang  the Clang build, warnings as errors (only the compiled sources matter)
#   macos  the macOS build with the tests
# A pull request compares its head with its base. A push compares the pushed commit with the commit
# of the newest CI run on that branch that passed (--last-green), not with the commit before the
# push: a run that failed or was cancelled leaves its changes in the comparison until a later run
# passes with them, so a push that changes nothing tested cannot pass (and, on testing, publish a
# nightly) on top of code no test has run on.
# It fails open: when the changed files cannot be known (a manual run, no passed run on the branch,
# a passed commit that is not an ancestor of the head, a comparison the API cannot make, too many
# files), every job runs.
#
# usage: ci-changes.sh <base commit> <head commit>          (pull request)
#        ci-changes.sh --last-green <branch> <head commit>  (push; needs actions: read)
# (GH_TOKEN, GITHUB_REPOSITORY, GITHUB_OUTPUT)
set -uo pipefail

linux=false clang=false macos=false

all() {
    echo "Running every job: $1"
    linux=true clang=true macos=true
}

# The commit of the newest CI run on branch $1 that passed on a push, or nothing.
last_green() {
    gh api "repos/$GITHUB_REPOSITORY/actions/workflows/ci.yml/runs?branch=$1&event=push&status=success&per_page=1" \
        --jq '.workflow_runs[0].head_sha // empty'
}

# What a file needs. Anything not listed below goes to every job.
classify() {
    case $1 in
        # Read by no job of ci.yml (release.yml builds the Flatpak, appimage.yml and macos.yml the packages).
        *.md | docs/* | LICENSE | LICENSES/* | *.license | .gitignore | po/* | \
        .github/ISSUE_TEMPLATE/* | .github/dependabot.yml | \
        .github/workflows/release.yml | .github/workflows/nightly.yml | \
        .github/workflows/appimage.yml | .github/workflows/macos.yml | \
        stream.sugo.app.yml | packaging/appimage/*)
            ;;

        # Linux metadata: validated (and installed) by the Linux build only.
        data/stream.sugo.app.desktop | data/stream.sugo.app.metainfo.xml)
            linux=true ;;

        # macOS only: packaging, the app icon, and the sources and tests built under if(APPLE).
        packaging/macos/* | data/icons/macos/* | src/platform/*.mm | \
        src/platform/MacApp.h | src/platform/MacNotifier.h | src/platform/MacPowerAssertion.* | \
        src/platform/MacSessionEvents.* | src/platform/MacKeychainStore.* | \
        tests/tst_macpowerassertion.cpp | tests/tst_mackeychainstore.cpp)
            macos=true ;;

        # Linux only: the D-Bus services and their tests (the if(NOT APPLE) blocks).
        src/platform/DBusTimerService.* | src/platform/FreedesktopInhibitor.* | \
        src/platform/FreedesktopNotifier.* | src/platform/KRunnerService.* | \
        src/platform/PortalAutostart.* | src/platform/SecretServiceStore.* | \
        src/platform/SingleInstance.* | tests/PrivateDBus.h | \
        tests/tst_powerinhibitor.cpp | tests/tst_freedesktopinhibitor.cpp | \
        tests/tst_singleinstance.cpp | tests/tst_krunnerservice.cpp | \
        tests/tst_freedesktopnotifier.cpp | tests/tst_dbustimerservice.cpp | tests/tst_secretstore.cpp)
            linux=true clang=true ;;
        tests/shutdown_app.sh | tests/check_qml_colours.sh | tests/check_qml_sizes.sh)
            linux=true ;;

        # The system the Linux jobs run in.
        .github/ci-image/*)
            linux=true clang=true ;;

        # Run by the tests on both systems, compiled by neither.
        tests/*.sh | tests/*.py | tests/*.json | data/*.in)
            linux=true macos=true ;;

        *)
            linux=true clang=true macos=true ;;
    esac
}

base='' head='' branch='' reason=''
if [[ ${1:-} == --last-green ]]; then
    branch=${2:-} head=${3:-}
    if [[ -z $branch ]]; then
        reason="no branch given"
    elif ! base=$(last_green "$branch" 2>&1); then
        reason="the CI runs of $branch could not be read ($base)"
    elif [[ -z $base ]]; then
        reason="no CI run on $branch has passed yet"
    elif [[ ! $base =~ ^[0-9a-f]{40}$ ]]; then
        reason="unexpected answer for the CI runs of $branch ($base)"
    else
        echo "Base: $base, the newest CI run on $branch that passed"
    fi
else
    base=${1:-} head=${2:-}
fi

if [[ -n $reason ]]; then
    all "$reason"
elif [[ -z $base || -z $head || $base =~ ^0+$ ]]; then
    all "no previous commit to compare with"
elif [[ -n $branch ]] && ! status=$(gh api "repos/$GITHUB_REPOSITORY/compare/$base...$head?per_page=1" \
        --jq '.status' 2>&1); then
    all "the comparison failed ($status)"
elif [[ -n $branch && $status == identical ]]; then
    all "$head is the commit that passed (a run started again)"
elif [[ -n $branch && $status != ahead ]]; then
    all "$base is not an ancestor of $head (comparison: $status)"
elif ! files=$(gh api "repos/$GITHUB_REPOSITORY/compare/$base...$head?per_page=100" --paginate \
        --jq '.files[].filename' 2>&1); then
    all "the comparison failed ($files)"
elif [[ -z $files ]]; then
    all "no changed file reported"
elif (($(wc -l <<<"$files") >= 300)); then
    all "300 changed files or more (the API lists no more)"
else
    echo "Changed files:"
    while IFS= read -r file; do
        echo "  $file"
        classify "$file"
    done <<<"$files"
fi

echo "linux=$linux clang=$clang macos=$macos"
{
    echo "linux=$linux"
    echo "clang=$clang"
    echo "macos=$macos"
} >>"${GITHUB_OUTPUT:-/dev/stdout}"
