#!/usr/bin/env bash
# Fetch a build input into the current directory.
# Usage: fetch_cached.sh <file name> <upstream url>
# 1. Take the copy stored in this repository's "build-cache" release, if present.
# 2. Otherwise download from upstream, retrying network errors and 5xx answers.
# 3. On push builds, store a freshly downloaded file in the build-cache release,
#    so a later outage of the upstream host no longer breaks the build.
# Files ending in .AppImage (build tools such as linuxdeploy) are stored with an
# extra ".cache" suffix: AppImage catalogues (appimage.github.io) scan all
# releases and would otherwise take a cached tool for this program's AppImage.
set -u
name="$1"
url="$2"
repo="${GITHUB_REPOSITORY:?}"

stored="$name"
case "$name" in
    *.AppImage) stored="$name.cache" ;;
esac

if gh release download build-cache --repo "$repo" --pattern "$stored" --clobber > /dev/null 2>&1 && [ -s "$stored" ]; then
    [ "$stored" = "$name" ] || mv -f "$stored" "$name"
    echo "$name: taken from build-cache"
    exit 0
fi

rm -f "$name" "$stored"
if ! wget -q --tries=6 --waitretry=30 --retry-connrefused --retry-on-http-error=429,500,502,503,504 \
        --timeout=120 -O "$name" "$url"; then
    rm -f "$name"
    echo "::error::download of $name failed: $url"
    exit 1
fi
echo "$name: downloaded from upstream"

if [ "${GITHUB_EVENT_NAME:-}" = "push" ]; then
    [ "$stored" = "$name" ] || cp -f "$name" "$stored"
    if gh release upload build-cache --repo "$repo" --clobber "$stored" > /dev/null 2>&1; then
        echo "$name: stored in build-cache"
        if [ "$stored" != "$name" ]; then
            echo "$name: stored as $stored"
            # remove a copy stored under the plain .AppImage name by older builds
            gh release delete-asset build-cache "$name" --repo "$repo" -y > /dev/null 2>&1 \
                && echo "$name: old copy under this name removed from build-cache"
        fi
    else
        echo "::warning::$name could not be stored in build-cache"
    fi
    [ "$stored" = "$name" ] || rm -f "$stored"
fi
exit 0
