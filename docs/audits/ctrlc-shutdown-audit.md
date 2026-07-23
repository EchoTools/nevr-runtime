# CTRL+C / Graceful Shutdown Audit — 2026-07-23

READ-ONLY. Causal chain from CTRL+C to port-6821 zombie.

---

## 1. N13 verbatim

```
### N13. CTRL+C sent to dedicated server does not trigger graceful shutdown

| **Where**   | Server process signal handling (Linux/Wine CTRL+C → SIGINT),
|             | lobby registration cleanup (src/gameserver/gameserver.cpp),
|             | game socket close path |
| **Reached** | Dedicated server running under Wine via launch-nevr-server.sh or
|             | equivalent. Sending CTRL+C / SIGINT terminates the process without
|             | unregistering the lobby from ServerDB or cleanly closing WebSocket
|             | connections to g.echovrce.com. |
| **Severity**| Medium |
| **Status**  | **Open** — not fixed. Owner explicitly deferred: "don't fix right now." |

When the dedicated server receives SIGINT (CTRL+C under Wine/Linux), the
process exits without running a graceful shutdown sequence. This means:

- No lobby unregistration message is sent to ServerDB — the lobby entry
  remains until it times out on the server side.
- The ws_bridge WebSocket connections to g.echovrce.com are torn down by
  the TCP stack rather than closed cleanly at the application layer.
- Any in-progress game state is lost rather than flushed.

A proper graceful shutdown would need a signal handler that sets a shutdown
flag, then on the next Update() tick: (1) send an unregistration message to
ServerDB, (2) close ws_bridge connections cleanly, (3) exit normally. This
has not been implemented; the owner has deferred it.
```

---

## 2. Signal/console handlers — one exists, but is silent on Wine

### Console handler (exists, but requires a console)

`src/gamepatches/crash_recovery.cpp:363-375`:
```cpp
void InstallConsoleCtrlHandler() {
  SetConsoleCtrlHandler(
      [](DWORD dwCtrlType) -> BOOL {
        if (dwCtrlType == CTRL_C_EVENT || dwCtrlType == CTRL_CLOSE_EVENT ||
            dwCtrlType == CTRL_BREAK_EVENT) {
          Log(EchoVR::LogLevel::Info, "[NEVR.PATCH] Console signal %lu received — exiting",
              dwCtrlType);
          if (OriginalExitProcess)
            OriginalExitProcess(0);
          else
            ExitProcess(0);
          return TRUE;
        }
        return FALSE;
      },
      TRUE);
}
```

Called from `initialize.cpp:316` during boot — this IS registered on every run. But `SetConsoleCtrlHandler` sends `CTRL_C_EVENT` **only to processes attached to a console**. With `-noconsole`, it's silently a no-op — the handler is registered but never invoked.

### Signal handlers (none exist)

Zero hits for `SIGINT`, `SIGTERM`, `signal(`, `sigaction`, `atexit`, `std::set_terminate` anywhere in `src/`, `modules/`, `plugins/`, or `extras/` (excluding legacy). **No POSIX signal handler exists.** Under Wine on Linux, CTRL+C sends `SIGINT` to the Linux `wine` process — with no signal handler, the default action is immediate termination.

---

## 3. The `-noconsole` hypothesis — CONFIRMED

### (a) `-noconsole` suppresses console creation

- `cli.cpp:25-26` — registers `-noconsole` argument: `"[NEVR] Disable console window creation"`
- `boot.cpp:111` — parses `-noconsole` → sets `g_noConsole = TRUE`
- `boot.cpp:167-172` — **auto-enables on Wine**: if `ntdll.dll` exports `wine_get_version`, `g_noConsole` is set TRUE automatically, even without the flag on the command line:
  ```cpp
  if (!g_noConsole) {
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    if (ntdll && GetProcAddress(ntdll, "wine_get_version") != NULL) {
      g_noConsole = TRUE;
      Log(EchoVR::LogLevel::Info, "[NEVR.PATCH] Wine detected — defaulting to -noconsole");
    }
  }
  ```
- `mode_patches.cpp:215` — `if (g_noConsole) { return; }` skips `AllocConsole()` call

### (b) No code path receives CTRL+C with no console

`SetConsoleCtrlHandler` from kernel32.dll requires a console. Without `AllocConsole` (skipped by `g_noConsole`), `CTRL_C_EVENT` is never delivered. The handler at `crash_recovery.cpp:367` **is registered unconditionally** but silent under `-noconsole` on Wine.

### (c) Does the game create its own console?

No. The game's `WinMain` entry point uses `-noconsole` (our flag) to skip console creation. There is no game-native console creation path that bypasses our hook.

**Result: on Wine, the server has NO mechanism to receive CTRL+C.** It dies hard on SIGINT with no graceful teardown.

---

