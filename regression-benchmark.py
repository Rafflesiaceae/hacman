#!/usr/bin/env python3
"""Compare cached hacman launches with a static musl program run directly."""

from __future__ import annotations

import argparse
import json
import math
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile

ROOT_DIR = Path(__file__).resolve().parent

# Hyperfine exports seconds; these fixed budgets apply strictly to mean time.
MAX_MEAN_SECONDS = {"direct": 800e-6, "hacman": 1000e-6}
# The allowed hacman/direct mean-time ratio is inclusive.
MEAN_RATIO_BOUNDS = (1.0, 1.6)


def settings(env: dict[str, str]) -> tuple[int, int, float, float]:
    """Validate budgets before spending time compiling or measuring anything."""
    defaults = {
        "HACMAN_CC": "musl-gcc",
        "BENCHMARK_WARMUP": "2000",
        "BENCHMARK_RUNS": "8000",
        "BENCHMARK_MIN_RATIO": str(MEAN_RATIO_BOUNDS[0]),
        "BENCHMARK_MAX_RATIO": str(MEAN_RATIO_BOUNDS[1]),
        "BENCHMARK_JSON": str(ROOT_DIR / "build-regression-benchmark/results.json"),
    }
    for name, default in defaults.items():
        env[name] = env.get(name) or default
    for name, minimum in (("BENCHMARK_WARMUP", 1), ("BENCHMARK_RUNS", 2)):
        value = env[name]
        if not value.isascii() or not value.isdecimal() or int(value) < minimum:
            raise ValueError(f"{name} must be an integer >= {minimum}")
    lower = float(env["BENCHMARK_MIN_RATIO"])
    upper = float(env["BENCHMARK_MAX_RATIO"])
    if not (math.isfinite(lower) and math.isfinite(upper) and 0 < lower <= upper):
        raise ValueError("ratio bounds must be finite and satisfy 0 < min <= max")
    return int(env["BENCHMARK_WARMUP"]), int(env["BENCHMARK_RUNS"]), lower, upper


def run(
    argv: list[str | Path], env: dict[str, str], *, capture: bool = False
) -> subprocess.CompletedProcess[str]:
    """Execute argument vectors directly, keeping diagnostics on stderr."""
    return subprocess.run(
        [str(argument) for argument in argv],
        env=env,
        check=True,
        text=True,
        stdout=subprocess.PIPE if capture else None,
    )


def check_output(argv: list[str | Path], env: dict[str, str], expected: str) -> None:
    """Check successful execution as well as the fixture's exact output."""
    if run(argv, env, capture=True).stdout != expected + "\n":
        raise ValueError("fixture output/argument forwarding check failed")


def check_report(report: Path, lower: float, upper: float) -> None:
    """Assert strict mean-time limits and the inclusive launch-time ratio."""
    with report.open(encoding="utf-8") as stream:
        results = json.load(stream)["results"]
    if len(results) != 2:
        raise ValueError("expected direct and hacman measurements")
    means = {}
    for result in results:
        # Hyperfine 1.x stores a summary per command; 2.x stores each sample.
        if "measurements" in result:
            name = result["name"]
            measurements = result["measurements"]
            if not measurements:
                raise ValueError("invalid timings or failed benchmark runs")
            mean = math.fsum(
                sample["time_wall_clock"]["value"] for sample in measurements
            ) / len(measurements)
            exit_codes = (sample["exit_code"] for sample in measurements)
        else:
            name = result["command"]
            mean = result["mean"]
            exit_codes = result["exit_codes"]
        if (
            not isinstance(mean, (int, float))
            or not math.isfinite(mean)
            or mean <= 0
            or any(exit_codes)
        ):
            raise ValueError("invalid timings or failed benchmark runs")
        means[name] = mean
    if set(means) != {"direct", "hacman"}:
        raise ValueError("expected direct and hacman measurements")
    ratio = means["hacman"] / means["direct"]
    # Scale the measured direct mean by each inclusive relative bound.
    hacman_ratio_range = (means["direct"] * lower, means["direct"] * upper)
    print(
        f"Max mean: direct {means['direct'] * 1e6:.1f} us "
        f"(limit {MAX_MEAN_SECONDS['direct'] * 1e6:g} us); "
        f"hacman {means['hacman'] * 1e6:.1f} us "
        f"(limit {MAX_MEAN_SECONDS['hacman'] * 1e6:g} us)"
    )
    print(
        f"Ratio mean: hacman/direct {ratio:.3f}; "
        f"allowed [{lower:g}, {upper:g}]; implied hacman mean range "
        f"[{hacman_ratio_range[0] * 1e6:.1f}, "
        f"{hacman_ratio_range[1] * 1e6:.1f}] us"
    )
    print(f"Report: {report}")
    # Report every exceeded limit so a slow run identifies both launch paths.
    failures = [
        f"{name} mean {means[name] * 1e6:.1f} us must be < {limit * 1e6:g} us"
        for name, limit in MAX_MEAN_SECONDS.items()
        if means[name] >= limit
    ]
    if not lower <= ratio <= upper:
        failures.append("launch-time ratio outside allowed range")
    if failures:
        raise ValueError("FAIL: " + "; ".join(failures))
    print("regression-benchmark.py: PASS")


