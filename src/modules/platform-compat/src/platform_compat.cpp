/*
 * platform_compat module — Schannel TLS modernization, CreateDirectory fixes,
 * and MSXML6 CoCreateInstance pass-through hook.
 *
 * Each module DLL has its own MinHook statics — calls Hooking::Initialize()
 * before any hook installation.
 *
 * Hooks:
 *   - AcquireCredentialsHandleW (Schannel): enables TLS 1.2/1.3 with modern cipher suites
 *   - CreateDirectoryW / CreateDirectoryA: fixes _temp directory creation failures under Wine
 *   - CoCreateInstance (ole32): logs the MSXML6 XMLHTTP CLSID and passes it through to the system object
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
#include "core/schannel_cred_guard.h"
#include "core/system_info.h"

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
    if (nevr::IsLegacySchannelCred(pAuthData)) {
      SCHANNEL_CRED* schannelCred = (SCHANNEL_CRED*)pAuthData;
      schannelCred->grbitEnabledProtocols = SP_PROT_TLS1_2_CLIENT | 0x00002000;
      schannelCred->dwFlags |= SCH_CRED_NO_DEFAULT_CREDS;
      schannelCred->dwFlags &= ~SCH_CRED_MANUAL_CRED_VALIDATION;
      schannelCred->dwFlags |= SCH_USE_STRONG_CRYPTO;
      Log(EchoVR::LogLevel::Debug,
          "[NEVR.PATCH] SSL/TLS modernized: Enabled TLS 1.2/1.3 with ECDSA/EdDSA/RSA support");
    } else if (pAuthData != NULL) {
      // A SCH_CREDENTIALS (dwVersion 5) is what libcurl's Schannel backend passes; it has a different
      // layout after dwVersion, so editing it as a SCHANNEL_CRED corrupts the credentials and the
      // handshake fails (curl_code=35 on native Windows). It already carries its own TLS settings.
      Log(EchoVR::LogLevel::Debug,
          "[NEVR.PATCH] Schannel credentials left as given: auth struct dwVersion=%lu is not a SCHANNEL_CRED",
          static_cast<unsigned long>(nevr::SchannelAuthVersion(pAuthData)));
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
      // This bug has no counterpart on native Windows (see the CoCreateInstance
      // hook comment below) — SystemInfo::Get().IsWine() makes that explicit
      // instead of leaving the branch looking like an unexplained ad hoc fix,
      // and flags the anomaly loudly if it's ever hit off Wine.
      static const bool s_isWine = SystemInfo::Get().IsWine();
      Log(s_isWine ? EchoVR::LogLevel::Debug : EchoVR::LogLevel::Warning,
          "[NEVR.PATCH] %smalformed NT path fixed (Wine _temp bug): '%ls' -> '%ls'",
          s_isWine ? "" : "UNEXPECTED on native Windows: ", lpPathName, fixedPath);
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
  // Match CreateDirectoryWHook's scope (only _temp paths) instead of logging
  // every CreateDirectoryA call in the process for what's the same workaround.
  if (!lpPathName || !strstr(lpPathName, "_temp")) {
    return OriginalCreateDirectoryA(lpPathName, lpSecurityAttributes);
  }

  Log(EchoVR::LogLevel::Debug, "[NEVR.PATCH] CreateDirectoryA(_temp) called path=%s", lpPathName);

  BOOL result = OriginalCreateDirectoryA(lpPathName, lpSecurityAttributes);
  DWORD lastError = GetLastError();
  Log(EchoVR::LogLevel::Debug, "[NEVR.PATCH] CreateDirectoryA result=%s lastError=%lu",
      result ? "true" : "false", lastError);

  if (!result && lastError == ERROR_ALREADY_EXISTS) {
    Log(EchoVR::LogLevel::Info,
        "[NEVR.PATCH] CreateDirectoryA('%s') failed with ERROR_ALREADY_EXISTS - returning success", lpPathName);
    SetLastError(ERROR_SUCCESS);
    return TRUE;
  }

  SetLastError(lastError);
  return result;
}

// ---------------------------------------------------------------------------
// MSXML6 XMLHTTP CoCreateInstance hook (observation only; the request is never redirected)
// ---------------------------------------------------------------------------

static const CLSID CLSID_FreeThreadedXMLHTTP60 = {
    0x88d96a09, 0xf192, 0x11d4, {0xa6, 0x5f, 0x00, 0x40, 0x96, 0x32, 0x51, 0xe5}};

typedef HRESULT(WINAPI* CoCreateInstanceFunc)(REFCLSID, LPUNKNOWN, DWORD, REFIID, LPVOID*);
static CoCreateInstanceFunc OriginalCoCreateInstance = nullptr;

HRESULT WINAPI CoCreateInstanceHook(REFCLSID rclsid, LPUNKNOWN pUnkOuter, DWORD dwClsContext,
                                    REFIID riid, LPVOID* ppv) {
  HRESULT hr = OriginalCoCreateInstance(rclsid, pUnkOuter, dwClsContext, riid, ppv);
  if (IsEqualCLSID(rclsid, CLSID_FreeThreadedXMLHTTP60)) {
    // The game drives this object through IXMLHTTPRequest2/3 (slot 3 Open, slot 4 Send); the
    // IWinHttpRequest-shaped stub wrote through arguments the game never passed (#133). The
    // system's object has the right layout, so the request goes to it.
    Log(EchoVR::LogLevel::Info, "[NEVR.PATCH] MSXML6 XMLHTTP CLSID passed through to the system object hr=0x%08lX",
        static_cast<unsigned long>(hr));
  }
  return hr;
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
        Log(EchoVR::LogLevel::Error, "[NEVR.PATCH] failed to install AcquireCredentialsHandleW hook: %s",
            Hooking::LastAttachError());
        return false;
      }
      Log(EchoVR::LogLevel::Debug, "[NEVR.PATCH] SSL/TLS modernization hook installed (Schannel)");
      return true;
    }
    Log(EchoVR::LogLevel::Warning,
        "[NEVR.PATCH] failed to find AcquireCredentialsHandleW export in Secur32.dll (error=%lu)", GetLastError());
  } else {
    Log(EchoVR::LogLevel::Warning, "[NEVR.PATCH] failed to load Secur32.dll for SSL/TLS hook (error=%lu)",
        GetLastError());
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
      Log(EchoVR::LogLevel::Error, "[NEVR.PATCH] failed to install CreateDirectoryW hook: %s",
          Hooking::LastAttachError());
      ok = false;
    } else {
      Log(EchoVR::LogLevel::Debug, "[NEVR.PATCH] CreateDirectoryW hook installed");
    }
  }

  OriginalCreateDirectoryA = (CreateDirectoryAFunc)GetProcAddress(hKernel32, "CreateDirectoryA");
  if (OriginalCreateDirectoryA != NULL) {
    if (!Hooking::Attach(reinterpret_cast<PVOID*>(&OriginalCreateDirectoryA),
                         reinterpret_cast<PVOID>(CreateDirectoryAHook))) {
      Log(EchoVR::LogLevel::Error, "[NEVR.PATCH] failed to install CreateDirectoryA hook: %s",
          Hooking::LastAttachError());
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
        Log(EchoVR::LogLevel::Error,
            "[NEVR.PATCH] failed to install CoCreateInstance hook: %s — the MSXML6 pass-through line will not be "
            "logged; the game still uses the system's XMLHTTP object",
            Hooking::LastAttachError());
        return false;
      }
      Log(EchoVR::LogLevel::Debug, "[NEVR.PATCH] MSXML6 pass-through hook installed (CoCreateInstance)");
      return true;
    }
    Log(EchoVR::LogLevel::Warning, "[NEVR.PATCH] failed to find CoCreateInstance export in ole32.dll (error=%lu)",
        GetLastError());
  } else {
    Log(EchoVR::LogLevel::Warning, "[NEVR.PATCH] failed to load ole32.dll for the MSXML6 hook (error=%lu)",
        GetLastError());
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
  // The host resolved and detoured these pointers before loading static modules.
  // Re-running InitializeFunctionPointers here replaces every MinHook trampoline
  // with its patched target; a hook calling its "original" then re-enters itself.

  const bool isServer = (ctx->flags & NEVR_MODULE_HOST_IS_SERVER) != 0;

  Hooking::Initialize();

  /* All three return bool and the returns were DISCARDED, while success logged
   * nothing and only failure logged. So this function printed "initialized"
   * identically whether three hooks installed or zero did — which is precisely
   * how a silently-failing hook looks like a working one.
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

  /* The CoCreateInstance hook only logs the MSXML6 XMLHTTP CLSID and passes it to the system
   * object (#133); the game's HTTP works with or without it, so its absence costs one log line
   * and never NoNetwork. A missing TLS hook is the one that degrades the network stack. */
  if (!httpOk) {
    Log(EchoVR::LogLevel::Warning,
        "[NEVR.MODULE] MSXML6 pass-through hook NOT installed — requests still reach the system "
        "XMLHTTP object, but the pass-through line will not be logged");
  }

  /* N120. This returned 0 — success — no matter how many hooks failed, including
   * the TLS hook. So a server whose TLS hook never installed reported "module loaded" and ran
   * on to fail later somewhere unrelated, which is the worst of both: broken, and misattributed.
   *
   * On a dedicated server there is nobody watching a console, so report the
   * failure and let module_loader's existing FatalError kill the process at the
   * point of the actual defect.
   *
   * Mandatory = tls. NOT createdir: that hook fixes a malformed NT path
   * Wine produces (the `_temp` bug) and has no counterpart on native Windows, so
   * requiring it would refuse to boot on the platform where it is meaningless.
   * NOT the CoCreateInstance hook: it only logs (see above).
   *
   * Server-gated deliberately. module_loader treats a non-zero init as fatal in
   * BOTH modes, so returning non-zero unconditionally would newly hard-fail a
   * client that previously limped along with a degraded network stack — the opposite of the
   * rule, which is that a server dies and a client warns. */
  if (isServer && !tlsOk) {
    Log(EchoVR::LogLevel::Error,
        "[NEVR.MODULE] platform_compat FAILED on a server (tls=FAILED) — "
        "reporting init failure; a server must not run with a degraded network "
        "stack");
    return 1;
  }
  return 0;
}

NEVR_MODULE_API void platform_compat_Shutdown(void) {
  Hooking::Shutdown();
}
