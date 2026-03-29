#include "platform_compat.h"

#define SECURITY_WIN32
#include <objbase.h>
#include <schannel.h>
#include <security.h>
#include <sspi.h>

#include "common/echovr_functions.h"
#include "common/logging.h"
#include "gamepatches_internal.h"

/// <summary>
/// Original function pointers for Schannel APIs
/// </summary>
typedef SECURITY_STATUS(SEC_ENTRY* AcquireCredentialsHandleWFunc)(
    _In_opt_ LPWSTR pszPrincipal, _In_ LPWSTR pszPackage, _In_ unsigned long fCredentialUse, _In_opt_ void* pvLogonId,
    _In_opt_ void* pAuthData, _In_opt_ SEC_GET_KEY_FN pGetKeyFn, _In_opt_ void* pvGetKeyArgument,
    _Out_ PCredHandle phCredential, _Out_opt_ PTimeStamp ptsExpiry);

static AcquireCredentialsHandleWFunc OriginalAcquireCredentialsHandleW = NULL;

/// <summary>
/// Hook for AcquireCredentialsHandleW - enables modern TLS cipher suites and protocols.
/// This allows the game to connect to servers using ECDSA, EdDSA, and modern RSA certificates.
/// </summary>
SECURITY_STATUS SEC_ENTRY AcquireCredentialsHandleWHook(_In_opt_ LPWSTR pszPrincipal, _In_ LPWSTR pszPackage,
                                                        _In_ unsigned long fCredentialUse, _In_opt_ void* pvLogonId,
                                                        _In_opt_ void* pAuthData, _In_opt_ SEC_GET_KEY_FN pGetKeyFn,
                                                        _In_opt_ void* pvGetKeyArgument, _Out_ PCredHandle phCredential,
                                                        _Out_opt_ PTimeStamp ptsExpiry) {
  // Check if this is an Schannel client credential request
  if (pszPackage != NULL && lstrcmpW(pszPackage, UNISP_NAME_W) == 0 && (fCredentialUse & SECPKG_CRED_OUTBOUND) != 0) {
    // Modify the credential parameters to enable modern TLS
    if (pAuthData != NULL) {
      SCHANNEL_CRED* schannelCred = (SCHANNEL_CRED*)pAuthData;

      // Enable TLS 1.2 and TLS 1.3 (if available)
      // SP_PROT_TLS1_2_CLIENT = 0x00000800
      // SP_PROT_TLS1_3_CLIENT = 0x00002000 (Windows 11/Server 2022+)
      schannelCred->grbitEnabledProtocols = SP_PROT_TLS1_2_CLIENT | 0x00002000;  // TLS 1.2 + 1.3

      // Enable all cipher suites (let the server choose the best)
      // This includes ECDHE-ECDSA, ECDHE-RSA, and modern RSA cipher suites
      schannelCred->dwFlags |= SCH_CRED_NO_DEFAULT_CREDS;         // Use explicit creds
      schannelCred->dwFlags &= ~SCH_CRED_MANUAL_CRED_VALIDATION;  // Use system cert validation
      schannelCred->dwFlags |= SCH_USE_STRONG_CRYPTO;             // Enable strong crypto only

      Log(EchoVR::LogLevel::Info, "[NEVR.PATCH] SSL/TLS modernized: Enabled TLS 1.2/1.3 with ECDSA/EdDSA/RSA support");
    }
  }

  // Call the original function
  if (OriginalAcquireCredentialsHandleW != NULL) {
    return OriginalAcquireCredentialsHandleW(pszPrincipal, pszPackage, fCredentialUse, pvLogonId, pAuthData, pGetKeyFn,
                                             pvGetKeyArgument, phCredential, ptsExpiry);
  }

  return SEC_E_UNSUPPORTED_FUNCTION;
}

/// <summary>
/// Hook for CreateDirectoryW to fix "_temp" directory creation failure
/// </summary>
typedef BOOL(WINAPI* CreateDirectoryWFunc)(LPCWSTR, LPSECURITY_ATTRIBUTES);
CreateDirectoryWFunc OriginalCreateDirectoryW = nullptr;

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
CreateDirectoryAFunc OriginalCreateDirectoryA = nullptr;

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

/// <summary>
/// WinHTTP CLSID and IID constants from findings document
/// </summary>
static const CLSID CLSID_WinHttpRequest = {
    0x88d96a09, 0xf192, 0x11d4, {0xa6, 0x5f, 0x00, 0x40, 0x96, 0x32, 0x51, 0xe5}};

typedef HRESULT(WINAPI* CoCreateInstanceFunc)(REFCLSID, LPUNKNOWN, DWORD, REFIID, LPVOID*);
CoCreateInstanceFunc OriginalCoCreateInstance = nullptr;

