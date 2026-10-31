#pragma once

// The host's POST for the crash report send (core/crash_send.h): the smallest HTTPS client BugSplat64.dll can carry.
//
// The host's imports are pinned to kernel32, crypt32, advapi32 and the UCRT (tools/verify_pe_contract.py), and it links no
// TLS stack, so this uses the one the system already has: winhttp.dll, loaded by name (LoadLibraryExW from System32) on the
// sender's thread and never at attach. A failed load is a failed POST, not a failed host. The certificate is verified by the
// system, TLS 1.2 is the floor, no proxy is consulted (a proxy auto-detect can stall for seconds), and no redirect is
// followed. The request carries no cookie and no credential. The response body is not read: the status is the answer.

#include "core/crash_send.h"

namespace nevr_host {

struct WinHttpPostContext {
  bool allowPlainHttp = false;  // a test against a loopback server only; the host never sets it
};

/// A nevr_crash_send::PostFn. `context` is a WinHttpPostContext (or null). Never throws; the last Win32 error is in
/// PostResult::errorCode when no status was obtained.
nevr_crash_send::PostResult WinHttpPost(void* context, const std::string& url, const std::string& body,
                                        const nevr_crash_send::Timeouts& timeouts);

}  // namespace nevr_host
