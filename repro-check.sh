#!/usr/bin/env bash
# Checks that hacman builds reproducibly: builds the current working tree twice
# under deliberately different conditions with ./build.sh and fails unless both
# binaries are identical byte for byte and carry a valid self digest.
#
# The two builds differ in:
#   - source path (name and depth) and a build directory inside vs. outside
#     the source tree;
#   - a Git checkout with its own commit vs. a plain copy without .git;
#   - umask, TZ and locale.
# Both use the same toolchain and configuration: CFLAGS, CPPFLAGS and LDFLAGS
# are removed from both, because Meson honours them deliberately. Reproducing
# across toolchains is out of scope.
#
# Files are copied from the working tree (tracked plus untracked, non-ignored),
# so uncommitted changes are checked too.
#
# Environment:
#   KEEP   non-empty = keep the temporary directory for inspection
#   DEBUG  non-empty = trace this script
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if [[ "${DEBUG:-}" != "" ]]; then
    echo "[dbg][repro-check.sh] ROOT_DIR=${ROOT_DIR}" >&2
    set -x
fi

WORK_DIR="$(mktemp -d "${TMPDIR:-/tmp}/hacman-repro.XXXXXX")"
cleanup() {
    if [[ -n "${KEEP:-}" ]]; then
        echo "repro-check.sh: kept ${WORK_DIR}"
    else
        rm -rf "$WORK_DIR"
    fi
}
trap cleanup EXIT

# Copies the working tree's files into $1. Tracked files deleted in the working
# tree are skipped, so the copy matches what a build here would see.
copy_tree() {
    local dest="$1"
    mkdir -p "$dest"
    git -C "$ROOT_DIR" ls-files -z --cached --others --exclude-standard |
        while IFS= read -r -d '' path; do
            [[ -e "$ROOT_DIR/$path" ]] && printf '%s\0' "$path"
        done |
        tar -C "$ROOT_DIR" --null -T - -cf - |
        tar -C "$dest" -xf -
}

# Runs build.sh for source dir $1 into build dir $2 with the remaining
# arguments as extra environment, logging to $2.log. Fails loudly with the log.
# Exported compiler flags are configuration, not noise, so neither build sees
# any; the check then holds whatever the calling shell exports.
build() {
    local src="$1" out="$2"
    shift 2
    if ! (cd "$src" && env -u CFLAGS -u CPPFLAGS -u LDFLAGS "$@" BUILD_DIR="$out" ./build.sh) \
        >"$out.log" 2>&1; then
        echo "repro-check.sh: build in $src failed:" >&2
        cat "$out.log" >&2
        exit 1
    fi
}

# Build A: a Git checkout with a commit of its own, build dir inside the tree.
src_a="$WORK_DIR/a/src"
out_a="$src_a/build"
copy_tree "$src_a"
git -C "$src_a" init -q
git -C "$src_a" add -A
git -C "$src_a" -c user.name=repro -c user.email=repro@invalid \
    commit -q -m "repro-check snapshot"
(umask 022 && build "$src_a" "$out_a" TZ=UTC LC_ALL=C)

# Build B: no Git, a longer source path, build dir outside the tree, and the
# opposite environment.
src_b="$WORK_DIR/b/a-much/longer/source-path"
out_b="$WORK_DIR/b/out-of-tree-build"
copy_tree "$src_b"
mkdir -p "$(dirname "$out_b")"
(umask 077 && build "$src_b" "$out_b" TZ=Pacific/Kiritimati LC_ALL=C.UTF-8)

status=0
for out in "$out_a" "$out_b"; do
    if ! "$out/hm-stamp" --check "$out/hacman" >/dev/null; then
        echo "repro-check.sh: FAILED: $out/hacman has no valid self digest" >&2
        status=1
    fi
done

if cmp -s "$out_a/hacman" "$out_b/hacman"; then
    echo "repro-check.sh: ok: $("$out_a/hacman" --version)"
    echo "repro-check.sh: ok: sha256 $(sha256sum <"$out_a/hacman" | cut -d' ' -f1)"
else
    echo "repro-check.sh: FAILED: the two builds differ" >&2
    cmp -l "$out_a/hacman" "$out_b/hacman" | head -20 >&2 || true
    echo "repro-check.sh: rerun with KEEP=1 and compare with diffoscope" >&2
    status=1
fi
exit "$status"
