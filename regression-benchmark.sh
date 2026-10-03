#!/usr/bin/env bash
# Compare cached hacman launches with the same static musl program run directly.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

usage() {
    cat <<'EOF'
Usage: ./regression-benchmark.sh

Build hacman and a small embedded static musl C project, check its behavior,
then assert the ratio of mean cached-launch time to mean direct-launch time.

Environment:
  HACMAN_CC           musl C compiler executable (default: musl-gcc)
  BENCHMARK_WARMUP     warmup runs per command (default: 20)
  BENCHMARK_RUNS       measured runs per command (default: 1000)
  BENCHMARK_MIN_RATIO  inclusive minimum hacman/direct ratio (default: 1)
  BENCHMARK_MAX_RATIO  inclusive maximum hacman/direct ratio (default: 3)
  BENCHMARK_JSON       report path (default: build-regression-benchmark/results.json)

Requires Bash, Python 3, hyperfine, Meson, Ninja, readelf and a musl compiler.
EOF
}

if [[ "${1:-}" == --help || "${1:-}" == -h ]]; then
    usage
    exit 0
fi
if [[ $# != 0 ]]; then
    usage >&2
    exit 1
fi

export HACMAN_CC="${HACMAN_CC:-musl-gcc}"
export BENCHMARK_WARMUP="${BENCHMARK_WARMUP:-20}"
export BENCHMARK_RUNS="${BENCHMARK_RUNS:-1000}"
export BENCHMARK_MIN_RATIO="${BENCHMARK_MIN_RATIO:-1}"
export BENCHMARK_MAX_RATIO="${BENCHMARK_MAX_RATIO:-3}"
BENCHMARK_JSON="${BENCHMARK_JSON:-$ROOT_DIR/build-regression-benchmark/results.json}"

for tool in python3 hyperfine meson ninja readelf "$HACMAN_CC"; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "regression-benchmark.sh: required executable not found: $tool" >&2
        exit 1
    fi
done

# Reject invalid budgets before spending time compiling or measuring anything.
python3 - <<'PY'
import math
import os
import sys

try:
    for name, minimum in (("BENCHMARK_WARMUP", 1), ("BENCHMARK_RUNS", 2)):
        value = os.environ[name]
        if not value.isascii() or not value.isdecimal() or int(value) < minimum:
            raise ValueError(f"{name} must be an integer >= {minimum}")
    lower = float(os.environ["BENCHMARK_MIN_RATIO"])
    upper = float(os.environ["BENCHMARK_MAX_RATIO"])
    if not (math.isfinite(lower) and math.isfinite(upper) and 0 < lower <= upper):
        raise ValueError("ratio bounds must be finite and satisfy 0 < min <= max")
except ValueError as error:
    sys.exit(f"regression-benchmark.sh: {error}")
PY

# A fresh build prevents Meson's cached compiler from selecting an older glibc
# configuration. Keep build and project caches separate from the user's cache.
BENCHMARK_TEMP="$(mktemp -d "${TMPDIR:-/tmp}/hacman-benchmark.XXXXXXXX")"
trap 'rm -rf -- "$BENCHMARK_TEMP"' EXIT
unset HACMAN HACMAN_DEBUG HACMAN_KEEP_TEMP
export HACMAN_CACHE="$BENCHMARK_TEMP/cache"
export HACMAN_BENCHMARK_CC="$HACMAN_CC"
mkdir -p "$HACMAN_CACHE" "$(dirname "$BENCHMARK_JSON")"
BUILD_DIR="$BENCHMARK_TEMP/build" HACMAN_STATIC=1 HACMAN_GLIBC=0 BUILDTYPE=release \
    "$ROOT_DIR/build.sh"
HACMAN_BIN="$BENCHMARK_TEMP/build/hacman"
FIXTURE="$ROOT_DIR/examples/regression-benchmark.siml"

# The first launch compiles the embedded source and checks the default output.
if ! output="$("$HACMAN_BIN" "$FIXTURE")" || [[ "$output" != "hello from static musl" ]]; then
    echo "regression-benchmark.sh: unexpected fixture output" >&2
    exit 1
fi
binaries=("$HACMAN_CACHE"/*.work/hello)
if [[ ${#binaries[@]} != 1 || ! -x "${binaries[0]}" ]]; then
    echo "regression-benchmark.sh: expected one cached fixture executable" >&2
    exit 1
fi
DIRECT_BIN="${binaries[0]}"

# Static linking must really have succeeded for both sides of the comparison.
for binary in "$HACMAN_BIN" "$DIRECT_BIN"; do
    headers="$(readelf --program-headers "$binary")"
    if [[ "$headers" == *INTERP* || "$headers" == *DYNAMIC* ]]; then
        echo "regression-benchmark.sh: binary is not fully static: $binary" >&2
        exit 1
    fi
done

# Poison the compiler after priming: any accidental rebuild must fail instead
# of silently turning the fast-path benchmark into a compilation benchmark.
export HACMAN_BENCHMARK_CC="$BENCHMARK_TEMP/no-compiler"
for command in "$DIRECT_BIN" "$HACMAN_BIN"; do
    args=(regression-benchmark)
    [[ "$command" != "$HACMAN_BIN" ]] || args=("$FIXTURE" "${args[@]}")
    if ! output="$("$command" "${args[@]}")" || [[ "$output" != regression-benchmark ]]; then
        echo "regression-benchmark.sh: output/argument forwarding check failed" >&2
        exit 1
    fi
done

# Hyperfine still parses argument strings with --shell=none. Use shlex quoting
# so spaces and apostrophes in repository and temporary paths remain literal.
quote_command() {
    python3 - "$@" <<'PY'
import shlex
import sys

print(" ".join(shlex.quote(argument) for argument in sys.argv[1:]))
PY
}
hyperfine --shell=none --warmup "$BENCHMARK_WARMUP" --runs "$BENCHMARK_RUNS" \
    --export-json "$BENCHMARK_JSON" \
    --command-name direct "$(quote_command "$DIRECT_BIN" regression-benchmark)" \
    --command-name hacman "$(quote_command "$HACMAN_BIN" "$FIXTURE" regression-benchmark)"

# JSON timings are seconds regardless of hyperfine's display units. Reject
# failed runs and unusable statistics before evaluating the inclusive budget.
python3 - "$BENCHMARK_JSON" <<'PY'
import json
import math
import os
import sys

with open(sys.argv[1], encoding="utf-8") as report:
    results = json.load(report)["results"]
if len(results) != 2 or {result["command"] for result in results} != {"direct", "hacman"}:
    sys.exit("regression-benchmark.sh: expected direct and hacman measurements")
means = {}
for result in results:
    mean = result["mean"]
    if not math.isfinite(mean) or mean <= 0 or any(result["exit_codes"]):
        sys.exit("regression-benchmark.sh: invalid timings or failed benchmark runs")
    means[result["command"]] = mean
ratio = means["hacman"] / means["direct"]
lower = float(os.environ["BENCHMARK_MIN_RATIO"])
upper = float(os.environ["BENCHMARK_MAX_RATIO"])
print(f"Mean direct: {means['direct'] * 1e6:.1f} us; "
      f"hacman: {means['hacman'] * 1e6:.1f} us")
print(f"hacman/direct: {ratio:.3f}; allowed: [{lower:g}, {upper:g}]")
print(f"Report: {os.path.abspath(sys.argv[1])}")
if not lower <= ratio <= upper:
    sys.exit("regression-benchmark.sh: FAIL: launch-time ratio outside allowed range")
print("regression-benchmark.sh: PASS")
PY
