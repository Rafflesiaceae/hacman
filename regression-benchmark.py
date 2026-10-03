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


def settings(env: dict[str, str]) -> tuple[int, int, float, float]:
    """Validate budgets before spending time compiling or measuring anything."""
    defaults = {
        "HACMAN_CC": "musl-gcc",
        "BENCHMARK_WARMUP": "20",
        "BENCHMARK_RUNS": "1000",
        "BENCHMARK_MIN_RATIO": "1",
        "BENCHMARK_MAX_RATIO": "3",
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
    """Assert the inclusive budget using hyperfine's timings in seconds."""
    with report.open(encoding="utf-8") as stream:
        results = json.load(stream)["results"]
    if len(results) != 2 or {result["command"] for result in results} != {
        "direct",
        "hacman",
    }:
        raise ValueError("expected direct and hacman measurements")
    means = {}
    for result in results:
        mean = result["mean"]
        if not math.isfinite(mean) or mean <= 0 or any(result["exit_codes"]):
            raise ValueError("invalid timings or failed benchmark runs")
        means[result["command"]] = mean
    ratio = means["hacman"] / means["direct"]
    print(
        f"Mean direct: {means['direct'] * 1e6:.1f} us; "
        f"hacman: {means['hacman'] * 1e6:.1f} us"
    )
    print(f"hacman/direct: {ratio:.3f}; allowed: [{lower:g}, {upper:g}]")
    print(f"Report: {report}")
    if not lower <= ratio <= upper:
        raise ValueError("FAIL: launch-time ratio outside allowed range")
    print("regression-benchmark.py: PASS")


def benchmark() -> None:
    """Build and check the fixture, then measure cached and direct launches."""
    env = os.environ.copy()
    warmup, runs, lower, upper = settings(env)
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

        # Hyperfine parses argument strings even with no shell. Quote paths so
        # spaces and apostrophes remain literal without timing a shell process.
        run(
            [
                "hyperfine",
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
        check_report(report, lower, upper)


def main() -> int:
    """Expose environment settings in help and report failures without a traceback."""
    parser = argparse.ArgumentParser(
        description=(
            "Build hacman and a small embedded static musl C project, check its "
            "behavior, then assert the ratio of mean cached-launch time to mean "
            "direct-launch time."
        ),
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""Environment:
  HACMAN_CC           musl C compiler executable (default: musl-gcc)
  BENCHMARK_WARMUP    warmup runs per command (default: 20)
  BENCHMARK_RUNS      measured runs per command (default: 1000)
  BENCHMARK_MIN_RATIO inclusive minimum hacman/direct ratio (default: 1)
  BENCHMARK_MAX_RATIO inclusive maximum hacman/direct ratio (default: 3)
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
