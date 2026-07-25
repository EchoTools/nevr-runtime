#pragma once
#include <windows.h>
#include <cstdint>

/// Set the remote wss:// URI that the proxy will connect to.
void SetWebSocketBridgeTarget(const char* uri);

/// Start the in-process ws:// proxy server. Must be called after SetWebSocketBridgeTarget.
void InstallWebSocketBridge();

/// Returns the local port the proxy is listening on (0 if not active).
uint16_t GetWebSocketBridgePort();

/// Returns true if the proxy is active and listening.
bool IsWebSocketBridgeActive();

/// Flag-only shutdown (avoids loader-lock deadlock).
void ShutdownWebSocketBridge();

// ============================================================================
// N61 behavioral test hooks — NEVR_TEST_HOOKS only.
// Expose the Close handler's callback-lifecycle decision to unit tests.
// ============================================================================

#ifdef NEVR_TEST_HOOKS
void* TestHook_N61_CreateMockWs();
void  TestHook_N61_DestroyMockWs(void* handle);
void* TestHook_N61_GetRawWsPtr(void* handle);
void* TestHook_N61_RegisterLogin(void* remoteHandle, void* gameWsHandle);
void* TestHook_N61_RegisterMatchmaker(void* gameWsHandle, bool* callbackFired);
bool  TestHook_N61_SimulateCloseAndCheckCleared(void* rawGameWsPtr);
bool  TestHook_N61_HasActiveCallback();
void  TestHook_N61_ResetState();
#endif
