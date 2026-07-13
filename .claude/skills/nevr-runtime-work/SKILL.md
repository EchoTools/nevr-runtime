---
name: nevr-runtime-work
description: Execution harness for any implementation, investigation, or ops task in the nevr-runtime repo (Windows DLL patches AND the new Quest/arm64 .so port). Walks the repo's discipline gate by gate — orient, ReVault-before-assert, plan-before-code, prologue-validated patching, BAC/TDD, closed-loop verification, no-prod-deploy, commit hygiene — with a verification action and an abort condition at every gate. Invoke BEFORE starting any task in this repository.
---

# /nevr-runtime-work — the execution harness for this repository

## This IS

- The harness that executes this repo's `CLAUDE.md`. The rules live there;
  this skill forces you to walk them in order and produce proof at each gate
  before passing it.
- A gate sequence. Every gate has a **verification action** (something you
  run or read) and an **abort condition** (when you stop and escalate).
  Passing a gate without its verification action is skipping the gate.

## This is NOT

- A replacement for `CLAUDE.md`. It cites it; it never overrides it. If this
  file conflicts with `CLAUDE.md`, `CLAUDE.md` WINS — note the conflict and
  continue under its reading.
- Optional for "small" tasks. A one-line address constant that was never
  validated against the binary is how you brick a patch. Small is where this
  decays.

## Gate 0 — Orient (before anything else)

**Do:** read `CLAUDE.md` (this repo), `git log --oneline -20`, `git status`,
`BUGS.md` (the echovr.exe *binary-bug* audit — bugs in the game, not our
code), `FIXPLAN.md`, and any doc under `docs/` that names your subsystem.
For Quest/arm64 work also read `docs/2026-06-09-levr-porting-analysis.md`
and `extras/docs/docs/BUGSPLAT_HOOK_IMPLEMENTATION.md`.

**Verification action:** your plan names the files you oriented on and the
one-line current state (what's built, what's open) you took from them.

**Abort if:** you cannot establish repo state or the task's subsystem from
these — escalate to Spritz rather than exploring blind.

## Gate 1 — ReVault before you assert (measure, never guess)

**Rule:** before asserting ANY fact about the binaries — a function's
address, what it does, a struct offset, a vtable slot, an import, a
prologue's bytes, how the game loads a library — look it up in **ReVault
first**. Not to confirm a memory; to find out. Never assert from training
[CLAUDE.md §ReVault; §NEVER ASSERT FROM TRAINING].

- Windows: `revault fn show <0xVA> --binary echovr.exe` / `pnsrad.dll` /
  `gameserver.dll`. ImageBase `0x140000000`.
- **Quest: `libr15.so` is ReVault binary `id=576` (arch aarch64, ELF).**
  arm64 addresses are NOT the x86 ones — never reuse an echovr.exe VA for
  the .so. Cross-reference symbol *names* between the two, not addresses.
- If ReVault doesn't have it, say so — don't guess. For raw ELF facts
  (`DT_NEEDED`, exports, sections) `readelf`/`nm` on the real .so is ground
  truth and is instant.

**Verification action:** every address/behavior claim in your plan or report
carries its source — a `revault fn show` line, a `readelf`/`nm` output, or an
xref. An unsourced claim is written `[UNVERIFIED]` or not written.

**Abort if:** a cog hands you an address with a story but no ReVault/ELF
citation — re-verify it yourself before patching on it. A wrong address in a
blind patch corrupts the process.

## Gate 2 — Plan before code (two review passes)

**Do:** non-trivial changes require a written plan BEFORE implementation,
through ≥2 self-review passes for gaps in testing, error handling, and edge
cases [CLAUDE.md §Methodology]. State the testing strategy in the plan.

