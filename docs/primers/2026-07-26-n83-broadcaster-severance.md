# Primer — N83: the broadcaster self-collision, and how to finish it

> **UPDATED 2026-07-26 (later, same day).** This primer was written before three
> things happened. Read this block before the body — parts of the body below are
> now historical.
>
> 1. **The `0xF87AA0` half is fixed.** It is no longer an unconditional
>    `if (g_isServer) return;`. It is a real null-guard on the exact fault chain
>    (`inner = *arg1`; `table = *(inner+0x5e0)`; AV at `[table + bucket*4]`), so
>    the broadcaster's listener dispatch runs normally when the structure is valid.
> 2. **The originating commit WAS found**, contradicting this primer's body: it is
>    `7beccee` (2026-03-24, Andrew Bates), whose message says *"Entity property
>    dispatch null-guard (fcn.140f87aa0) to prevent AV in server mode from
>    uninitialized client-side state."* The earlier "no originating commit" claim
>    came from `git log -S` scoped to `mode_patches.cpp` — a file created by a
>    LATER refactor, so that search could not have found it. The recorded intent
>    does not make the diagnosis correct (the explanation is falsified by the
>    function's own callers), but there WAS a builder and a reason.
> 3. **The open question now has partial data.** Hook-entry counters on a live
>    headless server report `listen_entries=82 dispatch_entries=1` with **0** guard
>    trips, stable across 4 samples. So the hooks run and the AV does not occur
>    during boot or registration. It is NOT settled: `dispatch_entries=1` shows the
>    message path is unexercised when idle, so gameplay/entrant churn is untested.
>    A Decision-line is recorded in N83.
>
> Also relevant and not in the body: the dedicated server now reaches
> registration (N85 — an empty `std::function` handed to ixwebsocket was killing
> it), and the per-frame tick that reports these counters had to be moved off a
> dead hook (N86).

**For the next agent.** This is multi-swipe work. Read this before touching
`mode_patches.cpp`, `echovr_functions.cpp`, or anything named `ENGINE_ENTITY_*`.

**Status at handoff (2026-07-26):** diagnosed, tripwired, **not fixed**. The fix
is a behaviour change on the crash-recovery path and must not be made by
reasoning alone. Everything below is measured; where it isn't, it says so.

---

## The finding in one paragraph

`src/abi/echovr_functions.cpp:102` assigns two live function pointers to
RVAs `0xF87AA0` and `0xF80ED0`. `src/runtime/patch/mode_patches.cpp:343`
installs MinHook detours on those same two RVAs, under the names
`ENGINE_ENTITY_PROP_DISPATCH` and `ENGINE_ENTITY_LOOKUP`
(`patch_addresses.h:297,303`). MinHook writes a `JMP` at the entry, so **our own
calls re-enter our own hooks.** `EngineEntityPropDispatchHook` does
`if (g_isServer) return;` — so on a dedicated server, all 13
`EchoVR::BroadcasterReceiveLocalEvent` call sites in
`src/runtime/server/gameserver.cpp` do nothing.

Those two addresses are not entity functions. Per disassembly:
`0x140f80ed0` is `CBroadcaster::Listen`; `0x140f87aa0` is
`CBroadcaster::ReceiveLocalEvent`, and it is the listener dispatcher —
`CALL qword ptr [R10 + 0x28]` at `0x140f87c90`, reached no other way.

## What is NOT wrong, and this is the important part

**The guard's arithmetic is correct.** `[inner+0x5e0]` genuinely is the first
thing `BroadcasterListen` dereferences (`0x140f80f17` → `0x140f80f1e` reads
`[RAX+0x3ff8]`), and the comment's claimed AV target is exact: a garbage pointer
of `0x10` gives `0x10 + 0x3ff8 = 0x4008`. **Somebody hit a real crash and wrote a
numerically sound guard for it.** Only the *name* was invented — and the name is
what made it destructive, because "return -1 when the listener table is unset"
reads as obviously load-bearing under the real name and as harmless render-state
cleanup under the fake one.

**Do not delete these hooks because the name was wrong.** The crash they were
written for may still exist. That is the open question.

## What is already in place (do not redo)

- **Static tripwire** `tools/verify_hook_invariants.py`, wired into
  `just verify`. Three checks: self-collision, identity pinning (38 addresses,
  prologue bytes from `echovr/bin/win10/echovr.exe`), double-detour. Known bugs
  are on an explicit register keyed by ledger ID and WARN; anything new is a
  hard failure. **When you fix N83, delete its entries from
  `KNOWN_SELF_COLLISIONS` — the checker then enforces it stays fixed.**
- **Runtime guard** `src/runtime/hook_guard.{h,cpp}` (N84) — detects a
  foreign detour on an address we own, including from third-party plugins.
- Ledger entries **N83** (Critical) and **N84** carry the full disassembly
  evidence. Read them before this doc if you want the raw bytes.

## The open question, and the only honest way to answer it

**Does the `+0x5e0` guard ever actually trip on a real server?**

Not statically determinable — I tried. It needs the line
`[NEVR.PATCH] Entity lookup null-guard triggered` (Listen) or
`[NEVR.PATCH] broadcaster dispatch guard tripped` (ReceiveLocalEvent) (`mode_patches.cpp:393`,
first 3 occurrences only) to appear, or not appear, in a real run's log.

- If it **never trips**: the guard is dead weight, the AV it was written for no
  longer occurs (plausibly fixed by N10's root-cause bit-0x1 clear, which landed
  later), and both hooks can be removed outright.
- If it **does trip**: something still passes an uninitialised broadcaster, and
  removing the guard reintroduces a crash. Then the fix is to scope the hooks —
  keep the AV guard on `Listen`, drop the blanket `return` on `ReceiveLocalEvent`
  — rather than delete them.

Those are different fixes. Guessing which one applies is exactly the failure
that produced this bug.

## The procedure, in order

1. **Tier-1 first (cheap, no game).** Build the fake-broadcaster tests for our
   own registration path. Today `CallbackRegistry` (`server_context.h`) has 19
   `uint16_t` handles all defaulting to `0`, and **a failed registration is
   indistinguishable from a successful one** — `0xFFFF` is `Listen`'s native
   failure sentinel and nothing checks for it. Fix that first; it is our bug, it
   needs no game binary, and you will want the assertion before step 3.

2. **One traced run, hooks ENABLED.** Capture the full log. Grep for
   `Entity lookup null-guard triggered`. That single line answers the open
   question above. Note `DISPLAY` state and allow ≥45 s before judging liveness
   (`AGENTS.md` §Startup Timing).

3. **One traced run, `InstallEntityHooks()` commented out.** Diff the two traces.
   Look for: does the server reach `Server Registered`; do the ServerDB→game
   injections land; does anything now crash that did not before.

4. **Only then** decide between delete-outright and re-scope. Whichever you pick,
   remove the `KNOWN_SELF_COLLISIONS` entries so the tripwire enforces it.

## Traps

- `src/gameserver/` is **dead code** — `CMakeLists.txt:192` has
  `# add_subdirectory(src/gameserver)`. The compiled copy is
  `src/runtime/server/`. The two files are near-identical, so editing the
  wrong one compiles clean and changes nothing. `just verify` guards this (N34).
- `revault fn show` CLI fails without `DATABASE_URL`; use the MCP tools. And note
  `revault_function` may return a *reconstruction* view rather than raw
  disassembly — that view is a validated layer and is legitimate to cite **with
  the VA**, but if you need instruction-level ground truth use
  `revault_disassemble`.
- `broadcaster_bridge.dll` also hooks `0x140f87aa0` (N84) and is deployed. If you
  change hook ordering, HookGuard will now tell you at ERROR level.
- Both `EngineEntityLookupHook` and `EngineEntityPropDispatchHook` are installed
  unconditionally from `initialize.cpp:329`, before CLI parsing — `g_isServer` is
  read at call time, not install time.

## Unrelated but outstanding

`N77`–`N82` on `main` collide with `N77`–`N79` on the unmerged branch
`wc-u2-reg-intent` (2026-07-25), which independently implemented pre-action
logging for the registration path. Their IDs have priority by date. This needs an
owner decision on which side renumbers; `N83`/`N84` were chosen to avoid making
it worse. See the existing N-ID collision record at the end of `BUGS.md` for the
established format.
