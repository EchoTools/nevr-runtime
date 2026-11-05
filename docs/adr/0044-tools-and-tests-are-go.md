---
# atwd read contract
read: whole
read_via: cat docs/adr/0044-tools-and-tests-are-go.md
audience: all
---

# ADR 0044: tools and their tests are written in Go; Python only with a recorded reason

Status: proposed. Applies to everything under `tools/` (tools, checks, code generators, test helpers), to `tools/tests/`,
and to every test of a tool, wherever it lives. Amends ADR 0041 rule 4 where it names Python. Tracked by #2132.

## Context

`just test-tools` runs on every pull request (AGENTS.md, "Builds": "Every PR, whatever it touches, also runs
`just test-tools`"). It is 223 Python test files (`tools/tests/test_*.py`, 51,136 lines, about 3,900 cases) over 103
Python files of tools (27,776 lines, `git ls-files 'tools/*.py'` outside `tools/tests`). Measured for #2132 on a 16-core,
32-thread Linux machine:

- 911 to 1,135 s serial, in each full run kept in `runs.jsonl` under the work root.
- 95.7 s under pytest-xdist (16 workers) with plugin autoload off, using 1,055 CPU-seconds.
- Where the time is: interpreter starts (`test_rollback_beta` makes 729 successful `execve`, about 700 of them
  Python: 354 fake `gh` and 252 fake `release-signer`, both Python scripts, and 93 more `python3` starts); nested pytest
  starts (5.2 to 5.9 s each with the machine's 22 auto-loaded plugins, 0.8 s without); fixed waits (an idle
  `serve_forever` takes 0.49 s to shut down, two `time.sleep(11)`, a 15 s kill-after); and real external toolchains
  (Wine, pwsh, JVM tools, compilers).

Parallel workers and removing the server waits recover much of the wall time in Python as well (#2132, phase A). What
they do not remove is the per-call cost of an interpreter: every tool run and every fake command a test makes is a
Python start, and the suite makes thousands. Nor do they give a test control of time: a Python timeout is tested by
waiting it out, or by a clock each tool has to carry by hand.

What the same work costs in Go, measured on the same machine at the same load:

- A fake command as a compiled Go program: 200 calls in 0.36 s (1.8 ms a call). As the Python shim the tests use
  today (`#!/usr/bin/python3` importing a module): 200 calls in 7.45 s (37 ms a call). As a bash script: 0.50 s.
- `go run` of a tool whose build is cached: 0.04 to 0.06 s, about one Python start.
- A test that a socket read gives up after 130 s and not before, under `testing/synctest`: 0.003 s, where the Python
  test of a 10 s patience sleeps 11 s.
- `gremlins` v0.6.0, a mutation tool, runs on Go 1.27.3 against that test: 5 mutants, 4 killed, 1 not covered (a
  constant expression), in 1.2 s. It makes operator mutants only (arithmetic, comparisons, increments, negations); it
  does not delete a call or change a constant.

Go's standard library covers what the tools do most: PE and ELF (`debug/pe`, `debug/elf`), zip, JSON, HTTP, process
control and signals; a tool builds to one static binary; `testing/synctest` gives a test a fake clock.

The tree already builds and tests Go: `tools/symcache` (run by `.github/workflows/echo-symbols-sync.yml`),
`tests/system`, `tests/plugins`, `tests/quest`, `tests/crash-ingest`. The builder image pins Go
(`.github/builder/Dockerfile`, `.github/workflows/build.yml` "Toolchain versions").

The Python tools import four third-party libraries. Each has a Go counterpart:

| Python | Used for | Go |
| --- | --- | --- |
| PyYAML | `tools/scenario/run_scenario.py`, nine tests that read workflow files | `go.yaml.in/yaml/v3` (already in `tests/system/go.sum`) |
| `zstandard` | `tools/echomod/` only | not needed while echomod stays (table below) |
| `pywinrm` | `tools/winvm/systest.py`: a session to `http://<vm>:5985/wsman` with NTLM, `run_ps` with exit status, stdout and stderr, read and operation timeouts | `github.com/masterzen/winrm`: `ClientNTLM` transport, `NewEncryption("ntlm")` message encryption over HTTP, `RunPSWithContext` (stdout, stderr, exit code), timeouts in `Parameters` and `NewEndpoint` |
| `minidump` | `tools/winvm/dump_stacks.py`: modules (base, size, name), the full-memory ranges, each thread's id and `Rip`/`Rsp` | `github.com/go-delve/delve/pkg/proc/core/minidump` (MIT): `Open`, `Modules` (`BaseOfImage`, `SizeOfImage`, `Name`), `MemoryRanges` with `ReadMemory`, `Threads` (`ID`, `Context.Rip`, `Context.Rsp`) |

## Decision

1. **Go is the language of tools and their tests.** A new tool, check, code generator, test helper or test is Go.
2. **Python only with a recorded reason.** A Python file may stay only as a row of "Where Python stays" below, with
   its reason and the event that ends it. A reason is one of:
   - a library the tool needs has no maintained Go equivalent (named, and the Go candidates looked at);
   - the file is being retired by a named issue;
   - the port costs clearly more than it saves, with the numbers: lines to port, test seconds saved, how often the
     file changes.

   "It works", "it is quicker to write in Python" and "the tests are already Python" are not reasons.
3. **Ports follow the units of #2132.** A Python tool not in the table is ported by its unit. Until then a fix may stay
   Python, and so may a change to a test's setup that adds no behaviour to a tool (an argument, an environment
   variable); a pull request that adds behaviour to a Python tool (a new option, step or output) ports it first.
4. **Shell is for sequencing.** A shell script that runs other commands one after another and reads nothing back but
   their exit status may stay shell. A script that parses output, loops, polls, waits or times anything is a tool and
   is Go, except:
   - the game launch scripts and the libraries they source stay shell: `launch-client.sh`, `launch-server.sh`,
     `verify-server.sh`, `tools/lib/game_install.sh`, `tools/lib/work_root.sh`. They start a game process and are
     read and changed as the sequence of commands they run, and each of their waits is a wait on a command they call.
     Their tests are Go, and a test drives their waits through those commands (a fake `timeout`, a fake `wine`), not by
     changing the scripts;
   - PowerShell and shell that players run stay in their language: `install.ps1`, `uninstall.ps1` and
     `quest-diagnose.ps1` ride in the Windows zip (`ZIP_MEMBERS` in `tools/package_release.py`), `quest-install.ps1`
     and `.sh` are the installers a player is handed (ADR 0023), and `tools/quest-diagnose.sh` is the zip script's twin
     for Linux and macOS. They run on players' machines, which have PowerShell or a shell and no Go. Their tests are Go.
5. **One module.** `tools/go.mod` (module `github.com/EchoTools/nevr-runtime/tools`) holds every Go tool;
   `tools/symcache` is a package of it. A command is `tools/cmd/<kebab-name>/`, and the binary takes the directory's
   name. Shared code is `tools/internal/<package>/`; the test kit is `tools/internal/testkit/`. A third-party module
   needs a line in the pull request saying why the standard library does not do it.
6. **`main` is a shell around `run`.** A command's `main` is
   `os.Exit(run(ctx, args, os.Getenv, os.Stdin, os.Stdout, os.Stderr))`. Tests call `run` in the test process; they
   start the binary only where the process boundary is what is tested (an exit status from a signal, a process group,
   an environment that must not leak to a child).
7. **Built by `go`, from `just`.** `just go-tools` runs `go -C tools build -trimpath -o ../build/go-tools/ ./cmd/...`;
   a recipe that runs a tool depends on it and runs `build/go-tools/<name>`. CI runs the same recipe in the builder
   image. A generator that CMake runs during a build is a binary that a CMake custom command makes with `go build`
   (CMake starts Go's own build tool and compiles no Go itself), and such a generator imports only the standard library,
   so a CMake build needs no network.
8. **Tests are `go test`, standard library only.** Each test calls `t.Parallel()` or says in a comment why it cannot.
   State lives under `t.TempDir()`. A tool reads its environment through the `getenv` it is given, never from
   `os.Getenv` below `main`, so no test needs `t.Setenv`, which forbids `t.Parallel()`.
9. **No sleeping to wait.** A test never calls `time.Sleep` to wait for something to happen. A timeout, poll interval
   or delay a tool uses is a parameter whose default is the production value. A test that a timeout fires, or does not
   fire, runs under `testing/synctest` against the fake clock. A test that waits for an event outside the test process
   (a child exiting, a line in a file, a write to a fifo) blocks on that event with a deadline: `testkit.WaitLines`
   for a fifo, `cmd.Wait` for a child, `testkit.WaitFor` for state that cannot be waited on, each failing the test with
   what it waited for when its budget (30 s unless the test gives another) runs out.
   `tools/internal/policy` scans the Go files under `tools/` for a `time.Sleep` that breaks this.
10. **Fake commands are Go.** A fake external command is the test binary itself, linked into `t.TempDir()` under the
    command's name: `testkit.Main` in the package's `TestMain` runs the fake named by `os.Args[0]` and exits, and the
    fake is ordinary Go in the test package. It records its argv, working directory and the environment keys the test
    names as one JSON line per call (`testkit.Fake`, `Calls`). It can replace itself with the real program of the same
    name (`Call.Exec`). No fake is a script in an interpreter.
11. **A hermetic environment.** Every command a test starts gets `testkit.Env`: `GIT_CONFIG_GLOBAL=/dev/null`,
    `GIT_CONFIG_NOSYSTEM=1`, a fixed identity and `commit.gpgsign=false` through `GIT_CONFIG_COUNT`, `HOME`,
    `XDG_*_HOME`, `NEVR_WORK_ROOT` and `TMPDIR` under `t.TempDir()`, the caller's `PATH`, and no `DISPLAY`,
    `WAYLAND_DISPLAY` or `GNUPGHOME`, plus only what the test adds. Nothing of the user's configuration reaches a test.
12. **Toolchain tests are gated by their inputs.** A test that runs a real Wine, pwsh, JVM tool, compiler, `cargo`,
    `ninja`, or a nested test suite is a toolchain test. The gate table, `tools/internal/gate`, names each one (a Go
    test, or a Python test class by its pytest node id) and its inputs (paths and globs in the tree). The runner behind
    `just test-tools` computes the files that differ from the merge-base with `origin/main` (committed, staged,
    unstaged or untracked); a toolchain test whose inputs are all unchanged is skipped (a Python class through
    `pytest --deselect`, a Go test through `testkit.Toolchain`), and the run record names it with its inputs. It
    always runs under `just test-tools --all`, which `just verify` uses, and whenever the runner cannot compute the
    changed files. A table entry whose test or input no longer exists fails the gate's own test. Until the unit that
    builds the gate (G1b of #2132) lands, every test runs.
13. **Logs.** A Go tool logs through `tools/internal/runlog`: `log/slog` with the JSON handler, a durable file
    `$XDG_STATE_HOME/nevr-runtime/<tool>.jsonl` (with `$XDG_STATE_HOME` at its default when unset), and a failure to
    open or write it is a line on standard error and a non-zero exit (`runlog.Run`). A port keeps the file name and the
    fields of the Python tool's log, so a query over the history keeps working.
14. **A port proves itself.** A pull request that ports a tool:
    1. runs the Python tests that start the tool as a process against the Go binary, once, before it deletes them,
       and shows them passing;
    2. runs `gremlins` over the ported packages, while it runs on the Go version the builder image pins, and lists
       each surviving or uncovered mutant with the test that now kills it, or why it is equivalent;
    3. adds the AGENTS.md hand mutants for what `gremlins` does not make: for each changed line that calls something
       or sets a constant, the line deleted or the constant changed, and the test that then fails.

## Where Python stays

| File(s) | Reason | Ends when |
| --- | --- | --- |
| `tools/echomod/**` (31 Python files, 8,708 lines) and the `generate-combat-overrides` recipe | Port cost against saving: no test in `tools/tests` names it, so a port saves no test time; 8,708 lines; five commits in its history, the last that changed what it does is `2ef919f32c88738dae649a0c1bbb1924d2c1a8d5`, since which it has had a 1-line path change (`2e23a64a5a9374d45e0d38a23d404a002531e5ae`), a mode bit (`c780136e41115d44e043abfdd36c3e3fb13bad7e`) and its README (`6cea8e3e684b2ec08ac462faa93c95e85210f98e`) | a pull request changes its behaviour; that pull request ports what it changes |
| `tools/run_tool_tests.py` | Retired by unit G1a of #2132 | G1a merges |

`tools/winvm/` is not in the table: both of its libraries have a Go counterpart (Context), and it ports in its own unit.

## Finding a violation

- `git ls-files 'tools/*.py'` (git's pathspec `*` crosses directories): every file not in the table above is either on
  the port list of #2132 or a violation. `tools/internal/policy` holds that list (`python_to_port.txt`) and its test
  fails on a `.py` file that is neither on it nor in the table, on a listed file that is no longer tracked, and on a
  list longer than the one at the merge-base with `origin/main`: the list only shrinks.
- `git grep -nE 'time\.Sleep\(' -- 'tools/*.go'`: a hit in a `_test.go` file is a violation unless it is inside a
  `synctest.Test` bubble; a hit in a tool is a violation unless its duration is a parameter. The policy test makes the
  same check on the syntax tree.

## Alternatives

- **Keep Python and make it parallel.** pytest-xdist with plugin autoload off brings the suite to 95.7 s (measured for
  #2132) and removing the server waits to a little under that. It leaves an interpreter start in every tool run and
  every fake command, thousands per run, and a timeout is still tested by waiting it out. It is phase A of #2132 and is
  kept; it does not replace this decision.
- **Fake commands as shell scripts.** 0.50 s for 200 calls against 0.36 s in Go, and a script cannot share a type or a
  helper with its test, record a call as a JSON line without parsing, or replace itself with the real program without
  `exec` plumbing in every script.
- **Go for the tests, Python for the tools.** A Go test of a Python tool starts the tool as a process for every case,
  which is the interpreter cost again, and it cannot reach inside the tool for time or for a socket.

## Consequences

- One language and one runner for the tool suite; pytest and pytest-xdist leave the quick check when the last unit
  lands, and `just verify` runs `go test` over `tools/`.
- A contributor needs Go 1.25 or later (for `testing/synctest`). Go is already needed to build: `configure` depends on
  `generate-symcache`, which runs `go run .` in `tools/symcache`. What goes is the need for Python and pytest.
- `.github/workflows/echo-symbols-sync.yml` takes its Go version from `tools/go.mod` (`go-version-file`), and its
  `go run .` and `go test ./...` still run in `tools/symcache`, now a package of that module.
- Test time on a pull request no longer grows with interpreter starts or fixed waits; a toolchain test costs time only
  on the pull requests that change its inputs.
- A regression a gated test would catch, made by a file not in its inputs, is found at the next `--all` run (the
  release cut), not on its pull request. Inputs are therefore declared wide (directories, not single files).
- Pull requests open when this ADR merges keep their Python tests; a Python file they add after it needs a row in the
  table or a line on the port list from a maintainer, which the policy test makes visible.

## Not claimed

- A wall time for the suite: the target and each unit's measured effect are in #2132.
- That `github.com/masterzen/winrm`'s NTLM encryption works against the test VM's WinRM settings, or how much memory
  Delve's minidump reader needs for a full-memory dump of the game: both are checked by the WinVM unit on the VM.
- That every Python test has a one-to-one Go counterpart; a port may merge or split tests, and says so.

## References

- ADR 0041, rule 5: "A language with its own build tool is built by it. The Rust installer (`installer/`) is built by
  Cargo and the Go test modules (`tests/`) by `go`."
- `testing/synctest` (`go doc testing/synctest`): "Within a bubble, the time package uses a fake clock" and "Time in a
  bubble only advances when every goroutine in the bubble is durably blocked."
- AGENTS.md, C++ Mingw Addendum, "No jank": "No `Sleep()` for timing. Use waits on events or fences."