**Verification action:** the plan exists as text, names its BACs, and states
how each will be tested (automated first; manual only for what can't be).

**Abort if:** you cannot write the plan or its test strategy — you don't yet
know what you're building. Return to Gate 1.

## Gate 3 — Spec the behavior: BAC + TDD

**Do:** every feature ties to a **BAC** (Behavioral Acceptance Criterion)
and has a **test that fails first, then passes** [coding standards §4].
Test surfaces here: Go system tests (`tests/system/`), plugin tests
(`tests/plugins/`), C++ GTest under Wine (`just test-auth-unit`). For the
Quest .so: a ground-truth test that asserts the built artifact's exported
symbols / ELF shape, since on-device runtime tests need hardware.

**Verification action:** quote the RED (failing) run, then the GREEN
(passing) run. No feature is "done" without a test tracing to its BAC.

**Abort if:** you can't express the behavior as a testable BAC — go back to
Gate 2.

## Gate 4 — Implement inside the guardrails

- **Prologue-validate before ANY binary patch. Never blind-write.** Read the
  expected bytes at the target and confirm them before detour/patch
  [CLAUDE.md §Guardrails]. This holds for arm64 too — verify the AARCH64
  prologue, not an assumed one.
- **Hook frequency analysis before adding Sleep/yield** — know if a hooked
  function is per-frame / per-tick / per-event first.
- **Never modify `src/legacy/`** (frozen v1). **Never commit generated
  protobuf** (`gen/cpp/*.pb.*`) without regenerating from BSR.
- **Logging** via `Log(EchoVR::LogLevel::…, …)` (Windows) / the platform log
  sink (Android `liblog`/`__android_log_print`). **Config from files/flags,
  never environment variables** [coding standards §7].
- **Injection model — know which platform you're on:**
  - *Windows:* the game statically imports `BugSplat64.dll` (and
    `LibOVRPlatform64_1.dll`); nevr-runtime ships replacements under those
    names to load at startup before `WinMain`. That is the whole trick.
  - *Quest/Android:* `libr15.so`'s crash reporter is **statically-linked
    google_breakpad — there is no BugSplat .so to hijack.** The startup
    vector is replacing an **APK-bundled `NEEDED` lib** (verified list:
    libandroid, libEGL, libGLESv3, libOpenSLES, liblog, libvrapi,
    **libovrplatformloader**, libdeviceconfigclient-jni, libc++_shared, libm,
    libdl, libc). `libovrplatformloader.so` mirrors the Windows LibOVR hijack
    and is the natural pick — but PROVE it's replaceable (bundled in the APK,
    our replacement exports the symbols the game resolves) before building on
    it. Alternative: `patchelf --add-needed` on a repacked libr15.so.
  - The Android crash reporter we ship is a **breakpad-based `.so`** (real
    minidumps + symbolicated backtraces), not a BugSplat clone.

## Gate 5 — Verify it landed (close the loop)

**Do:**
1. **Build after each logical step**, not only at the end [CLAUDE.md
   §Incremental verification]. `just build` / `just dist` (MinGW);
   the Android preset for arm64.
2. **When something concrete is broken, read the failing thing's OWN log
   first** — the server's application log, the build's own error — not
   `journalctl`, not hypothesis chains [AGENTS.md §DEBUGGING DISCIPLINE].
3. **Success derives from the artifact/runtime, never a proxy.** For a .so,
   verify the real ELF (`readelf -d`, `nm -D`) exports what you claim. For a
   server change, run it and read its log line proving the behavior.
4. **Performance claims need load testing** — idle measurements aren't
   validation; state what was tested.
5. **On-device gaps are recorded, not hidden.** If a Quest artifact can only
   be structurally verified (no headset in the loop), say so explicitly and
   name what remains to test on hardware.

**Abort if:** the only evidence of success is an exit code or a cog's
say-so — go get the substrate evidence.

## Gate 6 — Record and commit

- **PRODUCTION DEPLOY IS FORBIDDEN without Andrew's explicit, per-instance
  approval in the live conversation** [CLAUDE.md §Production Deployment]. No
  docker build/push, no ssh to fortytwo, no container lifecycle, no
  cross-repo deploy. Starting a *local* test server is not deployment.
- **BUGS.md** is the echovr.exe binary-bug audit — add binary findings there
  in its existing format. Project/tooling bugs and observations follow the
  `bugs-ledger` conventions.
- **Never leave uncommitted code.** Commit as
  `Spritz Metis Sprock <spritz@sprock.io>` (author AND committer), never as
  Andrew, conventional-commit subject (`feat`/`fix`/`docs`/`chore`/
  `refactor`/`test`) [coding standards §3].
- **Reusable Go tool over throwaway heredoc** — if you run a sequence twice,
  script it [coding standards §5].

## Pre-done self-review — the failure classes

Before declaring done, state one sentence per class:

1. **Unverified address** — does any patch/hook rest on an address or
   prologue not confirmed in ReVault or the real binary?
2. **Blind write** — did any binary patch skip prologue validation?
3. **Unwired promise** — does any flag/config field/doc claim lack a consumer
   and a test?
4. **Untested feature** — does any feature ship without a BAC-traced test
   (red→green quoted)?
5. **Proxy success** — does any "done" derive from an exit code or a cog's
   report instead of the artifact/runtime/substrate?
6. **Wrong-platform assumption** — did I apply a Windows injection/ABI
   assumption to the arm64 target (or vice-versa)?
7. **Silent deploy** — did anything touch a production surface without
   per-instance approval?

## Common Mistakes

- Reusing an echovr.exe VA on `libr15.so`. Different arch, different image.
- Assuming a Quest BugSplat .so exists to hijack. It doesn't — breakpad is
  static; hijack a bundled `NEEDED` lib instead.
- Blind-patching from a training-remembered prologue. Read the bytes.
- Treating a cog's report as verification — a cog is a good pair of hands and
  a bad memory. Its claims re-enter at Gate 1 and get re-verified.
- Leaving a build "for review" uncommitted, or committing as Andrew.
