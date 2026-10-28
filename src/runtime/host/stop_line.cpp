#include "runtime/host/stop_line.h"

namespace nevr_host {

std::size_t BuildStopLine(char*, std::size_t, const StopLineFacts&) noexcept { return 0; }  // scaffold

bool WriteStopLine(const StopLineFacts&) noexcept { return false; }  // scaffold

void DescribeRuntimeStop(const BootstrapDiagnostics&, const RuntimeLoadResult&, RuntimeStopText&) noexcept {}  // scaffold

bool WriteRuntimeStopLine(const BootstrapDiagnostics&, const RuntimeLoadResult&) noexcept { return false; }  // scaffold

const char* AttachFailureDetail(AttachFailure) noexcept { return ""; }  // scaffold

void ReportUnsupportedImage(BootstrapDiagnostics&, std::uint32_t) noexcept {}  // scaffold

void ReportNotAttached(BootstrapDiagnostics&) noexcept {}  // scaffold

}  // namespace nevr_host
