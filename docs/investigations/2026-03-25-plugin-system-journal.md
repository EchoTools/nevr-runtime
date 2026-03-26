# Plugin System Investigation Journal — 2026-03-25

## Metrics & Measurements

### CPU Usage (4-core Xeon Platinum 8168, Wine 9.0, Ubuntu 24.04)

| Configuration | Main Thread | wineserver | Total | Notes |
|---|---|---|---|---|
| Original EchoRelay DLLs (baseline) | 52% | 39% | **91%** | No throttling at all |
| Legacy dbgcore + server_timing (30Hz, Sleep hook) | 28% | 10% | **38%** | 2.4x improvement |
| Legacy dbgcore + no plugins | 24% | 9% | **33%** | Slightly less without plugin overhead |
| Main gamepatches.dll | — | — | **crash** | Crashes ~33s after level load |

### Syscall Profile (5-second strace, original DLLs)

| Syscall | % Time | Calls | Notes |
|---|---|---|---|
| pselect6 | 52% | 19 | Wine WaitableTimer — ~176ms each |
| read | 21% | 23,351 | Wine internal pipes, ~4600/sec |
| futex | 18% | 4,571 (1,348 err) | Thread synchronization |
| rt_sigprocmask | — | 41,935 | Wine signal overhead |

### CPrecisionSleep::Wait Arguments (traced)

```
Wait called: 7827 us (7 ms)
Wait called: 7841 us (7 ms)
Wait called: 5727 us (5 ms)
Wait called: 4531 us (4 ms)
```

Game calls Wait with 4-8ms, NOT the full 33ms frame budget. The game loop runs faster than the configured tick rate — Wait only sleeps the leftover after game processing.

### Memory Usage

| Configuration | RSS |
|---|---|
| Original DLLs (idle) | 465 MB |
| Legacy dbgcore + plugins (idle) | 486 MB |
| Legacy dbgcore + plugins (in session) | 975 MB |

### Connectivity Matrix

| dbgcore.dll | pnsradgameserver.dll | Plugins | Connects? |
|---|---|---|---|
| Original EchoRelay | Original EchoRelay | None | ✅ |
| Legacy (ours) | Original EchoRelay | None | ✅ |
| Legacy (ours) | Original EchoRelay | server_timing | ✅ |
| Legacy (ours) | Original EchoRelay | server_timing + game_rules_override | ❌ |
| Main (ours) | Original EchoRelay | None | ❌ (crash) |
| Legacy (ours) | Legacy (ours) | Any | ❌ (hangs) |
| Legacy (ours) | Main (ours) | Any | ❌ (hangs) |

---

## Hypothesis Flowchart

