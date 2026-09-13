#!/usr/bin/env bash
# hacman test suite.
#
# Two halves:
#
#   1. golden-file tests over ./tests/*.siml - each input is resolved with
#      `hacman --plan` and its combined output must match the .gold file next
#      to it. An input named xfail_*.siml must be rejected (exit 1); any other
#      input must plan successfully (exit 0). Plans expand {{VAR}} templates and
#      name cache files, so they run against a fixed HOME. Regenerate with:
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

# Ambient shim options must not change the test runner's own hacman calls.
unset HACMAN

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

# A plan resolves {{HOME}} and names a cache file, so it is only reproducible
# against a fixed environment.
plan() {
    env -u XDG_CACHE_HOME -u HACMAN_CACHE -u HACMAN_TEST_UNSET \
        HOME=/home/hacman-test "$BIN" --plan "$@"
}

for siml in tests/*.siml; do
    [ -e "$siml" ] || continue
    gold="${siml%.siml}.gold"
    base="$(basename "$siml")"

    case "$base" in
        xfail_*) want=1 ;;
        *)       want=0 ;;
    esac

    got="$(plan "$siml" 2>&1)"
    got_rc=$?

    # Embedded cache keys hash canonical source paths, which necessarily vary
    # with the checkout location. Preserve exact golden output for every other
    # field while giving those path-derived keys a portable spelling.
    if [[ "$got" == *"files-dir: "* ]]; then
        got="$(printf '%s\n' "$got" |
            sed -E \
                -e 's/cmd-[0-9a-f]{16}/cmd-<source-path-hash>/g' \
                -e 's#source:.* files:#source:<input-path> files:#')"
    fi

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
    if out="$(plan "$siml" 2>&1)"; then
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

# A shebang fixes the project path as argv[1], so HACMAN is how executable
# project files receive options intended for hacman itself.
mkdir -p "$TMP/hacman-path"
ln -s "$BIN" "$TMP/hacman-path/hacman"
expect "HACMAN passes --help through an executable project" 0 \
    env PATH="$TMP/hacman-path:$PATH" HACMAN=--help ./examples/hello-c.siml
contains "HACMAN help prints usage" "usage: hacman [OPTIONS] FILE"

expect "HACMAN accepts multiple and quoted options" 0 \
    env HACMAN='--plan --cache "/tmp/hacman env cache"' "$BIN" tests/minimal.siml
contains "quoted HACMAN value stays one argument" \
    "cache-file: /tmp/hacman env cache/"

expect "command-line option overrides HACMAN value" 0 \
    env HACMAN='--plan --cache /tmp/hacman-env-cache' \
    "$BIN" --cache /tmp/hacman-cli-cache tests/minimal.siml
contains "command-line cache wins" "cache-file: /tmp/hacman-cli-cache/"
missing "environment cache loses" "/tmp/hacman-env-cache/"

expect "unterminated HACMAN quote is rejected" 1 \
    env HACMAN='--cache "unterminated' "$BIN" tests/minimal.siml
contains "unterminated HACMAN quote is explained" "HACMAN contains an unterminated quote"

expect "HACMAN positional argument is rejected" 1 \
    env HACMAN='--verbose not-an-option' "$BIN" tests/minimal.siml
contains "HACMAN is options-only" "HACMAN may contain options only"

# Standard input is read only when the file argument is exactly "-".
expect "stdin input via -" 0 bash -c "printf 'url: https://example.com/x\n' | '$BIN' --plan -"
contains "stdin project is planned" "url: https://example.com/x"

expect "a missing file argument is a usage error" 1 bash -c "printf 'url: https://example.com/x\n' | '$BIN' --plan"
contains "missing file argument is explained" "no project file given"

# The default cache tree is deliberately stable at ~/.cache/hacman; ambient
# XDG settings must not scatter persistent build artifacts elsewhere.
expect "default cache stays below HOME" 0 \
    env HOME=/home/cache-test XDG_CACHE_HOME=/tmp/xdg-cache \
    "$BIN" --plan tests/minimal.siml
contains "default cache uses ~/.cache/hacman" \
    "cache-file: /home/cache-test/.cache/hacman/"
missing "XDG cache path is ignored" "/tmp/xdg-cache"

expect "missing HOME without a cache override is an error" 1 \
    env -u HOME -u HACMAN_CACHE "$BIN" --plan tests/minimal.siml
contains "missing HOME explains cache overrides" \
    "HOME is not set; use --cache or HACMAN_CACHE"

# --- check and install pipeline -----------------------------------------

# Embedded files are materialised below the cache before a command starts,
# and that directory is both its cwd and HACMAN_WORKDIR.
embedded_cache="$TMP/cache-embedded"
expect "embedded files stay untouched in dry-run" 10 \
    "$BIN" --dry-run --force --cache "$embedded_cache" tests/embedded_files.siml
tests=$((tests + 1))
if find "$embedded_cache" -name '*.work' -print -quit 2>/dev/null | grep -q .; then
    fail "dry-run created an embedded-file work directory"
else
    echo "[test] ok: dry-run creates no embedded files"
fi

expect "embedded files exist before command execution" 0 \
    "$BIN" --force --cache "$embedded_cache" tests/embedded_files.siml
contains "embedded command completed" "embedded-ready"

# An embedded command may build a program below its generated work directory
# and name it with a relative bin-path. Arguments after FILE go to that program.
cat >"$TMP/embedded-bin.siml" <<'EOF'
name: embedded-bin
command: mkdir -p build && cp runner build/prog && chmod +x build/prog
bin-path: build/prog
schedule: never
files:
  runner: |
    #!/bin/sh
    echo "embedded-prog"
    for arg in "$@"; do echo "arg:$arg"; done
EOF
expect "embedded relative bin-path runs" 0 \
    "$BIN" --force --cache "$TMP/cache-embedded-bin" \
    "$TMP/embedded-bin.siml" --help "two words"
contains "embedded program completed" "embedded-prog"
contains "embedded program receives option-looking arguments" "arg:--help"
contains "embedded program preserves argument boundaries" "arg:two words"

# Embedded command projects without an explicit schedule fingerprint their
# inputs and reuse the successful build until an input changes.
cat >"$TMP/embedded-cached.siml" <<'EOF'
name: embedded-cached
command: echo built >> build.log && chmod +x runner
bin-path: runner
files:
  runner: |
    #!/bin/sh
    echo cached-v1
EOF
cached_build_dir="$TMP/cache-embedded-cached"
expect "embedded cached build runs initially" 0 \
    "$BIN" --cache "$cached_build_dir" "$TMP/embedded-cached.siml"
contains "initial embedded executable runs" "cached-v1"
expect "unchanged embedded build is reused" 0 \
    "$BIN" -v --cache "$cached_build_dir" "$TMP/embedded-cached.siml"
contains "embedded cache hit is explained" "embedded inputs unchanged"
contains "cached embedded executable runs" "cached-v1"
tests=$((tests + 1))
if [ "$(find "$cached_build_dir" -name build.log -exec cat {} \; | wc -l)" = "1" ]; then
    echo "[test] ok: unchanged embedded command ran only once"
else
    fail "unchanged embedded command was rerun"
fi

# An explicit schedule adds periodic runs while keeping the executable and any
# command-owned state inside the same generated work directory.
cat >"$TMP/embedded-scheduled.siml" <<'EOF'
name: embedded-scheduled
command: echo checked >> checks.log && cp runner program && chmod +x program
bin-path: program
schedule: always
files:
  runner: |
    #!/bin/sh
    echo scheduled-program
EOF
scheduled_cache="$TMP/cache-embedded-scheduled"
expect "scheduled embedded command runs initially" 0 \
    "$BIN" --cache "$scheduled_cache" "$TMP/embedded-scheduled.siml"
contains "scheduled embedded executable runs" "scheduled-program"
expect "scheduled embedded command runs again" 0 \
    "$BIN" --cache "$scheduled_cache" "$TMP/embedded-scheduled.siml"
tests=$((tests + 1))
if [ "$(find "$scheduled_cache" -name checks.log -exec cat {} \; | wc -l)" = "2" ]; then
    echo "[test] ok: explicit schedule reran embedded command"
else
    fail "explicit schedule did not rerun embedded command"
fi

# Simultaneous due invocations share one updater. The leader is held inside
# its command until all followers have had time to block on the cache lock.
cat >"$TMP/embedded-concurrent.siml" <<EOF
name: embedded-concurrent
command: echo update >> update.log && touch "$TMP/update-started" && while [ ! -e "$TMP/update-release" ]; do sleep 0.01; done && cp runner program && chmod +x program
bin-path: program
schedule: 1h
files:
  runner: |
    #!/bin/sh
    echo concurrent-program
EOF
concurrent_cache="$TMP/cache-embedded-concurrent"
concurrent_pids=()
"$BIN" --cache "$concurrent_cache" "$TMP/embedded-concurrent.siml" \
    >"$TMP/concurrent-0.out" 2>&1 &
concurrent_pids+=("$!")
while [ ! -e "$TMP/update-started" ] && kill -0 "${concurrent_pids[0]}" 2>/dev/null; do
    sleep 0.01
done
for index in 1 2 3 4 5 6 7; do
    "$BIN" --cache "$concurrent_cache" "$TMP/embedded-concurrent.siml" \
        >"$TMP/concurrent-$index.out" 2>&1 &
    concurrent_pids+=("$!")
done
sleep 0.1
tests=$((tests + 1))
concurrent_alive=1
for pid in "${concurrent_pids[@]}"; do
    if ! kill -0 "$pid" 2>/dev/null; then concurrent_alive=0; fi
done
if [ "$concurrent_alive" = "1" ]; then
    echo "[test] ok: concurrent invocations wait for updater"
else
    fail "a concurrent invocation exited before the updater finished"
fi
touch "$TMP/update-release"
concurrent_failed=0
for pid in "${concurrent_pids[@]}"; do
    if ! wait "$pid"; then concurrent_failed=1; fi
done
tests=$((tests + 1))
if [ "$concurrent_failed" = "0" ]; then
    echo "[test] ok: concurrent invocations exec after update"
else
    fail "a concurrent invocation failed after the update"
fi
tests=$((tests + 1))
if [ "$(find "$concurrent_cache" -name update.log -exec cat {} \; | wc -l)" = "1" ]; then
    echo "[test] ok: concurrent invocations ran one updater"
else
    fail "concurrent invocations ran more than one updater"
fi

# Changing an embedded input clears and reuses its path-addressed work
# directory, including removal of files left by the previous build.
cached_workdir="$(find "$cached_build_dir" -maxdepth 1 -type d -name '*.work' -print -quit)"
touch "$cached_workdir/stale-output"
sed 's/cached-v1/cached-v2/' "$TMP/embedded-cached.siml" >"$TMP/embedded-cached.next"
mv "$TMP/embedded-cached.next" "$TMP/embedded-cached.siml"
expect "changed embedded input builds again" 0 \
    "$BIN" --cache "$cached_build_dir" "$TMP/embedded-cached.siml"
contains "changed embedded executable runs" "cached-v2"
tests=$((tests + 1))
changed_workdir="$(find "$cached_build_dir" -maxdepth 1 -type d -name '*.work' -print -quit)"
if [ "$changed_workdir" = "$cached_workdir" ] && \
    [ "$(find "$cached_build_dir" -maxdepth 1 -type d -name '*.work' | wc -l)" = "1" ]; then
    echo "[test] ok: changed embedded input reused its work directory"
else
    fail "changed embedded input did not reuse exactly one work directory"
fi
tests=$((tests + 1))
if [ ! -e "$changed_workdir/stale-output" ] && [ "$(cat "$changed_workdir/build.log")" = "built" ]; then
    echo "[test] ok: changed embedded input cleared stale build outputs"
else
    fail "changed embedded input left stale build outputs"
fi

# Identical definitions at different source paths own separate records and
# workspaces; changing one can therefore never clear the other's build.
cp "$TMP/embedded-cached.siml" "$TMP/embedded-copy.siml"
expect "copied embedded input builds separately" 0 \
    "$BIN" --cache "$cached_build_dir" "$TMP/embedded-copy.siml"
tests=$((tests + 1))
if [ "$(find "$cached_build_dir" -maxdepth 1 -type d -name '*.work' | wc -l)" = "2" ]; then
    echo "[test] ok: source paths select separate work directories"
else
    fail "different source paths shared an embedded work directory"
fi

payload="$TMP/payload.txt"
cache="$TMP/cache"
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

expect "first run installs" 0 "$BIN" --cache "$cache" "$TMP/hash.siml"
contains "first run reports new" "new      demo"
contains "install script ran" "install name=demo prev=none"
contains "response file is passed" "body=version 1.0.0"

expect "unchanged run is quiet" 0 "$BIN" --cache "$cache" "$TMP/hash.siml"
missing "no install on unchanged" "install name=demo"

expect "unchanged run is verbose on demand" 0 "$BIN" -v --cache "$cache" "$TMP/hash.siml"
contains "verbose reports ok" "ok       demo"

echo "version 1.0.1" >"$payload"
expect "changed run installs again" 0 "$BIN" --cache "$cache" "$TMP/hash.siml"
contains "change is reported with both marks" "changed  demo"
contains "previous mark reaches the script" "prev="

# --- schedules -----------------------------------------------------------

sched_cache="$TMP/cache-sched"
cat >"$TMP/sched.siml" <<EOF
name: scheduled
url: file://$payload
check: hash
schedule: 6h
install: echo "installed"
EOF

expect "scheduled first run checks" 0 "$BIN" --cache "$sched_cache" "$TMP/sched.siml"
contains "scheduled first run is a change" "new      scheduled"

echo "version 1.0.2" >"$payload"
expect "schedule suppresses the next check" 0 "$BIN" -v --cache "$sched_cache" "$TMP/sched.siml"
contains "skip mentions the wait" "skip     scheduled (next run in"
missing "no install while skipped" "installed"

expect "--force ignores the schedule" 0 "$BIN" -f --cache "$sched_cache" "$TMP/sched.siml"
contains "forced run installs" "installed"

cat >"$TMP/never.siml" <<EOF
name: manual
url: file://$payload
check: hash
schedule: never
install: echo "installed"
EOF
expect "schedule: never does nothing" 0 "$BIN" -v --cache "$TMP/cache-never" "$TMP/never.siml"
contains "never is reported as such" "skip     manual (schedule: never)"
expect "schedule: never yields to --force" 0 "$BIN" -f --cache "$TMP/cache-never" "$TMP/never.siml"
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
expect "version extraction" 0 "$BIN" --cache "$TMP/cache-version" "$TMP/version.siml"
contains "version is extracted between the anchors" "got 14.1.1"

cat >"$TMP/badversion.siml" <<EOF
name: versioned
url: file://$TMP/release.json
check: version
version-prefix: nothing-like-this
schedule: always
install: echo "unreachable"
EOF
expect "missing version-prefix fails the check" 2 "$BIN" --cache "$TMP/cache-bad" "$TMP/badversion.siml"
contains "missing prefix is explained" "version-prefix not found"

# --- modes ---------------------------------------------------------------

echo "version 2.0.0" >"$payload"
expect "--check-only reports changes as exit 10" 10 "$BIN" -c --cache "$cache" "$TMP/hash.siml"
missing "--check-only does not install" "install name=demo"
expect "--dry-run also reports exit 10" 10 "$BIN" -n --cache "$cache" "$TMP/hash.siml"
expect "--adopt records without installing" 0 "$BIN" -a --cache "$cache" "$TMP/hash.siml"
missing "--adopt does not install" "install name=demo"
expect "adopted state is now unchanged" 0 "$BIN" -v --cache "$cache" "$TMP/hash.siml"
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
expect "failing install exits 2" 2 "$BIN" --cache "$TMP/cache-fail" "$TMP/fail.siml"
contains "failure names the exit code" "exit code 3"
# The check time is dropped on failure, so the schedule must not hide a retry.
expect "failed install is retried immediately" 2 "$BIN" --cache "$TMP/cache-fail" "$TMP/fail.siml"
contains "retry runs the script again" "about to fail"

cat >"$TMP/missing.siml" <<EOF
name: gone
url: file://$TMP/does-not-exist
check: hash
schedule: always
install: echo "unreachable"
EOF
expect "unreachable url exits 2" 2 "$BIN" --cache "$TMP/cache-missing" "$TMP/missing.siml"
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
expect "indentation inside the block is preserved" 0 "$BIN" --cache "$TMP/cache-indent" "$TMP/indent.siml"
contains "nested shell block ran" "nested 2"

# The plan prints the same lines the installer writes into its script, so the
# script kept by HACMAN_KEEP_TEMP must match the plan's install block.
"$BIN" --plan "$TMP/indent.siml" | sed -n '/^install: |$/,$p' | tail -n +2 | sed 's/^  //' >"$TMP/planned.sh"
rm -rf "$TMP/cache-indent"
kept="$(HACMAN_KEEP_TEMP=1 "$BIN" --cache "$TMP/cache-indent" "$TMP/indent.siml" 2>&1 |
        sed -n 's/^hacman: indented: kept //p')"
tests=$((tests + 1))
if [ -n "$kept" ] && diff -u <(tail -n +2 "$kept") "$TMP/planned.sh" >"$TMP/diff" 2>&1; then
    echo "[test] ok: the plan matches the generated script"
else
    fail "the plan does not match the generated script"
    sed 's/^/    | /' "$TMP/diff" >&2
fi
[ -n "$kept" ] && rm -f "$kept"

# --- command projects ----------------------------------------------------

cmd_cache="$TMP/cache-cmd"
marker="$TMP/ran.log"
: >"$marker"
export TMPDIR_FOR_TEST="$TMP"

cat >"$TMP/cmd-a.siml" <<EOF
name: first-file
command: echo "ran in \$(pwd)" >>"$marker"
workdir: {{TMPDIR_FOR_TEST}}
schedule: 6h
EOF

# The same command and workdir, spelled in a second file under another name.
sed 's/^name: first-file$/name: second-file/' "$TMP/cmd-a.siml" >"$TMP/cmd-b.siml"

expect "command project runs" 0 "$BIN" --cache "$cmd_cache" -v "$TMP/cmd-a.siml"
contains "verbose announces the run" "run      first-file"
tests=$((tests + 1))
if [ "$(wc -l <"$marker")" = "1" ] && grep -q "ran in $TMP" "$marker"; then
echo "[test] ok: the command ran in its workdir"
else
fail "the command did not run in its workdir"
sed 's/^/    | /' "$marker" >&2
fi

expect "command respects its schedule" 0 "$BIN" --cache "$cmd_cache" -v "$TMP/cmd-a.siml"
contains "second invocation is skipped" "skip     first-file (next run in"

# The record is keyed by command+workdir, so the other file is skipped too.
expect "another file with the same command shares the record" 0 \
"$BIN" --cache "$cmd_cache" -v "$TMP/cmd-b.siml"
contains "the shared record skips the second file" "skip     second-file (next run in"
tests=$((tests + 1))
if [ "$(wc -l <"$marker")" = "1" ]; then
echo "[test] ok: the shared record prevented a second run"
else
fail "the command ran again despite the shared record"
fi

tests=$((tests + 1))
if [ "$(find "$cmd_cache" -type f ! -name '*.lock' | wc -l)" = "1" ]; then
echo "[test] ok: both files use one cache record"
else
fail "expected exactly one cache record"
find "$cmd_cache" -type f ! -name '*.lock' | sed 's/^/    | /' >&2
fi

expect "--force runs a scheduled command again" 0 "$BIN" -f --cache "$cmd_cache" "$TMP/cmd-a.siml"
tests=$((tests + 1))
if [ "$(wc -l <"$marker")" = "2" ]; then
echo "[test] ok: --force ran the command a second time"
else
fail "--force did not run the command"
fi

# A different working directory is a different thing to remember.
mkdir -p "$TMP/other"
sed "s|^workdir: .*|workdir: $TMP/other|" "$TMP/cmd-a.siml" >"$TMP/cmd-c.siml"
expect "a different workdir is a different record" 0 "$BIN" --cache "$cmd_cache" "$TMP/cmd-c.siml"
tests=$((tests + 1))
if [ "$(wc -l <"$marker")" = "3" ] && grep -q "ran in $TMP/other" "$marker"; then
echo "[test] ok: the same command elsewhere ran on its own schedule"
else
fail "the same command in another workdir did not run"
sed 's/^/    | /' "$marker" >&2
fi

# A failing command must not be recorded, so it is retried immediately.
cat >"$TMP/cmd-fail.siml" <<EOF
name: failing-command
command: echo attempt >>"$marker.fail"; exit 7
workdir: {{TMPDIR_FOR_TEST}}
schedule: 6h
EOF
: >"$marker.fail"
expect "a failing command exits 2" 2 "$BIN" --cache "$cmd_cache" "$TMP/cmd-fail.siml"
contains "the exit code is reported" "command failed with exit code 7"
expect "a failing command is retried at once" 2 "$BIN" --cache "$cmd_cache" "$TMP/cmd-fail.siml"
tests=$((tests + 1))
if [ "$(wc -l <"$marker.fail")" = "2" ]; then
echo "[test] ok: the failed run was not recorded"
else
fail "the failed command was recorded"
fi

expect "--check-only reports a due command as exit 10" 10 \
"$BIN" -c --cache "$cmd_cache" "$TMP/cmd-fail.siml"
contains "due command is named" "due      failing-command"

expect "--adopt records a command without running it" 0 \
"$BIN" -a --cache "$cmd_cache" "$TMP/cmd-fail.siml"
tests=$((tests + 1))
if [ "$(wc -l <"$marker.fail")" = "2" ]; then
echo "[test] ok: --adopt did not run the command"
else
fail "--adopt ran the command"
fi
expect "an adopted command is now on its schedule" 0 \
"$BIN" -v --cache "$cmd_cache" "$TMP/cmd-fail.siml"
contains "adopted command is skipped" "skip     failing-command (next run in"

# The default working directory is $HOME.
mkdir -p "$TMP/fake-home"
cat >"$TMP/cmd-home.siml" <<EOF
name: at-home
command: pwd >"$marker.home"
schedule: always
EOF
expect "a command without workdir runs in HOME" 0 \
env HOME="$TMP/fake-home" "$BIN" --cache "$cmd_cache" "$TMP/cmd-home.siml"
tests=$((tests + 1))
if [ "$(cat "$marker.home")" = "$TMP/fake-home" ]; then
echo "[test] ok: the default workdir is {{HOME}}"
else
fail "the default workdir was $(cat "$marker.home"), expected $TMP/fake-home"
fi

# --- shim behaviour ------------------------------------------------------

shim_cache="$TMP/cache-shim"
mkdir -p "$TMP/bin"
setup_log="$TMP/setup.log"
: >"$setup_log"

# Prints one argument per line, so argument boundaries are checked too.
cat >"$TMP/bin/prog" <<'EOF'
#!/bin/sh
echo "prog-ran"
for a in "$@"; do echo "arg:$a"; done
EOF
chmod +x "$TMP/bin/prog"

cat >"$TMP/shim.siml" <<EOF
name: shimmed
command: echo setup >>"$setup_log"
workdir: {{TMPDIR_FOR_TEST}}
bin-path: $TMP/bin/prog
schedule: 6h
EOF

expect "the shim sets up and then execs" 0 \
    "$BIN" -v --cache "$shim_cache" "$TMP/shim.siml" --flag "two words"
contains "the setup ran" "run      shimmed"
contains "the program ran" "prog-ran"
contains "an option-looking argument is forwarded" "arg:--flag"
contains "argument boundaries survive" "arg:two words"
tests=$((tests + 1))
if [ "$(wc -l <"$setup_log")" = "1" ]; then
    echo "[test] ok: the setup ran once"
else
    fail "the setup ran $(wc -l <"$setup_log") times"
fi

# The common case: nothing to do, so hacman is just a fast detour.
expect "a project that is not due still execs" 0 \
    "$BIN" --cache "$shim_cache" "$TMP/shim.siml" again
contains "the program ran again" "prog-ran"
contains "its argument came through" "arg:again"
tests=$((tests + 1))
if [ "$(wc -l <"$setup_log")" = "1" ]; then
    echo "[test] ok: the skipped setup did not run"
else
    fail "the setup ran although the project was not due"
fi

# hacman's own options are only its own before FILE.
expect "options after FILE belong to the program" 0 \
    "$BIN" --cache "$shim_cache" "$TMP/shim.siml" --plan -v
contains "--plan after FILE was forwarded" "arg:--plan"
missing "--plan after FILE did not print a plan" "cache-identity:"

# As a shim, hacman must not write to the program's stdout.
tests=$((tests + 1))
if [ "$("$BIN" -v --cache "$shim_cache" "$TMP/shim.siml" x 2>/dev/null)" = "prog-ran
arg:x" ]; then
    echo "[test] ok: status output stays off the program's stdout"
else
    fail "hacman wrote to the program's stdout"
fi

# The program's exit status becomes hacman's.
printf '#!/bin/sh\nexit 3\n' >"$TMP/bin/failprog"
chmod +x "$TMP/bin/failprog"
sed "s|^bin-path: .*|bin-path: $TMP/bin/failprog|" "$TMP/shim.siml" >"$TMP/shim-fail.siml"
expect "the program's exit status is passed through" 3 \
    "$BIN" --cache "$shim_cache" "$TMP/shim-fail.siml"

sed "s|^bin-path: .*|bin-path: $TMP/bin/absent|" "$TMP/shim.siml" >"$TMP/shim-absent.siml"
expect "a missing program exits 127" 127 "$BIN" --cache "$shim_cache" "$TMP/shim-absent.siml"
contains "the missing program is named" "cannot execute"

# A failed setup must not hand over a program that may be stale or missing.
cat >"$TMP/shim-badsetup.siml" <<EOF
name: bad-setup
command: exit 5
workdir: {{TMPDIR_FOR_TEST}}
bin-path: $TMP/bin/prog
schedule: always
EOF
expect "a failed setup does not exec" 2 "$BIN" --cache "$shim_cache" "$TMP/shim-badsetup.siml"
missing "the program was not started" "prog-ran"

# Forwarding needs somewhere to forward to.
expect "arguments without a bin-path are an error" 1 \
    "$BIN" --cache "$shim_cache" "$TMP/cmd-a.siml" surplus
contains "the missing bin-path is explained" "need a 'bin-path'"

# Inspection modes report instead of handing over.
expect "--plan does not exec" 0 "$BIN" --cache "$shim_cache" --plan "$TMP/shim.siml"
missing "--plan did not start the program" "prog-ran"
contains "--plan describes the hand-over" "exec: bin-path"

unset TMPDIR_FOR_TEST

echo "[test] ran ${tests} assertions"
if [ "$rc" = 0 ]; then
    echo "[test] all passed"
fi
exit "$rc"
