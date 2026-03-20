# pnsnevr - Nakama Backend for NEVR

This is a prototype DLL that replaces NEVR's social platform plugins (pnsdemo.dll, psnovr.dll, pnsrad.dll) with a [Nakama](https://heroiclabs.com/nakama/) backend.

## Loading the DLL

The game's plugin loading system supports **3 methods** to load pnsnevr.dll:

### Option 1: Config-Based (Recommended - No Patching Required!)

The game's `PlatformModuleDecisionAndInitialize` function uses `GetPluginNameFromConfig()` to determine which DLL to load. Simply modify the game's config file:

```json
{
    "matchmaking_plugin": "pnsnevr",
    "server_plugin": "pnsnevr"
}
```

The game will then load `pnsnevr.dll` from the same directory as the other plugin DLLs.

**Function addresses (for reference):**
- `GetPluginNameFromConfig`: 0x1405fe290
- `LogMatchmakingPluginLoad`: 0x14060b810
- `LogServerPluginLoad`: 0x14060bb70
- `PlatformModuleDecisionAndInitialize`: 0x140157fb0

### Option 2: IAT Hook / Detour

If config modification isn't possible, you can hook `GetPluginNameFromConfig` using [Microsoft Detours](https://github.com/microsoft/Detours):

```cpp
// Detour GetPluginNameFromConfig to return "pnsnevr" for plugin keys
static auto Original_GetPluginNameFromConfig = 
    (const char*(*)(void*, const char*, const char*, int))0x1405fe290;

const char* Hooked_GetPluginNameFromConfig(void* config, const char* key, 
                                           const char* default_val, int param4) {
    if (strcmp(key, "matchmaking_plugin") == 0 ||
        strcmp(key, "server_plugin") == 0) {
        return "pnsnevr";
    }
    return Original_GetPluginNameFromConfig(config, key, default_val, param4);
}
```

### Option 3: Binary Patch

Patch `PlatformModuleDecisionAndInitialize` to check for a new `"social_platform"` config key.

## Building

### Prerequisites

1. Visual Studio 2022 (or compatible)
2. CMake 3.20+
3. [nakama-cpp](https://github.com/heroiclabs/nakama-cpp) SDK

### Building nakama-cpp

```bash
cd pnsnevr/nakama-cpp
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . --config Release
```

### Building pnsnevr

```bash
cd pnsnevr
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release -DNAKAMA_SDK_PATH=../nakama-cpp
cmake --build . --config Release
```

This produces `pnsnevr.dll`.

### Mock Build (No Nakama SDK)

For testing without the full Nakama SDK:

```bash
cmake .. -DCMAKE_BUILD_TYPE=Debug -DPNSNEVR_MOCK=ON
cmake --build . --config Debug
```

The mock build uses stub implementations that log to `OutputDebugStringA`.

## Configuration

Create `nakama_config.json` in the game directory:

```json
{
    "api_endpoint": "http://127.0.0.1",
    "http_port": 7350,
    "grpc_port": 7349,
    "server_key": "defaultkey",
    "auth_method": "device",
    "auto_create_user": true,
    "features": {
        "friends": true,
        "parties": true,
        "matchmaking": true,
        "leaderboards": false,
        "storage": false
    }
}
```

## Exported Functions

The DLL exports these functions that EchoVR expects:

| Export | Description |
|--------|-------------|
| `DLL_Initialize` | Called on plugin load. Initializes Nakama client. |
| `DLL_Shutdown` | Called on plugin unload. Disconnects from Nakama. |
| `DLL_Tick` | Called every frame. Pumps Nakama async operations. |
| `DLL_GetVersion` | Returns version string. |

## Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                        EchoVR.exe                           │
├─────────────────────────────────────────────────────────────┤
│  PlatformModuleDecisionAndInitialize (0x140157fb0)          │
│         │                                                   │
│         ▼                                                   │
│  GetPluginNameFromConfig("matchmaking_plugin", ...)         │
│         │                                                   │
│         ▼                                                   │
│  LoadLibrary("pnsnevr.dll")                                │
│         │                                                   │
│         ▼                                                   │
│  DLL_Initialize(game_state)                                 │
└─────────────────────────────────────────────────────────────┘
                          │
                          ▼
┌─────────────────────────────────────────────────────────────┐
│                     pnsnevr.dll                            │
├─────────────────────────────────────────────────────────────┤
│  ┌─────────────┐   ┌──────────────┐   ┌─────────────────┐  │
│  │  dllmain    │──▶│  GameBridge  │──▶│ MessageHandlers │  │
│  └─────────────┘   └──────────────┘   └─────────────────┘  │
│         │                                      │            │
│         ▼                                      ▼            │
│  ┌─────────────┐                    ┌─────────────────┐    │
│  │   Config    │                    │  NakamaClient   │    │
│  └─────────────┘                    └─────────────────┘    │
│                                              │              │
│                                              ▼              │
│                                     ┌─────────────────┐    │
│                                     │  nakama-cpp SDK │    │
│                                     └─────────────────┘    │
└─────────────────────────────────────────────────────────────┘
                          │
                          ▼
              ┌───────────────────────┐
              │    Nakama Server      │
              │  (self-hosted/cloud)  │
              └───────────────────────┘
```

## Files

| File | Description |
|------|-------------|
| `CMakeLists.txt` | Build configuration |
| `src/dllmain.cpp` | DLL entry point and exports |
| `src/config.h/cpp` | Configuration parsing |
| `src/nakama_client.h/cpp` | Nakama SDK wrapper |
| `src/game_bridge.h/cpp` | Interface to EchoVR internals |
| `src/message_handlers.h/cpp` | Message handler registration |

## Game Structure Offsets

These offsets were discovered via Ghidra analysis of `PlatformModuleDecisionAndInitialize`:

| Offset | Description |
|--------|-------------|
| `+0xa30` | TCP Broadcaster pointer |
| `+0xa32` | UDP Broadcaster pointer |
| `+0xb54` | UDP handler count |
| `+0xb65` | TCP handler count |
| `+0xb68` | Mode flags pointer |

## Message Types

The DLL uses message IDs in the `0x8000+` range to avoid collision with game messages:

| ID | Message |
|----|---------|
| `0x8001-0x8006` | Friends system messages |
| `0x8010-0x8016` | Party system messages |
| `0x8020-0x8024` | Matchmaking messages |
| `0x8030-0x8031` | Presence/status messages |
| `0x8040-0x8042` | Connection/error messages |

## Testing

### Unit Testing

```bash
cd pnsnevr/build
ctest --output-on-failure
```

### Integration Testing

1. Start a local Nakama server:
   ```bash
   docker run -p 7350:7350 heroiclabs/nakama
   ```

2. Configure `nakama_config.json` with localhost endpoint

3. Run NEVR with the pnsnevr plugin

4. Monitor `OutputDebugStringA` output with DebugView or similar

## TODO

- [ ] Implement full nakama-cpp SDK integration (currently uses mocks)
- [ ] Add realtime socket support for live presence updates
- [ ] Implement party chat over Nakama channels
- [ ] Add authentication persistence (session tokens)
- [ ] Implement leaderboard integration
- [ ] Add cloud storage sync for player data

## License

This is a community reconstruction effort. Not affiliated with Ready At Dawn or Heroic Labs.
