#!/usr/bin/env bash
# hacman test suite.
#
# Two halves:
#
#   1. golden-file tests over ./tests/*.siml - each input is resolved with
#      `hacman --plan` and its combined output must match the .gold file next
#      to it. An input named xfail_*.siml must be rejected (exit 1); any other
#      input must plan successfully (exit 0). Regenerate with:
#
#          GOLD=update ./tests.sh
#
#   2. behavioural tests of the real pipeline, run entirely offline: the checks
#      point at file:// URLs, which curl serves from the local filesystem, so
#      check -> change detection -> install all run for real.
#
#   ./tests.sh                  build (if needed) and run
#   ./tests.sh path/to/hacman   test an existing binary (used by `meson test`)
set -uo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${BUILD_DIR:-"$ROOT_DIR/build"}"
BIN="${1:-${BIN:-"$BUILD_DIR/hacman"}}"
GOLD="${GOLD:-check}"

if [[ "${DEBUG:-}" != "" ]]; then
    echo "[dbg][tests.sh] ROOT_DIR=${ROOT_DIR}" >&2
    echo "[dbg][tests.sh] BIN=${BIN}" >&2
    set -x
fi

if [ ! -x "$BIN" ]; then
    "$ROOT_DIR/build.sh"
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

rc=0
tests=0

fail() {
    echo "[test] FAILED: $1" >&2
    rc=1
}

# --- golden-file plan tests ----------------------------------------------
#
# Run from the repository root so the paths inside error messages - and thus
# the .gold files - are stable.
cd "$ROOT_DIR" || exit 1

