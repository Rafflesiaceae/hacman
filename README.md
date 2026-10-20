# hacman

`hacman` sets a program up, then gets out of the way.

It reads **one project** in [SIML](vendor/siml/SPEC.rst) format — from a file, or
from standard input when the file argument is `-` — makes sure the thing that
file describes is set up, and then **execs the program it names**, forwarding
every argument that followed the file:

```sh
alias cargo='hacman ~/.config/hacman/cargo-shim.siml'

cargo build --release   # updates the toolchain at most once a week,
                        # then becomes the real cargo with `build --release`
```

"Set up" is one of two things, on a schedule: check a URL and run an install
script when it changed, or simply run a command.

```
url: https://go.dev/VERSION?m=text      command: cp ~/src/tool/tool tool
check: hash                             bin-path: tool
schedule: daily                         schedule: weekly
install: |
  ...
```

`bin-path` is optional: without it hacman just does the work and exits, which
is what a `cron` line or a shell profile wants.

One project per file: a file that describes a second project (a top-level
sequence, another mapping, another `---` document) is rejected. Watching many
things means many files, and a loop:

```sh
hacman ~/.config/hacman/ripgrep.siml

for f in ~/.config/hacman/*.siml; do hacman "$f"; done
```

```
changed  ripgrep 14.1.0 -> 14.1.1
installing ripgrep 14.1.1 (was: 14.1.0)
```

---

## Startup speed is the point

**hacman must always strive for absolute minimal startup overhead.** It is
built to be run constantly — from a shell profile, a `cron` line, a window
manager hook, a prompt — where the common case is *"nothing is due, exit"*.
That case must cost close to nothing, or people stop running it.

Concretely, the program has two paths, and they have opposite priorities:

| | fast path | slow path |
|---|---|---|
| what | read the SIML file → decide *is there anything to do?* | install the change, or run the command |
| priority | **lowest possible startup cost** | ergonomics and clarity |
| code | `src/main.c`, `src/config.c`, `src/state.c`, `src/check.c` | `src/install.c` |
| allowed to | do nothing it does not have to | be slow, fork shells, write files, use stdio |

Rules the fast path follows, and that changes to it must keep:

- **Static musl by default.** A static binary has no dynamic loader to run
  before `main()`. This is the single largest startup win and the reason
  `build.sh` looks for a musl toolchain first.
- **No allocator on normal cached launches.** Buffers are in BSS or on the
  stack (see the `HM_*` limits in `src/hacman.h`). Optional embedded-file and
  sandbox-directory storage is initialized only for entries actually used,
  leaving unused BSS pages unfaulted. On Linux, `openat2` can prove an absolute
  source path is canonical during the initial open. Relative paths and symlink
  aliases use the already-open `/proc/self/fd` link; `realpath` remains a
  fallback when procfs is unavailable. Static builds combine code and constants
  into one read/execute load segment, reducing mappings while keeping writable
  data and the stack non-executable.
- **One buffered pass per file.** The SIML input is read into one buffer and
  parsed straight out of it; the parser's line callback hands out slices.
  The cache record is one small file, never a table to scan. A complete record
  needs one `read`: its two terminated lines avoid an extra EOF probe, while
  incomplete reads still continue.
- **Almost no copies.** Config values — including install scripts and embedded
  file contents — point back into the input buffer. Only embedded-file names
  are copied, because parser key slices are transient. Block contents are not
  even de-indented until work is actually due.
- **No stdio on the fast path.** Output goes through a small `write(2)`-based
  formatter in `src/util.c`.
- **One environment scan.** `src/env.c` collects `HACMAN`, `HACMAN_DEBUG`,
  `HACMAN_CACHE`, and `HOME` together. Argument parsing, cache defaults, and
  `{{HOME}}` expansion reuse those borrowed values; other template variables
  still use ordinary environment lookup.
