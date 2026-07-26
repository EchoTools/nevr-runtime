# AGENTS.md — start here

Entry point for any agent working in this repository. It exists so that "I did
not know that document was binding" is never available as an explanation.

Everything listed under **Binding** below is mandatory. Not advisory, not
"consider" — a change that violates one is rejected in review regardless of
whether it works.

---

## Read in this order

| # | Document | When | Why it binds |
| - | -------- | ---- | ------------ |
| 1 | `~/src/metis-core/CPP-MINGW-ADDENDUM-GENERIC.md` | **Before any C++, CMake, justfile or preset change** | Toolchain hard stops: `-Werror`, no C-style casts, no `catch(...)`, no throw across a DLL boundary, no I/O from `DllMain`, Ninja only, vcpkg-manifest deps only |
| 2 | `CLAUDE.md` | Before any task | Project conventions, build/test commands, guardrails, deploy prohibition, onboarding decisions |
| 3 | `docs/standards/verification.md` | **Before writing "verified" or closing a ledger entry** | What counts as evidence, and the requirement to falsify every check you add |
| 4 | `docs/standards/logging.md` | Before adding or changing any log line | Log shape, subsystem tags, levels, Hard Stops |
| 5 | `BUGS.md` | Before starting; and to record findings | The work ledger. `# NEVR Runtime Source Bugs`, N-prefix IDs |
| 6 | `docs/process/driver-charter.md` | When dispatching or coordinating work | Fleet protocol, decision-lines, verify gate |

`.claude/skills/nevr-work/` walks these gate by gate. Invoke it before starting
work in this repository — it is the enforcement path for the documents above.

---

## Binding

**`CLAUDE.md`** — project conventions and guardrails. Note in particular:
production deploy is forbidden without per-instance owner approval; never modify
`src/legacy/`; prologue-validate before any binary patch; never commit generated
protobuf without regenerating from BSR.

**`docs/standards/verification.md`** — the meaning of "verified". Ranks evidence,
defines the four shapes of a component that reports success while doing nothing,
and requires that every check added to `just verify` be broken and observed
failing before it is trusted. Written because seven checks authored in a single
session were silently inert.

**`docs/standards/logging.md`** — what a component must say. Subsystem tags,
level guidelines, and ten rules including "silence is not success".

**`BUGS.md`** — the ledger. Two ID namespaces in one file: bare integers audit
the *original game binary*; `N`-prefixed IDs are this project's own work ledger.
Next ID: `grep '^### N' BUGS.md`, take the highest and add one. Every entry
carries the **invariant** it protects. Amend entries; never rewrite them —
closed entries live in git history and a silent deletion is indistinguishable
from the finding never existing.

**`~/src/metis-core/CPP-MINGW-ADDENDUM-GENERIC.md`** — cross-compilation hard
stops. Read in full before touching the build.

On conflict: the CPP addendum and `CLAUDE.md` win over everything here.

---

## The gate

```sh
just verify     # build + fail-closed GTest under Wine + all invariant sensors
```

It must exit **0**. Read the exit code from a file, never `$?` after a pipe —
`just build` greps its own output and always exits 0, so its exit code is a
proxy, not a signal.

Server behaviour is tested **only** with `./launch-server.sh`. Do not modify that
script and do not invoke `echovr.exe` with other arguments. Allow ≥45s from
process start before judging a server hung (`CLAUDE.md` §Startup Timing).

---

## Repository layout

```
BUGS.md            work ledger (N-prefix) + original-binary audit
CLAUDE.md          project conventions — tool-loaded, stays at root
README.md          what this project is
AGENTS.md          this file

src/gamepatches/   BugSplat64.dll — hooks, CLI, mode patches, crash recovery,
                   config, module/plugin loading, in-process gameserver
src/modules/       runtime-loaded modules: platform-compat, token-auth, ws-bridge
src/common/        libcommon.a — logging, globals, symbols, hooking
src/legacy/        FROZEN v1 — never modify
plugins/           optional plugins loaded from plugins/ next to the game binary
tools/             build/verify tooling (hook invariants, symbol corpus)
docs/
  standards/       binding standards — logging, verification
  process/         governance — driver charter
  guides/          runbooks and how-tos
  reference/       format specs and address maps
  design/          plans and analyses (dated where point-in-time)
  primers/         multi-session handoffs for unfinished work
  audits/          audit outputs and evidence
```

**Naming.** Root-level entry points are `SHOUTY.md` by convention. Everything
under `docs/` is `lowercase-kebab-case.md`. Point-in-time documents carry an
ISO date prefix (`2026-07-26-thing.md`); durable ones do not.

---

## Before you finish

- `just verify` exits 0, and you quoted its tail.
- Every check you added was **falsified** — broken, observed failing, restored.
- The ledger entry states its evidence rank (`docs/standards/verification.md`).
- Commits: author `agents@sprock.io`, `--no-gpg-sign`, conventional prefix, one
  logical change each. Verify with `git log --format='%h %G? %an %ae' -1`.
- No worktrees left behind; scratch under `/var/tmp/work-nevr-runtime/`, never in
  the repo and never `/tmp` (RAM-backed on this host).
- Anything unverified is named as unverified, in the same breath as what is.
