#!/bin/sh
# Format hacman's maintained C and Python sources without touching vendored code.
set -eu

# Make invocation independent of the caller's current working directory.
ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
cd "$ROOT_DIR"

if ! command -v clang-format >/dev/null 2>&1; then
    echo "autoformat.sh: clang-format is required" >&2
    exit 1
fi
if ! command -v black >/dev/null 2>&1; then
    echo "autoformat.sh: black is required" >&2
    exit 1
fi

# The default covers maintained sources while explicit paths select a formatter
# by extension, which keeps targeted editor and hook invocations convenient.
if [ "$#" -eq 0 ]; then
    clang-format --style=file -i src/*.c src/*.h
    black --quiet tests.py vendor.py
    exit 0
fi

for path do
    case "$path" in
        *.c | *.h) clang-format --style=file -i "$path" ;;
        *.py) black --quiet "$path" ;;
        *)
            echo "autoformat.sh: unsupported file type: $path" >&2
            exit 1
            ;;
    esac
done
