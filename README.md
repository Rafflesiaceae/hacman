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
url: https://go.dev/VERSION?m=text      command: rustup update stable
check: hash                             workdir: {{HOME}}
schedule: daily                         bin-path: {{HOME}}/.cargo/bin/cargo
install: |                              schedule: weekly
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
- **No dynamic allocation.** Every buffer is in BSS or on the stack (see the
  `HM_*` limits in `src/hacman.h`). There is no allocator to warm up, and no
  out-of-memory path to get wrong. BSS pages are faulted in lazily, so
  generous limits are free.
- **One read per file.** The SIML input is slurped with a single
  `open`/`read`/`close` and parsed straight out of that buffer; the parser's
  line callback hands out slices of it. The cache record is one small file, so
  reading it is one more `open`/`read`/`close` — never a table to scan.
- **Almost no copies.** Config values — including install scripts and embedded
  file contents — point back into the input buffer. Only embedded-file names
  are copied, because parser key slices are transient. Block contents are not
  even de-indented until work is actually due.
- **No stdio on the fast path.** Output goes through a small `write(2)`-based
  formatter in `src/util.c`.
- **Nothing eager.** Nothing happens at all unless the schedule says the
  project is due: no request, no command, no fork. Nothing is written unless
  the work that followed actually succeeded.

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

