#!/usr/bin/env python3
"""
fuzzer.py - libFuzzer harness driver for SIML.

Compiles fuzzer.c with clang if needed, seeds the corpus from the
testcases/ directory, then runs the fuzzer.  On a clean exit (no
crashes) it removes fuzz-*.log files, the compiled harness binary, and
the corpus cache so nothing stale accumulates.  Pass --cleanup to remove
those files without running the fuzzer.

Requires clang with libFuzzer support (Linux and macOS; not available
on Windows without WSL).
"""

import argparse
import platform
import shutil
import subprocess
import sys
from pathlib import Path

DEFAULT_TIME = 900   # seconds (15 min)
DEFAULT_JOBS = 16


def find_root() -> Path:
    return Path(__file__).parent.resolve()


def parse_args():
    # Split at '--' before handing to argparse so libFuzzer flags are
    # not consumed.
    argv = sys.argv[1:]
    if '--' in argv:
        idx = argv.index('--')
        our_argv, extra = argv[:idx], argv[idx + 1:]
    else:
        our_argv, extra = argv, []

    p = argparse.ArgumentParser(
        prog=Path(__file__).name,
        description='Run the siml libFuzzer harness.',
        epilog='Arguments after -- are passed directly to the fuzzer binary, e.g.:\n'
               '  %(prog)s -- -max_total_time=300 -jobs=4',
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    p.add_argument(
        '--forever', action='store_true',
        help=f'run until Ctrl-C (default: stop after {DEFAULT_TIME} s / 15 min)',
    )
    p.add_argument(
        '--max-len', type=int, metavar='N',
        help='cap generated input size at N bytes (-max_len=N)',
    )
    p.add_argument(
        '--cleanup', action='store_true',
        help='remove fuzzer temporary files and exit',
    )
    args = p.parse_args(our_argv)
    args.extra = extra
    return args


def needs_rebuild(src: Path, hdr: Path, binary: Path) -> bool:
    if not binary.exists():
        return True
    binary_mtime = binary.stat().st_mtime
    return src.stat().st_mtime > binary_mtime or hdr.stat().st_mtime > binary_mtime


def cleanup(root: Path, corpus: Path, fuzzer_bin: Path) -> None:
    for log in root.glob('fuzz-*.log'):
        log.unlink(missing_ok=True)
    fuzzer_bin.unlink(missing_ok=True)
    if corpus.exists():
        shutil.rmtree(corpus)


def main() -> int:
    args = parse_args()

    root  = find_root()
    corpus    = root / '.cache' / 'fuzzer'
    testcases = root / 'testcases'
    src       = root / 'fuzzer.c'
    hdr       = root / 'siml.h'
    fuzzer_bin = root / 'fuzzer'

    if args.cleanup:
        print('[fuzz] removing logs, harness binary, and corpus cache...')
        cleanup(root, corpus, fuzzer_bin)
        return 0

    if platform.system() == 'Windows':
        print('error: libFuzzer is not supported on Windows; use WSL', file=sys.stderr)
        return 1

    clang = shutil.which('clang')
    if clang is None:
        print('error: clang not found', file=sys.stderr)
        return 1

    if needs_rebuild(src, hdr, fuzzer_bin):
        print('[fuzz] compiling fuzzer...')
        r = subprocess.run(
            [clang, '-fsanitize=fuzzer,address', '-O1',
             '-o', str(fuzzer_bin), str(src)],
        )
        if r.returncode != 0:
            return r.returncode

    corpus.mkdir(parents=True, exist_ok=True)
    if not any(corpus.iterdir()):
        print('[fuzz] seeding corpus from testcases/...')
        for f in sorted(testcases.glob('*.siml')):
            shutil.copy(f, corpus / f.name)

    fuzz_args = [str(fuzzer_bin), str(corpus)]
    if not args.forever:
        fuzz_args.append(f'-max_total_time={DEFAULT_TIME}')
    fuzz_args += [f'-jobs={DEFAULT_JOBS}', f'-workers={DEFAULT_JOBS}']
    if args.max_len is not None:
        fuzz_args.append(f'-max_len={args.max_len}')
    fuzz_args += args.extra

    duration = 'running until Ctrl-C' if args.forever else f'stopping after {DEFAULT_TIME} s'
    print(f'[fuzz] starting fuzzer ({duration}, {DEFAULT_JOBS} jobs)...')

    rc = subprocess.run(fuzz_args).returncode

    if rc == 0:
        print('[fuzz] clean run - removing logs, harness binary, and corpus cache...')
        cleanup(root, corpus, fuzzer_bin)

    return rc


if __name__ == '__main__':
    sys.exit(main())
