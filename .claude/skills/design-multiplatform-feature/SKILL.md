---
name: design-multiplatform-feature
description: Use when designing or changing a feature that has to work on more than one platform (PC, Quest, a test double), or before writing platform-specific code. Seven steps, each with the place in the repo that says it - the port first, then where PC already does it, one core implementation, thin edges chosen by file, one name, proof on the test edge before each platform, and the review check that rejects a platform-only copy.
---

# /design-multiplatform-feature: one feature, one core, thin edges

`AGENTS.md` wins where this file disagrees with it. The repo is one codebase for two binaries (`AGENTS.md`, "Project":
"a feature is written once"), and a platform-only copy of shared logic needs a stated platform reason in its commit
message; "it's Quest" is not one. This skill is the order to work in. Each step cites where the repo says it, by file
and quoted text (a line number drifts; `grep -n` the quote). Where the repo does not say something yet, the step says
"after the C++23 move": the tree is C++17 (`AGENTS.md`, "Project"), and no rule below depends on concepts.

## 1. Start from the port

Name the core interface before any code: the operations the feature needs from the machine, and nothing else.

- The model is `src/core/cargo_env.h`: "everything the fetch, the store and the mount need from the machine, as one
  interface, so the same code runs on the Quest ... on the PC ... and in a test". Its class is `nevr_cargo::Env`.
- A second model is `src/core/update/fs.h` (`nevr_update::UpdateFs`): "behind one interface so the engine is the same on
  every platform and only the calls that touch a file differ", "Pure interface: no platform header, no I/O of its own".
- ADR 0003 ("Decision"): "Keep protocol and social rules in shared source and isolate platform-specific behavior in
  adapters."
