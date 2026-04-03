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
