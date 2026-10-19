# Proposal: automatic, self-maintaining cache cleanup

Status: proposal, nothing implemented yet.

## 1. Problem

hacman never removes anything from its cache directory. On the author's
machine `~/.cache/hacman` held 63 `.work` directories using 434G, and 47
records from 18 different hacman versions plus 16 records old enough to
predate the version suffix. A single entry, the Vault example's
`url-e0805b26666edd4d.work`, used 252G on its own.

Garbage comes from three independent sources:

1. **Interrupted setups.** A setup killed partway (for example by a caller
   with a short timeout) leaves whatever it had written behind. Install
   scripts cannot reliably clean up after themselves: `/bin/sh` is dash,
   which skips `EXIT` traps when killed by a signal, and nothing runs at all
   on `SIGKILL`. The Vault entry accumulated 637 half-extracted staging
   directories this way, one per minute.
   *Already fixed* by ae63b05: a url project's work directory is emptied
   before every install, and a command project's whenever its record does
   not match.

2. **Identity-keyed slots.** Records and work directories of url and plain
   command projects are named after a hash of their identity, and the
   identity contains the hacman version, the URL, the check scheme, the
   version anchors or the command text. Any change to one of these makes
   hacman use a brand-new slot and never look at the old one again. Every
   rebuild of `build/hacman` therefore orphans every url and plain command
   work directory, which is where most of the 434G come from.

3. **Projects that no longer exist.** A deleted or moved `.siml` file leaves
   its slot behind forever.

Small leftovers exist as well: `<record>.tmp.<pid>` files from a run killed
between write and `rename()`, and `hacman-install-*` / `hacman-response-*`
files in `$TMPDIR` from a `SIGKILL`ed unsandboxed setup.

## 2. Goals and invariants

- **Bounded by construction.** The cache holds at most one slot per `.siml`
  file that currently exists, plus a time-limited set of slots for inputs
  that have no path (standard input). Nothing else survives a cleanup pass.
- **Never delete what is in use.** A slot whose `.siml` file exists is never
  removed by the cleanup, however long it goes unused. Only the existing
  wipe-on-invalidation empties such a slot, and only right before its own
  setup runs again.
- **Crash-safe at every step.** Killing hacman or the cleanup at any point,
  including `SIGKILL`, leaves a state that the next run or the next cleanup
  pass converges from. No step depends on a later step being reached.
- **No new cost on the hot path beyond one `stat()`.** Cached shim launches
  stay lock-free and write-free.
- **Fully automatic.** No timer, cron job or daemon. Normal use drives it.
- **Fail closed on doubt.** Anything the cleanup cannot positively classify
  as garbage is kept.

Non-goals: a size quota or LRU eviction, cleaning other users' caches,
coordinating several hacman generations that share one cache (see 7.1).

## 3. Design

### 3.1 One slot per input file

Every project read from a file gets its slot named after the canonical path
of that file, exactly like embedded `files:` projects already do today:

    <cache>/p-<xxh3-64(canonical source path)>        record
    <cache>/p-<...>.work                              work directory
    <cache>/p-<...>.lock                              update lock

- The name no longer depends on the project kind (`url-` / `cmd-`), so
  turning a url project into a command project in the same file reuses the
  slot instead of orphaning it.
- The identity stays inside the record, as today. A hacman upgrade or an edit
  makes the stored identity mismatch, the record counts as invalid, and the
  existing wipe-on-invalidation empties the same work directory on the next
  run. **No orphan is ever created by edits or upgrades.**
- The canonical path is already resolved for embedded projects (openat2 or
  `/proc/self/fd`), so extending it to every project costs at most one
  `readlink()` on the fast path.

Inputs without a stable path (standard input, `/dev/fd/N` from process
substitution, anything that is not a regular file) keep a content-addressed
slot:

    <cache>/c-<xxh3-64(identity)>[.work|.lock]

These are the only slots that expire by age (see 3.7).

