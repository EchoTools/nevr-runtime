#pragma once

// The process's update worker, PC (docs/adr/0024-pc-self-update.md, sections 3 and 6): the entry points the runtime calls. The worker
// itself is core/update/worker.h, shared with Quest; the binding to this process (the core's C ABI, the verifier, the log, the clock,
// the root) is update_process.cpp.
//
//   * StartProcessWorker is called once, from RunDeferredRuntimeBootstrap after the command line is parsed (N146): nothing starts
//     from DllMain, initialize or the host.
//   * OnFrame is the per-frame dispatcher's hook (frame/tick.cpp, DispatchPerFrameWork). It does nothing until StartProcessWorker ran.

#include <windows.h>

#include <cstdint>

#include "core/update/worker.h"

namespace nevr_update {

/// The process's worker, built from the host's capability word (nevr_runtime_abi::HostCanSwap), the command line (`-no-update`),
/// config.yaml (`update.enabled`, `update.channel`) and the build (nevr_plugin_trust::IsDevBuild). Call once from
/// RunDeferredRuntimeBootstrap after the command line is parsed.
void StartProcessWorker(bool host_can_swap, bool flag_no_update, bool server);
/// The per-frame dispatcher's hook (frame/tick.cpp, DispatchPerFrameWork). A no-op until StartProcessWorker ran.
void OnFrame(std::uint64_t now_us);

}  // namespace nevr_update