def benchmark() -> None:
    """Build and check the fixture, then measure cached and direct launches."""
    env = os.environ.copy()
    warmup, runs, lower, upper = settings(env)
    if not hasattr(os, "sched_getaffinity") or not hasattr(os, "sched_setaffinity"):
        raise ValueError(
            "CPU pinning requires sched_getaffinity/sched_setaffinity support"
        )
    available_cpus = os.sched_getaffinity(0)
    cpu = min(available_cpus)
    for tool in ("bash", "hyperfine", "meson", "ninja", "readelf", env["HACMAN_CC"]):
        if shutil.which(tool) is None:
            raise ValueError(f"required executable not found: {tool}")
    # Resolve a compiler path before the fixture changes its working directory.
    # Keep symlinks intact because compiler wrappers can depend on their name.
    env["HACMAN_CC"] = os.path.abspath(shutil.which(env["HACMAN_CC"]))
    report = Path(env["BENCHMARK_JSON"]).absolute()
    report.parent.mkdir(parents=True, exist_ok=True)

    # A fresh build avoids Meson's cached compiler and keeps build/project caches
    # separate from the user's cache. Cleanup also runs when any assertion fails.
    with tempfile.TemporaryDirectory(prefix="hacman-benchmark-") as directory:
        temp = Path(directory).absolute()
        cache = temp / "cache"
        cache.mkdir()
        for name in ("HACMAN", "HACMAN_DEBUG", "HACMAN_KEEP_TEMP"):
            env.pop(name, None)
        env.update(
            HACMAN_CACHE=str(cache),
            HACMAN_BENCHMARK_CC=env["HACMAN_CC"],
            BUILD_DIR=str(temp / "build"),
            HACMAN_STATIC="1",
            HACMAN_GLIBC="0",
            BUILDTYPE="release",
        )
        run([ROOT_DIR / "build.sh"], env)
        hacman = temp / "build/hacman"
        fixture = ROOT_DIR / "examples/regression-benchmark.siml"

        # Prime the cache once; compilation must stay outside the timed runs.
        check_output([hacman, fixture], env, "hello from static musl")
        binaries = list(cache.glob("*.work/hello"))
        if len(binaries) != 1 or not os.access(binaries[0], os.X_OK):
            raise ValueError("expected one cached fixture executable")
        direct = binaries[0]
        for binary in (hacman, direct):
            headers = run(["readelf", "--program-headers", binary], env, capture=True)
            if "INTERP" in headers.stdout or "DYNAMIC" in headers.stdout:
                raise ValueError(f"binary is not fully static: {binary}")

        # Any accidental rebuild after priming must fail rather than silently
        # turning this fast-path benchmark into a compilation benchmark.
        env["HACMAN_BENCHMARK_CC"] = str(temp / "no-compiler")
        direct_command = [direct, "regression-benchmark"]
        hacman_command = [hacman, fixture, "regression-benchmark"]
        for command in (direct_command, hacman_command):
            check_output(command, env, "regression-benchmark")

        # Hyperfine and its children inherit this affinity, keeping both launch
        # paths on the same allowed CPU without restricting parallel compilation.
        print(f"Benchmark CPU: {cpu}", flush=True)
        os.sched_setaffinity(0, {cpu})
        try:
            # Hyperfine parses argument strings even with no shell. Quote paths so
            # spaces and apostrophes remain literal without timing a shell process.
            run(
                [
                    "hyperfine",
                    "--style",
                    "basic",
                    "--shell=none",
                    "--warmup",
                    str(warmup),
                    "--runs",
                    str(runs),
                    "--export-json",
                    report,
                    "--command-name",
                    "direct",
                    shlex.join(str(argument) for argument in direct_command),
                    "--command-name",
                    "hacman",
                    shlex.join(str(argument) for argument in hacman_command),
                ],
                env,
            )
        finally:
            # Restore the caller's affinity even when Hyperfine fails.
            os.sched_setaffinity(0, available_cpus)
        check_report(report, lower, upper)


def main() -> int:
    """Expose environment settings in help and report failures without a traceback."""
    parser = argparse.ArgumentParser(
        description=(
            "Build hacman and a small embedded static musl C project, check its "
            "behavior, then assert mean launch times and the ratio of mean "
            "cached-launch time to mean direct-launch time."
        ),
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=f"""Mean launch times must be strictly below
{MAX_MEAN_SECONDS["direct"] * 1e6:g} us for direct and
{MAX_MEAN_SECONDS["hacman"] * 1e6:g} us for hacman, with inclusive ratio bounds
[{MEAN_RATIO_BOUNDS[0]:g}, {MEAN_RATIO_BOUNDS[1]:g}].

Hyperfine and its children are pinned to the lowest-numbered allowed CPU.
Requires OS support for sched_getaffinity and sched_setaffinity.

Environment:
  HACMAN_CC           musl C compiler executable (default: musl-gcc)
  BENCHMARK_WARMUP    warmup runs per command (default: 20)
  BENCHMARK_RUNS      measured runs per command (default: 1000)
  BENCHMARK_MIN_RATIO inclusive minimum hacman/direct ratio (default: {MEAN_RATIO_BOUNDS[0]:g})
  BENCHMARK_MAX_RATIO inclusive maximum hacman/direct ratio (default: {MEAN_RATIO_BOUNDS[1]:g})
  BENCHMARK_JSON      report path (default: build-regression-benchmark/results.json)

Requires Python 3, hyperfine, Meson, Ninja, readelf, a musl compiler and
Bash for build.sh.""",
    )
    parser.parse_args()
    try:
        benchmark()
    except subprocess.CalledProcessError as error:
        print(
            f"regression-benchmark.py: command failed with exit code "
            f"{error.returncode}: {shlex.join(error.cmd)}",
            file=sys.stderr,
        )
        return 1
    except (OSError, ValueError) as error:
        print(f"regression-benchmark.py: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
