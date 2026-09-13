#!/bin/sh
# Format hacman's maintained C sources without touching vendored code.
set -eu

# Make invocation independent of the caller's current working directory.
ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
cd "$ROOT_DIR"

if ! command -v clang-format >/dev/null 2>&1; then
    echo "autoformat.sh: clang-format is required" >&2
    exit 1
fi

# Explicit paths can limit formatting while the default covers the whole core.
if [ "$#" -eq 0 ]; then
    set -- src/*.c src/*.h
fi
clang-format --style=file -i "$@"
