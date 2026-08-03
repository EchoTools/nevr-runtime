/*
 * platform_compat module — Schannel TLS modernization, CreateDirectory fixes,
 * and WinHTTP-to-libcurl CoCreateInstance hook.
 *
 * Each module DLL has its own MinHook statics — calls Hooking::Initialize()
 * before any hook installation.
 *
 * Hooks:
 *   - AcquireCredentialsHandleW (Schannel): enables TLS 1.2/1.3 with modern cipher suites
 *   - CreateDirectoryW / CreateDirectoryA: fixes _temp directory creation failures under Wine
 *   - CoCreateInstance (ole32): redirects WinHTTP COM creation to libcurl stub
 */

#include <windows.h>

#define SECURITY_WIN32
#include <objbase.h>
#include <schannel.h>
#include <security.h>
#include <sspi.h>

#include "extension/module_interface.h"
#include "core/hooking.h"
#include "abi/echovr_functions.h"
#include "core/logging.h"

// ---------------------------------------------------------------------------
// Schannel TLS hook
// ---------------------------------------------------------------------------

typedef SECURITY_STATUS(SEC_ENTRY* AcquireCredentialsHandleWFunc)(
    _In_opt_ LPWSTR pszPrincipal, _In_ LPWSTR pszPackage, _In_ unsigned long fCredentialUse,
    _In_opt_ void* pvLogonId, _In_opt_ void* pAuthData, _In_opt_ SEC_GET_KEY_FN pGetKeyFn,
    _In_opt_ void* pvGetKeyArgument, _Out_ PCredHandle phCredential, _Out_opt_ PTimeStamp ptsExpiry);

static AcquireCredentialsHandleWFunc OriginalAcquireCredentialsHandleW = NULL;

SECURITY_STATUS SEC_ENTRY AcquireCredentialsHandleWHook(
    _In_opt_ LPWSTR pszPrincipal, _In_ LPWSTR pszPackage,
    _In_ unsigned long fCredentialUse, _In_opt_ void* pvLogonId,
    _In_opt_ void* pAuthData, _In_opt_ SEC_GET_KEY_FN pGetKeyFn,
    _In_opt_ void* pvGetKeyArgument, _Out_ PCredHandle phCredential,
    _Out_opt_ PTimeStamp ptsExpiry) {

  if (pszPackage != NULL && lstrcmpW(pszPackage, UNISP_NAME_W) == 0 &&
      (fCredentialUse & SECPKG_CRED_OUTBOUND) != 0) {
    if (pAuthData != NULL) {
      SCHANNEL_CRED* schannelCred = (SCHANNEL_CRED*)pAuthData;
      schannelCred->grbitEnabledProtocols = SP_PROT_TLS1_2_CLIENT | 0x00002000;
      schannelCred->dwFlags |= SCH_CRED_NO_DEFAULT_CREDS;
      schannelCred->dwFlags &= ~SCH_CRED_MANUAL_CRED_VALIDATION;
      schannelCred->dwFlags |= SCH_USE_STRONG_CRYPTO;
      Log(EchoVR::LogLevel::Debug,
          "[NEVR.PATCH] SSL/TLS modernized: Enabled TLS 1.2/1.3 with ECDSA/EdDSA/RSA support");
    }
  }

  if (OriginalAcquireCredentialsHandleW != NULL) {
    return OriginalAcquireCredentialsHandleW(
        pszPrincipal, pszPackage, fCredentialUse, pvLogonId, pAuthData,
        pGetKeyFn, pvGetKeyArgument, phCredential, ptsExpiry);
  }
  return SEC_E_UNSUPPORTED_FUNCTION;
}

// ---------------------------------------------------------------------------
// CreateDirectory hooks (Wine _temp fix)
// ---------------------------------------------------------------------------

typedef BOOL(WINAPI* CreateDirectoryWFunc)(LPCWSTR, LPSECURITY_ATTRIBUTES);
static CreateDirectoryWFunc OriginalCreateDirectoryW = nullptr;

