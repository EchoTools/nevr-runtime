/* SYNTHESIS -- custom tool code, not from binary */
#pragma once

#include <cstdint>

namespace nevr_builtin_log_filter {
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

/// #1846: the GPU name and driver the runtime read (render_config's first line), for the GPU-hung notice (gpu_hung_notice.h),
/// which names them when the game's own System Info lines come after its HUNG line. Safe from any thread.
void NoteRuntimeAdapter(const char* gpu_name, const char* gpu_driver);
}