Consequence: two `.siml` files that watch the same URL or run the same
command no longer share one record and one work directory. Each file installs
separately. The README documents the sharing as a feature and has to be
updated. In practice each file has its own install script and `bin-path`, so
sharing a work directory between them was fragile anyway.

### 3.2 Record format 2

    #hacman-cache 2 <identity>
    <last_check>\t<last_change>\t<mark>
    source\t<canonical source path or "-">
    missing-since\t<epoch>                    (optional, written by cleanup)

- Line 3 lets the cleanup find the project's file without parsing anything
  else. `-` marks a content-addressed slot.
- `missing-since` is written only by the cleanup, under the slot's lock, and
  dropped again by any later save or by a cleanup pass that sees the source
  present. Readers ignore lines they do not know.
- The fast path still stops reading after line 2 in the common case: the
  identity check and the schedule decision only need lines 1 and 2.
- The format number protects other generations: a cleanup only ever deletes
  records whose format number is lower than or equal to its own, and treats
  higher numbers as foreign and untouchable. This keeps a future format 3
  safe from today's binary.

### 3.3 Deletable lock files

Today `.lock` files are permanent on purpose ("keeping the inode stable
avoids the unlink/recreate race"). The cleanup has to remove them too, or a
deleted project leaves an empty file behind forever. The standard fix makes
deletion safe:

1. `open(lock, O_CREAT)` and take the `fcntl` write lock, as today.
2. After acquiring it, compare `fstat(fd)` with `stat(lock path)`. If the
   path is gone or names a different inode, someone deleted the file while
   this process was waiting: close and start over at step 1.
3. A lock file may only be unlinked by a process that currently holds the
   lock on it, and only as the last step before closing it.

A waiter on the old inode wakes up after the deleter closes, sees the
mismatch and retries on the new file, so two processes can never both
believe they hold the lock. The re-entrancy check (`held_by_ancestor`) is
unaffected.

`fcntl` locks are owned by a process, are not inherited across `fork()` and
are released when the process closes *any* descriptor of the file. The
cleanup therefore opens each lock file exactly once, takes every lock in the
process that does the deleting, and never holds a slot's lock while also
reading that slot's lock file through another descriptor.

### 3.4 Delete by renaming to trash first

Removing a large tree takes time, and nobody should wait for it while
holding a lock or before their tool starts. Both the existing
wipe-on-invalidation and the cleanup therefore work in two phases:

1. Under the slot's lock: `rename(<key>.work, <key>.trash-<random>)`. This is
   atomic and instant on the same filesystem. From this moment the slot is
   empty, and a new install can create a fresh `<key>.work` right away.
2. Outside the lock: remove the trash tree with the existing `remove_tree()`
   (no symlink following, read-only directories made writable first).

For the wipe-on-invalidation, phase 2 runs in the detached cleanup worker
(3.6), so an install no longer waits for 250G to be deleted first. If phase 2
is interrupted, the next cleanup pass removes every `*.trash-*` entry
unconditionally, because nothing ever reads from trash.

### 3.5 Trigger: at most once a day, driven by normal use

- A stamp file `<cache>/gc.stamp` records the last cleanup in its mtime.
- Every normal invocation does one `stat()` of the stamp after its own
  scheduling decision and before exec. If the mtime is older than 24h, or
  more than 24h in the future (clock moved back), the run starts the cleanup
  worker. Otherwise nothing happens.
- The worker takes a non-blocking lock on the stamp. If another worker holds
  it, it exits. It re-checks the mtime through the descriptor, then updates
  it to now **before** doing any work, so a worker that keeps getting killed
  does not restart on every launch. Whatever it did not finish waits for the
  next day.
- No cleanup is started by `--plan`, `--check-only` or `--dry-run`, which
  promise not to change anything, nor by a hacman running inside another
  hacman's setup (detected by the `HACMAN_NAME` variable every setup
  exports). Nested runs share the parent's sandbox and cache, and the parent
  already covers it.
