# ADR 0034: one hook manager per process owns every hooked slot, and plugins hook through it

Status: proposed. Applies to the Quest build and the PC client.

## Context

**Quest.** The sentinel's GOT backend (`src/quest/sentinel/got_hook.h`) keeps one registry of every slot it holds, writes
each slot with a compare-and-swap under one lock, refuses a slot another handle owns, and has a budget the tools test
checks (`src/quest/sentinel/hook_budget.h`). Internal calls are hooked through the build-time rows of ADR 0021
(`src/quest/sentinel/libr15_attach.h`).

The q15t plugin has a second mechanism with its own page-protection calls and stores
(`src/quest/plugins/q15t/src/got_swap.cpp`) and its own install helpers (`InstallImportHook`, `InstallChainedImportHook`
in `src/quest/plugins/q15t/src/import_hook.h`). Most of its modules install through those helpers, outside the sentinel's
registry. N151 names `got_swap.cpp` as an allowed page-protection site.

The two refuse each other. `InstallImportHook` with an expected address refuses a slot that holds anything other than the
game's function, and `GotHook` refuses a slot another handle owns. So a slot taken by one mechanism cannot be shared with
the other: the second consumer either refuses or must be written against the first.

On the release base, a third mechanism runs in the same process: the reference build's runtime (`libvrapi.so`, built from
the afkxr modules of [kotorvr/echoquest-combat](https://github.com/kotorvr/echoquest-combat)) swaps libr15 GOT and RELRO
words with its own helper (`SetWord` in `runtime/native/afkxr/afk_texres.cpp`).

**PC.** The host installs every hook through `PatchDetour` and records it with HookGuard (N84). A plugin that installs its
own detours declares `NEVR_PLUGIN_CAP_HOOKS_ENGINE`, whose comment says "a second MinHook instance does not share the
host's hook table" (`sdk/include/nevr/plugin_interface.h`). The SDK's `nevr/pc/hook_manager.h` is a scoped MinHook
lifecycle the plugin supplies itself.

Prior art: ByteHook, ByteDance's Android PLT hook library, manages every hook in the process from one place. It hooks
newly loaded libraries automatically, several hooks on the same function do not conflict, and each proxy reaches the
previous one through `BYTEHOOK_CALL_PREV()` ([bytedance/bhook](https://github.com/bytedance/bhook)).

## Decision

1. **One hook manager per process.** On Quest, the sentinel's GOT backend and its rows. On PC, the host's hook table
   behind `PatchDetour` and HookGuard.
2. **Several handlers per slot, in a defined order.** The manager keeps a chain for each slot and each handler reaches the
   next through the manager, so a second consumer of a slot never needs a second mechanism. The order and every
   insertion are logged (N17's per-hook outcome and aggregate line).
3. **Plugins hook through the host.** The plugin API gains a hook service on both platforms, which bumps
   `NEVR_PLUGIN_API_VERSION` and changes N114's statement. Once the service exists, a plugin's own page-protection call or
   GOT write is a violation of N151, and `NEVR_PLUGIN_CAP_HOOKS_ENGINE` is retired.
4. **Code ported from another project is ported onto the service.** The module track's afkxr modules keep their logic
   and drop their own word-swap helpers.
5. **A refusal names the slot, the owner chain and the reason** at Warning (`docs/standards/logging.md`, Rule 6).

## Finding a violation

Scope: `src/quest`, `src/runtime`, `src/core`, `sdk`. Out of scope: tests, documents, `src/reconstruction`, and the
manager's own files (`src/quest/sentinel/got_hook.cpp`, `src/quest/sentinel/libr15_attach.cpp`,
`src/runtime/hook/patching.h`, `src/core/hooking.h`).

- **H1.** A page-protection call (`mprotect`, `mmap` or `mremap` on an image page) or a store into a GOT, RELRO, vtable
  or data slot of a game library, outside the manager.
- **H2.** An install path that does not register with the manager: `InstallImportHook`, `InstallChainedImportHook`,
  `SwapWord`, `got_swap`, or a plugin's own `MH_CreateHook`.
- **H3.** Two installers anywhere in scope that name the same slot address or the same function.
- **H4.** A capability flag, comment or document that accepts a second hook table.

## Consequences

- q15t moves onto the manager before the module track adds modules, and the N151 allowlist loses `got_swap.cpp`.
- A probe, a fix and a feature can share one game function without knowing about each other.

## Not claimed

The cost of a chained call against a single hook on a hot path; the frequency measurements of #1737 decide where a full
replacement must be one function with no chain.
