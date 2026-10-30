#pragma once
// winsock2.h must precede windows.h: windows.h without WIN32_LEAN_AND_MEAN pulls in
// winsock.h, and a later winsock2.h then warns (MinGW -Wcpp) or errors (MSVC).
#include <winsock2.h>
#include <windows.h>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

/// Set the remote wss:// URI that the proxy will connect to.
void SetWebSocketBridgeTarget(const char* uri);

/// Start the in-process ws:// proxy server. Must be called after SetWebSocketBridgeTarget.
void InstallWebSocketBridge();

/// Returns the local port the proxy is listening on (0 if not active).
uint16_t GetWebSocketBridgePort();

/// Returns the local port the matchmaker listener is bound to (0 if it
/// failed to bind or hasn't started yet — see N146 in ws_bridge.cpp).
/// pnsradmatchmaking.dll's compiled-in default host is patched in-memory
/// (pnsrad_enabler.cpp) to point at whatever port this returns, so the two
/// sides always agree even though the port is chosen at runtime.
uint16_t GetMatchmakerBridgePort();

/// Returns true if the proxy is active and listening.
bool IsWebSocketBridgeActive();

/// Flag-only shutdown. Safe under the loader lock (DLL_PROCESS_DETACH), where a
/// thread join would deadlock. Does NOT release the listening socket.
void ShutdownWebSocketBridge();

/// N105: the real stop — closes remote connections and the listener, releasing
/// the socket FD. Call ONLY from a context that does not hold the loader lock:
/// console-control shutdown and gameserver's BeginGracefulShutdown.
void StopWebSocketBridgeListener();

// ============================================================================
// N61 behavioral test hooks — NEVR_TEST_HOOKS only.
// Expose the Close handler's callback-lifecycle decision to unit tests.
// ============================================================================

#ifdef NEVR_SCENARIO_CONTROL
/// Scenario-control builds only: delivers `frame` to the game as a server->game frame on the login
/// connection (roster/party feed, frame log, send). False with `error` set when it cannot.
bool InjectServerFrameForTest(const std::string& frame, std::string* error);
#endif

#ifdef NEVR_TEST_HOOKS
// Production helpers exposed only to the Wine unit-test target. These keep the
// login wire format and callback boundary covered without exporting them from
// BugSplat64.dll.
std::string TestHook_BuildLoginRequest(uint64_t discordId, uint64_t platformCode,
                                       const std::string& displayName,
                                       const std::string& accessToken);
const char* TestHook_PlatformPrefix(uint64_t platformCode);
int TestHook_GuardWsCallbackForwardsArguments(int first, int second);
bool TestHook_GuardWsCallbackContainsStdException();
bool TestHook_GuardWsCallbackPropagatesNonStdException();
int TestHook_LogFrameMessages(const char* direction, int connIdx, const std::string& frame);
int TestHook_LogGameToServerFrame(const std::string& frame);
void TestHook_ObserveSocialFrames(const char* direction, int connIdx, const std::string& frame);
/// Forgets every lobby-record window (compat/lobby_frame.h RecordLimiter), so a test starts with a full burst.
void TestHook_ResetLobbyFrameLimiter();
/// The clock (milliseconds) the lobby-record limiter reads; nullptr restores GetTickCount64.
void TestHook_SetLobbyFrameClock(uint64_t (*clockMs)());
namespace nevr_social_party {
struct View;
}
/// The party a session record names (social_party.h View); nullptr restores the process-wide party state.
void TestHook_SetLobbyPartySource(nevr_social_party::View (*view)());
bool TestHook_ReadLoginFailureDiagnostic(const std::string& frame, uint64_t* statusCode, size_t* messageBytes);
bool TestHook_LogLoginFailureDiagnostic(const std::string& frame, bool serverMode);
bool TestHook_MoveCodeLineFirst(const std::string& frame, std::string* out);
uint64_t TestHook_SelectPlatformCode(bool hasUrlCredentials, bool noVr);
// Stands in for the game's flag word at game+0x7ae0 (what the login reads): a value >= 0 is the word, -1 reads the game.
void TestHook_SetGameFlagWord(int64_t flags);
std::string TestHook_SelectRemoteBearer(bool hasUrlCredentials, const std::string& jwt,
                                        const std::string& serverKey);
bool TestHook_IsBearerReplacingPath(const std::string& url);
void* TestHook_N61_CreateMockWs();
void  TestHook_N61_DestroyMockWs(void* handle);
void* TestHook_N61_GetRawWsPtr(void* handle);
void* TestHook_N61_RegisterLogin(void* remoteHandle, void* gameWsHandle);
void* TestHook_N61_RegisterMatchmaker(void* gameWsHandle, bool* callbackFired);
bool  TestHook_N61_SimulateCloseAndCheckCleared(void* rawGameWsPtr);
size_t TestHook_GameSocketsBoundTo(void* remoteHandle);
bool TestHook_SendFrameToServer(const std::string& frame);  // the sender the bridge gives the self-check unit
void TestHook_CloseGameSocketsForRemote(void* remoteHandle, int connIdx, unsigned int code);
// A real ix client/server pair: the server drops the login remote and its Close callback ends the session. False when
// the callback's thread never came back (then end the process; do not tear down).
bool TestHook_LoginRemoteDroppedThroughRealIx(unsigned int timeoutMs);
// #738: a send on a login remote whose connection has gone, from the thread that made it. `path` 0 SendFrameToServer, 1 the
// game->server forward, 2 the flush on the remote's open. 0: the send failed and returned; 1: the sending thread never
// came back (end the process); 2: no send failed (the rig, not the bridge).
int TestHook_SendOnDroppedRemote(int path, unsigned int timeoutMs);
bool TestHook_ForgetLoginSession(void* remoteHandle, int* nextConnIdx);
bool  TestHook_N61_HasActiveCallback();
int   TestHook_SharedRouteConn();  // the conn frames from the shared login session go to, -1 none
void  TestHook_N61_ResetState();
bool  TestHook_N60_IsMutexFree();
size_t TestHook_ConnectedGameSockets();  // game sockets the bridge's two listeners still hold
void* TestHook_LoginGameWsPtr();
// Shared-route test seam: fn (nullptr clears it) runs with the target socket just before the frame is sent to it.
void TestHook_SharedRoute_SetBeforeSend(void (*fn)(void* target));
// A weak handle on the first game socket the service listener holds (empty when none).
std::weak_ptr<void> TestHook_FirstServiceClientWeak();
// #708 S2b: the router's account gate and remote-open policy, for a test (production wires neither).
// gate: -1 none (the account is always available), 0 awaiting, 1 ready, 2 refused. Read on every decision; call
// TestHook_ReevaluateRouterGate after changing it to release or end a held session.
void TestHook_SetRouterGate(int gate);
void TestHook_SetRefuseRemoteOpens(bool refuse);  // the adapter refuses to start a remote, as Quest's does with no token
void TestHook_ReevaluateRouterGate();
// The live router's counters; false when the bridge routes nothing through it.
bool TestHook_RouterStats(uint64_t* droppedGame, uint64_t* droppedRemote, uint64_t* droppedUnrequires,
                          uint64_t* loginsRemoved, uint64_t* heldRemotes);
#endif