- `HACMAN_GC=0` disables the automatic cleanup completely, for example for
  test suites or read-only caches.

### 3.6 The cleanup worker

The invoking hacman must not slow down, and in the shim case it is replaced
by `exec()` immediately. The worker is therefore fully detached:

- **Double fork.** hacman forks a child, which forks the worker and exits
  right away. hacman reaps the child before continuing. The worker is
  reparented to init (or the nearest subreaper) instead of becoming a child
  of the program hacman execs. Otherwise a `wait` without arguments in an
  exec'd shell script would wait for the cleanup.
- **`setsid()`.** The worker leaves the caller's session and process group,
  so a terminal's Ctrl-C or `timeout(1)` killing the group does not hit it.
  If it is killed anyway, the crash-safety rules apply.
- **Standard descriptors to `/dev/null`, every other descriptor closed.**
  Otherwise a caller reading hacman's output through a pipe, for example
  `$(vault token lookup)`, would wait until the cleanup finishes.
- **Lowest priority.** `nice 19` and the idle I/O scheduling class.
- **Log.** Each pass rewrites `<cache>/gc.log` with what it removed or kept
  and every error it hit, so the cleanup is never a black box. One failing
  entry (for example `EPERM` on files another user put there) is logged and
  skipped, it never stops the pass.

`hacman --gc` runs the same pass in the foreground right away, ignoring the
stamp, and prints the log. `hacman --gc --dry-run` only lists what it would
remove.

### 3.7 What the cleanup removes

The worker lists `<cache>` once and only considers entries whose names match
these patterns exactly. Anything else, including other files a user put
there or a `--cache` pointed at the wrong directory, is never touched:

    p-<16 hex>   c-<16 hex>   url-<16 hex>   cmd-<16 hex>
    each optionally followed by .work, .lock, .tmp.<pid> or .trash-<random>

| entry | rule |
|---|---|
| `*.trash-*` | always remove |
| `*.tmp.<pid>` older than 1 hour | remove (a record is renamed into place within milliseconds) |
| legacy `url-*` / `cmd-*` record, work directory or lock | remove. Format 1 slots are named by identity hash or carry the old prefix, so no format 2 hacman will ever read them again. On the author's machine this frees the existing 434G on the first pass |
| `p-*` record whose source path still exists | **keep**, and drop `missing-since` if present |
| `p-*` record whose source path is gone (`ENOENT` or `ENOTDIR` from `stat()`) | first pass: write `missing-since`. Remove once the source has been missing for 7 days |
| `p-*` record whose source cannot be checked (`EACCES`, `EIO`, `ESTALE`, `ELOOP`, ...) | keep |
| `c-*` record whose `last_check` and `last_change` are both older than 30 days | remove |
| `.work` or `.lock` without a matching record, with an acquirable lock | remove. This is a first setup that never finished or an earlier cleanup that was interrupted, and the next run would wipe it anyway |
| record with a format number above the worker's own | keep (foreign generation) |
| anything with a held lock | skip until the next pass |

The 7-day grace period covers a `.siml` file that is only temporarily absent:
a branch switch, a rebase, an unmounted disk or an automount that is not
mounted right now.

### 3.8 Removing one slot

For every slot the rules above select:

1. Open `<key>.lock` once and take the lock with `F_SETLK` (non-blocking).
   If it is held, skip the slot. The worker never waits, so it can neither
   deadlock with a running setup nor with a re-entered one.
2. Run the inode check from 3.3.
3. Re-read the record with `O_NOFOLLOW` and evaluate the rule again. The
   project may have been used, edited or restored since the directory
   listing.
4. `unlink(<key>)`: the record is gone, so any later run treats the slot as
   new and starts from an empty work directory.
5. `rename(<key>.work, <key>.trash-<random>)`. A `.work` that is a symlink
   is unlinked instead, never followed.
