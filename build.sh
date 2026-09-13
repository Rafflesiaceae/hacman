#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${BUILD_DIR:-"$ROOT_DIR/build"}"

if [[ "${DEBUG:-}" != "" ]]; then
    echo "[dbg][build.sh] ROOT_DIR=${ROOT_DIR}" >&2
    echo "[dbg][build.sh] BUILD_DIR=${BUILD_DIR}" >&2
fi

if [ ! -d "${BUILD_DIR}" ]; then
    meson setup "${BUILD_DIR}" "${ROOT_DIR}"
else
    meson setup --reconfigure "${BUILD_DIR}" "${ROOT_DIR}"
fi

meson compile -C "${BUILD_DIR}"