- The shape to prefer is the Go shape (sprockee's decision of 11-14, ADR 0004 of nevr-apk): free functions declared once in the component's header and defined once per platform
  file, selected by CMake, with no base class. The test edge is its own test target that defines the same functions.
  Keep a virtual interface (the models above) only where a test must swap implementations at run time, and say so in the
  header.
- Either shape: results as a `bool` or a result struct, nothing that throws (`cargo_env.h`: "Nothing here throws; every
  call that can fail returns a bool or a result"), one operation per thing the machine does. A fact the machine did not
  give is absent, never a constant (`cargo_env.h`, `DeviceFacts`).
- Checking the shape: in C++17 the check is that the declarations compile in a host test with only a test edge behind
  them. After the C++23 move the shape is checked by a concept, and the check is a `static_assert` on each edge.

Proof: the port header is written and a fake of it compiles, before any edge exists.

## 2. Find where PC already does it

Search the tree for the PC implementation first and cite it with `file:line`.

- `AGENTS.md`, "Project": "Before adding any file under `src/quest/` that does what a `src/runtime/` file already does,
  use the shared one". `.claude/skills/nevr-work/SKILL.md`, "Mistakes that recur": "Taking a Quest-only copy of
  something `src/runtime/` already does".
- The search is stated as the commit evidence asks (`AGENTS.md`, "What a landed commit carries", "A search statement"):
  the `git grep` commands, the scope and what they found, over files and history.
- If PC has nothing, stop. Report that PC has no implementation, with the search statement, and do not write the
  one-platform version. `AGENTS.md`, rule 1 of "How an agent works here": with no pattern, "stop that action, report
  which pattern is missing".
- A port's header may name an edge that the tree lacks. `src/core/cargo_env.h` names a PC edge ("on the PC (Win32 files,
  libcurl)"); the tree has `PosixEnv` (`src/quest/cargo/posix_env.h`) and the test `FakeEnv`
  (`src/quest/tests/cargo_fake_env.h`) and no Win32 `Env`. Read the header's claim against the tree, and report the gap.

Proof: the PC `file:line`, or the report that there is none.

## 3. One core implementation, against the port

The logic lives once, in the unsuffixed files of the component's directory, `src/COMPONENT/` (COMPONENT is the
component's name, e.g. `src/cdn/`), written against the port. Today's tree still has `src/core`, `src/runtime` and
`src/quest`; a component moves to `src/COMPONENT/` when it is touched.

- `AGENTS.md`, "Shared Libraries (static)": `src/core/` holds "nEVR's own primitives ... *we wrote this*"; the dependency
  "runs `core` -> `abi`, never the reverse".
- A core file never names a platform. ADR 0003 ("Architecture"): "A shared translation unit compiles under MinGW and the
  NDK without Windows headers, MinHook, `EchoVR::g_GameBaseAddress`, `GetModuleFileNameA`, `ResolveModuleProc` or a game
  object layout." `src/core/cargo_run.cpp` and the other `cargo_*.cpp` files take only `nevr_cargo::Env`.
- Not models: `src/core/auth_token.h`, `crash_send.h` and `credential_write.h` carry `#ifdef _WIN32`. Do not copy them.
- Decisions that a host test can drive without the game go in a `<name>_rules.h` (`docs/standards/naming.md`, "File
  roles": "pure C++: what the component decides from values it is handed"; "Never holds ... a read of game memory, a call
  into the game or the OS, or a log write, other than through a seam the caller passes (an `Env`, an `Ops`)").

Proof: the core compiles for MinGW and the NDK with no platform header.

## 4. Edges are thin, chosen by file, selected in CMake

An edge implements the port for one platform and holds only what truly differs.

- Each component is one directory, `src/COMPONENT/`. Its core is the unsuffixed files; its platform edges sit beside
  them, and the suffix names the platform: `_windows`, `_posix` (Linux and Android), `_android` (Quest and Frame),
  `_quest` or `_frame` only for one headset, plus a test edge in the test target. Never `win32`: it reads as 32-bit. An
  existing `_win32` file (`src/runtime/lifecycle/update_fs_win32.cpp`) is renamed the next time it is touched.
  Example: `src/host/{host.cpp,host_windows.cpp,host_android.cpp}`. No per-platform top directory for new work
  (`docs/standards/naming.md`, "File roles": "The suffix names the platform").
- CMake picks the suffixes per target, by listing the file in the target that needs it, not by `#ifdef` in shared code.
  Today's examples, in the old layout: `src/quest/sentinel/CMakeLists.txt` lists `../cargo/posix_env.cpp`; `src/runtime/CMakeLists.txt` lists
  `lifecycle/update_fs_win32.cpp`.
- "Do NOT write new platform abstraction layers. Use existing `common/` patterns. If none exist yet, surface it - don't
  write ad-hoc `#ifdef` blocks" (`AGENTS.md`, C++ Mingw Addendum, "What Not to Do").
- An edge holds an OS call, a conversion for it (`update_fs_win32.h`: UTF-8 to UTF-16) or a library binding. A rule or a
  decision in an edge is a defect: move it to the core.

Proof: each edge's body is calls the OS requires; deleting an edge's calls leaves no decision behind.

## 5. One name across platforms, and a schema before the messages

- `docs/standards/naming.md`, "The name": "A component has **one name on PC and Quest.**" Every surface spells it the same
  way: file stems, `<name>_rules.h`, namespace, log events, config key, doc, mutant file and tests. The stem is the name
  plus at most one role suffix from the table in "File roles". The review checks it (naming.md, "What checks it").
- The messages that cross between pieces (core and edge, client and server, PC and Quest) get a schema before code.
  `AGENTS.md`, "Conventions": protocol messages are protobuf `rtapi::v1::Envelope`, generated from BSR by `just proto`;
  never edit generated files. "No hand-built serialization": use the library, and "Any function that produces wire-format
  bytes MUST have a test that parses its output". A record the runtime writes has its schema in an ADR
  (`docs/adr/0027-structured-logging.md`, "schema"; `docs/adr/0029-quest-bundled-content-pack.md`, `pack.json`: "schema 1").
  The repo has no single rule that says "schema first"; this step is those rules in order.

Proof: a grep for the component's name finds every surface; the schema is in the plan before the first serializer.

## 6. Prove it on the test edge first, then each platform

- The test edge first: the core runs against a fake with no game and no device (`src/quest/tests/cargo_fake_env.h`,
  "A memory-backed nevr_cargo::Env for the host tests"). "A test must fail without the change" (`AGENTS.md`, "Review,
  verification and landing"), with the mutant for each changed line ("What a landed commit carries").
- Then each platform's edge, built and run. A feature is not done until it builds and runs on PC and Quest.
  `AGENTS.md`, "What a landed commit carries", "Platform parity": "PCVR and Quest stay at parity", and the message says
  the other platform's status: done in this commit, already present (`file:line`), not applicable (the reason), or a
  linked twin issue filed in the same turn.
- What runs where: the branch runs the quick check ("Builds"); the PC login test and `just verify` run on main after the
  release cut ("Verification before landing"). Behavior only a headset shows is named as not run unless it was run.

Proof: the fake's test red without the change and green with it; the PC and Quest edges each built; what was not run, named.

## 7. Review check

Reject the change if it has any of these:

- A platform-only copy of shared logic: a `_quest`, `_android` or `_posix` file, or a `src/quest/` file, that does what
  the component's core or a `src/runtime/` file already does, with no stated platform reason (`AGENTS.md`, "Project"; `nevr-work`, "Mistakes that recur").
- A core file that names a platform: `<windows.h>`, `_WIN32`, `__ANDROID__`, a game address, a hook (ADR 0003,
  "Architecture").
- An edge holding logic that is not a platform difference (step 4).
- A second name for one component, or a stem that carries the platform (`docs/standards/naming.md`, "File roles", N-ROLES).
- A message with no schema, or serialization built by hand (step 5).
- A commit that does not state the other platform's status (step 6).

After the C++23 move, add: a shape that is not checked by a concept, and an edge with no `static_assert` against it.