## 4. What does shutdown do today?

### state_machine.cpp session-end path (the only "graceful" exit)

`src/gamepatches/state_machine.cpp:64-68`:
```cpp
// Session ended: we were in-game and now returning to lobby. Exit cleanly
// so the fleet manager can spawn a fresh instance.
if (g_serverWasInGame && state == EchoVR::NetGameState::Lobby) {
  Log(EchoVR::LogLevel::Info, "[NEVR] Session ended. Server exiting.");
  ExitProcess(0);
}
```

This calls `ExitProcess(0)` directly. No lobby unregistration. No WebSocket close. No ws_bridge listener stop. Just immediate process exit.

### GameServerLib::BeginGracefulShutdown (exists but not wired to CTRL+C)

`src/gamepatches/gameserver/gameserver.cpp:1089-1129` — runs a detached thread that waits for round end (up to 20 min), then exits. Called only on ServerDB disconnect with `-exitonerror`. NOT called on CTRL+C or session-end.

### DLL_PROCESS_DETACH (full teardown, but unreachable on hard kill)

`src/gamepatches/dllmain.cpp:98-113`:
```cpp
case DLL_PROCESS_DETACH:
  // Only do clean shutdown on dynamic unload (lpReserved == NULL).
  // During process termination (lpReserved != NULL), threads are already
  // dead and joining would deadlock under the loader lock.
  if (lpReserved == NULL) {
    UnloadModules();            // → each module's NvrModuleShutdown
    AssetCDN::Shutdown();
    Wave0::Shutdown();          // unhooks MinHook table hooks
    TokenAuth::Shutdown();
    BroadcasterGuard::Shutdown();
    BuiltinLogFilter::Shutdown();
    ServerTiming::Shutdown();
    ShutdownResourceOverride();
    ShutdownWebSocketBridge();  // sets flag, does NOT stop server (see §5)
  }
  UnloadPlugins();              // each plugin's NvrPluginShutdown
```

`lpReserved` is non-null during `ExitProcess` / `TerminateProcess` — the DLL receives `DLL_PROCESS_DETACH` but **skips the entire teardown block** because "threads are already dead and joining would deadlock under the loader lock." All hooks remain installed. All sockets remain open. The OS (or Wine in this case) is supposed to reclaim everything — but Wine's wineserver keeps the TCP socket alive.

---

## 5. Does the ws_bridge listener ever get closed?

### ShutdownWebSocketBridge — flag-only, no stop()

`src/modules/ws-bridge/src/ws_bridge.cpp:600-604` (module, active path):
```cpp
void ShutdownWebSocketBridge() {
  g_bridgeEnabled = false;
  // Do NOT call g_server->stop() — runs under loader lock during DLL_PROCESS_DETACH.
  // Thread joins can deadlock. OS reclaims everything on process exit.
}
```

`src/gamepatches/ws_bridge.cpp:608-612` (gamepatches copy, same):
```cpp
void ShutdownWebSocketBridge() {
  g_bridgeEnabled = false;
  // Do NOT call g_server->stop() — runs under loader lock during DLL_PROCESS_DETACH.
  // Thread joins can deadlock. OS reclaims everything on process exit.
}
```

### NvrModuleShutdown (module unload path)

`src/modules/ws-bridge/src/ws_bridge.cpp:635-637`:
```cpp
NEVR_MODULE_API void NvrModuleShutdown(void) {
  ShutdownWebSocketBridge();
}
```

Called from `UnloadModules()` → module's shutdown function pointer. But `UnloadModules()` only runs inside `DLL_PROCESS_DETACH` with `lpReserved == NULL` (dynamic unload), which never happens during process termination.

### Stop path that DOES stop

`src/modules/ws-bridge/src/ws_bridge.cpp:572` — `it->second->remoteWs->stop()` — stops individual REMOTE connections (to g.echovrce.com), not the local listener. Called during error/retry handling, not during shutdown.

`src/gamepatches/ws_bridge.cpp:574-577,595` — same: stops remote connections, calls `g_server.reset()` (destructor), but only during error handling (e.g., connection failure), not during shutdown.

### Verdict: the listening socket is NEVER explicitly closed.

`g_server->stop()` is never called. The `g_server` unique_ptr destructor is never reached because `g_server.reset()` is only called in error paths, not in `DLL_PROCESS_DETACH`. The comment acknowledges this: "OS reclaims everything on process exit." But under Wine, the **wineserver process outlives the game process**, and the wineserver holds the TCP socket open — the OS (Linux kernel) never gets a close() syscall, so the socket stays in LISTEN.

---

## 6. Causal chain — CONFIRMED

