#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Signs an update feed (stable.json, nightly.json): adds "signature", an Ed25519 signature, to
# every download entry in it (the top-level one for the x86_64 AppImage, "linux-aarch64",
# "macos"), what the program checks against packaging/update-public-key.txt
# (src/core/UpdateSignature.h). The message is the same lines the program builds: "sugo-update-v1",
# the feed's version, the entry's url, sha256 and size, with no newline after the last.
#
# Usage: UPDATE_SIGNING_KEY="<PEM of the private key>" sign-update-feed.sh feed.json
#
# Without UPDATE_SIGNING_KEY the feed is left as it is, unless the repository already has a
# public key: then the release must not go out unsigned (the programs would only report it).
set -euo pipefail

feed=${1:?usage: sign-update-feed.sh feed.json}
root=$(cd "$(dirname "$0")/../.." && pwd)
public_key=$(tr -d '[:space:]' < "$root/packaging/update-public-key.txt")

if [ -z "${UPDATE_SIGNING_KEY:-}" ]; then
    if [ -n "$public_key" ]; then
        echo "::error::packaging/update-public-key.txt is set but the repository secret UPDATE_SIGNING_KEY is not: the feed would go out unsigned" >&2
        exit 1
    fi
    echo "No signing key: $feed is not signed"
    exit 0
fi

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
umask 077
printf '%s\n' "$UPDATE_SIGNING_KEY" > "$work/key.pem"

# The key must be the one the programs carry.
derived=$(openssl pkey -in "$work/key.pem" -pubout -outform DER | tail -c 32 | base64 -w0)
if [ -n "$public_key" ] && [ "$derived" != "$public_key" ]; then
    echo "::error::UPDATE_SIGNING_KEY is not the key of packaging/update-public-key.txt" >&2
    exit 1
fi

version=$(jq -er '.version' "$feed")
# "" is the feed itself (the x86_64 AppImage), the others are the entries under their own key.
for entry in '' '.["linux-aarch64"]' '.macos'; do
    selected=${entry:-.}
    if [ "$(jq "($selected) != null" "$feed")" != true ]; then
        continue
    fi
    url=$(jq -er "($selected).url" "$feed")
    sha256=$(jq -er "($selected).sha256" "$feed")
    size=$(jq -er "($selected).size" "$feed")
    printf 'sugo-update-v1\n%s\n%s\n%s\n%s' "$version" "$url" "$sha256" "$size" > "$work/message"
    signature=$(openssl pkeyutl -sign -rawin -inkey "$work/key.pem" -in "$work/message" | base64 -w0)
    jq --arg signature "$signature" "$entry.signature = \$signature" "$feed" > "$work/feed.json"
    mv "$work/feed.json" "$feed"
done
echo "Signed $feed"