for siml in tests/*.siml; do
    [ -e "$siml" ] || continue
    gold="${siml%.siml}.gold"
    base="$(basename "$siml")"

    case "$base" in
        xfail_*) want=1 ;;
        *)       want=0 ;;
    esac

    got="$("$BIN" --plan "$siml" 2>&1)"
    got_rc=$?

    if [ "$GOLD" = "update" ]; then
        printf '%s\n' "$got" >"$gold"
        echo "[test] updated: $gold"
        continue
    fi

    tests=$((tests + 1))
    if [ "$got_rc" != "$want" ]; then
        fail "$siml: exit code $got_rc, expected $want"
        printf '%s\n' "$got" | sed 's/^/    | /' >&2
        continue
    fi

    if [ ! -e "$gold" ]; then
        fail "$siml: no golden file at $gold"
        continue
    fi

    if ! diff -u "$gold" <(printf '%s\n' "$got") >"$TMP/diff" 2>&1; then
        fail "$siml: plan differs from $gold"
        sed 's/^/    | /' "$TMP/diff" >&2
        continue
    fi
    echo "[test] ok: $siml"
done

[ "$GOLD" = "update" ] && exit 0

# Every shipped example must resolve, too.
for siml in examples/*.siml; do
    [ -e "$siml" ] || continue
    tests=$((tests + 1))
    if out="$("$BIN" --plan "$siml" 2>&1)"; then
        echo "[test] ok: $siml plans"
    else
        fail "$siml: does not plan"
        printf '%s\n' "$out" | sed 's/^/    | /' >&2
    fi
done

# --- behavioural tests ----------------------------------------------------

if ! command -v curl >/dev/null 2>&1; then
    echo "[test] SKIP behavioural tests: curl is not installed" >&2
    echo "[test] ran ${tests} assertions"
    exit "$rc"
fi

# expect <name> <expected-exit-code> <command...>; the output is captured in
# $out and can be inspected by the caller afterwards.
expect() {
    local name="$1" want="$2"
    shift 2
    tests=$((tests + 1))
    out="$("$@" 2>&1)"
    local got=$?
    if [ "$got" != "$want" ]; then
        fail "$name: exit code $got, expected $want"
        echo "$out" | sed 's/^/    | /' >&2
        return 1
    fi
    echo "[test] ok: $name"
    return 0
}

contains() {
    local name="$1" needle="$2"
    tests=$((tests + 1))
    if [[ "$out" != *"$needle"* ]]; then
        fail "$name: output does not contain '$needle'"
        echo "$out" | sed 's/^/    | /' >&2
        return 1
    fi
    echo "[test] ok: $name"
    return 0
}

missing() {
    local name="$1" needle="$2"
    tests=$((tests + 1))
    if [[ "$out" == *"$needle"* ]]; then
        fail "$name: output unexpectedly contains '$needle'"
        echo "$out" | sed 's/^/    | /' >&2
        return 1
    fi
    echo "[test] ok: $name"
    return 0
}

# --- input handling -------------------------------------------------------

# Standard input is read only when the file argument is exactly "-".
expect "stdin input via -" 0 bash -c "printf 'url: https://example.com/x\n' | '$BIN' --plan -"
contains "stdin project is planned" "url: https://example.com/x"

expect "a missing file argument is a usage error" 1 bash -c "printf 'url: https://example.com/x\n' | '$BIN' --plan"
contains "missing file argument is explained" "no project file given"

# --- check and install pipeline -----------------------------------------

payload="$TMP/payload.txt"
state="$TMP/state.tsv"
echo "version 1.0.0" >"$payload"

cat >"$TMP/hash.siml" <<EOF
name: demo
url: file://$payload
check: hash
schedule: always
install: |
  set -eu
  echo "install name=\$HACMAN_NAME prev=\${HACMAN_PREVIOUS:-none}"
  if [ -n "\${HACMAN_RESPONSE:-}" ]; then
    echo "body=\$(cat "\$HACMAN_RESPONSE")"
  fi
EOF

expect "first run installs" 0 "$BIN" --state "$state" "$TMP/hash.siml"
contains "first run reports new" "new      demo"
contains "install script ran" "install name=demo prev=none"
contains "response file is passed" "body=version 1.0.0"

expect "unchanged run is quiet" 0 "$BIN" --state "$state" "$TMP/hash.siml"
missing "no install on unchanged" "install name=demo"

expect "unchanged run is verbose on demand" 0 "$BIN" -v --state "$state" "$TMP/hash.siml"
contains "verbose reports ok" "ok       demo"

echo "version 1.0.1" >"$payload"
expect "changed run installs again" 0 "$BIN" --state "$state" "$TMP/hash.siml"
contains "change is reported with both marks" "changed  demo"
contains "previous mark reaches the script" "prev="

# --- schedules -----------------------------------------------------------

sched_state="$TMP/sched.tsv"
cat >"$TMP/sched.siml" <<EOF
name: scheduled
url: file://$payload
check: hash
schedule: 6h
install: echo "installed"
EOF

expect "scheduled first run checks" 0 "$BIN" --state "$sched_state" "$TMP/sched.siml"
contains "scheduled first run is a change" "new      scheduled"

echo "version 1.0.2" >"$payload"
expect "schedule suppresses the next check" 0 "$BIN" -v --state "$sched_state" "$TMP/sched.siml"
contains "skip mentions the wait" "skip     scheduled (next check in"
missing "no install while skipped" "installed"

expect "--force ignores the schedule" 0 "$BIN" -f --state "$sched_state" "$TMP/sched.siml"
contains "forced run installs" "installed"

cat >"$TMP/never.siml" <<EOF
name: manual
url: file://$payload
check: hash
schedule: never
install: echo "installed"
EOF
expect "schedule: never does nothing" 0 "$BIN" -v --state "$TMP/never.tsv" "$TMP/never.siml"
contains "never is reported as such" "skip     manual (schedule: never)"
expect "schedule: never yields to --force" 0 "$BIN" -f --state "$TMP/never.tsv" "$TMP/never.siml"
contains "forced manual project installs" "installed"

# --- version extraction --------------------------------------------------

cat >"$TMP/release.json" <<'EOF'
{"tag_name": "14.1.1", "name": "ripgrep 14.1.1"}
EOF
cat >"$TMP/version.siml" <<EOF
name: versioned
url: file://$TMP/release.json
check: version
version-prefix: "tag_name": "
version-suffix: "
schedule: always
install: echo "got \$HACMAN_VERSION"
EOF
expect "version extraction" 0 "$BIN" --state "$TMP/version.tsv" "$TMP/version.siml"
contains "version is extracted between the anchors" "got 14.1.1"

cat >"$TMP/badversion.siml" <<EOF
name: versioned
url: file://$TMP/release.json
check: version
version-prefix: nothing-like-this
schedule: always
install: echo "unreachable"
EOF
expect "missing version-prefix fails the check" 2 "$BIN" --state "$TMP/bad.tsv" "$TMP/badversion.siml"
contains "missing prefix is explained" "version-prefix not found"

# --- modes ---------------------------------------------------------------

echo "version 2.0.0" >"$payload"
expect "--check-only reports changes as exit 10" 10 "$BIN" -c --state "$state" "$TMP/hash.siml"
missing "--check-only does not install" "install name=demo"
expect "--dry-run also reports exit 10" 10 "$BIN" -n --state "$state" "$TMP/hash.siml"
expect "--adopt records without installing" 0 "$BIN" -a --state "$state" "$TMP/hash.siml"
missing "--adopt does not install" "install name=demo"
expect "adopted state is now unchanged" 0 "$BIN" -v --state "$state" "$TMP/hash.siml"
contains "adopted project reads as ok" "ok       demo"

# --- failure handling ----------------------------------------------------

cat >"$TMP/fail.siml" <<EOF
name: failing
url: file://$payload
check: hash
schedule: 6h
install: |
  echo "about to fail"
  exit 3
EOF
expect "failing install exits 2" 2 "$BIN" --state "$TMP/fail.tsv" "$TMP/fail.siml"
contains "failure names the exit code" "exit code 3"
# The check time is dropped on failure, so the schedule must not hide a retry.
expect "failed install is retried immediately" 2 "$BIN" --state "$TMP/fail.tsv" "$TMP/fail.siml"
contains "retry runs the script again" "about to fail"

cat >"$TMP/missing.siml" <<EOF
name: gone
url: file://$TMP/does-not-exist
check: hash
schedule: always
install: echo "unreachable"
EOF
expect "unreachable url exits 2" 2 "$BIN" --state "$TMP/missing.tsv" "$TMP/missing.siml"
contains "unreachable url is reported" "request failed"

# --- the plan describes the script that actually runs --------------------

cat >"$TMP/indent.siml" <<EOF
name: indented
url: file://$payload
check: hash
schedule: always
install: |
  for i in 1 2; do
    if [ "\$i" = 2 ]; then
      echo "nested \$i"
    fi
  done
EOF
expect "indentation inside the block is preserved" 0 "$BIN" --state "$TMP/indent.tsv" "$TMP/indent.siml"
contains "nested shell block ran" "nested 2"

# The plan prints the same lines the installer writes into its script, so the
# script kept by HACMAN_KEEP_TEMP must match the plan's install block.
"$BIN" --plan "$TMP/indent.siml" | sed -n '/^install: |$/,$p' | tail -n +2 | sed 's/^  //' >"$TMP/planned.sh"
rm -f "$TMP/indent.tsv"
kept="$(HACMAN_KEEP_TEMP=1 "$BIN" --state "$TMP/indent.tsv" "$TMP/indent.siml" 2>&1 |
        sed -n 's/^hacman: indented: kept //p')"
tests=$((tests + 1))
if [ -n "$kept" ] && diff -u <(tail -n +2 "$kept") "$TMP/planned.sh" >"$TMP/diff" 2>&1; then
    echo "[test] ok: the plan matches the generated script"
else
    fail "the plan does not match the generated script"
    sed 's/^/    | /' "$TMP/diff" >&2
fi
[ -n "$kept" ] && rm -f "$kept"

echo "[test] ran ${tests} assertions"
if [ "$rc" = 0 ]; then
    echo "[test] all passed"
fi
exit "$rc"
