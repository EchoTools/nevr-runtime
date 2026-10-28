#pragma once

// The runtime's side of the exports BugSplat64.dll publishes for it (core/runtime_abi.h, NevrRuntimeExports;
// the host's thunks are runtime/host/runtime_exports.cpp). One entry per export, each with the export's
// signature, and each run through the guard in export_guard.h: nothing crosses the DLL boundary as an exception.

#include "core/runtime_abi.h"

namespace nevr_runtime_exports {

/// The table the runtime's descriptor carries. Static storage, never freed.
const NevrRuntimeExports& Table();

/// Logs what threw out of a guarded entry or the runtime's initialize, once, at Warning. The reporter every guard here uses.
void ReportThrew(const char* entry, const char* what) noexcept;

/// What an entry returns when the function behind it threw a std::exception. The values the host's thunks use
/// before the runtime is up, so a caller sees the same answer for "not up" and "failed".
inline constexpr int kInitFailed = -1;
inline constexpr int kAssetUnavailable = -1;

}  // namespace nevr_runtime_exports
