# Bridge Port Audit — 2026-07-23

READ-ONLY. Question: who references the ws_bridge listen port, and can bind-then-publish work?

---

## 1. Every site that hardcodes or derives port 6821

### Code (hardcoded constant `6821`)

| File:Line | Code |
|-----------|------|
| `src/modules/ws-bridge/src/ws_bridge.cpp:164` | `g_server = std::make_unique<ix::WebSocketServer>(6821, "127.0.0.1");` |
| `src/gamepatches/ws_bridge.cpp:170` | `g_server = std::make_unique<ix::WebSocketServer>(6821, "127.0.0.1");` |

**Note:** The comment on the line above in the module copy says `// Start local ws:// server on a dynamic port` (line 163), but the port is in fact hardcoded — "dynamic" is aspirational, not the current reality.

### Symbol/constant bound to port 6821

| File:Line | Symbol | Role |
|-----------|--------|------|
| `src/modules/ws-bridge/src/ws_bridge.cpp:40` | `static uint16_t g_proxyPort = 0;` | Set to `g_server->getPort()` AFTER `listen()` succeeds (line 592) |
| `src/modules/ws-bridge/src/ws_bridge.cpp:145-146` | `GetWebSocketBridgePort()` | Returns `g_proxyPort` |
| `src/modules/ws-bridge/src/ws_bridge.cpp:640-641` | `WsBridge_GetPort()` | Module export, returns `GetWebSocketBridgePort()` |
| `src/modules/ws-bridge/src/ws_bridge.h:12` | `uint16_t GetWebSocketBridgePort();` | Header declaration |
| `src/gamepatches/ws_bridge.cpp:37` | `static uint16_t g_proxyPort = 0;` | Duplicate; same pattern |
| `src/gamepatches/ws_bridge.cpp:151-152` | `GetWebSocketBridgePort()` | Returns `g_proxyPort` |
| `src/gamepatches/ws_bridge.h:12` | `uint16_t GetWebSocketBridgePort();` | Duplicate header |
| `src/gamepatches/config.cpp:259` | `extern uint16_t GetWebSocketBridgePort();` | Used by AutoRelayThroughBridge |
| `src/gamepatches/config.cpp:385` | `ResolveModuleProc("WsBridge_GetPort")` | Used by RedirectServiceUrl |
| `src/gamepatches/boot.cpp:92-94` | `GetProcAddress(hWsBridge, "WsBridge_GetPort")` | Registration into module proc table for config.cpp cross-module resolution |

The `g_proxyPort` is the **discovered** port — it's zero until `listen()` succeeds, then updated from `g_server->getPort()`. The constant `6821` is what ixwebsocket receives, but `g_proxyPort` (and therefore `GetWebSocketBridgePort()` and `WsBridge_GetPort()`) reflects whatever port the OS actually assigned. **The machinery for bind-then-publish already exists — only the hardcoded 6821 initializer prevents it from working.**

### Config / evidence files

| File:Line | Value |
|-----------|-------|
| `echovr/_local/config.json:10` | `"matchingservice_host": "ws://127.0.0.1:6821"` |
| `docs/design/module-extraction-plan.md:85` | Documents expected log line with `:6821` |
| `BUGS.md` (multiple lines) | Documents `127.0.0.1:6821` in evidence/log excerpts |

### Scripts (`launch-server.sh`)
No hardcoded 6821 — the script copies DLLs but doesn't reference the port.

### `justfile`
No reference to 6821.

### Tests
No reference to 6821 in `tests/`.

---

## 2. The bind site

**Module copy (active path):**
`src/modules/ws-bridge/src/ws_bridge.cpp:164`
```cpp
g_server = std::make_unique<ix::WebSocketServer>(6821, "127.0.0.1");
```

Then at line 584:
```cpp
auto [ok, errMsg] = g_server->listen();
```

Then at line 592:
```cpp
g_proxyPort = g_server->getPort();
```

**Gamepatches copy (inactive — module is loaded at runtime):**
`src/gamepatches/ws_bridge.cpp:170` — identical pattern.

**The port arrives as a hardcoded integer literal `6821`.** It is not read from config, not passed as a parameter. The `ix::WebSocketServer` constructor takes `(int port, const std::string& host, ...)`.

---

## 3. The publish side — service-redirect URL construction

### Site A: `AutoRelayThroughBridge` (`config.cpp:257-272`)

```cpp
static CHAR* AutoRelayThroughBridge(const CHAR* serviceKey, CHAR* url) {
  extern bool IsWebSocketBridgeActive();
  extern uint16_t GetWebSocketBridgePort();
  // ...
  thread_local CHAR relayUrl[512];
  snprintf(relayUrl, sizeof(relayUrl), "ws://127.0.0.1:%u", GetWebSocketBridgePort());
  // ...
  return relayUrl;
}
```

