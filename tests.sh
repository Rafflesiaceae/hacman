#!/bin/sh
# Compatibility entrypoint; the test harness now lives in Python.
exec python3 "$(dirname "$0")/tests.py" "$@"
