---
# atwd read contract
read: whole
read_via: cat docs/adr/0034-one-hook-manager-per-process.md
audience: all
---

# ADR 0034: one hook manager per process owns every hooked slot, and plugins hook through it

Status: proposed. Applies to the Quest build and the PC client. Supersedes the parts of ADR 0019 that refuse a second
handler on one slot: decision 5's "no chaining in v1", decision 9's refusal of a plugin claim on a registered slot, the
"Chaining" bullet of section 5
(chaining refused in v1, "a later ADR"), the first two rules of section 9, and "Chaining of hooks on one slot" among
section 13's non-goals. It also supersedes, from ADR 0019 section 5, the statement that a plugin's handler is wrapped by
`InstallThunk`: the plugin hook service installs the handler as a plain function, the exception that
`callback_thunk.h` rule 4 names. It supersedes the refusal of a plugin whose imports include `__gxx_personality_v0`,
which the tree never implemented (the name appears only in comments under `src/quest`). And from ADR 0019 section 11 it
supersedes predictions P5 and P6: a second claim on a slot chains, and a plugin claim on a sentinel slot joins the chain.
`DuplicateRefusedAndADifferentHandlerChains` and `ASentinelClaimAndAPluginClaimShareOneChain` in
`src/quest/tests/got_hook_test.cpp` test those joins.

## Context

**Quest.** The sentinel's GOT backend (`src/quest/sentinel/got_hook.h`) keeps one registry of every slot it holds, writes
each slot with a compare-and-swap under one lock, refuses a slot another handle owns, and has a budget the tools test
checks (`src/quest/sentinel/hook_budget.h`). Internal calls are hooked through the build-time inline rows of ADR 0021: a
row's activation stores the detour into the row's slot (`src/quest/sentinel/libr15_attach.h`), so those rows are the same
manager's mechanism for calls no import slot reaches. ADR 0021's word-patch rows (`kWordPatch`, its section 11) hook
nothing: they replace instruction words in place.