6. `unlink(<key>.lock)`, then close it, which releases the lock.
7. `remove_tree(<key>.trash-<random>)`.

Every prefix of these steps leaves a state that a rule in 3.7 picks up on
the next pass: a record without a work directory is a normal fresh slot,
a work directory without a record is removed, and trash is always removed.

### 3.9 The hot path

| step | today | proposed |
|---|---|---|
| read `.siml` | 1 open/read/close | unchanged |
| canonical source path | embedded projects only | every file input, at most one `readlink()` |
| read record | 1 open/read/close | unchanged |
| cleanup trigger | none | 1 `stat()` of `gc.stamp` |
| writes, locks | none | none |

`regression-benchmark.py` must keep passing its existing limits.

## 4. Migration

The first run of the new version:

1. creates `p-*` slots for every project it runs. The first run of each
   project reinstalls it once, which already happens today after every
   hacman upgrade because the version is part of the identity;
2. finds `gc.stamp` missing and starts the first cleanup, which removes all
   format 1 entries (rule "legacy" in 3.7).

The legacy slots of a still-installed older binary, for example
`/usr/local/bin/hacman 0.1.0`, are removed as well. That binary keeps working
but reinstalls into fresh legacy slots, which the next daily cleanup removes
again. Section 7.1 covers how to run an older binary side by side.

## 5. Behavior changes

- Two files describing the same URL or command no longer share a record and
  a work directory.
- Moving a `.siml` file means a fresh install at the new path. The old slot
  disappears 7 days later.
- `--plan` prints the slot name and the cleanup rule that applies to it,
  e.g. `cleanup: removed 7 days after /home/you/bin/vault.siml disappears`.
- The cache gains `gc.stamp` and `gc.log`.

## 6. Failure analysis

| situation | outcome |
|---|---|
| setup killed partway | unchanged from today: nothing recorded, next run wipes the work directory before retrying |
| cleanup worker killed at any step of 3.8 | next pass finishes the job (see the end of 3.8) |
| worker killed on every attempt | stamp was updated first, so it retries daily rather than on every launch |
| two workers started at once | non-blocking stamp lock, the second exits |
| cleanup and install of the same project at once | the install holds the slot lock, the cleanup skips the slot |
| cleanup deletes a slot, a waiter was blocked on its lock | waiter wakes up, inode check fails, it reopens the new lock file |
| cached shim launch races with the cleanup | the cleanup only deletes slots whose source has been missing for 7 days, and the launch is reading that very source, so the race needs the file to disappear exactly during the launch. Then the launch sees no record, takes the lock and reinstalls |
| clock jumps backwards | stamp in the future counts as stale; `missing-since` in the future is reset to now |
| `--cache` points at a wrong directory | only exact hacman name patterns are considered, legacy and format 2 records must start with the magic line |
| files below `.work` owned by another user or immutable | removal of that entry fails, is logged, the entry is kept, the pass continues |
| a long-running process uses files from a removed slot | Linux keeps unlinked files alive for open descriptors. Files it opens later are gone, which only happens after its `.siml` has been missing for 7 days |

## 7. Known limitations

### 7.1 Several hacman generations on one cache

Format 2 hacman removes format 1 entries, so an older binary sharing the
cache keeps reinstalling. Two format 2 binaries with different versions share
slots, and each one's identity mismatch wipes the other's work directory on
every switch. This already happens today in principle, because the version is
part of the identity. Running another generation side by side needs its own
cache: `HACMAN_CACHE=~/.cache/hacman-0.1`.

### 7.2 Caches shared across hosts or mount namespaces

A cache on an NFS home, or mounted into a container where `.siml` files live
at different paths, makes sources look missing to the other side, which
removes their slots after 7 days. Mitigation if this matters: store the
hostname and the mount namespace inode (`/proc/self/ns/mnt`) in the record
and only judge source liveness for records written in the same namespace.
Not part of the first version.

### 7.3 Temporary files outside the cache

