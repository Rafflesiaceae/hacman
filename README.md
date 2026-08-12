# hacman

`hacman` watches URLs and installs what changed.

It reads a list of projects in [SIML](vendor/siml/SPEC.rst) format — from a file,
or from standard input when the file argument is `-` — asks (per project, on a
schedule) whether the URL changed, and runs that project's install script when
it did.

```sh
hacman ~/.config/hacman/projects.siml
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
| what | read the SIML file → decide *did it change?* | install/update the changed project |
| priority | **lowest possible startup cost** | ergonomics and clarity |
| code | `src/main.c`, `src/config.c`, `src/state.c`, `src/check.c` | `src/install.c` |
| allowed to | do nothing it does not have to | be slow, fork shells, write files, use stdio |

Rules the fast path follows, and that changes to it must keep:

- **Static musl by default.** A static binary has no dynamic loader to run
  before `main()`. This is the single largest startup win and the reason
  `build.sh` looks for a musl toolchain first.
- **No dynamic allocation.** Every buffer is in BSS or on the stack (see the
  `HM_*` limits in `src/hacman.h`). There is no allocator to warm up, and no
  out-of-memory path to get wrong. BSS pages are faulted in lazily, so
  generous limits are free.
- **One read per file.** The SIML input is slurped with a single
  `open`/`read`/`close` and parsed straight out of that buffer; the parser's
  line callback hands out slices of it. Same for the state file.
- **No copies.** Every config value — including the `install:` script — is an
  `hm_str` pointing back into the input buffer. The install script is not even
  de-indented until an install actually happens.
- **No stdio on the fast path.** Output goes through a small `write(2)`-based
  formatter in `src/util.c`.
- **Nothing eager.** No network request happens unless a project's schedule
  says it is due; no state file is written unless something actually changed;
  no process is forked unless there is something to fetch.

Measured on this repository (glibc-static build, 4 projects, none due):

```
$ strace -c hacman projects.siml    # 22 syscalls total, no network
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

Tests are offline — they serve fixtures over `file://` URLs:

```sh
./tests.sh
```

### Dependencies

- Build: a C99 compiler, Meson ≥ 0.60, Ninja.
- Runtime: `curl` and `/bin/sh`. The SIML parser is vendored in
  `vendor/siml/` (see `vendor.py`); nothing else is linked in.

HTTP is delegated to `curl(1)` on purpose: statically linking a TLS stack (and
shipping a CA bundle with it) would dwarf the program, and one `fork`+`exec` is
noise next to a network round trip — a cost that is only paid when a project is
actually due.

---

## Usage

```
usage: hacman [OPTIONS] FILE

  -c, --check-only    check only, never install (exit 10 if changes found)
  -n, --dry-run       report what would be installed, change nothing
  -f, --force         ignore schedules and check every project now
  -a, --adopt         record the current remote state without installing
  -l, --list          print the parsed project list and exit
  -o, --only NAME     only act on NAME (repeatable)
  -s, --state PATH    state file (default: $XDG_STATE_HOME/hacman/state.tsv)
  -t, --timeout SECS  per-request timeout (default: 15)
  -v, --verbose       report unchanged and skipped projects too
  -h, --help          show this help
  -V, --version       show the version
```

`FILE` is required; pass `-` to read the project list from standard input, so
`hacman` still composes:

```sh
cat a.siml b.siml | hacman -
```

Exit codes:

| code | meaning |
|---|---|
| `0` | nothing to do, or everything installed successfully |
| `1` | usage error or invalid configuration |
| `2` | a check or an install failed |
| `10` | changes found while `--check-only`/`--dry-run` |

Output is quiet by default: one line per changed project, plus whatever the
install script prints. `-v` also reports unchanged and skipped projects.

---

## The project list

A SIML document containing one mapping per project, either as a sequence:

```
- name: ripgrep
  url: https://api.github.com/repos/BurntSushi/ripgrep/releases/latest
  check: version
  version-prefix: "tag_name": "
  version-suffix: "
  schedule: daily
  install: |
    set -eu
    echo "installing ripgrep $HACMAN_VERSION (was: ${HACMAN_PREVIOUS:-none})"
```

…or as a single mapping, or as several `---`-separated documents. See
[`examples/projects.siml`](examples/projects.siml).

| key | required | default | meaning |
|---|---|---|---|
| `url` | yes | — | the URL to watch |
| `name` | no | the `url` | identifier used in output and in the state file |
| `check` | no | `etag` | how "changed" is decided (below) |
| `schedule` | no | `daily` | when a check may happen (below) |
| `version-prefix` | for `check: version` | — | text immediately before the version |
| `version-suffix` | no | end of line | text immediately after the version |
| `install` | no | — | shell script to run when the URL changed |

Unknown keys, missing `url`s and duplicate names are hard errors, reported with
a line number. Use `hacman --list` to see how a file was understood.

### Schedule schemes — *when* to look

The schedule is evaluated locally against the last check time in the state
file. A project that is not due costs no network traffic and no subprocess.

| `schedule:` | behaviour |
|---|---|
| `always` | check on every run |
| `hourly`, `daily`, `weekly`, `monthly` | check when that much time has passed |
| `<n>s`, `<n>m`, `<n>h`, `<n>d`, `<n>w` | e.g. `30m`, `6h`, `10d` |
| `never` | never check unless `--force` is given |

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
  installs everything on its first run. Use `--adopt` to record the current
  state instead ("this is already installed").
- A **failed check** does not update the recorded check time, so the next run
  retries instead of waiting out the schedule.
- A **failed install** records neither the new marker nor the check time: the
  next run tries that project again, whatever its schedule says. The generated
  script is kept and its path is printed.

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

Its output goes straight to the terminal. `HACMAN_KEEP_TEMP=1` keeps the
generated script and response file for debugging.

This is where hacman deliberately stops being clever: an update is whatever
`sh` can do, written inline next to the URL it belongs to.

### State

`hacman` records what it last saw in a tab-separated table, by default
`$XDG_STATE_HOME/hacman/state.tsv` (falling back to
`~/.local/state/hacman/state.tsv`); `--state` and `$HACMAN_STATE` override it.

```
#hacman-state 1
ripgrep	1786527549	1786527549	14.1.1
```

The columns are name, last check time, last change time and the recorded
marker. It is written atomically (write + `rename`), and only when something
actually changed — a run where every project is skipped writes nothing.

### SIML gotchas

SIML is strict, which is what makes it fast to parse. Two rules surprise people
writing project lists by hand:

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
tests.sh                  offline test suite (also `meson test`)
examples/projects.siml    annotated project list
src/main.c                fast path: args, schedule decision, orchestration
src/config.c              SIML -> hm_project[], zero-copy
src/state.c               state table load/save
src/check.c               HTTP via curl + the three check schemes
src/install.c             slow path: script materialisation and execution
src/util.c                write(2)-based output, string and file helpers
vendor/siml/              vendored SIML parser (see vendor.py)
```