BOOL WINAPI CreateDirectoryWHook(LPCWSTR lpPathName, LPSECURITY_ATTRIBUTES lpSecurityAttributes) {
  if (lpPathName && wcsstr(lpPathName, L"_temp")) {
    wchar_t fixedPath[512];
    const wchar_t* pathToUse = lpPathName;

    if (wcsncmp(lpPathName, L"\\\\?\\", 4) == 0 && lpPathName[4] != L'\\' && lpPathName[5] != L':') {
      WCHAR currentDir[MAX_PATH];
      GetCurrentDirectoryW(MAX_PATH, currentDir);
      _snwprintf(fixedPath, 512, L"%ls\\%ls", currentDir, lpPathName + 4);
      pathToUse = fixedPath;
      Log(EchoVR::LogLevel::Debug, "[NEVR.PATCH] Fixed malformed NT path: '%ls' -> '%ls'", lpPathName, fixedPath);
    }

    BOOL result = OriginalCreateDirectoryW(pathToUse, lpSecurityAttributes);
    DWORD lastError = GetLastError();

    if (!result) {
      if (lastError == ERROR_ALREADY_EXISTS) {
        Log(EchoVR::LogLevel::Debug, "[NEVR.PATCH] Directory .%ls. already exists - returning success", pathToUse);
        SetLastError(ERROR_SUCCESS);
        return TRUE;
      } else if (lastError == ERROR_FILE_NOT_FOUND || lastError == ERROR_PATH_NOT_FOUND) {
        Log(EchoVR::LogLevel::Debug, "[NEVR.PATCH] Parent path missing for '%ls', creating recursively", pathToUse);
        wchar_t parentPath[512];
        wcsncpy(parentPath, pathToUse, 512);
        wchar_t* lastSlash = wcsrchr(parentPath, L'\\');
        if (lastSlash && lastSlash != parentPath) {
          *lastSlash = L'\0';
          CreateDirectoryWHook(parentPath, lpSecurityAttributes);
        }
        result = OriginalCreateDirectoryW(pathToUse, lpSecurityAttributes);
        if (result || GetLastError() == ERROR_ALREADY_EXISTS) {
          Log(EchoVR::LogLevel::Debug, "[NEVR.PATCH] Successfully created .%ls. after parent creation", pathToUse);
          SetLastError(ERROR_SUCCESS);
          return TRUE;
        }
      }
    } else {
      Log(EchoVR::LogLevel::Info, "[NEVR.PATCH] Successfully created directory '%ls'", pathToUse);
    }

    SetLastError(lastError);
    return result;
  }

  return OriginalCreateDirectoryW(lpPathName, lpSecurityAttributes);
}

typedef BOOL(WINAPI* CreateDirectoryAFunc)(LPCSTR, LPSECURITY_ATTRIBUTES);
static CreateDirectoryAFunc OriginalCreateDirectoryA = nullptr;

BOOL WINAPI CreateDirectoryAHook(LPCSTR lpPathName, LPSECURITY_ATTRIBUTES lpSecurityAttributes) {
  Log(EchoVR::LogLevel::Debug, "[NEVR.PATCH] CreateDirectoryA(.%s.) called", lpPathName ? lpPathName : "<null>");

  BOOL result = OriginalCreateDirectoryA(lpPathName, lpSecurityAttributes);
  DWORD lastError = GetLastError();
  Log(EchoVR::LogLevel::Debug, "[NEVR.PATCH] CreateDirectoryA result=%d, lastError=%lu", result, lastError);

  if (!result && lastError == ERROR_ALREADY_EXISTS) {
    if (lpPathName && strstr(lpPathName, "_temp")) {
      Log(EchoVR::LogLevel::Info,
          "[NEVR.PATCH] CreateDirectoryA('%s') failed with ERROR_ALREADY_EXISTS - returning success", lpPathName);
      SetLastError(ERROR_SUCCESS);
      return TRUE;
    }
  }

  SetLastError(lastError);
  return result;
}

// ---------------------------------------------------------------------------
// WinHTTP CoCreateInstance hook
// ---------------------------------------------------------------------------

static const CLSID CLSID_WinHttpRequest = {
    0x88d96a09, 0xf192, 0x11d4, {0xa6, 0x5f, 0x00, 0x40, 0x96, 0x32, 0x51, 0xe5}};

typedef HRESULT(WINAPI* CoCreateInstanceFunc)(REFCLSID, LPUNKNOWN, DWORD, REFIID, LPVOID*);
static CoCreateInstanceFunc OriginalCoCreateInstance = nullptr;