ADR 0019 section 5 already gives plugins a host API instead of a hook engine ("Plugins never carry a hook engine. The host
implements every write, under one lock, against one registry"), through `NevrQuestHostApi`
(`sdk/include/nevr/plugin_interface.h`), versioned by its own `struct_size` (ADR 0019 section 4). Today that API offers the
input filter and the script redirect, not a general hook, and it refuses chaining: "Refused in v1: a second claim on a
slot is a refusal ... Chaining is a later ADR." Section 9 refuses a plugin's claim on a slot the sentinel registered.

So the q15t plugin carries a second mechanism, with its own page-protection calls and stores
(`src/quest/plugins/q15t/src/got_swap.cpp`) and its own install helpers (`InstallImportHook`, `InstallChainedImportHook` in
`src/quest/plugins/q15t/src/import_hook.h`). Most of its modules install through those helpers, outside the sentinel's
registry. N151 names `got_swap.cpp` as an allowed page-protection site.

The two mechanisms do not compose. The sentinel installs its render-configuration hooks from its constructor
(`kInstallRenderConfig`, `src/quest/integration/ctor_sequence.cpp`) and q15t installs at the first call after load
(`PluginsAction`, `src/quest/integration/production_steps.cpp`), so the sentinel takes `vrapi_SetDisplayRefreshRate` and
`vrapi_SetClockLevels` first. q15t's `InstallImportHook`, given no expected address, looks the function up with `dlsym`
and refuses a slot that holds anything else (`src/quest/plugins/q15t/src/import_hook.cpp`), so it refuses both
(`src/quest/plugins/q15t/src/refresh_rate.cpp`, `src/quest/plugins/q15t/src/clock_levels.cpp`; #2000 is that case, by
code); were the lookup to find nothing, it would accept any executable word outside libr15 and chain without a line. In the other order, a `GotHook` with no expected original takes the slot's current value as its original
(`Prepare` in `src/quest/sentinel/got_hook.cpp`), and the sentinel's handlers call their original first
(`src/quest/sentinel/render_config_hook.cpp`), so both would run, the sentinel's outermost, in an order set by which
installed first, and no line would say so.

On the release base, a third mechanism runs in the same process: the reference build's runtime (`libvrapi.so`, the
reference APK's own file as `tools/quest_release_base.py` names it) carries modules that hook libr15 with their own
helper. The file imports `mprotect`, `dl_iterate_phdr`, `dlopen` and `dlsym`, and its log strings name libr15 GOT words it
replaces ("bvh: GOT slot for CBVHTree::%s doesn't hold the stub", "gunconnect: ExclusiveUpdateGame's GOT word isn't build
4987566's; no hook"; `readelf --dyn-syms` and `strings` on the reference APK's `lib/arm64-v8a/libvrapi.so`). #1737 row
G6A-094 names those modules' source: the `afkxr` modules of Ducky's reference combat build.

**PC.** The runtime (`nevr-runtime.dll`) installs most of its hooks through `PatchDetour`, which records each with HookGuard
(N84). Others install directly and record themselves in HookGuard (the `CSysDLL` hooks of
`src/runtime/lifecycle/initialize.cpp`, `src/runtime/log/builtin_filter.cpp`, `src/runtime/patch/resource_override.cpp`,
`src/runtime/patch/social_facade.cpp`, `src/runtime/hook/dll_load_hook.cpp`). Some install directly and are recorded
nowhere: the `BuildCmdLineSyntaxDefinitions` and `PreprocessCommandLine` hooks of `src/runtime/lifecycle/initialize.cpp`
(`Hooking::Attach`), `src/runtime/patch/asset_cdn.cpp` (`Hooking::Attach`), the four hooks of
`src/modules/platform-compat/src/platform_compat.cpp` and `src/runtime/patch/plugin_assets.cpp` (`MH_CreateHook`). A plugin that installs its own
detours declares `NEVR_PLUGIN_CAP_HOOKS_ENGINE`, whose comment says "a second MinHook instance does not share the host's
hook table" (`sdk/include/nevr/plugin_interface.h`); the SDK's `nevr/pc/hook_manager.h` is a scoped MinHook lifecycle the
plugin supplies itself.

Prior art: ByteHook, ByteDance's Android PLT hook library, manages every PLT hook in the process from one place, hooks
newly loaded libraries automatically, and lets several hooks on one function run without conflict; a proxy calls the
original through its `BYTEHOOK_CALL_PREV()` macro ([bytedance/bhook](https://github.com/bytedance/bhook)).

## Decision

1. **One hook manager per process.** On Quest, the sentinel's GOT backend and its build-time inline rows. On PC, the runtime's
   hook table behind `PatchDetour` and HookGuard.
2. **Several handlers per slot, in a defined order.** The manager keeps a chain for each slot and each handler reaches the
   next through the manager, so a second consumer of a slot never needs a second mechanism. The order and every
   insertion are logged (N17's per-hook outcome and aggregate line). The manager can also retire a hook whose target
   went away (a script DLL the game unloaded and loads again), so a module that hooks every load of a library has no
   reason to bypass it.
3. **Plugins hook through the host.** On Quest the hook service is new members of `NevrQuestHostApi`, gated by its
   `struct_size` as ADR 0019 section 4 designed it, so plugins built against the current header keep loading. PC gets a
   service of the same shape (the plugin API change it needs is stated in its own PR, with N114). Once the services
   exist, `got_swap.cpp` leaves N151's allowlist, so a page-protection call in a plugin under `src/quest` fails N151's
   scan, and `NEVR_PLUGIN_CAP_HOOKS_ENGINE` is retired.
4. **Code ported from another project is ported onto the service.** The module track's modules keep their logic and drop
   their own word-swap helpers.
5. **A refusal names the slot, the owner chain and the reason** at Warning (`docs/standards/logging.md`, Rule 6).
6. **Outside this ADR:** checked byte patches (AGENTS.md "Guardrails": a byte patch where a hook would cost something
   measurable; `nevr_checked_patch` on PC, and on Quest ADR 0021's word-patch rows, of which the public table holds none
   today); stores of data the game reads (a
   timestep, a score, a credits pointer); and the one hook the PC host (`BugSplat64.dll`) puts on the game's main wrapper
   before the runtime exists, which HookGuard records (`docs/adr/0008-bugsplat-host-and-nevr-runtime.md`, N125).

## Finding a violation

Scope: `src/quest`, `src/runtime`, `src/modules`, `src/core`, `sdk`, and, for H4, the documents under `docs/` other than
`docs/audits/`, and the comments in those directories. Out of scope: tests, `src/reconstruction`, the manager's own files
(`src/quest/sentinel/got_hook.cpp`, `src/quest/sentinel/libr15_attach.cpp`, `src/runtime/hook/patching.h`,
`src/core/hooking.h`), and the write helper behind checked patches and data stores (`src/runtime/hook/process_memory.h`).

- **H1.** A page-protection call (`mprotect`, `mmap` or `mremap` on an image page, `VirtualProtect` on a game module's
  page) or a store into a slot that holds a code pointer (a GOT, RELRO or vtable entry, or a function-pointer data word)
  of a game library, outside the manager. A checked byte patch and a store of game data (rule 6) are not hits.
- **H2.** An install path that does not register with the manager: `InstallImportHook`, `InstallChainedImportHook`,
  `SwapWord`, `got_swap`, a direct `Hooking::Attach` or `MH_CreateHook` whose target HookGuard does not record, or a
  plugin's own `MH_CreateHook`.
- **H3.** Two installers in scope that name the same slot address or the same function, other than handlers in one
  manager chain (rule 2).
- **H4.** A capability flag, comment or document that accepts a second hook table. Search terms:
  `NEVR_PLUGIN_CAP_HOOKS_ENGINE`, `hook_manager.h`, `MH_Initialize`, `got_swap`, `second MinHook`.

## Consequences

- q15t moves onto the manager before the module track adds modules, and the N151 allowlist loses `got_swap.cpp`.
- A probe, a fix and a feature can share one game function without knowing about each other.

## Not claimed

The cost of a chained call against a single hook on a hot path; the hot-path measurement #2017 specifies decides where a
full replacement must be one function with no chain.