```
ROOT: Why don't plugins work correctly on the live server?
│
├─ H1: Plugin DLLs can't load (missing deps)
│  └─ ✅ CONFIRMED — libwinpthread-1.dll was missing
│     Fix: added -static linking to CMakeLists.txt
│
├─ H2: Plugins initialize too early
│  ├─ ✅ CONFIRMED — LoadPlugins() was in Initialize() (DLL_PROCESS_ATTACH)
│  │  Fix: moved to PreprocessCommandLineHook
│  │
│  ├─ ✅ CONFIRMED — DllMain auto-init caused double initialization
│  │  Fix: removed NvrPluginInit calls from DllMain in 3 plugins
│  │
│  └─ ✅ CONFIRMED — g_balance_config NULL at PreprocessCommandLineHook time
│     Fix: deferred ApplyOverrides to OnGameStateChange
│
├─ H3: Plugins unload prematurely
│  └─ ✅ CONFIRMED — DLL_THREAD_DETACH triggered UnloadPlugins()
│     Fix: separated THREAD_DETACH from PROCESS_DETACH in switch
│
├─ H4: Deadlock monitor kills server
│  └─ ✅ CONFIRMED — false-triggers on Wine during level load
│     Fix: patch unconditionally (not just _DEBUG)
│
├─ H5: super_hyper_turbo hook doesn't fire
│  └─ ✅ CONFIRMED — missing MH_Initialize() before MH_CreateHook
│     Fix: added MH_Initialize() call
│     ⚠️  NOT YET TESTED post-fix (blocked by game_rules_override issue)
│
├─ H6: CJsonGetFloat hook overrides round_time
│  ├─ ❌ WRONG — CJsonGetFloat (0x5FCA60) is NOT called for arena rules
│  │  Evidence: 422 hook calls, zero contained "round_time"
│  │
│  ├─ ❌ WRONG — CJsonInspectorReadFloat (0x174EC0) also didn't help
│  │  Evidence: ReadFloat trace showed time/score fields but not round_time
│  │  (hook also broke connectivity — disabled)
│  │
│  └─ ⏸️  TABLED — Arena rules loaded from binary archives via
│     CBinaryStreamInspector, not JSON parsing. Source config at
│     sourcedb/rad15/json/balance/mp_arena_rules.jsonc is compiled
│     to binary at build time. Need different approach.
│
├─ H7: game_rules_override breaks connectivity
│  ├─ ❌ WRONG assumption: "writing correct offsets = safe"
│  │  Offsets ARE correct (+0x40=max_health, +0x3C=max_stun_duration)
│  │  but the SEMANTIC CHANGE breaks client-server agreement
│  │
│  └─ ✅ CONFIRMED root cause: client-server state divergence
│     - Server patches max_health=100 (enables damage)
│     - Client loaded arena mode, expects max_health=0 (no damage)
│     - CanPlayerTakeDamage() returns different values on each side
│     - Damage validation fails → connection drops
│     - Balance settings broadcast via EncodeAllSettingsToBroadcast
│       (0x140cd4980) happens BEFORE plugin patches the struct
│
├─ H8: Main gamepatches crashes with original gameserver
│  └─ ⏸️  NOT INVESTIGATED — crash ~33s after level load
│     Main has OVR bypass, Wwise disable, WinHTTP→libcurl bridge,
│     server crash fixes — any could be incompatible with original
│     gameserver. Low priority since legacy works.
│
├─ H9: High CPU at idle
│  ├─ ✅ CONFIRMED — game loop runs unthrottled without GPU vsync
│  │
│  ├─ ❌ WRONG — BusyWait→RET alone doesn't fix it
│  │  BusyWait is only the precision phase; the WaitableTimer
│  │  phase in CPrecisionSleep::Wait also fails on Wine 9.0
│  │
│  ├─ ✅ CONFIRMED — hooking CPrecisionSleep::Wait with Sleep(ms) helps
│  │  Reduced from 91% to 38% total CPU
│  │
│  └─ ⚠️  REMAINING — 38% is still high for an idle server
│     Wait receives 4-8ms values (not 33ms for 30Hz)
│     Game loop runs ~200Hz despite 30Hz fixed timestep config
│     Fixed timestep controls SIMULATION rate, not LOOP rate
│
└─ H10: Our gameserver DLLs don't work
   └─ ⏸️  KNOWN ISSUE — both legacy and main gameserver DLLs
      cause hangs after connect. Using original EchoRelay
      gameserver (116KB). Separate investigation needed.
```

---

## Next Steps

