#!/usr/bin/env python3
"""Golden-file and offline behavioral tests for hacman."""

from __future__ import annotations

import difflib
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import time


class Suite:
    """Small assertion harness that preserves the shell suite's output style."""

    def __init__(self) -> None:
        self.root = Path(__file__).resolve().parent
        build_dir = Path(os.environ.get("BUILD_DIR", self.root / "build"))
        selected = sys.argv[1] if len(sys.argv) > 1 else os.environ.get("BIN")
        self.binary = Path(selected) if selected else build_dir / "hacman"
        if not self.binary.is_absolute():
            self.binary = (Path.cwd() / self.binary).resolve()
        self.gold_mode = os.environ.get("GOLD", "check")
        self.debug = bool(os.environ.get("DEBUG"))
        self.base_env = os.environ.copy()
        self.base_env.pop("HACMAN", None)
        self.failures = 0
        self.assertions = 0
        self.output = ""
        self.temp: Path

    def log_debug(self) -> None:
        """Report the resolved paths without enabling shell-style tracing."""
        if self.debug:
            print(f"[dbg][tests.py] ROOT_DIR={self.root}", file=sys.stderr)
            print(f"[dbg][tests.py] BIN={self.binary}", file=sys.stderr)

    def ensure_binary(self) -> None:
        """Build the default binary when no executable was supplied."""
        if not self.binary.is_file() or not os.access(self.binary, os.X_OK):
            subprocess.run([str(self.root / "build.sh")], cwd=self.root, check=True)

    def command_env(
        self,
        updates: dict[str, str] | None = None,
        remove: tuple[str, ...] = (),
    ) -> dict[str, str]:
        """Return an isolated environment for one child process."""
        env = self.base_env.copy()
        for name in remove:
            env.pop(name, None)
        if updates:
            env.update(updates)
        return env

    def run(
        self,
        argv: list[str | Path],
        *,
        env: dict[str, str] | None = None,
        stdin: str | None = None,
        stderr_to_stdout: bool = True,
        timeout: float = 15,
    ) -> subprocess.CompletedProcess[str]:
        """Run a command from the repository root and capture its text output."""
        command = [str(arg) for arg in argv]
        if self.debug:
            print(f"[dbg][tests.py] run: {command!r}", file=sys.stderr)
        result = subprocess.run(
            command,
            cwd=self.root,
            env=env or self.base_env,
            input=stdin,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT if stderr_to_stdout else subprocess.PIPE,
            text=True,
            timeout=timeout,
            check=False,
        )
        self.output = result.stdout
        return result

    def hacman(
        self,
        *args: str | Path,
        env: dict[str, str] | None = None,
        stdin: str | None = None,
    ) -> subprocess.CompletedProcess[str]:
        """Invoke the selected hacman binary."""
        return self.run([self.binary, *args], env=env, stdin=stdin)

    @staticmethod
    def indent(text: str) -> str:
        """Format captured child output beneath a failure message."""
        return "".join(f"    | {line}\n" for line in text.rstrip("\n").split("\n"))

    def fail(self, message: str, details: str = "") -> None:
        """Record a failure while allowing independent assertions to continue."""
        self.failures += 1
        print(f"[test] FAILED: {message}", file=sys.stderr)
        if details:
            print(self.indent(details), end="", file=sys.stderr)

    def ok(self, name: str) -> None:
        """Print one successful assertion in the established test format."""
        print(f"[test] ok: {name}")

    def check(self, name: str, condition: bool, details: str = "") -> bool:
        """Count and report one boolean assertion."""
        self.assertions += 1
        if condition:
            self.ok(name)
            return True
        self.fail(name, details)
        return False

    def expect(
        self,
        name: str,
        status: int,
        argv: list[str | Path],
        *,
        env: dict[str, str] | None = None,
        stdin: str | None = None,
    ) -> bool:
        """Run a command, retain its output, and assert its exit status."""
        result = self.run(argv, env=env, stdin=stdin)
        return self.check(
            name,
            result.returncode == status,
            f"exit code {result.returncode}, expected {status}\n{result.stdout}",
        )

    def expect_hacman(
        self,
        name: str,
        status: int,
        *args: str | Path,
        env: dict[str, str] | None = None,
        stdin: str | None = None,
    ) -> bool:
        """Assert the exit status of the selected hacman binary."""
        return self.expect(name, status, [self.binary, *args], env=env, stdin=stdin)

    def contains(self, name: str, needle: str) -> bool:
        """Assert that the most recently captured output contains text."""
        return self.check(
            name,
            needle in self.output,
            f"output does not contain {needle!r}\n{self.output}",
        )

    def missing(self, name: str, needle: str) -> bool:
        """Assert that the most recently captured output omits text."""
        return self.check(
            name,
            needle not in self.output,
            f"output unexpectedly contains {needle!r}\n{self.output}",
        )

    @staticmethod
    def write(path: Path, content: str, executable: bool = False) -> None:
        """Create a UTF-8 fixture, optionally marking it executable."""
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content, encoding="utf-8")
        if executable:
            path.chmod(0o755)

    @staticmethod
    def line_count(path: Path) -> int:
        """Count text lines in a small test artifact."""
        return len(path.read_text(encoding="utf-8").splitlines())

    def plan(self, path: Path) -> subprocess.CompletedProcess[str]:
        """Produce a reproducible plan under the golden-test environment."""
        env = self.command_env(
            {"HOME": "/home/hacman-test"},
            ("XDG_CACHE_HOME", "HACMAN_CACHE", "HACMAN_TEST_UNSET"),
        )
        try:
            argument: Path = path.relative_to(self.root)
        except ValueError:
            argument = path
        return self.hacman("--plan", argument, env=env)

    @staticmethod
    def normalize_plan(output: str) -> str:
        """Replace source-path-derived embedded cache identifiers."""
        if "files-dir: " in output:
            output = re.sub(r"cmd-[0-9a-f]{16}", "cmd-<source-path-hash>", output)
            output = re.sub(r"source:.* files:", "source:<input-path> files:", output)
        return output.rstrip("\n") + "\n"

    def golden_tests(self) -> bool:
        """Compare every parser fixture with its deterministic plan."""
        for siml in sorted((self.root / "tests").glob("*.siml")):
            gold = siml.with_suffix(".gold")
            wanted = 1 if siml.name.startswith("xfail_") else 0
            result = self.plan(siml)
            got = self.normalize_plan(result.stdout)

            if self.gold_mode == "update":
                gold.write_text(got, encoding="utf-8")
                print(f"[test] updated: {gold.relative_to(self.root)}")
                continue

            relative = siml.relative_to(self.root)
            self.assertions += 1
            if result.returncode != wanted:
                self.fail(
                    f"{relative}: exit code {result.returncode}, expected {wanted}",
                    got,
                )
                continue
            if not gold.exists():
                self.fail(
                    f"{relative}: no golden file at {gold.relative_to(self.root)}"
                )
                continue
            expected = gold.read_text(encoding="utf-8")
            if got != expected:
                diff = "".join(
                    difflib.unified_diff(
                        expected.splitlines(keepends=True),
                        got.splitlines(keepends=True),
                        fromfile=str(gold.relative_to(self.root)),
                        tofile="actual",
                    )
                )
                self.fail(
                    f"{relative}: plan differs from {gold.relative_to(self.root)}", diff
                )
            else:
                self.ok(str(relative))
        return self.gold_mode == "update"

    @staticmethod
    def merge_snapshot(path: Path) -> bool:
        """Identify untracked conflict snapshots left beside real examples."""
        return bool(
            re.search(r"_(?:BASE|LOCAL|REMOTE|BACKUP)_[0-9]+\.siml$", path.name)
        )

    def example_plans(self) -> None:
        """Ensure each shipped example resolves without performing work."""
        unsandboxed = []
        for siml in sorted((self.root / "examples").glob("*.siml")):
            if self.merge_snapshot(siml):
                continue
            result = self.plan(siml)
            relative = siml.relative_to(self.root)
            self.check(f"{relative} plans", result.returncode == 0, result.stdout)
            if "sandboxed: true\n" not in result.stdout:
                unsandboxed.append(str(relative))
        self.check(
            "all shipped examples use the default sandbox",
            not unsandboxed,
            "\n".join(unsandboxed),
        )

    def input_tests(self) -> None:
        """Cover environment options, stdin, and cache-path selection."""
        hacman_path = self.temp / "hacman-path"
        hacman_path.mkdir()
        (hacman_path / "hacman").symlink_to(self.binary)
        env = self.command_env(
            {
                "PATH": f"{hacman_path}:{self.base_env.get('PATH', '')}",
                "HACMAN": "--help",
            }
        )
        self.expect(
            "HACMAN passes --help through an executable project",
            0,
            [self.root / "examples/hello-c.siml"],
            env=env,
        )
        self.contains("HACMAN help prints usage", "usage: hacman [OPTIONS] FILE")

        env = self.command_env({"HACMAN": '--plan --cache "/tmp/hacman env cache"'})
        self.expect_hacman(
            "HACMAN accepts multiple and quoted options",
            0,
            "tests/minimal.siml",
            env=env,
        )
        self.contains(
            "quoted HACMAN value stays one argument",
            "cache-file: /tmp/hacman env cache/",
        )

        env = self.command_env({"HACMAN": "--plan --cache /tmp/hacman-env-cache"})
        self.expect_hacman(
            "command-line option overrides HACMAN value",
            0,
            "--cache",
            "/tmp/hacman-cli-cache",
            "tests/minimal.siml",
            env=env,
        )
        self.contains("command-line cache wins", "cache-file: /tmp/hacman-cli-cache/")
        self.missing("environment cache loses", "/tmp/hacman-env-cache/")

        env = self.command_env({"HACMAN": '--cache "unterminated'})
        self.expect_hacman(
            "unterminated HACMAN quote is rejected", 1, "tests/minimal.siml", env=env
        )
        self.contains(
            "unterminated HACMAN quote is explained",
            "HACMAN contains an unterminated quote",
        )

        env = self.command_env({"HACMAN": "--verbose not-an-option"})
        self.expect_hacman(
            "HACMAN positional argument is rejected", 1, "tests/minimal.siml", env=env
        )
        self.contains("HACMAN is options-only", "HACMAN may contain options only")

        self.expect_hacman(
            "stdin input via -", 0, "--plan", "-", stdin="url: https://example.com/x\n"
        )
        self.contains("stdin project is planned", "url: https://example.com/x")
        self.expect_hacman(
            "a missing file argument is a usage error",
            1,
            "--plan",
            stdin="url: https://example.com/x\n",
        )
        self.contains("missing file argument is explained", "no project file given")

        env = self.command_env(
            {"HOME": "/home/cache-test", "XDG_CACHE_HOME": "/tmp/xdg-cache"}
        )
        self.expect_hacman(
            "default cache stays below HOME", 0, "--plan", "tests/minimal.siml", env=env
        )
        self.contains(
            "default cache uses ~/.cache/hacman",
            "cache-file: /home/cache-test/.cache/hacman/",
        )
        self.missing("XDG cache path is ignored", "/tmp/xdg-cache")

        env = self.command_env(remove=("HOME", "HACMAN_CACHE"))
        self.expect_hacman(
            "missing HOME without a cache override is an error",
            1,
            "--plan",
            "tests/minimal.siml",
            env=env,
        )
        self.contains(
            "missing HOME explains cache overrides",
            "HOME is not set; use --cache or HACMAN_CACHE",
        )

    def trace_tests(self) -> None:
        """Verify setup output policy and shell tracing from both option sources."""
        command = self.temp / "trace-command.siml"
        self.write(
            command,
            """name: trace-command
command: printf 'command-out\\n'; printf 'command-err\\n' >&2
schedule: always
""",
        )
        quiet = self.run(
            [self.binary, "--cache", self.temp / "cache-trace-quiet", command],
            stderr_to_stdout=False,
        )
        self.check("quiet command succeeds", quiet.returncode == 0)
        self.check(
            "successful command output is hidden",
            "command-out" not in quiet.stdout and "command-err" not in quiet.stderr,
            f"stdout:\n{quiet.stdout}\nstderr:\n{quiet.stderr}",
        )

        trace_env = self.command_env({"HACMAN": "-x"})
        self.expect_hacman(
            "HACMAN=-x traces a command",
            0,
            "--cache",
            self.temp / "cache-trace-env",
            command,
            env=trace_env,
        )
        self.contains("command trace is live", "+ printf command-out")
        self.contains("traced command stdout is live", "command-out")
        self.contains("traced command stderr is prefixed", "hacman: command-err")

        payload = self.temp / "trace-payload"
        payload.write_text("payload\n", encoding="utf-8")
        install = self.temp / "trace-install.siml"
        self.write(
            install,
            f"""name: trace-install
url: {payload.as_uri()}
check: hash
schedule: always
install: printf 'install-out\\n'; printf 'install-err\\n' >&2
""",
        )
        self.expect_hacman(
            "quiet install succeeds",
            0,
            "--cache",
            self.temp / "cache-install-quiet",
            install,
        )
        self.missing("successful install stdout is hidden", "install-out")
        self.missing("successful install stderr is hidden", "install-err")
        self.expect_hacman(
            "CLI -x traces an install",
            0,
            "-x",
            "--cache",
            self.temp / "cache-install-trace",
            install,
        )
        self.contains("install trace is live", "+ printf install-out")
        self.contains("traced install stdout is live", "install-out")
        self.contains("traced install stderr is prefixed", "hacman: install-err")

        failing = self.temp / "trace-failing.siml"
        self.write(
            failing,
            """name: trace-failing
command: printf 'failure-out\\n'; printf 'failure-err\\n' >&2; exit 9
schedule: always
""",
        )
        failed = self.run(
            [self.binary, "--cache", self.temp / "cache-trace-failing", failing],
            stderr_to_stdout=False,
        )
        self.check("failing captured command exits 2", failed.returncode == 2)
        self.check(
            "failed setup stdout is replayed to stderr",
            "failure-out" not in failed.stdout and "hacman: failure-out" in failed.stderr,
            f"stdout:\n{failed.stdout}\nstderr:\n{failed.stderr}",
        )
        self.check(
            "failed setup stderr is replayed to stderr",
            "failure-err" not in failed.stdout and "hacman: failure-err" in failed.stderr,
            f"stdout:\n{failed.stdout}\nstderr:\n{failed.stderr}",
        )

    def embedded_tests(self) -> None:
        """Exercise embedded files, scheduled rebuilds, and serialization."""
        embedded_cache = self.temp / "cache-embedded"
        self.expect_hacman(
            "embedded files stay untouched in dry-run",
            10,
            "--dry-run",
            "--force",
            "--cache",
            embedded_cache,
            "tests/embedded_files.siml",
        )
        workdirs = (
            list(embedded_cache.glob("*.work")) if embedded_cache.exists() else []
        )
        self.check("dry-run creates no embedded files", not workdirs)
        self.expect_hacman(
            "embedded files exist before command execution",
            0,
            "--force",
            "--cache",
            embedded_cache,
            "tests/embedded_files.siml",
        )
        self.missing("successful embedded command output is hidden", "embedded-ready")

        embedded_bin = self.temp / "embedded-bin.siml"
        self.write(
            embedded_bin,
            """name: embedded-bin
command: mkdir -p build && cp runner build/prog && chmod +x build/prog
bin-path: build/prog
schedule: never
files:
  runner: |
    #!/bin/sh
    echo "embedded-prog"
    for arg in "$@"; do echo "arg:$arg"; done
""",
        )
        self.expect_hacman(
            "embedded relative bin-path runs",
            0,
            "--force",
            "--cache",
            self.temp / "cache-embedded-bin",
            embedded_bin,
            "--help",
            "two words",
        )
        self.contains("embedded program completed", "embedded-prog")
        self.contains(
            "embedded program receives option-looking arguments", "arg:--help"
        )
        self.contains("embedded program preserves argument boundaries", "arg:two words")

        embedded_cached = self.temp / "embedded-cached.siml"
        self.write(
            embedded_cached,
            """name: embedded-cached
command: echo built >> build.log && chmod +x runner
bin-path: runner
files:
  runner: |
    #!/bin/sh
    echo cached-v1
""",
        )
        cached_dir = self.temp / "cache-embedded-cached"
        self.expect_hacman(
            "embedded cached build runs initially",
            0,
            "--cache",
            cached_dir,
            embedded_cached,
        )
        self.contains("initial embedded executable runs", "cached-v1")
        self.expect_hacman(
            "unchanged embedded build is reused",
            0,
            "-v",
            "--cache",
            cached_dir,
            embedded_cached,
        )
        self.contains("embedded cache hit is explained", "embedded inputs unchanged")
        self.contains("cached embedded executable runs", "cached-v1")
        workdir = next(cached_dir.glob("*.work"))
        readonly_tree = workdir / "readonly-tree"
        readonly_nested = readonly_tree / "nested"
        readonly_nested.mkdir(parents=True)
        readonly_marker = readonly_nested / "marker"
        readonly_marker.write_text("remove me\n", encoding="utf-8")
        readonly_marker.chmod(0o444)
        # Match Go's module cache, which removes owner write permission from
        # downloaded module directories before hacman later clears the tree.
        readonly_nested.chmod(0o555)
        readonly_tree.chmod(0o555)
        build_logs = list(cached_dir.glob("*.work/build.log"))
        self.check(
            "unchanged embedded command ran only once",
            len(build_logs) == 1 and self.line_count(build_logs[0]) == 1,
        )

        embedded_scheduled = self.temp / "embedded-scheduled.siml"
        self.write(
            embedded_scheduled,
            """name: embedded-scheduled
command: echo checked >> checks.log && cp runner program && chmod +x program
bin-path: program
schedule: always
files:
  runner: |
    #!/bin/sh
    echo scheduled-program
""",
        )
        scheduled_cache = self.temp / "cache-embedded-scheduled"
        self.expect_hacman(
            "scheduled embedded command runs initially",
            0,
            "--cache",
            scheduled_cache,
            embedded_scheduled,
        )
        self.contains("scheduled embedded executable runs", "scheduled-program")
        self.expect_hacman(
            "scheduled embedded command runs again",
            0,
            "--cache",
            scheduled_cache,
            embedded_scheduled,
        )
        check_logs = list(scheduled_cache.glob("*.work/checks.log"))
        self.check(
            "explicit schedule reran embedded command",
            len(check_logs) == 1 and self.line_count(check_logs[0]) == 2,
        )

        self.concurrent_embedded_test()

        (workdir / "stale-output").touch()
        embedded_cached.write_text(
            embedded_cached.read_text(encoding="utf-8").replace(
                "cached-v1", "cached-v2"
            ),
            encoding="utf-8",
        )
        self.expect_hacman(
            "changed embedded input builds again",
            0,
            "--cache",
            cached_dir,
            embedded_cached,
        )
        self.contains("changed embedded executable runs", "cached-v2")
        changed_dirs = list(cached_dir.glob("*.work"))
        self.check(
            "changed embedded input reused its work directory",
            changed_dirs == [workdir],
        )
        self.check(
            "changed embedded input cleared stale build outputs",
            not (workdir / "stale-output").exists()
            and (workdir / "build.log").read_text(encoding="utf-8").strip() == "built",
        )
        self.check(
            "changed embedded input removed read-only stale directories",
            not readonly_tree.exists(),
        )

        embedded_copy = self.temp / "embedded-copy.siml"
        shutil.copyfile(embedded_cached, embedded_copy)
        self.expect_hacman(
            "copied embedded input builds separately",
            0,
            "--cache",
            cached_dir,
            embedded_copy,
        )
        self.check(
            "source paths select separate work directories",
            len(list(cached_dir.glob("*.work"))) == 2,
        )

    def concurrent_embedded_test(self) -> None:
        """Prove that concurrent due shims wait and share one updater."""
        started = self.temp / "update-started"
        release = self.temp / "update-release"
        project = self.temp / "embedded-concurrent.siml"
        self.write(
            project,
            f"""name: embedded-concurrent
sandboxed: false
command: echo update >> update.log && touch "{started}" && while [ ! -e "{release}" ]; do sleep 0.01; done && cp runner program && chmod +x program
bin-path: program
schedule: 1h
files:
  runner: |
    #!/bin/sh
    echo concurrent-program
""",
        )
        cache = self.temp / "cache-embedded-concurrent"
        processes: list[subprocess.Popen[str]] = []
        outputs: list[Path] = []
        handles = []

        def launch(index: int) -> None:
            output = self.temp / f"concurrent-{index}.out"
            handle = output.open("w", encoding="utf-8")
            process = subprocess.Popen(
                [str(self.binary), "--cache", str(cache), str(project)],
                cwd=self.root,
                env=self.base_env,
                stdout=handle,
                stderr=subprocess.STDOUT,
                text=True,
            )
            outputs.append(output)
            handles.append(handle)
            processes.append(process)

        try:
            launch(0)
            deadline = time.monotonic() + 5
            while (
                not started.exists()
                and processes[0].poll() is None
                and time.monotonic() < deadline
            ):
                time.sleep(0.01)
            for index in range(1, 8):
                launch(index)
            time.sleep(0.1)
            self.check(
                "concurrent invocations wait for updater",
                started.exists()
                and all(process.poll() is None for process in processes),
            )
            release.touch()
            statuses = [process.wait(timeout=10) for process in processes]
            self.check(
                "concurrent invocations exec after update",
                statuses == [0] * len(processes),
                "\n".join(path.read_text(encoding="utf-8") for path in outputs),
            )
            update_logs = list(cache.glob("*.work/update.log"))
            self.check(
                "concurrent invocations ran one updater",
                len(update_logs) == 1 and self.line_count(update_logs[0]) == 1,
            )
        finally:
            release.touch(exist_ok=True)
            for process in processes:
                if process.poll() is None:
                    process.kill()
                process.wait()
            for handle in handles:
                handle.close()

    def sandbox_tests(self) -> None:
        """Verify default confinement and the explicit opt-out for both runners."""
        outside = self.temp / "sandbox-command-outside"
        readable = self.temp / "sandbox-readable"
        readable.write_text("host input\n", encoding="utf-8")
        protected = self.temp / "sandbox-protected"
        protected.write_text("unchanged\n", encoding="utf-8")
        env = self.command_env(
            {
                "SANDBOX_OUTSIDE": str(outside),
                "SANDBOX_PROTECTED": str(protected),
                "SANDBOX_READABLE": str(readable),
                "TMPDIR": str(self.temp),
            }
        )
        cache = self.temp / "cache-sandbox-command"
        project = self.temp / "sandbox-command.siml"
        self.write(
            project,
            """name: sandbox-command
command: cat "$SANDBOX_READABLE" > "$TMPDIR/read-copy" && printf changed > "$SANDBOX_PROTECTED"; printf blocked > "$SANDBOX_OUTSIDE"
schedule: always
""",
        )
        self.expect_hacman(
            "sandboxed command rejects an external write",
            2,
            "--cache",
            cache,
            project,
            env=env,
        )
        workdir = next(cache.glob("*.work"))
        self.check(
            "sandboxed command can read the host",
            (workdir / "read-copy").read_text(encoding="utf-8") == "host input\n",
        )
        self.check("sandboxed command cannot write to the host", not outside.exists())
        self.check(
            "sandboxed command cannot replace host contents",
            protected.read_text(encoding="utf-8") == "unchanged\n",
        )

        dev_null = self.temp / "sandbox-dev-null.siml"
        self.write(
            dev_null,
            """name: sandbox-dev-null
command: printf discarded > /dev/null
schedule: always
""",
        )
        self.expect_hacman(
            "sandboxed command can write to /dev/null",
            0,
            "--cache",
            self.temp / "cache-sandbox-dev-null",
            dev_null,
        )

        project.write_text(
            project.read_text(encoding="utf-8").replace(
                "name: sandbox-command\n", "name: sandbox-command\nsandboxed: false\n"
            ),
            encoding="utf-8",
        )
        self.expect_hacman(
            "sandboxed false lets a command write outside its cache",
            0,
            "--cache",
            cache,
            project,
            env=env,
        )
        self.check(
            "unsandboxed command external write is visible",
            outside.read_text(encoding="utf-8") == "blocked",
        )

        outside.unlink()
        payload = self.temp / "sandbox-payload"
        payload.write_text("response body\n", encoding="utf-8")
        script_cache = self.temp / "cache-sandbox-script"
        script = self.temp / "sandbox-script.siml"
        script_env = self.command_env(
            {"SANDBOX_OUTSIDE": str(outside), "TMPDIR": str(self.temp)}
        )
        self.write(
            script,
            f"""name: sandbox-script
url: {payload.as_uri()}
check: hash
schedule: always
install: |
  cat "$HACMAN_RESPONSE" > "$TMPDIR/response-copy"
  printf blocked > "$SANDBOX_OUTSIDE"
""",
        )
        self.expect_hacman(
            "sandboxed install script rejects an external write",
            2,
            "--cache",
            script_cache,
            script,
            env=script_env,
        )
        script_workdir = next(script_cache.glob("*.work"))
        self.check(
            "sandboxed install script writes in its cache workdir",
            (script_workdir / "response-copy").read_text(encoding="utf-8")
            == "response body\n",
        )
        self.check(
            "sandboxed install script cannot write to the host", not outside.exists()
        )

        script.write_text(
            script.read_text(encoding="utf-8").replace(
                "name: sandbox-script\n", "name: sandbox-script\nsandboxed: false\n"
            ),
            encoding="utf-8",
        )
        self.expect_hacman(
            "sandboxed false lets an install script write outside its cache",
            0,
            "--cache",
            script_cache,
            script,
            env=script_env,
        )
        self.check(
            "unsandboxed install-script external write is visible",
            outside.read_text(encoding="utf-8") == "blocked",
        )

        cached_program = self.temp / "sandbox-cached-bin.siml"
        self.write(
            cached_program,
            f"""name: sandbox-cached-bin
url: {payload.as_uri()}
check: hash
schedule: weekly
bin-path: cached-tool
install: |
  echo installed >> installs.log
  cat > cached-tool <<'SCRIPT'
  #!/bin/sh
  printf 'cached:%s\\n' "$1"
  SCRIPT
  chmod 755 cached-tool
""",
        )
        self.expect_hacman(
            "sandboxed URL project builds and execs a relative bin-path",
            0,
            "--cache",
            self.temp / "cache-sandbox-cached-bin",
            cached_program,
            "argument",
        )
        self.contains(
            "relative bin-path resolves inside the sandbox workdir",
            "cached:argument",
        )
        cached_workdir = next((self.temp / "cache-sandbox-cached-bin").glob("*.work"))
        (cached_workdir / "cached-tool").unlink()
        self.expect_hacman(
            "missing cached relative bin-path triggers setup before schedule",
            0,
            "--cache",
            self.temp / "cache-sandbox-cached-bin",
            cached_program,
            "rebuilt",
        )
        self.check(
            "missing cached relative bin-path was rebuilt",
            self.line_count(cached_workdir / "installs.log") == 2,
        )

        redirect = self.temp / "sandbox-symlink-target"
        redirect.mkdir()
        redirect_cache = self.temp / "cache-sandbox-symlink"
        redirect_project = self.temp / "sandbox-symlink.siml"
        self.write(
            redirect_project,
            """name: sandbox-symlink
command: printf escaped > "$TMPDIR/escaped"
schedule: always
""",
        )
        plan = self.hacman("--plan", "--cache", redirect_cache, redirect_project)
        match = re.search(r"^sandbox-write-dir: (.+)$", plan.stdout, re.MULTILINE)
        redirected_workdir = Path(match.group(1)) if match else Path()
        redirect_cache.mkdir()
        if match:
            redirected_workdir.symlink_to(redirect, target_is_directory=True)
        self.expect_hacman(
            "sandbox rejects a symlinked writable workdir",
            2,
            "--cache",
            redirect_cache,
            redirect_project,
        )
        self.check(
            "symlinked workdir cannot redirect sandbox writes",
            match is not None and not (redirect / "escaped").exists(),
        )

    def url_pipeline_tests(self) -> tuple[Path, Path]:
        """Exercise URL checks, schedules, modes, and failures."""
        payload = self.temp / "payload.txt"
        payload.write_text("version 1.0.0\n", encoding="utf-8")
        cache = self.temp / "cache"
        project = self.temp / "hash.siml"
        self.write(
            project,
            f"""name: demo
url: {payload.as_uri()}
check: hash
schedule: always
install: |
  set -eu
  echo "install name=$HACMAN_NAME prev=${{HACMAN_PREVIOUS:-none}}" >> install.log
  if [ -n "${{HACMAN_RESPONSE:-}}" ]; then
    echo "body=$(cat "$HACMAN_RESPONSE")" >> install.log
  fi
""",
        )
        self.expect_hacman("first run installs", 0, "--cache", cache, project)
        self.contains("first run reports new", "new      demo")
        install_log = next(cache.glob("*.work/install.log"))
        install_output = install_log.read_text(encoding="utf-8")
        self.check(
            "install script ran", "install name=demo prev=none" in install_output
        )
        self.check("response file is passed", "body=version 1.0.0" in install_output)
        self.expect_hacman("unchanged run is quiet", 0, "--cache", cache, project)
        self.missing("no install on unchanged", "install name=demo")
        self.expect_hacman(
            "unchanged run is verbose on demand", 0, "-v", "--cache", cache, project
        )
        self.contains("verbose reports ok", "ok       demo")
        payload.write_text("version 1.0.1\n", encoding="utf-8")
        self.expect_hacman("changed run installs again", 0, "--cache", cache, project)
        self.contains("change is reported with both marks", "changed  demo")
        install_output = install_log.read_text(encoding="utf-8")
        install_lines = [
            line
            for line in install_output.splitlines()
            if line.startswith("install name=")
        ]
        self.check(
            "previous mark reaches the script",
            len(install_lines) == 2
            and install_lines[0] == "install name=demo prev=none"
            and install_lines[1] != "install name=demo prev=none",
            install_output,
        )

        self.schedule_tests(payload)
        self.version_tests()
        self.mode_tests(payload, cache, project)
        self.failure_tests(payload)
        self.plan_script_test(payload)
        return payload, cache

    def schedule_tests(self, payload: Path) -> None:
        """Verify interval and never schedules."""
        schedule = self.temp / "sched.siml"
        self.write(
            schedule,
            f"""name: scheduled
url: {payload.as_uri()}
check: hash
schedule: 6h
install: echo "installed"
""",
        )
        cache = self.temp / "cache-sched"
        self.expect_hacman("scheduled first run checks", 0, "--cache", cache, schedule)
        self.contains("scheduled first run is a change", "new      scheduled")
        payload.write_text("version 1.0.2\n", encoding="utf-8")
        self.expect_hacman(
            "schedule suppresses the next check", 0, "-v", "--cache", cache, schedule
        )
        self.contains("skip mentions the wait", "skip     scheduled (next run in")
        self.missing("no install while skipped", "installed")
        self.expect_hacman(
            "--force ignores the schedule", 0, "-f", "--cache", cache, schedule
        )
        self.missing("successful forced install output is hidden", "installed")

        never = self.temp / "never.siml"
        self.write(
            never,
            f"""name: manual
url: {payload.as_uri()}
check: hash
schedule: never
install: echo "installed"
""",
        )
        never_cache = self.temp / "cache-never"
        self.expect_hacman(
            "schedule: never does nothing", 0, "-v", "--cache", never_cache, never
        )
        self.contains("never is reported as such", "skip     manual (schedule: never)")
        self.expect_hacman(
            "schedule: never yields to --force", 0, "-f", "--cache", never_cache, never
        )
        self.missing("successful manual install output is hidden", "installed")

    def version_tests(self) -> None:
        """Verify successful and failed version extraction."""
        release = self.temp / "release.json"
        release.write_text(
            '{"tag_name": "14.1.1", "name": "ripgrep 14.1.1"}\n', encoding="utf-8"
        )
        version = self.temp / "version.siml"
        version_cache = self.temp / "cache-version"
        self.write(
            version,
            f"""name: versioned
url: {release.as_uri()}
check: version
version-prefix: "tag_name": "
version-suffix: "
schedule: always
install: printf '%s\\n' "$HACMAN_VERSION" > observed-version
""",
        )
        self.expect_hacman("version extraction", 0, "--cache", version_cache, version)
        observed = list(version_cache.glob("*.work/observed-version"))
        self.check(
            "version is extracted between the anchors",
            len(observed) == 1
            and observed[0].read_text(encoding="utf-8") == "14.1.1\n",
        )

        bad = self.temp / "badversion.siml"
        self.write(
            bad,
            f"""name: versioned
url: {release.as_uri()}
check: version
version-prefix: nothing-like-this
schedule: always
install: echo "unreachable"
""",
        )
        self.expect_hacman(
            "missing version-prefix fails the check",
            2,
            "--cache",
            self.temp / "cache-bad",
            bad,
        )
        self.contains("missing prefix is explained", "version-prefix not found")

    def mode_tests(self, payload: Path, cache: Path, project: Path) -> None:
        """Ensure inspection and adoption modes do not install unexpectedly."""
        payload.write_text("version 2.0.0\n", encoding="utf-8")
        self.expect_hacman(
            "--check-only reports changes as exit 10",
            10,
            "-c",
            "--cache",
            cache,
            project,
        )
        self.missing("--check-only does not install", "install name=demo")
        self.expect_hacman(
            "--dry-run also reports exit 10", 10, "-n", "--cache", cache, project
        )
        self.expect_hacman(
            "--adopt records without installing", 0, "-a", "--cache", cache, project
        )
        self.missing("--adopt does not install", "install name=demo")
        self.expect_hacman(
            "adopted state is now unchanged", 0, "-v", "--cache", cache, project
        )
        self.contains("adopted project reads as ok", "ok       demo")

    def failure_tests(self, payload: Path) -> None:
        """Ensure failed checks, installs, and commands remain retryable."""
        failing = self.temp / "fail.siml"
        self.write(
            failing,
            f"""name: failing
url: {payload.as_uri()}
check: hash
schedule: 6h
install: |
  echo "about to fail"
  exit 3
""",
        )
        cache = self.temp / "cache-fail"
        self.expect_hacman("failing install exits 2", 2, "--cache", cache, failing)
        self.contains("failure names the exit code", "exit code 3")
        self.expect_hacman(
            "failed install is retried immediately", 2, "--cache", cache, failing
        )
        self.contains("retry runs the script again", "about to fail")

        missing = self.temp / "missing.siml"
        self.write(
            missing,
            f"""name: gone
url: {(self.temp / 'does-not-exist').as_uri()}
check: hash
schedule: always
install: echo "unreachable"
""",
        )
        self.expect_hacman(
            "unreachable url exits 2",
            2,
            "--cache",
            self.temp / "cache-missing",
            missing,
        )
        self.contains("unreachable url is reported", "request failed")

    def plan_script_test(self, payload: Path) -> None:
        """Compare the planned install block with the generated script."""
        project = self.temp / "indent.siml"
        self.write(
            project,
            f"""name: indented
url: {payload.as_uri()}
check: hash
schedule: always
install: |
  for i in 1 2; do
    if [ "$i" = 2 ]; then
      echo "nested $i"
    fi
  done
""",
        )
        cache = self.temp / "cache-indent"
        self.expect_hacman(
            "indentation inside the block is preserved",
            0,
            "-x",
            "--cache",
            cache,
            project,
        )
        self.contains("nested shell block ran", "nested 2")

        plan = self.hacman("--plan", project).stdout
        marker = "install: |\n"
        planned = plan.split(marker, 1)[1] if marker in plan else ""
        planned = "".join(
            line[2:] if line.startswith("  ") else line
            for line in planned.splitlines(keepends=True)
        )
        shutil.rmtree(cache)
        env = self.command_env({"HACMAN_KEEP_TEMP": "1"})
        result = self.hacman("--cache", cache, project, env=env)
        match = re.search(r"^hacman: indented: kept (.+)$", result.stdout, re.MULTILINE)
        kept = Path(match.group(1)) if match else None
        actual = ""
        if kept and kept.exists():
            lines = kept.read_text(encoding="utf-8").splitlines(keepends=True)
            actual = "".join(lines[1:])
        self.check(
            "the plan matches the generated script", bool(kept) and actual == planned
        )
        if kept:
            kept.unlink(missing_ok=True)

    def command_tests(self) -> None:
        """Exercise scheduled commands, identities, workdirs, and failures."""
        cache = self.temp / "cache-cmd"
        marker = self.temp / "ran.log"
        marker.write_text("", encoding="utf-8")
        env = self.command_env({"TMPDIR_FOR_TEST": str(self.temp)})
        first = self.temp / "cmd-a.siml"
        self.write(
            first,
            f"""name: first-file
sandboxed: false
command: echo "ran in $(pwd)" >>"{marker}"
workdir: {{{{TMPDIR_FOR_TEST}}}}
schedule: 6h
""",
        )
        second = self.temp / "cmd-b.siml"
        second.write_text(
            first.read_text(encoding="utf-8").replace(
                "name: first-file", "name: second-file"
            ),
            encoding="utf-8",
        )
        self.expect_hacman(
            "command project runs", 0, "--cache", cache, "-v", first, env=env
        )
        self.contains("verbose announces the run", "run      first-file")
        self.check(
            "the command ran in its workdir",
            self.line_count(marker) == 1
            and f"ran in {self.temp}" in marker.read_text(encoding="utf-8"),
        )
        self.expect_hacman(
            "command respects its schedule", 0, "--cache", cache, "-v", first, env=env
        )
        self.contains(
            "second invocation is skipped", "skip     first-file (next run in"
        )
        self.expect_hacman(
            "another file with the same command shares the record",
            0,
            "--cache",
            cache,
            "-v",
            second,
            env=env,
        )
        self.contains(
            "the shared record skips the second file",
            "skip     second-file (next run in",
        )
        self.check(
            "the shared record prevented a second run", self.line_count(marker) == 1
        )
        records = [
            path
            for path in cache.rglob("*")
            if path.is_file() and path.suffix != ".lock"
        ]
        self.check(
            "both files use one cache record",
            len(records) == 1,
            "\n".join(map(str, records)),
        )

        self.expect_hacman(
            "--force runs a scheduled command again",
            0,
            "-f",
            "--cache",
            cache,
            first,
            env=env,
        )
        self.check(
            "--force ran the command a second time", self.line_count(marker) == 2
        )

        other = self.temp / "other"
        other.mkdir()
        third = self.temp / "cmd-c.siml"
        third.write_text(
            re.sub(
                r"^workdir: .*$",
                f"workdir: {other}",
                first.read_text(encoding="utf-8"),
                flags=re.MULTILINE,
            ),
            encoding="utf-8",
        )
        self.expect_hacman(
            "a different workdir is a different record",
            0,
            "--cache",
            cache,
            third,
            env=env,
        )
        marker_text = marker.read_text(encoding="utf-8")
        self.check(
            "the same command elsewhere ran on its own schedule",
            self.line_count(marker) == 3 and f"ran in {other}" in marker_text,
        )

        failed_marker = self.temp / "ran.log.fail"
        failed_marker.write_text("", encoding="utf-8")
        failing = self.temp / "cmd-fail.siml"
        self.write(
            failing,
            f"""name: failing-command
sandboxed: false
command: echo attempt >>"{failed_marker}"; exit 7
workdir: {{{{TMPDIR_FOR_TEST}}}}
schedule: 6h
""",
        )
        self.expect_hacman(
            "a failing command exits 2", 2, "--cache", cache, failing, env=env
        )
        self.contains("the exit code is reported", "command failed with exit code 7")
        self.expect_hacman(
            "a failing command is retried at once",
            2,
            "--cache",
            cache,
            failing,
            env=env,
        )
        self.check(
            "the failed run was not recorded", self.line_count(failed_marker) == 2
        )
        self.expect_hacman(
            "--check-only reports a due command as exit 10",
            10,
            "-c",
            "--cache",
            cache,
            failing,
            env=env,
        )
        self.contains("due command is named", "due      failing-command")
        self.expect_hacman(
            "--adopt records a command without running it",
            0,
            "-a",
            "--cache",
            cache,
            failing,
            env=env,
        )
        self.check(
            "--adopt did not run the command", self.line_count(failed_marker) == 2
        )
        self.expect_hacman(
            "an adopted command is now on its schedule",
            0,
            "-v",
            "--cache",
            cache,
            failing,
            env=env,
        )
        self.contains(
            "adopted command is skipped", "skip     failing-command (next run in"
        )

        fake_home = self.temp / "fake-home"
        fake_home.mkdir()
        home_marker = self.temp / "ran.log.home"
        at_home = self.temp / "cmd-home.siml"
        self.write(
            at_home,
            f"""name: at-home
sandboxed: false
command: pwd >"{home_marker}"
schedule: always
""",
        )
        home_env = env.copy()
        home_env["HOME"] = str(fake_home)
        self.expect_hacman(
            "a command without workdir runs in HOME",
            0,
            "--cache",
            cache,
            at_home,
            env=home_env,
        )
        self.check(
            "the default workdir is {{HOME}}",
            home_marker.read_text(encoding="utf-8").strip() == str(fake_home),
        )

    def shim_tests(self) -> None:
        """Verify handoff, argument boundaries, output, and exit statuses."""
        cache = self.temp / "cache-shim"
        bin_dir = self.temp / "bin"
        bin_dir.mkdir()
        setup_log = self.temp / "setup.log"
        setup_log.write_text("", encoding="utf-8")
        program = bin_dir / "prog"
        self.write(
            program,
            """#!/bin/sh
echo "prog-ran"
for arg in "$@"; do echo "arg:$arg"; done
""",
            executable=True,
        )
        env = self.command_env({"TMPDIR_FOR_TEST": str(self.temp)})
        shim = self.temp / "shim.siml"
        self.write(
            shim,
            f"""name: shimmed
sandboxed: false
command: echo setup >>"{setup_log}"
workdir: {{{{TMPDIR_FOR_TEST}}}}
bin-path: {program}
schedule: 6h
""",
        )
        self.expect_hacman(
            "the shim sets up and then execs",
            0,
            "-v",
            "--cache",
            cache,
            shim,
            "--flag",
            "two words",
            env=env,
        )
        self.contains("the setup ran", "run      shimmed")
        self.contains("the program ran", "prog-ran")
        self.contains("an option-looking argument is forwarded", "arg:--flag")
        self.contains("argument boundaries survive", "arg:two words")
        self.check("the setup ran once", self.line_count(setup_log) == 1)

        self.expect_hacman(
            "a project that is not due still execs",
            0,
            "--cache",
            cache,
            shim,
            "again",
            env=env,
        )
        self.contains("the program ran again", "prog-ran")
        self.contains("its argument came through", "arg:again")
        self.check("the skipped setup did not run", self.line_count(setup_log) == 1)

        self.expect_hacman(
            "options after FILE belong to the program",
            0,
            "--cache",
            cache,
            shim,
            "--plan",
            "-v",
            env=env,
        )
        self.contains("--plan after FILE was forwarded", "arg:--plan")
        self.missing("--plan after FILE did not print a plan", "cache-identity:")

        result = self.run(
            [self.binary, "-v", "--cache", cache, shim, "x"],
            env=env,
            stderr_to_stdout=False,
        )
        self.check(
            "status output stays off the program's stdout",
            result.stdout == "prog-ran\narg:x\n",
        )

        stderr_program = bin_dir / "stderr-prog"
        self.write(
            stderr_program,
            """#!/bin/sh
printf 'first problem\nsecond problem\nlast problem' >&2
printf 'program output\n'
""",
            executable=True,
        )
        stderr_shim = self.temp / "stderr-shim.siml"
        self.write(
            stderr_shim,
            f"""name: stderr-shim
sandboxed: false
command: :
workdir: {{{{TMPDIR_FOR_TEST}}}}
bin-path: {stderr_program}
schedule: always
""",
        )
        # Capture stderr separately so this checks the exact line-oriented
        # status prefix while proving the handed-off program is untouched.
        stderr_result = self.run(
            [self.binary, "-v", "--cache", self.temp / "cache-stderr", stderr_shim],
            env=env,
            stderr_to_stdout=False,
        )
        self.check(
            "hacman status is prefixed and program stderr is untouched",
            stderr_result.stderr
            == "hacman: run      stderr-shim\nfirst problem\n"
            "second problem\nlast problem",
            f"stdout:\n{stderr_result.stdout}stderr:\n{stderr_result.stderr}",
        )
        self.check("program stdout remains unchanged", stderr_result.stdout == "program output\n")

        fail_program = bin_dir / "failprog"
        self.write(fail_program, "#!/bin/sh\nexit 3\n", executable=True)
        shim_fail = self.temp / "shim-fail.siml"
        shim_fail.write_text(
            re.sub(
                r"^bin-path: .*$",
                f"bin-path: {fail_program}",
                shim.read_text(encoding="utf-8"),
                flags=re.MULTILINE,
            ),
            encoding="utf-8",
        )
        self.expect_hacman(
            "the program's exit status is passed through",
            3,
            "--cache",
            cache,
            shim_fail,
            env=env,
        )

        absent = bin_dir / "absent"
        shim_absent = self.temp / "shim-absent.siml"
        shim_absent.write_text(
            re.sub(
                r"^bin-path: .*$",
                f"bin-path: {absent}",
                shim.read_text(encoding="utf-8"),
                flags=re.MULTILINE,
            ),
            encoding="utf-8",
        )
        self.expect_hacman(
            "a missing program exits 127", 127, "--cache", cache, shim_absent, env=env
        )
        self.contains("the missing program is named", "cannot execute")

        bad_setup = self.temp / "shim-badsetup.siml"
        self.write(
            bad_setup,
            f"""name: bad-setup
sandboxed: false
command: exit 5
workdir: {{{{TMPDIR_FOR_TEST}}}}
bin-path: {program}
schedule: always
""",
        )
        self.expect_hacman(
            "a failed setup does not exec", 2, "--cache", cache, bad_setup, env=env
        )
        self.missing("the program was not started", "prog-ran")

        self.expect_hacman(
            "arguments without a bin-path are an error",
            1,
            "--cache",
            cache,
            self.temp / "cmd-a.siml",
            "surplus",
            env=env,
        )
        self.contains("the missing bin-path is explained", "need a 'bin-path'")
        self.expect_hacman(
            "--plan does not exec", 0, "--cache", cache, "--plan", shim, env=env
        )
        self.missing("--plan did not start the program", "prog-ran")
        self.contains("--plan describes the hand-over", "exec: bin-path")

    def execute(self) -> int:
        """Run the complete suite and return its process exit status."""
        self.log_debug()
        self.ensure_binary()
        os.chdir(self.root)
        if self.golden_tests():
            return 0
        self.example_plans()

        if shutil.which("curl") is None:
            print(
                "[test] SKIP behavioural tests: curl is not installed", file=sys.stderr
            )
            print(f"[test] ran {self.assertions} assertions")
            return 1 if self.failures else 0

        with tempfile.TemporaryDirectory() as directory:
            self.temp = Path(directory)
            self.input_tests()
            self.trace_tests()
            self.embedded_tests()
            self.sandbox_tests()
            self.url_pipeline_tests()
            self.command_tests()
            self.shim_tests()

        print(f"[test] ran {self.assertions} assertions")
        if not self.failures:
            print("[test] all passed")
        return 1 if self.failures else 0


def main() -> int:
    """Entrypoint kept separate for straightforward traceback behavior."""
    return Suite().execute()


if __name__ == "__main__":
    raise SystemExit(main())
