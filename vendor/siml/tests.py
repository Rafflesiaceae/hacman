#!/usr/bin/env python3
import difflib
import os
import subprocess
import sys
from pathlib import Path


def find_root():
    if 'MESON_SOURCE_ROOT' in os.environ:
        return Path(os.environ['MESON_SOURCE_ROOT'])
    return Path(__file__).parent


def find_build():
    if 'MESON_BUILD_ROOT' in os.environ:
        return Path(os.environ['MESON_BUILD_ROOT'])
    return find_root() / 'build'


root = find_root()
build = find_build()
siml_tool = Path(os.environ.get('SIML_TOOL', build / 'siml-tool'))
test_dir = root / 'testcases'

failed = 0

for siml in sorted(test_dir.glob('*.siml')):
    print(f'[test] {siml}')
    xfail = siml.with_suffix('.xfail')

    if xfail.exists():
        expected_err = xfail.read_text()
        env = os.environ.copy()
        if siml.name == 'xfail_io_error.siml':
            env['SIML_TEST_READ_ERROR_AFTER'] = '1'
        result = subprocess.run(
            [siml_tool, 'dump', siml],
            capture_output=True,
            text=True,
            env=env,
        )
        if result.returncode == 0:
            print(f'[test] FAILED (expected error but succeeded): {siml}', file=sys.stderr)
            failed += 1
            continue
        if expected_err not in result.stderr:
            print(f'[test] FAILED (error mismatch): {siml}', file=sys.stderr)
            print(f'[test]   expected substring: {expected_err}', file=sys.stderr)
            print(f'[test]   got stderr:', file=sys.stderr)
            print(result.stderr, file=sys.stderr)
            failed += 1
        continue

    result = subprocess.run([siml_tool, 'dump', siml], capture_output=True, text=True)
    if result.returncode != 0:
        print(f'[test] FAILED (parse error): {siml}', file=sys.stderr)
        failed += 1
        continue

    gold = siml.with_suffix('.gold')
    if not gold.exists():
        gold.write_text(result.stdout)
        print(f'[test] generated {gold}')
    else:
        expected = gold.read_text()
        if result.stdout != expected:
            diff = difflib.unified_diff(
                expected.splitlines(keepends=True),
                result.stdout.splitlines(keepends=True),
                fromfile=str(gold),
                tofile='actual',
            )
            print(''.join(diff), end='')
            print(f'[test] FAILED (output mismatch): {siml}', file=sys.stderr)
            failed += 1

    result_rt = subprocess.run([siml_tool, 'roundtrip', siml], capture_output=True, text=True)
    if result_rt.returncode != 0:
        print(f'[test] FAILED (roundtrip mismatch): {siml}', file=sys.stderr)
        failed += 1

    result_verify = subprocess.run([siml_tool, siml], capture_output=True, text=True)
    if result_verify.returncode != 0 or result_verify.stdout or result_verify.stderr:
        print(f'[test] FAILED (implicit verify): {siml}', file=sys.stderr)
        print(result_verify.stdout, file=sys.stderr, end='')
        print(result_verify.stderr, file=sys.stderr, end='')
        failed += 1

sys.exit(1 if failed else 0)
