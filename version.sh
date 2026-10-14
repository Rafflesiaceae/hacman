#!/bin/sh
# Print an exact release version or the current short commit for Meson's
# vcs_tag helper. A failure lets Meson use its source-archive fallback.
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

# Do not accidentally use a parent repository when building a source archive.
[ -e "$ROOT_DIR/.git" ] || exit 1

# Inspect all tags at HEAD so an unrelated tag cannot hide a release tag.
release=$(git -C "$ROOT_DIR" tag --points-at HEAD | awk '
    /^v[0-9]+\.[0-9]+\.[0-9]+$/ { print substr($0, 2); exit }
')
if [ -n "$release" ]; then
    printf '%s\n' "$release"
else
    git -C "$ROOT_DIR" rev-parse --short HEAD
fi