Tests need no network (see [Tests](#tests)):

```sh
./tests.py
```

### Dependencies

- Build: a C99 compiler, Meson ≥ 0.60, Ninja.
- Tests: Python ≥ 3.8 and `curl`.
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

  -c, --check-only    check only, never install or run (exit 10 if due)
  -n, --dry-run       report what would happen, change nothing
  -f, --force         ignore the schedule and act now
  -a, --adopt         record the current state without installing or running
  -p, --plan          print what FILE resolves to, as SIML, and exit
      --cache DIR     cache directory (default: ~/.cache/hacman)
  -t, --timeout SECS  per-request timeout (default: 15)
  -v, --verbose       report an unchanged or skipped project too
  -h, --help          show this help
  -V, --version       show the version
```

`FILE` is required; pass `-` to read the project from standard input:

```sh
generate-project | hacman -
```

For executable `.siml` files, `HACMAN` supplies options before the implicit
file argument added by the shebang:

```sh
HACMAN=--help ./examples/hello-c.siml
HACMAN='--verbose --timeout 30' ./examples/koca.siml --version
```

`HACMAN` accepts options only. Its value is split without starting a shell;
single quotes, double quotes, and backslashes may group or escape whitespace.
Real command-line options are applied afterwards and override scalar settings
such as `--cache` and `--timeout`. Arguments following the `.siml` path remain
arguments to `bin-path`, as usual.

### Options before FILE, arguments after it

hacman's own options must come **before** `FILE`. The first non-option argument
is `FILE`, and everything after it belongs to the program named by `bin-path` —
including arguments that look like hacman's own:

```sh
hacman -v proj.siml --plan -v      # -v is hacman's; --plan -v go to the program
```

When a project names a `bin-path`, hacman:

- prints its own status output on **stderr**, so the program owns stdout;
- `exec()`s the program once the setup succeeded — the program inherits the
  terminal, and its exit status becomes hacman's;
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

Output is quiet by default: one line when the project changed, plus whatever
the install script or command prints. `-v` also reports an unchanged or skipped
project. With a `bin-path`, all of hacman's own output moves to stderr.

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

### command projects

Run a command, but no more often than the schedule allows:

```
name: nixcfg-pull
command: git pull --ff-only && nix flake update
workdir: {{HOME}}/workspace/nixcfg
schedule: 12h
```

| key | required | default | meaning |
|---|---|---|---|
| `command` | yes | — | passed to `sh -c`, run inside `workdir` |
| `workdir` | no | `{{HOME}}` | working directory; must expand to an absolute path; incompatible with `files` |
| `files` | no | — | files to materialize in a cache work directory before running the command |
| `name` | no | the `command` | label used in output |
| `schedule` | no | `daily` | when the command may run (below) |
| `bin-path` | no | — | program to exec afterwards; may be relative to the generated work directory when used with `files` (see [above](#options-before-file-arguments-after-it)) |

`workdir` and `bin-path` support `{{VAR}}` templating against the environment:
`{{HOME}}/workspace/nixcfg`. An unset variable is an error rather than an empty
string, so a typo cannot silently point the command at `/workspace/nixcfg`.
Both must normally expand to an absolute path. For a command project with
embedded `files`, `bin-path` may instead be a safe relative path beneath the
generated work directory, such as `build/tool`.

There is no `install` and no `check`: the command *is* the work, and its exit
status is the whole verdict. hacman records the run **only if the command
succeeded**, so a failure is retried on the next invocation instead of waiting
out the schedule.

The record is keyed by the pair (command, working directory) — see
[Cache](#cache) — so the same command in the same directory shares one
last-run across every file that mentions it, whatever those files are called.

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
their bytes and line breaks are unambiguous. A project with `files` cannot set
`workdir`: hacman derives a project-specific `<cache>/cmd-….work` directory,
writes every embedded file there, then runs the command there. The directory
is also exported as `HACMAN_WORKDIR`.

Materialization happens only immediately before a real command execution:
cache hits, `--plan`, `--check-only`, `--dry-run`, and `--adopt` do not touch
the files. Embedded projects are input-fingerprinted rather than scheduled by
default. Setting `schedule` explicitly also reruns their command at that
interval, which is useful for cached download shims that need periodic update
checks while retaining their executable in the generated work directory.
The canonical absolute `.siml` path selects a stable cache record and work
directory. That source path, the embedded paths and contents, and the command
form the identity stored in that record; when the inputs change, hacman clears
and reuses the work directory before the next normal build. Inputs read from
standard input have no stable path and retain a content-addressed work
directory.

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
command: git pull --ff-only && nix flake update
workdir: /home/you/workspace/nixcfg
run: /bin/sh -c <command>, in workdir
record-when: the command exits 0
schedule: 12h
check-when: 43200 seconds after the last run
cache-identity: cmd /home/you/workspace/nixcfg $ git pull --ff-only && nix flake update
cache-file: /home/you/.cache/hacman/cmd-e1afb450457ef858
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
content identity. An explicit schedule adds time-based command runs without
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

Its output goes straight to the terminal. `HACMAN_KEEP_TEMP=1` keeps the
generated script and response file for debugging.

This is where hacman deliberately stops being clever: an update is whatever
`sh` can do, written inline next to the URL it belongs to.

### Cache

`hacman` keeps every persistent artifact under `~/.cache/hacman/`: cache
records live directly below it and embedded source trees, build directories,
and binaries live in `cmd-….work` children. `--cache DIR` and `$HACMAN_CACHE`
can explicitly relocate the entire tree. It contains one tiny record per
watched thing:

```
$ cat ~/.cache/hacman/cmd-e1afb450457ef858
#hacman-cache 1 cmd /home/you/workspace/nixcfg $ git pull --ff-only
1786527549	1786527549
```

The first line is the record's **identity**, the second is last check, last
change and the recorded marker. Ordinary record names hash that identity;
embedded projects instead hash the canonical input path so edits reuse one
record and work directory. A record whose identity does not match is ignored
rather than trusted, so changed embedded inputs rebuild instead of inheriting
the prior successful state.

When work is due, hacman takes a per-project advisory lock beside the cache
record before checking or updating. A concurrent invocation blocks in the
kernel, reloads the record after acquiring the lock, and skips duplicate work
when the first invocation succeeded. The ordinary not-due path takes no lock,
so serialization adds no syscall to cached shim launches.

What the identity is made of decides what shares a record:

| project | cache address and identity | consequence |
|---|---|---|
| command | command and working directory | the same command in the same directory shares one last-run across every file that names it |
| command with `files` | canonical input path; identity also includes that path, the command and a digest of embedded paths and contents | each input owns one work directory; an edit clears and rebuilds it |
| url | URL, check scheme and version anchors | two files watching the same URL the same way share one history |

`name` is deliberately *not* part of it: renaming a project keeps its history,
and two people naming the same thing differently still agree on it.

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
src/config.c              SIML -> one hm_project, zero-copy
src/plan.c                --plan serialisation
src/cache.c               cache records: identity, load, atomic save
src/check.c               HTTP via curl + the three check schemes
src/install.c             slow path: embedded files, install scripts, commands, exec
src/util.c                write(2)-based output, string and file helpers
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
