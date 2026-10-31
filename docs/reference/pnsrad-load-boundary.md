# Where the pnsrad patches land: the LoadLibraryW return

The runtime rewrites code and data in `pnsrad.dll` (four login and provider patches) and `pnsradmatchmaking.dll`
(the matchmaker host default) when each loads. The loader's DLL notification sees every load, but it runs under the
loader lock, where calling into another module (`VirtualProtect`, `WriteProcessMemory`, the game's logger) is
unsafe. This records the boundary the patches use instead, from ReVault (`echovr.exe`, `pnsradmatchmaking.dll`)
and the runtime source.

## The game side: how both modules load

| Step | Address | What happens |
| --- | --- | --- |
| pnsrad.dll caller | `CR15Game::CreateNetServiceProviders`, `0x140109810` | passes the provider name (the strings at `0x1416d35c4` "pnsovr" and `0x1416d35e8` "pnsdemo") to the provider loader `CNSProvider::LoadByName`, `0x140606690`, which the runtime hooks (`src/runtime/patch/provider_name_redirect.cpp`) to pass "pnsrad" for both; the loader reaches the module loader within three calls |
| pnsradmatchmaking.dll caller | `LoadMatchmakingSupport`, `0x14060b810` | see `docs/reference/matchmaking-module-reload.md` |
| Module loader | `0x14105aa70` -> `0x14105ab40` | `GetModuleHandleA` (`0x1400eaf40`) first; when the module is not loaded, `0x1400eb0f0` loads it, then the loader resolves and calls the module's `RadPluginSet*` exports (`InitNonMemoryStatics`, `0x14105a600`) and further exports, all before it returns |
| The load | `0x1400eb0f0` | `call [0x1416c3210]` at `0x1400eb13d`: `KERNEL32!LoadLibraryW`. Right after it returns, `GetProcAddress` (`[0x1416c3208]`) and a call through the result (`0x1400eb185`) |

The callers of `0x14105aa70` are `LoadMatchmakingSupport`, `LoadServerSupport` and four provider loaders
(`0x140635db0`, `0x140636090`, `0x140636180`, `0x1406362e0`). `pnsradmatchmaking.dll` does not import `pnsrad.dll`, so
neither module is mapped as another module's dependency.

So the first point after the loader lock is released, and before the game reaches any code in the module, is the
return of `KERNEL32!LoadLibraryW` at `0x1400eb13d`. The module's own `DllMain` runs earlier, inside the load.

## The runtime side

| Piece | Where |
| --- | --- |
| Record | `OnDllLoaded` in `src/runtime/patch/pnsrad_enabler.cpp`, the `LdrRegisterDllNotification` callback: classifies the module by `BaseDllName` (`nevr_pnsrad_load::Classify`) and records its kind and base in `nevr_pnsrad_load::PendingLoads` (`src/runtime/patch/pnsrad_load_queue.h`). Atomic stores only; no write, no log, no call out of the module |
| Apply at the boundary | `nevr_dll_load_hook` runs its post-load observers after the real `LoadLibraryA/W/ExA/ExW` returns, on the calling thread, before the hook returns to the game (`src/runtime/hook/dll_load_hook.cpp`). `nevr_pnsrad_enabler::Init` registers `ApplyPendingLoadsAtLibraryReturn`, which drains the records and applies the patches (`ApplyPnsradLoad`, `ApplyMatchmakingLoad`) under one mutex, so a `LoadLibrary` returning on another thread waits for a patch in progress |
| Loader lock held | the observers are skipped when the calling thread owns the loader lock (a `LoadLibrary` from inside a `DllMain`, `LoaderLockHeldByThisThread`); the next return outside the lock applies the record |
| Fallback | the frame tick (`nevr_pnsrad_enabler::ApplyLoadsMissedAtLibraryReturn`) applies a record at least one tick old, with a Warning that the load did not pass through a hooked `LoadLibrary*` and the game may have used the module unpatched |

What a client run's log shows on success: `[NEVR.PATCH] pnsrad module patches: 4 succeeded, 0 failed — social layer
(friends/party/login) ENABLED`, with no `patched at the frame tick` Warning.

## Tests

- `src/runtime/tests/test_dll_load_hook.cpp`, `DllLoadHookPostLoad`: a real `LoadLibraryW` of a stand-in `pnsrad.dll`
  under the real hooks and a real DLL notification; the caller's first read of the module after `LoadLibraryW`
  returns must see the patched value. A second test holds the loader lock and checks the observers wait.
- `src/runtime/tests/test_pnsrad_load_queue.cpp`: the record and drain contract (once per record, reloads as
  separate records, a full queue counted, the tick's one-tick delay, concurrent producers and drains).
- `tools/tests/test_pnsrad_load_boundary.py`: the notification calls nothing but the record; every `LoadLibrary*` hook
  runs the observers after the real call; `Init` registers the observer; the tick drains what was missed.

## Not proven

- That neither module's `DllMain` or static initializers run a patched site. The loader's LOADED notification is
  delivered before `DllMain`; the patches land after it. In
  `pnsrad.dll` the four sites have no direct callers in ReVault: `LoginIdResponseCB` (`0x18008f140`) and
  `LogInSuccessCB` (`0x18008eea0`) are referenced only as data, at `0x18008e785` and `0x18008cf3e` (callback
  registration); `CNSRADUser`'s login check (`0x180085b40`) is a virtual function and `UserProviderID` (`0x180088d70`)
  an export, with no xrefs. The paths from the module's entry point to those registration sites were not traced.
  A login that succeeds on a client run is the observation that the patches landed before first use.
- A matchmaking reload under this path on a real client (it needs a lobby to be torn down and rebuilt).