`hacman-install-*` and `hacman-response-*` files left in `$TMPDIR` by a
`SIGKILL`ed unsandboxed setup are outside the cache and not touched. Sandboxed
setups already write them into the work directory, which the wipe covers.
`/tmp` is normally cleaned by `systemd-tmpfiles` or on reboot.

## 8. Testing

All in `tests.py`, using `--cache` in the test's temporary directory and
`os.utime()` / handwritten records to simulate elapsed time:

- legacy `url-*` / `cmd-*` entries and their locks are removed on the first
  pass, unrelated files in the cache directory stay
- a slot whose `.siml` was deleted gets `missing-since`, survives the next
  pass inside the grace period, is removed after it, and is kept if the file
  comes back in between
- a slot whose source is unreadable (`chmod 000` on its directory) is kept
- `c-*` slots expire after 30 days without a check
- a slot whose lock is held by another process (held from Python via
  `fcntl.lockf`) is skipped
- `SIGKILL` the worker after each step of 3.8 (via a test-only hook), then
  run again: the cache converges to the same state as an uninterrupted pass
- a waiter blocked on a lock that the cleanup deletes ends up holding the
  new lock file, never the deleted one
- the shim case: hacman exec's a program that runs `wait` and reads its
  stdout through a pipe while a cleanup with a large trash tree runs; the
  program finishes immediately
- `--plan`, `--check-only`, `--dry-run`, nested runs and `HACMAN_GC=0` never
  start a cleanup
- `hacman --gc --dry-run` lists exactly what a real pass then removes
- symlinked `.work` directories and records are unlinked, their targets stay
- `regression-benchmark.py` stays within its limits

## 9. Open questions

1. **Version in the identity.** Every hacman rebuild invalidates every
   record and reinstalls everything, for example 180M for Vault, and two
   binaries invalidate each other (7.1). An explicit "setup epoch" constant,
   bumped only when hacman changes how setup behaves, would avoid that.
   Independent of the cleanup, but it decides how much churn the wipe causes.
2. **`install` and `bin-path` in the identity.** Editing a url project's
   install script or `bin-path` currently does not reinstall. With one slot
   per file it would be easy to include both, so edits take effect on the
   next run.
3. **Constants.** 24h stamp interval, 7-day grace for missing sources, 30
   days for content-addressed slots, 1 hour for record temp files. Fixed
   constants, or configurable through `HACMAN_GC_*` variables?
4. **Standard-input projects** could instead get no persistent slot at all
   (a temporary work directory removed after setup), which drops the only
   age-based rule. That breaks `bin-path` for them, because hacman execs the
   program afterwards and cannot remove the directory.

## 10. Rejected alternatives

- **Expire slots some days after their last invalidation.** That measures
  time since the last install, not the last use, so it deletes tools used
  every day whose upstream simply did not change (`schedule: never`,
  unchanged embedded projects, a quiet release cycle).
- **Expire slots by last use.** Correct in principle, but it needs a write
  on the lock-free fast path, and it still deletes a tool that is merely
  used rarely although its `.siml` exists.
- **Remove slots whose record has a different hacman version.** Versions
  are git hashes, so "older" can only mean "different", and two binaries on
  one cache would delete each other's slots daily. It also misses orphans
  from edited URLs, commands and deleted files.
- **Parse the `.siml` at the source path and compare identities.**
  Identities can depend on the environment (`{{VAR}}` templates), so a
  worker running with a different environment would see false mismatches.
  The wipe-on-invalidation already fixes a mismatched slot the next time it
  is used, and one slot per file keeps the space bounded until then.
- **A systemd timer or cron job running `hacman --gc`.** Works, but is not
  automatic: every user has to set it up, and caches selected by `--cache`
  or `HACMAN_CACHE` would be missed. `hacman --gc` stays available for
  people who want it anyway.
- **A size quota with LRU eviction.** Needs last-use tracking on the fast
  path, and evicts slots that are still in use.