HRESULT WINAPI CoCreateInstanceHook(REFCLSID rclsid, LPUNKNOWN pUnkOuter, DWORD dwClsContext, REFIID riid,
                                    LPVOID* ppv) {
  char logBuf[512];
  snprintf(logBuf, sizeof(logBuf),
           "[NEVR.PATCH] CoCreateInstance called: CLSID={%08lX-%04hX-%04hX-%02X%02X-%02X%02X%02X%02X%02X%02X}",
           rclsid.Data1, rclsid.Data2, rclsid.Data3, rclsid.Data4[0], rclsid.Data4[1], rclsid.Data4[2], rclsid.Data4[3],
           rclsid.Data4[4], rclsid.Data4[5], rclsid.Data4[6], rclsid.Data4[7]);
  Log(EchoVR::LogLevel::Info, logBuf);

  if (IsEqualCLSID(rclsid, CLSID_WinHttpRequest)) {
    Log(EchoVR::LogLevel::Info, "[NEVR.PATCH] WinHTTP COM → libcurl bridge");

    // The game stores COM-related data (IID, CLSID, type descriptors) in .rdata
    // which is read-only. Wine's COM implementation writes to these during
    // marshaling/QI, causing an AV. Make the surrounding pages writable.
    // The COM data lives around 0x16E8C88..0x16E9000 in the game's address space.
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

void InstallTLSHook() {
  // Hook SSL/TLS functions for modern cipher suite support (ECDSA, EdDSA, RSA)
  HMODULE hSecur32 = GetModuleHandleA("Secur32.dll");
  if (hSecur32 != NULL) {
    OriginalAcquireCredentialsHandleW =
        (AcquireCredentialsHandleWFunc)GetProcAddress(hSecur32, "AcquireCredentialsHandleW");
    if (OriginalAcquireCredentialsHandleW != NULL) {
      PatchDetour(&OriginalAcquireCredentialsHandleW, reinterpret_cast<PVOID>(AcquireCredentialsHandleWHook));
      Log(EchoVR::LogLevel::Info, "[NEVR.PATCH] SSL/TLS modernization hook installed (Schannel)");
    } else {
      Log(EchoVR::LogLevel::Warning, "[NEVR.PATCH] Failed to find AcquireCredentialsHandleW for SSL/TLS modernization");
    }
  } else {
    Log(EchoVR::LogLevel::Warning, "[NEVR.PATCH] Failed to load Secur32.dll for SSL/TLS modernization");
  }
}

void InstallWinHTTPHook() {
  // Hook CoCreateInstance for WinHTTP replacement
  HMODULE hOle32 = GetModuleHandleA("ole32.dll");
  if (hOle32 != NULL) {
    OriginalCoCreateInstance = (CoCreateInstanceFunc)GetProcAddress(hOle32, "CoCreateInstance");
    if (OriginalCoCreateInstance != NULL) {
      PatchDetour(&OriginalCoCreateInstance, reinterpret_cast<PVOID>(CoCreateInstanceHook));
      Log(EchoVR::LogLevel::Info, "[NEVR.PATCH] WinHTTP to libcurl hook installed (CoCreateInstance)");
    } else {
      Log(EchoVR::LogLevel::Warning, "[NEVR.PATCH] Failed to find CoCreateInstance for WinHTTP hook");
    }
  } else {
    Log(EchoVR::LogLevel::Warning, "[NEVR.PATCH] Failed to load ole32.dll for WinHTTP hook");
  }
}

void InstallCreateDirectoryHooks() {
  HMODULE hKernel32 = GetModuleHandleA("kernel32.dll");
  if (hKernel32 != NULL) {
    OriginalCreateDirectoryW = (CreateDirectoryWFunc)GetProcAddress(hKernel32, "CreateDirectoryW");
    if (OriginalCreateDirectoryW != NULL) {
      PatchDetour(&OriginalCreateDirectoryW, reinterpret_cast<PVOID>(CreateDirectoryWHook));
      Log(EchoVR::LogLevel::Info, "[NEVR.PATCH] CreateDirectoryW hook installed (fixes _temp creation)");
    } else {
      Log(EchoVR::LogLevel::Warning, "[NEVR.PATCH] Failed to find CreateDirectoryW");
    }

    OriginalCreateDirectoryA = (CreateDirectoryAFunc)GetProcAddress(hKernel32, "CreateDirectoryA");
    if (OriginalCreateDirectoryA != NULL) {
      PatchDetour(&OriginalCreateDirectoryA, reinterpret_cast<PVOID>(CreateDirectoryAHook));
      Log(EchoVR::LogLevel::Info, "[NEVR.PATCH] CreateDirectoryA hook installed (fixes _temp creation)");
    } else {
      Log(EchoVR::LogLevel::Warning, "[NEVR.PATCH] Failed to find CreateDirectoryA");
    }
  } else {
    Log(EchoVR::LogLevel::Warning, "[NEVR.PATCH] Failed to load kernel32.dll for CreateDirectory hooks");
  }
}