HRESULT WINAPI CoCreateInstanceHook(REFCLSID rclsid, LPUNKNOWN pUnkOuter, DWORD dwClsContext,
                                    REFIID riid, LPVOID* ppv) {
  if (IsEqualCLSID(rclsid, CLSID_WinHttpRequest)) {
    Log(EchoVR::LogLevel::Debug, "[NEVR.PATCH] WinHTTP COM → libcurl bridge");

    static bool s_protectionFixed = false;
    if (!s_protectionFixed) {
      DWORD oldProtect;
      PVOID rdataStart = (PVOID)(EchoVR::g_GameBaseAddress + 0x16E8000);
      if (VirtualProtect(rdataStart, 0x2000, PAGE_READWRITE, &oldProtect)) {
        Log(EchoVR::LogLevel::Debug, "[NEVR.PATCH] Made COM rdata page writable (was 0x%lX)", oldProtect);
      }
      s_protectionFixed = true;
    }

    extern HRESULT CreateWinHttpRequestStub(REFIID riid, void** ppvObject);
    HRESULT hr = CreateWinHttpRequestStub(riid, ppv);
    if (FAILED(hr)) {
      Log(EchoVR::LogLevel::Warning, "[NEVR.PATCH] WinHTTP stub creation failed: 0x%08lX", hr);
    }
    return hr;
  }

  return OriginalCoCreateInstance(rclsid, pUnkOuter, dwClsContext, riid, ppv);
}

// ---------------------------------------------------------------------------
// Hook installation helpers
// ---------------------------------------------------------------------------

static bool InstallTLSHook() {
  HMODULE hSecur32 = GetModuleHandleA("Secur32.dll");
  if (hSecur32 == NULL) {
    hSecur32 = LoadLibraryA("Secur32.dll");
  }
  if (hSecur32 != NULL) {
    OriginalAcquireCredentialsHandleW =
        (AcquireCredentialsHandleWFunc)GetProcAddress(hSecur32, "AcquireCredentialsHandleW");
    if (OriginalAcquireCredentialsHandleW != NULL) {
      if (!Hooking::Attach(reinterpret_cast<PVOID*>(&OriginalAcquireCredentialsHandleW),
                           reinterpret_cast<PVOID>(AcquireCredentialsHandleWHook))) {
        Log(EchoVR::LogLevel::Error, "[NEVR.PATCH] Failed to install AcquireCredentialsHandleW hook");
        return false;
      }
      Log(EchoVR::LogLevel::Debug, "[NEVR.PATCH] SSL/TLS modernization hook installed (Schannel)");
      return true;
    }
    Log(EchoVR::LogLevel::Warning, "[NEVR.PATCH] Failed to find AcquireCredentialsHandleW");
  } else {
    Log(EchoVR::LogLevel::Warning, "[NEVR.PATCH] Failed to load Secur32.dll for SSL/TLS hook");
  }
  return false;
}

static bool InstallCreateDirectoryHooks() {
  HMODULE hKernel32 = GetModuleHandleA("kernel32.dll");
  if (hKernel32 == NULL) return false;

  bool ok = true;
  OriginalCreateDirectoryW = (CreateDirectoryWFunc)GetProcAddress(hKernel32, "CreateDirectoryW");
  if (OriginalCreateDirectoryW != NULL) {
    if (!Hooking::Attach(reinterpret_cast<PVOID*>(&OriginalCreateDirectoryW),
                         reinterpret_cast<PVOID>(CreateDirectoryWHook))) {
      Log(EchoVR::LogLevel::Error, "[NEVR.PATCH] Failed to install CreateDirectoryW hook");
      ok = false;
    } else {
      Log(EchoVR::LogLevel::Debug, "[NEVR.PATCH] CreateDirectoryW hook installed");
    }
  }

  OriginalCreateDirectoryA = (CreateDirectoryAFunc)GetProcAddress(hKernel32, "CreateDirectoryA");
  if (OriginalCreateDirectoryA != NULL) {
    if (!Hooking::Attach(reinterpret_cast<PVOID*>(&OriginalCreateDirectoryA),
                         reinterpret_cast<PVOID>(CreateDirectoryAHook))) {
      Log(EchoVR::LogLevel::Error, "[NEVR.PATCH] Failed to install CreateDirectoryA hook");
      ok = false;
    } else {
      Log(EchoVR::LogLevel::Debug, "[NEVR.PATCH] CreateDirectoryA hook installed");
    }
  }
  return ok;
}

