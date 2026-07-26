/* SYNTHESIS -- custom tool code, not from binary */
#pragma once

#include <cstdint>

namespace BuiltinLogFilter {
    void Init(uintptr_t base_addr, bool is_server);
    void Shutdown();

/// N90: hook pnsrad.dll's OWN statically-linked CLog::PrintfImpl. echovr.exe's
/// copy does not cover it — measured: our hook never saw "[NSUSER] saved" while
/// those lines reached the console. Call AFTER pnsrad.dll is loaded; idempotent.
void InstallPnsradHook();

/// Drive the periodic health report from OUTSIDE the log hook. If another module
/// takes the CLog target, the hook stops running — and a health check called only
/// from inside it stops with it, exactly when its warning is needed (N89).
void PollHealth();
}
