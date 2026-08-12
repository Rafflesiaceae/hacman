#!/usr/bin/env bash
# Builds hacman. By default this produces a *static musl* binary, because
# hacman's fast path is startup-time dominated and a static binary skips the
# dynamic loader entirely (see README.md).
#
# Environment:
#   BUILD_DIR      build directory                     (default: ./build)
#   HACMAN_CC      C compiler to use                   (default: autodetected musl compiler)
#   HACMAN_STATIC  1 = static link, 0 = dynamic link   (default: 1)
#   HACMAN_GLIBC   1 = allow falling back to a non-musl compiler (default: 1)
#   BUILDTYPE      meson buildtype                     (default: release)
#   DEBUG          non-empty = trace this script
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${BUILD_DIR:-"$ROOT_DIR/build"}"
BUILDTYPE="${BUILDTYPE:-release}"
HACMAN_STATIC="${HACMAN_STATIC:-1}"
HACMAN_GLIBC="${HACMAN_GLIBC:-1}"

if [[ "${DEBUG:-}" != "" ]]; then
    echo "[dbg][build.sh] ROOT_DIR=${ROOT_DIR}" >&2
    echo "[dbg][build.sh] BUILD_DIR=${BUILD_DIR}" >&2
    set -x
fi

# Returns 0 if $1 is a compiler that targets musl.
is_musl_cc() {
    local cc="$1"
    command -v "$cc" >/dev/null 2>&1 || return 1
    [[ "$("$cc" -dumpmachine 2>/dev/null || true)" == *musl* ]]
}

# Picks a musl-capable C compiler, most conventional candidate first.
detect_cc() {
    local candidate
    for candidate in musl-gcc musl-clang x86_64-linux-musl-gcc aarch64-linux-musl-gcc; do
        if command -v "$candidate" >/dev/null 2>&1; then
            echo "$candidate"
            return 0
        fi
    done

    # Alpine and other musl-native distros: the system compiler already is musl.
    for candidate in cc gcc clang; do
        if is_musl_cc "$candidate"; then
            echo "$candidate"
            return 0
        fi
    done

    # zig ships a complete musl toolchain; wrap it so meson sees a plain CC.
    if command -v zig >/dev/null 2>&1; then
        local wrapper="$BUILD_DIR/.zig-musl-cc"
        mkdir -p "$BUILD_DIR"
        cat >"$wrapper" <<'EOF'
#!/bin/sh
exec zig cc -target "${ZIG_MUSL_TARGET:-x86_64-linux-musl}" "$@"
EOF
        chmod +x "$wrapper"
        echo "$wrapper"
        return 0
    fi

    return 1
}

CC_BIN="${HACMAN_CC:-}"
if [[ -z "$CC_BIN" ]]; then
    if ! CC_BIN="$(detect_cc)"; then
        if [[ "$HACMAN_GLIBC" != "1" ]]; then
            echo "build.sh: no musl compiler found (install musl-tools, a" >&2
            echo "          *-linux-musl-gcc cross toolchain, or zig)" >&2
            exit 1
        fi
        CC_BIN="${CC:-cc}"
        echo "build.sh: WARNING no musl compiler found, falling back to '${CC_BIN}'." >&2
        echo "build.sh:         Install 'musl-tools' for the intended static musl build." >&2
    fi
fi

static_opt=true
[[ "$HACMAN_STATIC" == "1" ]] || static_opt=false

echo "build.sh: CC=${CC_BIN} static=${static_opt} buildtype=${BUILDTYPE}"

setup_args=(
    "--buildtype=${BUILDTYPE}"
    "-Dstatic=${static_opt}"
)

if [ ! -d "${BUILD_DIR}/meson-info" ]; then
    CC="$CC_BIN" meson setup "${setup_args[@]}" "${BUILD_DIR}" "${ROOT_DIR}"
else
    CC="$CC_BIN" meson setup --reconfigure "${setup_args[@]}" "${BUILD_DIR}" "${ROOT_DIR}"
fi

meson compile -C "${BUILD_DIR}"

# Report what we actually produced; a static build must have no interpreter.
if command -v file >/dev/null 2>&1; then
    file "${BUILD_DIR}/hacman"
fi