- **Nothing eager.** Nothing happens at all unless the schedule says the
  project is due: no request, no command, no fork. Nothing is written unless
  the work that followed actually succeeded. A quiet cached handoff uses
  `exec` itself to check the program, avoiding a separate `access` syscall.
  Failed handoffs still repair a missing or nonexecutable cached binary under
  the update lock; other execution errors do not rerun setup. Inspection and
  verbose modes retain their explicit executable check.
- **Sandboxed setup.** Commands and install scripts can read the host but may
  only create, modify, or remove files inside their cache-owned `.work`
  directory. Host-maintenance projects must explicitly opt out.

Measured on this repository (glibc-static build, project not due):

```
$ strace -c hacman ripgrep.siml    # 22 syscalls total, no network
                                   # (the same build linked dynamically: 37)
```

The scheduling decision is deliberately made *before* the network is touched,
so `hacman` invoked every minute with a `weekly` schedule does open two files,
compare two integers and exit.

---

## Building

```sh
./build.sh
```

The default build is **static musl**. `build.sh` picks the first musl compiler
it finds — `musl-gcc`, `musl-clang`, `*-linux-musl-gcc`, a musl-native system
compiler (Alpine), or `zig cc` — and configures Meson with `-Dstatic=true`.

On Debian/Ubuntu:

```sh
sudo apt install musl-tools meson ninja-build
./build.sh
file build/hacman   # ... statically linked ...
```

If no musl compiler is present, `build.sh` warns and falls back to the system
compiler (still statically linked). Set `HACMAN_GLIBC=0` to make that a hard
error instead.

| variable | default | meaning |
|---|---|---|
| `BUILD_DIR` | `./build` | build directory |
| `HACMAN_CC` | autodetected | compiler to use |
| `HACMAN_STATIC` | `1` | `0` builds a dynamically linked binary |
| `HACMAN_GLIBC` | `1` | `0` refuses to build without musl |
| `BUILDTYPE` | `release` | Meson buildtype |
| `DEBUG` | unset | trace the build script |

Meson directly, if you prefer:

```sh
CC=musl-gcc meson setup build --buildtype=release -Dstatic=true
meson compile -C build
meson test -C build
```

Meson options: `-Dstatic=`, `-Dcurl=` (HTTP client, default `curl`),
`-Dshell=` (install-script interpreter, default `/bin/sh`).

### Version and reproducible builds

The build is deterministic: the same source built with the same toolchain is
the same file byte for byte, whatever the checkout path, build directory, time
or Git history. Nothing from Git, the clock or the build machine is embedded.

The version names the program itself rather than a commit:

```
$ build/hacman --version
hacman 0.1.0+ca1874dad561cb2d
```