```
PHASE 1: Stabilize Current Setup (DONE)
├─ ✅ Legacy dbgcore.dll + original gameserver + server_timing
├─ ✅ CPU reduced from 91% → 38%
└─ ✅ Client connectivity confirmed

PHASE 2: Fix game_rules_override Connectivity
│
├─ Option A: Patch balance config BEFORE client joins
│  │  Hook EncodeAllSettingsToBroadcast (0x140cd4980) or the
│  │  session setup path so the patched values are included
│  │  in the initial balance broadcast to clients.
│  │
│  │  Pros: Client and server agree from the start
│  │  Cons: Need to find exact hook point in session setup
│  │
│  ├─ [ ] Decompile EncodeAllSettingsToBroadcast in reconstruction
│  ├─ [ ] Find where balance config is first read during session start
│  ├─ [ ] Patch BEFORE that read, not on state change
│  └─ [ ] Verify client receives patched values
│
├─ Option B: Force re-broadcast after patching
│  │  After writing to balance config, trigger
│  │  EncodeAllSettingsToBroadcast to re-send to all clients.
│  │
│  │  Pros: Simpler — just call one function after patching
│  │  Cons: May cause duplicate broadcast, timing-sensitive
│  │
│  ├─ [ ] Find how to invoke EncodeAllSettingsToBroadcast
│  ├─ [ ] Determine required arguments (broadcaster, session ctx)
│  └─ [ ] Test with client join
│
└─ Option C: Only patch values that don't affect client state
   │  Set grab_range, aim_assist, etc. but NOT max_health
   │  (which changes arena→combat behavior).
   │
   │  Pros: Safe, no client divergence
   │  Cons: Can't enable combat damage in arena mode
   │
   ├─ [ ] Test with grab_range/aim_assist only
   └─ [ ] Verify connectivity preserved

PHASE 3: Arena Timing Override (round_time, celebration_time)
│
├─ Finding: Values baked into binary archives, not runtime JSON
│  Source: sourcedb/rad15/json/balance/mp_arena_rules.jsonc
│  Compiled to binary, loaded via CBinaryStreamInspector
│
├─ [ ] Hook archive resource loader to intercept mp_arena_rules load
│  ├─ [ ] Find CResourceLoader::LoadResource (0x140fa2820)
│  │     or the specific balance config loader
│  ├─ [ ] Intercept the loaded data buffer
│  ├─ [ ] Patch float values in the buffer before game parses them
│  └─ [ ] Also useful for CDN asset loading later
│
├─ [ ] Alternative: memory scan for 300.0f after session start
│  ├─ [ ] Scan near CR15NetGameplay struct (+0x2AA8 from net_game)
│  ├─ [ ] Look for 300.0f (0x43960000) at or near +0xB4
│  └─ [ ] Patch to 480.0f and test
│
└─ [ ] Verify round_time change doesn't cause client desync
      (same risk as balance config — does client need to agree?)

PHASE 4: Further CPU Reduction
│
├─ Current: 38% total (28% main + 10% wineserver)
│  Target: <10% total at idle
│
├─ [ ] Investigate why game loop runs at ~200Hz despite 30Hz config
│  ├─ [ ] Fixed timestep flag controls simulation, not loop rate
│  ├─ [ ] Game may have a separate frame rate limiter that's GPU-dependent
│  └─ [ ] Without GPU, no vsync → unlimited loop rate
│
├─ [ ] Try Sleep(33) unconditionally in Wait hook (force 30Hz loop)
│  └─ [ ] Risk: may cause gameplay timing issues
│
├─ [ ] Hook game's main update function directly
│  ├─ [ ] Insert Sleep based on elapsed frame time
│  └─ [ ] More precise than hooking Wait
│
└─ [ ] Profile: where does the 28% CPU go?
   ├─ [ ] perf record + perf report for hot functions
   └─ [ ] May reveal other spin loops or busy work

PHASE 5: Gameserver DLL Investigation
│
├─ Both legacy and main gameserver DLLs hang after connect
├─ [ ] Compare with original 116KB EchoRelay gameserver
├─ [ ] Check WebSocket client implementation differences
├─ [ ] Check ServerDB message format compatibility
└─ [ ] Check if original uses different session flow
```

---

## Key Addresses (Verified)

| Address (VA) | Offset | Name | Status |
|---|---|---|---|
| 0x1420D3450 | — | g_balance_config (pointer-to-struct) | ✅ verified |
| 0x140CB1040 | — | CanPlayerTakeDamage | ✅ verified |
| 0x140CB1110 | — | GetStunFraction | ✅ verified |
| 0x140CD4980 | — | EncodeAllSettingsToBroadcast | ⚠️ from docs, not decompiled |
| 0x140D76CA0 | — | UpdateBlocking (checks balance ptr) | ✅ verified |
| 0x1405FCA60 | 0x5FCA60 | CJson_GetFloat (thunk) | ✅ verified, NOT used for archives |
| 0x140174EC0 | 0x174EC0 | CJsonInspectorRead::ReadFloat | ✅ verified, NOT used for archives |
| 0x1401CE0B0 | 0x1CE0B0 | CPrecisionSleep::Wait | ✅ verified, hooked for CPU fix |
| 0x1401CE4C0 | 0x1CE4C0 | CPrecisionSleep::BusyWait | ✅ verified |
| 0x14015E920 | 0x15E920 | CreateSession | ⚠️ from address_registry, not verified in Ghidra |
| 0x140FA2820 | 0xFA2820 | CResourceLoader::LoadResource | ⚠️ from reconstruction |
| balance+0x3C | — | max_stun_duration | ✅ verified |
| balance+0x40 | — | max_health | ✅ verified |
| game+2088 | — | Fixed timestep flags | ✅ verified |
| 0x020A00E8→+0x90 | — | Fixed timestep value (μs) | ✅ verified |
| 0xCF46D | — | Delta time comparison | ✅ verified |
