# Draft: Testing Integration & Legacy Gameserver Debugging

## Research Findings

### nevr-runtime Repository
- **Type**: C++ DLL-based runtime patches for Echo VR
- **Build System**: CMake + vcpkg + MinGW cross-compilation
- **Components**:
  - `gameserver.dll` (current) - Protobuf-based, ServerContext abstraction
  - `gameserverlegacy.dll` (legacy) - Custom WebSocket, simpler architecture
  - `gamepatches.dll`, `telemetryagent.dll`
- **Testing Status**: NONE - Only manual shell scripts exist
- **Total Source**: ~6000+ lines across gameserver implementations

### evr-test-harness Repository
- **Type**: Go MCP server for AI-assisted testing
- **21 MCP Tools**:
  - `echovr_start/stop` - Process management
  - `echovr_state` - HTTP API queries
  - `echovr_events` - Log parsing (13 event types)
  - `echovr_input` - Keyboard/mouse simulation
  - `echovr_screenshot` - Visual verification
  - `debug_*` (12 tools) - Debugger integration via winedbg/GDB
- **Integration Tests**: 44 tests covering all tools
- **Capabilities**: Multi-instance, headless mode, event streaming

### Protocol Architecture

**Current gameserver.dll**:
- Uses protobuf `Envelope` messages via TCP
- `OnTcpMsgProtobuf()` dispatches based on message type
- Sends `LobbyEntrantConnected` when players join
- Uses `ServerContext` for state management

**Legacy gameserverlegacy.dll**:
- Custom WebSocket client (ixwebsocket)
- Binary message format: `[8 bytes msgId][8 bytes size][payload]`
- `OnTcpMsgLobbyEntrantsV3()` handles player joins
- Forwards as `SNSLobbyAcceptPlayersSuccessv2` legacy event
- Uses flat callback handlers

### Player Join Flow (Legacy)
1. ServerDB sends `SYMBOL_SERVERDB_LOBBY_ENTRANTS_V3` via WebSocket
2. Legacy DLL receives in WebSocket message handler
3. Calls `OnTcpMsgLobbyEntrantsV3()` 
4. Forwards to internal broadcaster as `SNSLobbyAcceptPlayersSuccessv2`
5. Game processes player acceptance

## Confirmed Requirements

### Error Type
- **Log error message** when player joins (not a crash)
- Needs A/B testing to identify exact failure point

### Working Backup Location
- `/mnt/games/CustomLibrary/echovr-34.4.631547.1/bin/win10/`
- `pnsradgameserver.dll` (110K, Jan 7) - WORKING
- This is the original game's DLL, NOT a NEVR build

### Current Build (Broken)
- `/home/andrew/src/nevr-server/dist/nevr-server-v3.2.0+30.83a0518/`
- `pnsradgameserver.dll` (18M, Feb 4) - current implementation with protobuf
- `gameserverlegacy.dll` (15M, Feb 4) - legacy implementation with WebSocket

### Testing Goals
- **Primary**: Debug player join issue using A/B testing
- **Secondary**: Use evr-test-harness for real game instance testing

### Test Mode
- Real EchoVR game instances via Wine (no mocks)

## Recent Code Changes (Feb 4, 2026)

### Commit Timeline
1. **6d24ea8** - WebSocket client integration for ServerDB
2. **8c4b456** - Fix vtable crashes, ServerDB message handling
3. **f9e8edd** - Thread-safe message queue, deduplication
4. **cc7acec** - LobbyEntrantsV3 handler (claimed to fix player join)

### Key Architectural Change
- Legacy: Used TcpBroadcaster vtable calls (crashed with MinGW)
- Current: Custom WebSocket client (ixwebsocket) for ServerDB

### The Missing Handler Problem
- Nakama sends `LobbyEntrantsV3` for player joins
- Legacy gameserver had no handler → messages dropped
- Fix added `OnTcpMsgLobbyEntrantsV3()` to forward as `ACCEPT_PLAYERS_SUCCESS`

## Testing Architecture (Current)

### Game Installation
- **Symlink**: `/home/andrew/src/evr-test-harness/ready-at-dawn-echo-arena` → `/mnt/games/CustomLibrary/ready-at-dawn-echo-arena`
- **NOT the backup**: Backup is at `/mnt/games/CustomLibrary/echovr-34.4.631547.1/`

### How DLLs Are Tested
1. Copy DLLs to game folder: `/mnt/games/CustomLibrary/ready-at-dawn-echo-arena/bin/win10/`
2. Start game via evr-test-harness (Wine + Xvfb headless)
3. Monitor events via `echovr_events` tool
4. Query state via `echovr_state` tool

### Multi-Instance Testing
- Can run up to 70 instances on ports 6721-6790
- Each instance is independent (different game/player)
- For player join testing, need:
  - Server instance (hosting game)
  - Client instance (joining as player)
  - OR Nakama backend to simulate player matching

### Player Join Simulation
- NOT direct injection of fake players
- Instead: Run multiple game instances
- Player joins detected via log parsing: `player_join` event type
- Pattern: `Player connected: userid=(\d+) name="([^"]+)"`

### Nakama Backend
- Required for player join flow (matchmaking)
- Starts PostgreSQL via docker-compose
- API on port 7350

## Test Strategy for Diagnosis

### Phase 1: A/B Test Setup
1. Create two game installations (or swap DLLs):
   - **Test A**: Working backup DLLs from `/mnt/games/CustomLibrary/echovr-34.4.631547.1/bin/win10/`
   - **Test B**: Current NEVR build from `/home/andrew/src/nevr-server/dist/`

2. For each:
   - Start Nakama backend
   - Start game as server
   - Attempt player join (via client or direct API)
   - Capture all logs

### Phase 2: Comparison
- Diff game logs between A and B
- Identify exact point of failure
- Check for missing message handlers, WebSocket issues, etc.

### Phase 3: Debugging
- Use evr-test-harness debugger tools if needed
- Set breakpoints at key handlers
- Trace message flow

## Scope Boundaries

### INCLUDE
- A/B test infrastructure in evr-test-harness
- Player join test case
- Log capture and comparison
- Debug tooling to trace message flow

### EXCLUDE
- Mock/simulation testing
- CI/CD integration (for now)
- Unit tests for nevr-runtime