```
CTRL+C (Linux terminal)
  → SIGINT sent to wine process
  → -noconsole (auto-enabled on Wine) prevents console creation
  → SetConsoleCtrlHandler never invoked (no console to deliver CTRL_C_EVENT to)
  → No POSIX signal handler exists (no SIGINT/SIGTERM handler in codebase)
  → Default SIGINT action: terminate process immediately
  → DLL_PROCESS_DETACH fires with lpReserved != NULL
  → ShutdownWebSocketBridge sets flag but does NOT call g_server->stop()
  → wineserver survives (same WINEPREFIX, or a persistent wineserver)
  → wineserver holds TCP LISTEN socket for port 6821
  → Socket stays in LISTEN state with no owning process (kernel zombie)
  → Next launch: bind() on 6821 fails with EADDRINUSE
  → ws_bridge logs "Failed to listen"
  → Game bypasses bridge, connects directly to g.echovrce.com
  → No login injection → [NSUSER] Creating user ???-1 → never registers
```

**N13 and N38 are the same root cause seen from two ends.** N13 is the missing graceful shutdown path. N38 is the consequence: wineserver leaks the port. N37 (missing SO_REUSEADDR) is a defense-in-depth mitigation that would paper over the symptom but not fix the root cause.

---

## 7. N38 verbatim

```
### N38. Server-run teardown leaks wineserver — stale socket prevents rebind

| **Where**   | Server lifecycle: launch-server.sh → wine echovr.exe → kill → wineserver. |
| **Reached** | Every server kill that doesn't go through graceful shutdown (N13).     |
| **Severity**| Medium                                                                |
| **Status**  | Open — documented, not yet implemented.                               |

When the server process is killed externally (SIGTERM, kill, CTRL+C), the Wine
wineserver process does not clean up its TCP sockets. The LISTEN socket on port
6821 remains in the kernel's TCP table as a zombie — no owning process, still
LISTEN. This blocks the next server start (see N37). The wineserver for the
echovr prefix (echovr/.wineprefix) also accumulates stale lock/socket files
that confuse wineserver -p.

Evidence: ps aux | grep wineserver returned empty, but ss -tlnp showed
port 6821 in LISTEN with no PID. wineserver -p returned exit 0 (thinks it's
running). The socket survived for >10 minutes across multiple kill attempts.

Fix direction: (a) implement N13 (graceful CTRL+C shutdown) so the ws-bridge
closes its listening socket cleanly before exit; (b) add a pre-launch guard in
launch-server.sh that detects and clears the stale socket (e.g. ss -K or
fuser -k 6821/tcp); (c) add SO_REUSEADDR (N37) as defense-in-depth so the
zombie socket doesn't block the next bind.  Not implemented — deferred.
```

---

## Summary

| Question | Finding |
|----------|---------|
| Signal handler exists? | One: `SetConsoleCtrlHandler` at `crash_recovery.cpp:363-375`. Silent under Wine — requires a console, and `-noconsole` is auto-enabled on Wine. **No POSIX signal handler.** |
| `-noconsole` explains it? | **Yes.** Auto-enabled on Wine (`boot.cpp:167-172`), skips `AllocConsole` (`mode_patches.cpp:215`), so `SetConsoleCtrlHandler` never fires. |
| Teardown today? | `state_machine.cpp:67`: `ExitProcess(0)` on session end — no cleanup. `dllmain.cpp:99-113`: full shutdown sequence, but **skipped** during process termination (`lpReserved != NULL`). `ShutdownWebSocketBridge`: flag-only, does NOT stop the listener. |
| N13 and N38 same root? | **Yes.** Missing graceful shutdown (N13) → hard kill → wineserver holds socket (N38) → next bind fails → no registration. |
| N37 confirmed as symptom-patch? | **Yes.** SO_REUSEADDR would let bind succeed despite the zombie, but the zombie still exists. Fixing N13 (graceful shutdown) prevents the zombie from forming in the first place. Both are worth doing: N13 is the root-cause fix; N37 is defense-in-depth. |

**Smallest fix shape (proposed, not implemented):**

1. Register a `SIGINT`/`SIGTERM` handler (POSIX `signal()`) in the ws_bridge module's init, or in `initialize.cpp` after `InstallConsoleCtrlHandler`. The handler sets a `volatile sig_atomic_t g_shutdownRequested` flag.
2. On the next game tick (or in a dedicated poll thread), when the flag is set:
   a. Call `g_server->stop()` to close the listener socket (prevents the zombie)
   b. Close each remote ws connection via `remoteWs->stop()`
   c. Send a GameServerUnregistration protobuf to ServerDB
   d. Call `ExitProcess(0)` or `ForceFatalExit(0)`
3. The `g_server->stop()` call is the critical step — it's the specific thing that releases the socket FD, preventing the wineserver zombie.
4. Also add `SO_REUSEADDR` (N37) as defense-in-depth, so if any path still leaves a zombie, the next launch recovers.
