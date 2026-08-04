#pragma once
#include <windows.h>
#include <cstdint>
#include <string>

/// Set the remote wss:// URI that the proxy will connect to.
void SetWebSocketBridgeTarget(const char* uri);

/// Start the in-process ws:// proxy server. Must be called after SetWebSocketBridgeTarget.
void InstallWebSocketBridge();

/// Returns the local port the proxy is listening on (0 if not active).
uint16_t GetWebSocketBridgePort();

/// Returns true if the proxy is active and listening.
bool IsWebSocketBridgeActive();

/// Flag-only shutdown. Safe under the loader lock (DLL_PROCESS_DETACH), where a
/// thread join would deadlock. Does NOT release the listening socket.
void ShutdownWebSocketBridge();

/// N105: the real stop — closes remote connections and the listener, releasing
/// the socket FD. Call ONLY from a context that does not hold the loader lock:
/// the SIGINT/SIGTERM graceful path and gameserver's BeginGracefulShutdown.
/// Replaces the GetProcAddress("ws_bridge.dll", "WsBridge_Shutdown") lookup that
/// has resolved to null on every run since the N92 fold.
void StopWebSocketBridgeListener();

// ============================================================================
// N61 behavioral test hooks — NEVR_TEST_HOOKS only.
// Expose the Close handler's callback-lifecycle decision to unit tests.
// ============================================================================

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
uint64_t TestHook_SelectPlatformCode(bool hasUrlCredentials, bool noOvr);
void* TestHook_N61_CreateMockWs();
void  TestHook_N61_DestroyMockWs(void* handle);
void* TestHook_N61_GetRawWsPtr(void* handle);
void* TestHook_N61_RegisterLogin(void* remoteHandle, void* gameWsHandle);
void* TestHook_N61_RegisterMatchmaker(void* gameWsHandle, bool* callbackFired);
bool  TestHook_N61_SimulateCloseAndCheckCleared(void* rawGameWsPtr);
bool  TestHook_N61_HasActiveCallback();
void  TestHook_N61_ResetState();
bool  TestHook_N60_IsMutexFree();
#endif
