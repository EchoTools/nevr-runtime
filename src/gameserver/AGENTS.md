# src/gameserver/

**Generated:** 2026-02-09  
**Commit:** abc5734  
**Branch:** build/mingw-minhook

## OVERVIEW

Multiplayer server DLL injected as `pnsradgameserver.dll`. Implements IServerLib interface, manages ServerContext state machine (lobby/match/postgame), encodes/decodes realtime messages, communicates with external services (ServerDB, WebSocket).

## STRUCTURE

```
gameserver.cpp (938 lines)  # IServerLib implementation, state machine
servercontext.h             # ServerContext class, state transitions
messages.h                  # Protocol symbols, message encoding
dllmain.cpp                 # DLL entry point, IServerLib exports
```

## WHERE TO LOOK

| Task | Location | Notes |
|------|----------|-------|
| IServerLib entry points | `gameserver.cpp` line 25-45 | NewGameServer, Create, Load, etc. |
| State machine | `servercontext.h` line 78 | Lobby → InMatch → PostGame transitions |
| Message encoding | `messages.h` line 41 | Symbol IDs, realtime::Envelope protocol |
| Event handling | `gameserver.cpp` line 345 | ProcessGameEvent, state-driven dispatch |
| ServerDB communication | `gameserver.cpp` line 567 | HTTP POST to external service |
| WebSocket streaming | `gameserver.cpp` line 623 | ixwebsocket, event broadcasting |
| Player session tracking | `servercontext.h` line 95 | PlayerSession map, GUID keys |

## CONVENTIONS

- **Links parent src/common/** (NOT self-contained like gamepatches)
- **IServerLib interface** (exported symbols for game engine)
- **ServerContext singleton** (one instance per game session)
- **Realtime protocol** (protobuf messages via realtime::Envelope)
- **Static runtime** (`/MT` for MSVC, `-static-libstdc++` for MinGW)

## ANTI-PATTERNS (THIS PROJECT)

| Forbidden | Why |
|-----------|-----|
| Direct game memory access | Use IServerLib callbacks, not memory scanning |
| Blocking I/O in event handlers | ServerContext is single-threaded, use async |
| Hardcoded ServerDB URLs | Read from config or environment variables |
| Modifying realtime protocol | Defined in extern/nevr-common, codegen only |
| Dynamic CRT | Must use static `/MT` for injection |

## BUILD

- **CMake target**: `gameserver`
- **Output**: `pnsradgameserver.dll` (renamed from gameserver.dll)
- **Dependencies**: src/common, protobufnevr, ixwebsocket, jsoncpp, curl
- **Exports**: IServerLib symbols (NewGameServer, Create, Load, Save, etc.)

## DEPLOYMENT

1. Build produces `gameserver.dll`
2. PowerShell script (`admin/Game-Version-Check.ps1` line 106) renames to `pnsradgameserver.dll`
3. Copy to Echo VR install directory (game LoadLibrary at runtime)
4. Game calls NewGameServer() → IServerLib lifecycle begins

## STATE MACHINE (ServerContext)

**Transitions**: Lobby → (MatchStarted) → InMatch → (MatchEnded) → PostGame → (NewMatchRequested) → Lobby

**Implementation**: `servercontext.h` line 78 (state enum), `gameserver.cpp` line 345 (event dispatch)

## MESSAGE PROTOCOL

- **Symbol IDs**: `messages.h` line 41 (game symbols), line 81 (broadcaster symbols)
- **Encoding**: `gameserver.cpp` line 234 (EncodeMessage, realtime::Envelope)
- **Decoding**: `gameserver.cpp` line 278 (DecodeMessage, protobuf deserialization)

## DEBUGGING

- **State transitions**: Log ServerContext state changes in ProcessGameEvent
- **Message errors**: Check symbol ID mismatches in messages.h
- **WebSocket disconnect**: Verify ixwebsocket config, check network logs
- **Missing echovr.h**: Build parent src/common/ first, check include paths
