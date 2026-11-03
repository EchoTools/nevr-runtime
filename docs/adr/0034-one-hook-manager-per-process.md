# ADR 0034: one hook manager per process owns every hooked slot, and plugins hook through it

Status: proposed. Applies to the Quest build and the PC client. Supersedes the "Chaining" bullet of ADR 0019 section 5
(chaining refused in v1, "a later ADR").

## Context

**Quest.** The sentinel's GOT backend (`src/quest/sentinel/got_hook.h`) keeps one registry of every slot it holds, writes
each slot with a compare-and-swap under one lock, refuses a slot another handle owns, and has a budget the tools test
checks (`src/quest/sentinel/hook_budget.h`). Internal calls are hooked through the build-time rows of ADR 0021: a row's
activation stores the detour into the row's slot (`src/quest/sentinel/libr15_attach.h`), so rows are the same manager's
mechanism for calls no import slot reaches.

ADR 0019 section 5 already gives plugins a host API instead of a hook engine ("Plugins never carry a hook engine. The host
implements every write, under one lock, against one registry"), through `NevrQuestHostApi`
(`sdk/include/nevr/plugin_interface.h`), versioned by its own `struct_size`. Today that API offers the input filter and the
script redirect, not a general hook, and it refuses chaining: "Refused in v1: a second claim on a slot is a refusal ...
Chaining is a later ADR."

So the q15t plugin carries a second mechanism, with its own page-protection calls and stores
(`src/quest/plugins/q15t/src/got_swap.cpp`) and its own install helpers (`InstallImportHook`, `InstallChainedImportHook` in
`src/quest/plugins/q15t/src/import_hook.h`). Most of its modules install through those helpers, outside the sentinel's
registry. N151 names `got_swap.cpp` as an allowed page-protection site.

The two mechanisms cannot share a slot. When the sentinel takes a slot first, q15t's `InstallImportHook` with an expected
address refuses it, because the slot no longer holds the game's function (#2000 is that case, by code). When q15t takes
it first, a `GotHook` with no expected original accepts any executable address and takes the slot over, so q15t's handler
stops running. Neither order runs both.

On the release base, a third mechanism runs in the same process: the reference build's runtime (`libvrapi.so`, the
reference APK's own file as `tools/quest_release_base.py` names it) carries modules that swap libr15 GOT and RELRO words
with their own helper; per the reference build's source, those are the afkxr modules of Ducky's reference combat build.

**PC.** The runtime (`nevr-runtime.dll`) installs most of its hooks through `PatchDetour`, which records each with HookGuard
(N84). Others install directly and record themselves in HookGuard (`src/runtime/lifecycle/initialize.cpp`,
`src/runtime/log/builtin_filter.cpp`, `src/runtime/patch/resource_override.cpp`, `src/runtime/patch/social_facade.cpp`,
`src/runtime/hook/dll_load_hook.cpp`). Some install directly and are recorded nowhere: `src/runtime/patch/asset_cdn.cpp`
(`Hooking::Attach`), the four hooks of `src/modules/platform-compat/src/platform_compat.cpp`,
`src/runtime/patch/plugin_assets.cpp` (`MH_CreateHook`) and `src/runtime/patch/arena_public_clock.cpp`, which says why:
HookGuard has no retire, so a hook per reload of a script DLL would fill its table. A plugin that installs its own
detours declares `NEVR_PLUGIN_CAP_HOOKS_ENGINE`, whose comment says "a second MinHook instance does not share the host's
hook table" (`sdk/include/nevr/plugin_interface.h`); the SDK's `nevr/pc/hook_manager.h` is a scoped MinHook lifecycle the
plugin supplies itself.

Prior art: ByteHook, ByteDance's Android PLT hook library, manages every PLT hook in the process from one place, hooks
newly loaded libraries automatically, and lets several hooks on one function run without conflict; a proxy calls the
original through its `BYTEHOOK_CALL_PREV()` macro ([bytedance/bhook](https://github.com/bytedance/bhook)).

## Decision

1. **One hook manager per process.** On Quest, the sentinel's GOT backend and its build-time rows. On PC, the runtime's
   hook table behind `PatchDetour` and HookGuard.
2. **Several handlers per slot, in a defined order.** The manager keeps a chain for each slot and each handler reaches the
   next through the manager, so a second consumer of a slot never needs a second mechanism. The order and every
   insertion are logged (N17's per-hook outcome and aggregate line). The manager can also retire a hook whose target
   went away (a script DLL the game unloaded and loads again), so a module that hooks every load of a library has no
   reason to bypass it.
3. **Plugins hook through the host.** On Quest the hook service is new members of `NevrQuestHostApi`, gated by its
   `struct_size` as ADR 0019 section 5 designed it, so plugins built against the current header keep loading. PC gets a
   service of the same shape (the plugin API change it needs is stated in its own PR, with N114). Once the services
   exist, N151's scan covers a plugin's page-protection calls the way it covers the sentinel's today, and
   `NEVR_PLUGIN_CAP_HOOKS_ENGINE` is retired.
4. **Code ported from another project is ported onto the service.** The module track's modules keep their logic and drop
   their own word-swap helpers.
5. **A refusal names the slot, the owner chain and the reason** at Warning (`docs/standards/logging.md`, Rule 6).
6. **Outside this ADR:** checked byte patches (AGENTS.md "Guardrails": a byte patch where a hook would cost something
   measurable, `nevr_checked_patch`); stores of data the game reads (a timestep, a score, a credits pointer); and the
   one hook the PC host (`BugSplat64.dll`) puts on the game's main wrapper before the runtime exists, which HookGuard
   records (`docs/adr/0008-bugsplat-host-and-nevr-runtime.md`, N125).

## Finding a violation

Scope: `src/quest`, `src/runtime`, `src/modules`, `src/core`, `sdk`, and, for H4, the documents under `docs/` and the
comments in those directories. Out of scope: tests, `src/reconstruction`, and the manager's own files
(`src/quest/sentinel/got_hook.cpp`, `src/quest/sentinel/libr15_attach.cpp`, `src/runtime/hook/patching.h`,
`src/core/hooking.h`).

- **H1.** A page-protection call (`mprotect`, `mmap` or `mremap` on an image page, `VirtualProtect` on a game module's
  page) or a store into a slot that holds a code pointer (a GOT, RELRO or vtable entry, or a function-pointer data word)
  of a game library, outside the manager. A checked byte patch and a store of game data (rule 6) are not hits.
- **H2.** An install path that does not register with the manager: `InstallImportHook`, `InstallChainedImportHook`,
  `SwapWord`, `got_swap`, a direct `Hooking::Attach` or `MH_CreateHook` whose target HookGuard does not record, or a
  plugin's own `MH_CreateHook`.
- **H3.** Two installers anywhere in scope that name the same slot address or the same function.
- **H4.** A capability flag, comment or document that accepts a second hook table.

## Consequences

- q15t moves onto the manager before the module track adds modules, and the N151 allowlist loses `got_swap.cpp`.
- A probe, a fix and a feature can share one game function without knowing about each other.

## Not claimed

The cost of a chained call against a single hook on a hot path; the measurements ADR 0038 calls for decide where a full
replacement must be one function with no chain.