static bool InstallWinHTTPHook() {
  HMODULE hOle32 = GetModuleHandleA("ole32.dll");
  if (hOle32 == NULL) {
    hOle32 = LoadLibraryA("ole32.dll");
  }
  if (hOle32 != NULL) {
    OriginalCoCreateInstance = (CoCreateInstanceFunc)GetProcAddress(hOle32, "CoCreateInstance");
    if (OriginalCoCreateInstance != NULL) {
      if (!Hooking::Attach(reinterpret_cast<PVOID*>(&OriginalCoCreateInstance),
                           reinterpret_cast<PVOID>(CoCreateInstanceHook))) {
        Log(EchoVR::LogLevel::Error, "[NEVR.PATCH] Failed to install CoCreateInstance hook — WinHTTP bridge inactive");
        return false;
      }
      Log(EchoVR::LogLevel::Debug, "[NEVR.PATCH] WinHTTP to libcurl hook installed (CoCreateInstance)");
      return true;
    }
    Log(EchoVR::LogLevel::Warning, "[NEVR.PATCH] Failed to find CoCreateInstance");
  } else {
    Log(EchoVR::LogLevel::Warning, "[NEVR.PATCH] Failed to load ole32.dll for WinHTTP hook");
  }
  return false;
}

// ---------------------------------------------------------------------------
// Module interface
// ---------------------------------------------------------------------------

// N133 S5: report the module API version so the loader can refuse a module built
// against a newer host ABI. platform_compat reads no config keys, so the config_get
// addition is transparent to it — but it must still recompile against the new
// NvrModuleContext and report the current version.
NEVR_MODULE_API uint32_t platform_compat_ApiVersion(void) {
  return NEVR_MODULE_API_VERSION;
}

NEVR_MODULE_API int platform_compat_Init(const NvrModuleContext* ctx) {
  EchoVR::g_GameBaseAddress = (CHAR*)ctx->base_addr;
  EchoVR::InitializeFunctionPointers();

  Hooking::Initialize();

  /* All three return bool and the returns were DISCARDED, while success logged
   * nothing and only failure logged. So this function printed "initialized"
   * identically whether three hooks installed or zero did — which is precisely
   * how a silently-failing WinHTTP hook looks like a working one, and why
   * "the WinHTTP errors are back" had no corresponding log change.
   *
   * Report each outcome by name, plus an aggregate, in the shape N17 defined for
   * hook installs. */
  const bool tlsOk = InstallTLSHook();
  const bool dirOk = InstallCreateDirectoryHooks();
  const bool httpOk = InstallWinHTTPHook();
  const int okCount = (tlsOk ? 1 : 0) + (dirOk ? 1 : 0) + (httpOk ? 1 : 0);

  Log(okCount == 3 ? EchoVR::LogLevel::Info : EchoVR::LogLevel::Warning,
      "[NEVR.MODULE] platform_compat initialized: %d/3 hooks installed "
      "(tls=%s createdir=%s winhttp=%s)",
      okCount, tlsOk ? "ok" : "FAILED", dirOk ? "ok" : "FAILED",
      httpOk ? "ok" : "FAILED");

  /* The WinHTTP bridge is the one whose absence is silent-but-fatal: without it
   * the game falls back to its own HTTP stack and reports NoNetwork (N11). */
  if (!httpOk) {
    Log(EchoVR::LogLevel::Error,
        "[NEVR.MODULE] WinHTTP bridge NOT installed — the game will use its own "
        "HTTP stack and may report NoNetwork (N11)");
  }

  /* N120. This returned 0 — success — no matter how many hooks failed, including
   * the one the comment above calls silent-but-fatal. So a server whose TLS or
   * HTTP bridge never installed reported "module loaded" and ran on to fail later
   * somewhere unrelated, which is the worst of both: broken, and misattributed.
   *
   * On a dedicated server there is nobody watching a console, so report the
   * failure and let module_loader's existing FatalError kill the process at the
   * point of the actual defect.
   *
   * Mandatory = tls + winhttp. NOT createdir: that hook fixes a malformed NT path
   * Wine produces (the `_temp` bug) and has no counterpart on native Windows, so
   * requiring it would refuse to boot on the platform where it is meaningless.
   *
   * Server-gated deliberately. module_loader treats a non-zero init as fatal in
   * BOTH modes, so returning non-zero unconditionally would newly hard-fail a
   * client that previously limped along with degraded HTTP — the opposite of the
   * rule, which is that a server dies and a client warns. */
  const bool isServer = (ctx->flags & NEVR_MODULE_HOST_IS_SERVER) != 0;
  if (isServer && (!tlsOk || !httpOk)) {
    Log(EchoVR::LogLevel::Error,
        "[NEVR.MODULE] platform_compat FAILED on a server (tls=%s winhttp=%s) — "
        "reporting init failure; a server must not run with a degraded network "
        "stack (N120)",
        tlsOk ? "ok" : "FAILED", httpOk ? "ok" : "FAILED");
    return 1;
  }
  return 0;
}

NEVR_MODULE_API void platform_compat_Shutdown(void) {
  Hooking::Shutdown();
}