`0.1.0` is the `version:` in `meson.build`; the suffix is an XXH3 digest of the
binary. Meson links `hacman-unstamped` with an all-zero 8-byte slot in the
`.hm_selfhash` section, then `hm-stamp` (`tools/stamp.c`) hashes that file and
writes the digest into the slot to produce `build/hacman`. Anything that
changes the program, including the compiler, flags or Meson options, therefore
changes the version, and a rebuild that reproduces the same bytes keeps it. The
version is part of every [cache identity](#cache), so such a rebuild also keeps
the cache. A release bumps `version:` in `meson.build`; tagging alone changes
nothing.

To verify a binary, zero the slot and hash the file again:

```sh
build/hm-stamp --check build/hacman   # prints the digest, or fails
```

Tools that rewrite the binary afterwards (`strip`, packers, signing) keep the
stored version but make `--check` fail on the rewritten copy.

Tests need no network (see [Tests](#tests)):

```sh
./tests.py
```

Formatting maintained C and Python sources requires `clang-format` and Black:

```sh
./autoformat.sh
```

### Regression benchmark

```sh
./regression-benchmark.py
BENCHMARK_MIN_RATIO=1 BENCHMARK_MAX_RATIO=2.5 ./regression-benchmark.py
```

This builds a fresh release, static musl hacman and runs the small embedded C
project in [`examples/regression-benchmark.siml`](examples/regression-benchmark.siml).
It checks output, argument forwarding, static linking and cached execution,
then uses hyperfine with no shell to compare direct program launches against
cached launches through hacman. Compilation stays outside the timed runs.
Hyperfine and its children are pinned to the lowest-numbered CPU in the
process's allowed set to reduce scheduling variation. This requires OS support
for `sched_getaffinity` and `sched_setaffinity`; the script prints the chosen CPU.

Mean launch times must be strictly below **600 microseconds for direct** and
**1000 microseconds for hacman**, in addition to the ratio check. Reaching or
exceeding either limit fails the benchmark and retains the JSON report.

The script exits nonzero unless the ratio of mean hacman launch time to mean
direct launch time is within the inclusive range, defaulting to `[1, 3]`.
Set `BENCHMARK_MIN_RATIO` and `BENCHMARK_MAX_RATIO` for your machine's budget;
measure on an otherwise idle machine to reduce scheduling noise. The defaults
are 20 warmup runs and 1000 measured runs per command, configurable through
`BENCHMARK_WARMUP` and `BENCHMARK_RUNS`.

Dependencies are Bash, Python 3, hyperfine, Meson, Ninja, readelf and musl-gcc.
`HACMAN_CC` can select another musl compiler executable. Temporary build and
cache directories are removed on exit; measurements remain in
`build-regression-benchmark/results.json`, or the path in `BENCHMARK_JSON`,
including when the performance assertion fails. Run
`./regression-benchmark.py --help` to list the settings.

### Dependencies

- Build: a C99 compiler, Meson ≥ 0.60, Ninja.
- Tests: Python ≥ 3.8 and `curl`.
- Runtime: `curl` and `/bin/sh`. The SIML parser is vendored in
  `vendor/siml/` (see `vendor.py`); nothing else is linked in.
- Default sandbox: Linux with Landlock ABI ≥ 3 (Linux 5.19 or newer).

HTTP is delegated to `curl(1)` on purpose: statically linking a TLS stack (and
shipping a CA bundle with it) would dwarf the program, and one `fork`+`exec` is
noise next to a network round trip — a cost that is only paid when a project is
actually due.

---

## Usage

```
usage: hacman [OPTIONS] FILE [ARGS...]

  -c, --check-only    check only, never install or run (exit 10 if due)
  -n, --dry-run       report what would happen, change nothing
  -f, --force         ignore the schedule and act now
  -a, --adopt         record the current state without installing or running
  -p, --plan          print what FILE resolves to, as SIML, and exit
      --cache DIR     cache directory (default: ~/.cache/hacman)
  -t, --timeout SECS  per-request timeout (default: 15)
  -v, --verbose       report an unchanged or skipped project too
  -x, --trace         show and trace command and install shells
  -h, --help          show this help
  -V, --version       show the version

environment read by hacman:
  HACMAN              options prepended before command-line options
  HACMAN_CACHE        cache directory overridden by --cache
  HACMAN_DEBUG=0|1    report decisions and stream shell-traced setup live
  HACMAN_KEEP_TEMP=1  keep generated install and response files

environment exported to setup code:
  HACMAN_NAME         project name (commands and install scripts)
  HACMAN_WORKDIR      command working directory (its cache work directory)
  HACMAN_URL          checked URL
  HACMAN_CHECK        etag, hash or version
  HACMAN_VERSION      newly observed marker
  HACMAN_PREVIOUS     previous marker, empty on first sight
  HACMAN_RESPONSE     downloaded response path when available

build.sh environment:
  HACMAN_CC           compiler to use
  HACMAN_STATIC=0|1   disable or enable static linking
  HACMAN_GLIBC=0|1    disallow or allow fallback from musl to glibc
```

`FILE` is required; pass `-` to read the project from standard input:

```sh
generate-project | hacman -
```

For executable `.siml` files, `HACMAN` supplies options before the implicit
file argument added by the shebang:

```sh
HACMAN=--help ./examples/hello-c.siml
HACMAN='-x --timeout 30' ./examples/koca.siml --version
```

`HACMAN` accepts options only. Its value is split without starting a shell;
single quotes, double quotes, and backslashes may group or escape whitespace.
Real command-line options are applied afterwards and override scalar settings
such as `--cache` and `--timeout`. Arguments following the `.siml` path remain
arguments to `bin-path`, as usual.

`HACMAN_DEBUG=1` reports unchanged and skipped projects and sends setup output
to the terminal as it happens while tracing each shell command. It is
equivalent to enabling both `--verbose` and `--trace`. `HACMAN_DEBUG=0` is the
default behavior.

### Options before FILE, arguments after it

hacman's own options must come **before** `FILE`. The first non-option argument
is `FILE`, and everything after it belongs to the program named by `bin-path` —
including arguments that look like hacman's own:

```sh
hacman -v proj.siml --plan -v      # -v is hacman's; --plan -v go to the program
```

When a project names a `bin-path`, hacman:

- prints its own status output on **stderr**, prefixed with `hacman: `, so the
  program owns stdout;
- runs the program once the setup succeeds with its original stdout and stderr,
  and passes its exit status through;
- hands over even when the schedule said "not yet", which is the common case:
  the whole detour is two file reads and a comparison;
- does **not** hand over when the setup failed (exit 2), or under `--plan`,
  `--check-only`, `--dry-run` and `--adopt`, which report rather than act;
- exits 127 if the program cannot be started at all.

Passing arguments after `FILE` without a `bin-path` is an error, since there
would be nowhere to forward them.

Exit codes:

| code | meaning |
|---|---|
| `0` | nothing to do, or everything installed successfully |
| `1` | usage error or invalid configuration |
| `2` | a check, an install or a command failed |
| `10` | work is pending while `--check-only`/`--dry-run` |
| `127` | the `bin-path` program could not be started |
| *other* | whatever the `bin-path` program exited with |

Output is quiet by default: hacman reports its own status, but discards stdout
and stderr from a successful install script or command. If setup fails, its
complete captured output is replayed to stderr before the failure diagnostic;
displayed setup stderr is prefixed with `hacman: `. `-x` instead shows setup
output live and runs the shell with tracing enabled, prefixing its stderr.
`-v` also reports an unchanged or skipped project. With a `bin-path`, all of
hacman's own output moves to stderr, while the wrapped program's stdout and
stderr remain untouched.

---

## The project file

A SIML document that is one mapping — the project. It is either a **url**
project or a **command** project, depending on which of the two keys it has.

### url projects

Watch a URL, install when it changed:

```
name: ripgrep
url: https://api.github.com/repos/BurntSushi/ripgrep/releases/latest
check: version
version-prefix: "tag_name": "
version-suffix: "
schedule: daily
install: |
  set -eu
  echo "installing ripgrep $HACMAN_VERSION (was: ${HACMAN_PREVIOUS:-none})"
```

See [`examples/`](examples/) for one file per check scheme.

| key | required | default | meaning |
|---|---|---|---|
| `url` | yes | — | the URL to watch |
| `name` | no | the `url` | label used in output |
| `check` | no | `etag` | how "changed" is decided (below) |
| `schedule` | no | `daily` | when a check may happen (below) |
| `version-prefix` | for `check: version` | — | text immediately before the version |
| `version-suffix` | no | end of line | text immediately after the version |
| `install` | no | — | shell script to run when the URL changed |
| `bin-path` | no | — | program to exec afterwards (see [above](#options-before-file-arguments-after-it)) |
| `sandboxed` | no | `true` | confine install-script writes to the project's cache `.work` directory |
| `sandbox-write-dirs` | no | `[]` | additional absolute directory templates that a sandboxed install script may write below |

For a sandboxed URL project, a safe relative `bin-path` resolves inside its
cache work directory. This lets an install script keep a cached tool without
writing to `~/.local/bin` or another host directory.

The install script always runs in the project's `<cache>/url-….work`
directory, sandboxed or not, and that directory is emptied before every
install: an install only happens when the record is invalid (a new marker, no
matching record, or a missing `bin-path`), so nothing from an earlier or
interrupted install is carried over. The flip side is that the previous tool
is gone until the next install succeeds.

### command projects

Run a command, but no more often than the schedule allows:

```
name: nixcfg-pull
command: test -d "$TMPDIR/nixcfg/.git" || cp -a "$HOME/workspace/nixcfg" "$TMPDIR/nixcfg"; git -C "$TMPDIR/nixcfg" pull --ff-only && XDG_CACHE_HOME="$TMPDIR/xdg-cache" XDG_STATE_HOME="$TMPDIR/xdg-state" nix flake update --flake "$TMPDIR/nixcfg"
schedule: 12h
```

| key | required | default | meaning |
|---|---|---|---|
| `command` | yes | — | passed to `sh -c`, run inside the project's cache `.work` directory |
| `files` | no | — | files to materialize in a cache work directory before running the command |
| `name` | no | the `command` | label used in output |
| `schedule` | no | `daily` | when the command may run (below) |
| `bin-path` | no | — | program to exec afterwards; a safe relative path resolves inside the sandbox work directory (see [above](#options-before-file-arguments-after-it)) |
| `sandboxed` | no | `true` | confine command writes to the project's cache `.work` directory |
| `sandbox-write-dirs` | no | `[]` | additional absolute directory templates that a sandboxed command may write below |

Every command runs in a project-specific `<cache>/cmd-….work` directory that
hacman owns, whether sandboxed or not; a `.siml` file cannot choose another
working directory. The directory is exported as `HACMAN_WORKDIR`, and a command
that needs host files reads them by absolute path, as `nixcfg-pull` does above.
The directory persists across scheduled reruns, so a command such as an
updater can reuse what it downloaded before. hacman empties it only when the
project has no matching record: on its first run, after its inputs changed, or
after the record was removed.

`bin-path` supports `{{VAR}}` templating against the environment:
`{{HOME}}/.cargo/bin/cargo`. An unset variable is an error rather than an empty
string, so a typo cannot silently point the shim at `/.cargo/bin/cargo`. A
sandboxed project's `bin-path` may instead be a safe relative path beneath the
generated work directory, such as `build/tool`; an unsandboxed project's
`bin-path` must be absolute.

There is no `install` and no `check`: the command *is* the work, and its exit
status is the whole verdict. hacman records the run **only if the command
succeeded**, so a failure is retried on the next invocation instead of waiting
out the schedule.

The record and its work directory belong to the project file's exact
contents — see [Cache](#cache) — so editing the file, even a comment, starts
the project over, while a byte-identical copy of it shares the record.

#### Embedded files

A command project can carry a small, self-contained source tree directly in
its `.siml` file:

```
name: embedded-hello
command: meson setup build && meson compile -C build
bin-path: build/hello
files:
  main.c: |
    #include <stdio.h>
    int main(void)
    {
        puts("hello from hacman");
        return 0;
    }
  meson.build: |
    project('embedded-hello', 'c', default_options: ['c_std=c99'])
    executable('hello', 'main.c')
```

See [`examples/hello-c.siml`](examples/hello-c.siml) for the runnable version.
Running `./examples/hello-c.siml` materializes `main.c` and `meson.build`, builds
them with Meson, and runs the resulting program. Arguments are forwarded to
the built program in the same way as for any other `bin-path`. Later launches
exec the cached program immediately. The command runs again only when its text,
an embedded path, or embedded contents change, when the cached executable is
missing, or when `--force` is used.

`files` is a mapping from a filename to a literal block scalar. Filenames use
SIML's mapping-key syntax (`[a-zA-Z_][a-zA-Z0-9_.-]*`); values must use `|`, so
their bytes and line breaks are unambiguous. hacman writes every embedded file
into the project's `<cache>/cmd-….work` directory, then runs the command there.

Materialization happens only immediately before a real command execution:
cache hits, `--plan`, `--check-only`, `--dry-run`, and `--adopt` do not touch
the files. Embedded projects are input-fingerprinted rather than scheduled by
default. Setting `schedule` explicitly also reruns their command at that
interval, which is useful for cached download shims that need periodic update
checks while retaining their executable in the generated work directory.
The canonical absolute `.siml` path selects a stable cache record and work
directory. The identity stored in that record covers the whole file, embedded
files included; when the file is edited, hacman clears and reuses the work
directory before the next normal build. Inputs read from standard input have
no stable path and retain a content-addressed work directory.

Unknown keys, a missing `url` and anything that would introduce a second
project are hard errors, reported with a line number. Use `hacman --plan` to
see how a file was understood.

### `--plan`: what a file resolves to

`hacman --plan FILE` prints the project with every default filled in, together
with the steps that would follow from it — including which cache record decides
whether the work is due — and stops there. It reads neither the cache, nor the
clock, nor the network, so the same input always plans to the same bytes:

```
$ hacman --plan examples/nixcfg-pull.siml
name: nixcfg-pull
sandboxed: true
sandbox-write-dir: /home/you/.cache/hacman/cmd-c6cdf236f31c2010.work
command: test -d "$TMPDIR/nixcfg/.git" || cp -a "$HOME/workspace/nixcfg" "$TMPDIR/nixcfg"; git -C "$TMPDIR/nixcfg" pull --ff-only && XDG_CACHE_HOME="$TMPDIR/xdg-cache" XDG_STATE_HOME="$TMPDIR/xdg-state" nix flake update --flake "$TMPDIR/nixcfg"
workdir: /home/you/.cache/hacman/cmd-c6cdf236f31c2010.work
run: /bin/sh -c <command>, in workdir
record-when: the command exits 0
schedule: 12h
check-when: 43200 seconds after the last run
cache-identity: siml:fb69132bdb7443ab hacman:f2d8969
cache-file: /home/you/.cache/hacman/cmd-c6cdf236f31c2010
```

The plan is itself SIML, and its `install:` block is exactly what the installer
writes into the script it runs. That determinism is what the golden-file tests
in [`tests/`](tests/) assert against — they run with a fixed `HOME`, since
plans resolve `{{VAR}}` templates and name cache files.

### Schedule schemes — *when* to look

The schedule is evaluated locally against the last run recorded in the cache.
A project that is not due costs no network traffic and no subprocess.

| `schedule:` | behaviour |
|---|---|
| `always` | act on every invocation |
| `hourly`, `daily`, `weekly`, `monthly` | act when that much time has passed |
| `<n>s`, `<n>m`, `<n>h`, `<n>d`, `<n>w` | e.g. `30m`, `6h`, `10d` |
| `never` | never act unless `--force` is given |

The default is `daily`, i.e. a full 24h: nothing hacman watches is worth asking
about more often than that unless the file says so explicitly.
Command projects with embedded `files` are the exception: their inputs provide
the invalidation signal, so without an explicit `schedule` they build once per
identity. An explicit schedule adds time-based command runs without
giving up input-based invalidation.

### Check schemes — *how* change is decided

| `check:` | request | compares |
|---|---|---|
| `etag` | `HEAD` | the `ETag` header, falling back to `Last-Modified` |
| `hash` | `GET` | a hash of the whole response body |
| `version` | `GET` | the text between `version-prefix` and `version-suffix` |

`etag` is the cheapest — no body is transferred — but needs a server that sends
validators. `version` is what you want for release feeds: it ignores changes
that are not a new version, and hands the extracted version to the install
script.

Change is decided against what was recorded on the previous successful run:

- A project hacman has never seen counts as **changed**, so a fresh checkout
  installs on its first run. Use `--adopt` to record the current state instead
  ("this is already installed"); on a command project, `--adopt` marks it as
  just run without running it.
- A **failed check** records nothing, so the next run retries instead of
  waiting out the schedule.
- A **failed install** — or a **failed command** — records nothing either: the
  next run tries again, whatever the schedule says. A failed install script is
  kept and its path is printed.

### The install script

The script is run by `/bin/sh -e` with these variables:

| variable | value |
|---|---|
| `HACMAN_NAME` | the project name |
| `HACMAN_URL` | the URL that was checked |
| `HACMAN_CHECK` | `etag`, `hash` or `version` |
| `HACMAN_VERSION` | the newly observed marker (the version, for `check: version`) |
| `HACMAN_PREVIOUS` | the previously recorded marker, empty on first sight |
| `HACMAN_RESPONSE` | path to the downloaded body (`hash`/`version` checks only) |

Its output is captured and discarded when the script succeeds, then replayed
to stderr when it fails. Pass `-x` to stream it live and invoke the script as
`/bin/sh -e -x`. `HACMAN_KEEP_TEMP=1` keeps the generated script and response
file for debugging.

A setup never outlives the hacman that started it. A `SIGTERM`, `SIGINT`,
`SIGHUP` or `SIGQUIT` sent to hacman while it waits for setup is passed on to
the setup shell; hacman then cleans up its temporary files, records nothing,
and exits from the same signal, so the next run repeats the setup. Should
hacman be killed outright, the setup shell receives `SIGTERM` as well.

This is where hacman deliberately stops being clever: an update is whatever
`sh` can do, written inline next to the URL it belongs to.

### Sandbox

Commands and install scripts are sandboxed by default. On Linux, hacman uses
Landlock to grant them read and execute access across the host filesystem and
full filesystem access only below their project-specific
`<cache>/<key>.work` directory. `/dev/null` is the only writable host-file
exception. Network access is unchanged. For sandboxed install scripts, that
writable directory is also the current directory; `TMPDIR` points to it for
both scripts and commands.

Setup code may run other hacman-managed tools, e.g. a build that calls a
`meson` shim. The whole cache directory therefore stays writable as well, and
`HACMAN_CACHE` is exported to sandboxed setup code so such a nested hacman
uses the very cache the sandbox allows, even when the outer run was started
with `--cache`. A setup that re-enters its own project while that project's
setup is still running, directly or through other tools, fails right away
instead of waiting forever on its own update lock.

Projects can grant setup code write access to a small set of additional host
directories without disabling the rest of the sandbox. Each directory must
already exist, expand to an absolute path, and may use the same `{{VAR}}`
environment templates as `bin-path`:

```siml
sandbox-write-dirs:
  - {{HOME}}/go
  - {{HOME}}/.cache/go-build
```

The configured directory and everything below it becomes writable. Other host
paths remain read-only.

The program named by `bin-path` is handed control after setup and is not
sandboxed. This distinction lets a cached compiler, package client, or other
tool operate normally in the caller's directory while keeping its updater
confined.

Projects that intentionally modify the host can opt out explicitly:

```siml
name: nixcfg-pull
sandboxed: false
command: git -C ~/workspace/nixcfg pull --ff-only && nix flake update --flake ~/workspace/nixcfg
```

Sandbox setup fails closed: hacman does not run project-controlled code if its
Landlock policy cannot be established. The running Linux kernel must provide
Landlock ABI 3 or newer; use `sandboxed: false` only when the project genuinely
needs external writes or must run without Landlock. Landlock mediates file
content and namespace changes; metadata-only operations that Landlock does not
yet cover remain governed by normal Unix permissions.

### Cache

`hacman` keeps every persistent artifact under `~/.cache/hacman/`: cache
records live directly below it and each project gets a `<key>.work` directory
for sandboxed writes. Embedded source trees, build directories, and binaries
also live in that directory. `--cache DIR` and `$HACMAN_CACHE` can explicitly
relocate the entire tree. It contains one tiny record per watched thing:

```
$ cat ~/.cache/hacman/cmd-e1afb450457ef858
#hacman-cache 1 siml:fb69132bdb7443ab hacman:0.1.0+ca1874dad561cb2d
1786527549	1786527549
```

The first line is the record's **identity**, the second is last check, last
change and the recorded marker. Ordinary record names hash that identity;
embedded projects instead hash the canonical input path so edits reuse one
record and work directory. A record whose identity does not match is ignored
rather than trusted, so a changed project rebuilds instead of inheriting the
prior successful state.

The identity is a digest of every byte of the project file plus the version of
the hacman running it (`src/identity.c`). Nothing is parsed or left out:
comments, `name`, `install` and every other key count, so any edit starts the
project's setup over and no key can be forgotten when one is added. The version
includes a digest of the hacman binary (see
[Version and reproducible builds](#version-and-reproducible-builds)), so a build
that differs in any byte never inherits setup state from another one, even for
an unchanged file, while a rebuild that reproduces the same binary keeps it. Things outside the file, such as the environment, the network
or files a command reads, are what `schedule` and `check` are for.

When work is due, hacman takes a per-project advisory lock beside the cache
record before checking or updating. A concurrent invocation blocks in the
kernel, reloads the record after acquiring the lock, and skips duplicate work
when the first invocation succeeded. The ordinary not-due path takes no lock,
so serialization adds no syscall to cached shim launches.

What shares a record follows from that: a byte-identical copy of a project
file (or the same bytes on standard input) shares one last-run and one work
directory, while files that differ in any byte, even only in `name`, never do.
Embedded projects are the exception in the other direction: each input path
owns one record and work directory, whatever its contents.

One file per record rather than one shared table means the fast path reads
exactly the bytes it needs, and several hacman runs started in parallel — one
per project file — cannot lose each other's updates. Records are written
atomically (write + `rename`), and only after the work succeeded, so an
interrupted or failed run leaves nothing behind.

### SIML gotchas

SIML is strict, which is what makes it fast to parse. A few rules surprise
people writing project files by hand:

- A project may start with `#!/usr/bin/env hacman`, allowing an executable
  `.siml` file to invoke hacman directly. The shebang is stream-level metadata
  and does not otherwise affect the project or its plan.

- **No blank lines** outside of block scalars, and no trailing whitespace. Use
  comment lines to separate entries.
- **Values cannot start or end with a space.** For `version-prefix` /
  `version-suffix` this means anchoring on non-space text; extracted versions
  are whitespace-trimmed, so `version-prefix: go` matches `go 1.26.5`.

Comments (`# text`, with the space) are allowed, `#` alone is not.

---

## Layout

```
build.sh                  static-musl build wrapper
meson.build               build definition
tests.py                  test suite (also `meson test`)
tests/*.siml, *.gold      golden-file fixtures for `hacman --plan`
examples/*.siml           one annotated project per file
src/main.c                fast path: args, schedule decision, orchestration
src/env.c                 one scan for borrowed startup environment settings
src/config.c              SIML -> one hm_project, zero-copy
src/plan.c                --plan serialisation
src/identity.c            the project identity: file digest + hacman version
src/version.c             the version: project version + the binary's own digest
src/selfhash.h            the self-digest slot shared with tools/stamp.c
src/cache.c               cache records: key, load, atomic save
src/check.c               HTTP via curl + the three check schemes
src/install.c             slow path: embedded files, install scripts, commands, exec
src/sandbox.c             Landlock filesystem confinement for setup code
src/util.c                write(2)-based output, string and file helpers
tools/stamp.c             hm-stamp: writes the self digest into the linked binary
vendor/siml/              vendored SIML parser (see vendor.py)
```

### Tests

```sh
./tests.py              # golden plans + the offline pipeline
GOLD=update ./tests.py  # rewrite tests/*.gold after an intended change
```

Every `tests/<name>.siml` is resolved with `hacman --plan` and diffed against
`tests/<name>.gold`; a fixture named `xfail_*.siml` must be rejected instead,
and its `.gold` holds the expected error. The behavioural half of the suite
serves fixtures over `file://` URLs, so it needs no network.