`GetWebSocketBridgePort()` returns `g_proxyPort`, which is set to `g_server->getPort()` AFTER `listen()` succeeds. **This IS bind-then-publish.** The redirect reads the port discovered at bind time — it does NOT read a config file or a hardcoded constant for the port number.

### Site B: `RedirectServiceUrl` (`config.cpp:380-394`)

```cpp
using GetPortFn = uint16_t (*)();
auto getPort = (GetPortFn)ResolveModuleProc("WsBridge_GetPort");
if (isActive && isActive() && isWebSocket) {
  snprintf(redirected, sizeof(redirected), "ws://127.0.0.1:%u", getPort ? getPort() : 0);
}
```

Resolves `WsBridge_GetPort` from the loaded `ws_bridge` module, which returns `g_proxyPort`. **Same pattern: reads the port discovered at bind time.**

### Answer: bind-then-publish is already wired.

The redirect code (`config.cpp`) does NOT independently read a hardcoded port or a config-file port. It reads `g_proxyPort`, which is set from `g_server->getPort()` AFTER the `listen()` call succeeds. If the bind site were changed from a hardcoded `6821` to `0` (OS-assigned), or to a retry loop, the redirects would automatically use whatever port the OS assigned — no changes needed in `config.cpp`.

The only coupling is that the hardcoded `6821` at the bind site must be replaced with the new port-selection logic. The publish side is ready.

---

## 4. External consumers

### Outside the game process

**`echovr/_local/config.json:10`:**
```json
"matchingservice_host": "ws://127.0.0.1:6821",
```
This is a LOCAL config override. If the ws_bridge port changes, this config value must be updated to match. With a dynamic port, this config key becomes either incorrect or must be removed (letting the game's own service-redirect handle it). This is the **only external consumer that would break.**

**docs/design/module-extraction-plan.md:85:** Documents expected log output — not a consumer, just documentation.

**No other external consumers found.** No plugins, scripts, tests, dashboards, or tools connect to port 6821. The broadcaster-bridge plugin connects to the game's UDP broadcaster port, not the ws_bridge. No test references 6821. No script references 6821.

---

## 5. Config surface

**The port is NOT currently overridable.** There is no config.yaml key, no config.json key, no CLI flag, no environment variable that controls the ws_bridge listen port.

- `config.json` has `matchingservice_host` set to `ws://127.0.0.1:6821` — this is the game's service URL that points AT the bridge, not a bridge config that sets the bridge's own port.
- The bridge's port is purely a compile-time constant.

To make it configurable, a new key would need to be added (e.g., `nevr_bridge_port` in config.json), read during bridge startup, and used in place of the `6821` literal.

---

## 6. Existing ledger entries

### N37

```
### N37. ws-bridge listen() lacks SO_REUSEADDR — zombie LISTEN socket blocks restart

| **Where**   | `src/modules/ws-bridge/src/ws_bridge.cpp:164` … |
| **Severity**| Medium                                           |
| **Status**  | Open — documented, not yet implemented.          |

The ixwebsocket SocketServer does not set SO_REUSEADDR on its listening socket…
Fix direction: (a) patch ixwebsocket; (b) contribute upstream;
(c) pre-create socket with SO_REUSEADDR. Not implemented — deferred.
```

### N38

```
### N38. Server-run teardown leaks wineserver — stale socket prevents rebind

| **Where**   | Server lifecycle: launch-server.sh → wine echovr.exe → kill → wineserver. |
| **Severity**| Medium                                                                     |
| **Status**  | Open — documented, not yet implemented.                                    |

When the server process is killed externally, the Wine wineserver does not clean up
its TCP sockets. The LISTEN socket on 6821 remains in the kernel's TCP table as a
zombie. Fix direction: (a) N13 graceful shutdown; (b) pre-launch guard in
launch-server.sh; (c) SO_REUSEADDR (N37) as defense-in-depth. Deferred.
```

---

## Summary

| Question | Answer |
|----------|--------|
| Hardcoded sites | 2 code sites (`ws_bridge.cpp:164`, `ws_bridge.cpp:170`), 1 config (`config.json:10`) |
| Bind site | `ws_bridge.cpp:164` — constant `6821`, but `g_proxyPort` captures the actual assigned port |
| Publish site | `config.cpp:268,387` — reads `g_proxyPort` via `GetWebSocketBridgePort()` / `WsBridge_GetPort()` — already bind-then-publish |
| Bind-then-publish possible without redesign? | **Yes.** The publish side already reads the discovered port. Only the bind-side constant needs to change. |
| External consumer that breaks under random port? | `echovr/_local/config.json:10` (`matchingservice_host: ws://127.0.0.1:6821`) — must be updated or removed |
| Config overridable? | No. No config key, CLI flag, or env var exists for the bridge port. |
