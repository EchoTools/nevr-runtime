# AGENTS.md — start here

Entry point for any agent working in this repository. It exists so that "I did
not know that document was binding" shall not be available as an explanation.

Everything listed under **Binding** below binds. A change that violates one is
rejected in review regardless of whether it works.

---

## Normative language

These documents are addressed to an agent that acts. The words below are used in
their specification sense and carry obligation, not description.

| Term | Meaning |
| ---- | ------- |
| **SHALL** | An obligation on the agent. Doing the work without doing this is not doing the work. Non-compliance is a rejected change, not a judgement call. |
| **SHALL NOT** | A prohibition on the agent. There is no case in which this is the right move; if you believe you have found one, escalate with a Decision-line instead of proceeding. |
| **SHOULD** | A strong recommendation. Departing from it is permitted and **shall** be stated, with the reason, in the change that departs. |
| **MAY** | Genuinely optional. No justification is owed either way. |

"Must" and "never" are avoided deliberately. *Must* describes a property of the
world — "the build must succeed" — and *never* describes a frequency. Neither
places a duty on anyone. **Shall** and **shall not** bind a party who will act,
which is what every rule in these documents is doing.

**Every SHALL is mechanically enforced where it can be.** A SHALL that could be a
check in `just verify` and is not is a defect in this documentation, not a
standard — prose cannot enforce, and a rule nothing enforces has already been
violated silently at least once. Where a SHALL cannot be mechanized, it **shall**
say so and say why.

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
you **shall not** deploy to production without per-instance owner approval; you
**shall not** modify `src/legacy/`; you **shall** prologue-validate before any
binary patch; you **shall not** commit generated protobuf without regenerating
from BSR.

**`docs/standards/verification.md`** — the meaning of "verified". Ranks evidence,
defines the four shapes of a component that reports success while doing nothing,
and the rule that every check added to `just verify` **shall** be broken and
observed failing before it is trusted. Written because seven checks authored in a single
session were silently inert.

**`docs/standards/logging.md`** — what a component **shall** say. Subsystem tags,
level guidelines, and ten rules including "silence is not success".

**`BUGS.md`** — the ledger. Two ID namespaces in one file: bare integers audit
the *original game binary*; `N`-prefixed IDs are this project's own work ledger.
Next ID: `grep '^### N' BUGS.md`, take the highest and add one. Every entry
carries the **invariant** it protects. You **shall** amend entries and **shall
not** rewrite them — closed entries live in git history, and a silent deletion is
indistinguishable from the finding never having existed.

**`~/src/metis-core/CPP-MINGW-ADDENDUM-GENERIC.md`** — cross-compilation hard
stops. You **shall** read it in full before touching the build.

On conflict: the CPP addendum and `CLAUDE.md` win over everything here.

---

## The gate

```sh
just verify     # build + fail-closed GTest under Wine + all invariant sensors
```

It **shall** exit **0**. You **shall** read the exit code from a file and
**shall not** read `$?` after a pipe —
`just build` greps its own output and always exits 0, so its exit code is a
proxy, not a signal.

Server behaviour **shall** be tested only with `./launch-server.sh`. You **shall
not** modify that script and **shall not** invoke `echovr.exe` with other
arguments. You **shall** allow ≥45s from process start before judging a server hung (`CLAUDE.md` §Startup Timing).

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
src/legacy/        FROZEN v1 — shall not be modified
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

- `just verify` exits 0, and you **shall** have quoted its tail.
- Every check you added **shall** have been falsified — broken, observed
  failing, restored.
- The ledger entry **shall** state its evidence rank
  (`docs/standards/verification.md`).
- Commits: author `agents@sprock.io`, `--no-gpg-sign`, conventional prefix, one
  logical change each. Verify with `git log --format='%h %G? %an %ae' -1`.
- No worktrees left behind. Scratch **shall** live under
  `/var/tmp/work-nevr-runtime/`, and **shall not** live in the repo or in `/tmp`
  (RAM-backed on this host).
- Anything unverified **shall** be named as unverified, in the same breath as
  what is.
