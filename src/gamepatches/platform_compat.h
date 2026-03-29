#pragma once

#include "common/pch.h"

/// Hooks AcquireCredentialsHandleW to enable modern TLS cipher suites (ECDSA, EdDSA, RSA).
void InstallTLSHook();

/// Hooks CoCreateInstance to redirect WinHTTP COM creation to the libcurl bridge.
void InstallWinHTTPHook();

/// Hooks CreateDirectoryW and CreateDirectoryA to fix _temp directory creation failures.
void InstallCreateDirectoryHooks();
