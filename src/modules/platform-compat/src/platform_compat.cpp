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

#include "common/nevr_module_interface.h"
#include "common/hooking.h"
#include "common/echovr_functions.h"
#include "common/logging.h"

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
      Log(EchoVR::LogLevel::Info,
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
      Log(EchoVR::LogLevel::Info, "[NEVR.PATCH] Fixed malformed NT path: '%ls' -> '%ls'", lpPathName, fixedPath);
    }

    BOOL result = OriginalCreateDirectoryW(pathToUse, lpSecurityAttributes);
    DWORD lastError = GetLastError();

    if (!result) {
      if (lastError == ERROR_ALREADY_EXISTS) {
        Log(EchoVR::LogLevel::Info, "[NEVR.PATCH] Directory '%ls' already exists - returning success", pathToUse);
        SetLastError(ERROR_SUCCESS);
        return TRUE;
      } else if (lastError == ERROR_FILE_NOT_FOUND || lastError == ERROR_PATH_NOT_FOUND) {
        Log(EchoVR::LogLevel::Info, "[NEVR.PATCH] Parent path missing for '%ls', creating recursively", pathToUse);
        wchar_t parentPath[512];
        wcsncpy(parentPath, pathToUse, 512);
        wchar_t* lastSlash = wcsrchr(parentPath, L'\\');
        if (lastSlash && lastSlash != parentPath) {
          *lastSlash = L'\0';
          CreateDirectoryWHook(parentPath, lpSecurityAttributes);
        }
        result = OriginalCreateDirectoryW(pathToUse, lpSecurityAttributes);
        if (result || GetLastError() == ERROR_ALREADY_EXISTS) {
          Log(EchoVR::LogLevel::Info, "[NEVR.PATCH] Successfully created '%ls' after parent creation", pathToUse);
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
  Log(EchoVR::LogLevel::Info, "[NEVR.PATCH] CreateDirectoryA('%s') called", lpPathName ? lpPathName : "<null>");

  BOOL result = OriginalCreateDirectoryA(lpPathName, lpSecurityAttributes);
  DWORD lastError = GetLastError();
  Log(EchoVR::LogLevel::Info, "[NEVR.PATCH] CreateDirectoryA result=%d, lastError=%lu", result, lastError);

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
    Log(EchoVR::LogLevel::Info, "[NEVR.PATCH] WinHTTP COM → libcurl bridge");

    static bool s_protectionFixed = false;
    if (!s_protectionFixed) {
      DWORD oldProtect;
      PVOID rdataStart = (PVOID)(EchoVR::g_GameBaseAddress + 0x16E8000);
      if (VirtualProtect(rdataStart, 0x2000, PAGE_READWRITE, &oldProtect)) {
        Log(EchoVR::LogLevel::Info, "[NEVR.PATCH] Made COM rdata page writable (was 0x%lX)", oldProtect);
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
      Log(EchoVR::LogLevel::Info, "[NEVR.PATCH] SSL/TLS modernization hook installed (Schannel)");
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
      Log(EchoVR::LogLevel::Info, "[NEVR.PATCH] CreateDirectoryW hook installed");
    }
  }

  OriginalCreateDirectoryA = (CreateDirectoryAFunc)GetProcAddress(hKernel32, "CreateDirectoryA");
  if (OriginalCreateDirectoryA != NULL) {
    if (!Hooking::Attach(reinterpret_cast<PVOID*>(&OriginalCreateDirectoryA),
                         reinterpret_cast<PVOID>(CreateDirectoryAHook))) {
      Log(EchoVR::LogLevel::Error, "[NEVR.PATCH] Failed to install CreateDirectoryA hook");
      ok = false;
    } else {
      Log(EchoVR::LogLevel::Info, "[NEVR.PATCH] CreateDirectoryA hook installed");
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
      Log(EchoVR::LogLevel::Info, "[NEVR.PATCH] WinHTTP to libcurl hook installed (CoCreateInstance)");
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

NEVR_MODULE_API int NvrModuleInit(const NvrModuleContext* ctx) {
  EchoVR::g_GameBaseAddress = (CHAR*)ctx->base_addr;
  EchoVR::InitializeFunctionPointers();

  Hooking::Initialize();

  InstallTLSHook();
  InstallCreateDirectoryHooks();
  InstallWinHTTPHook();

  Log(EchoVR::LogLevel::Info, "[NEVR.MODULE] platform_compat initialized");
  return 0;
}

NEVR_MODULE_API void NvrModuleShutdown(void) {
  Hooking::Shutdown();
}
