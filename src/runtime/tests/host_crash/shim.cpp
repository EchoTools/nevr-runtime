// BugSplat64.dll shim for the crash harness: a faithful preview of the stable
// host. It implements the real BugSplat_createReport export (mapped from the
// MSVC-mangled MiniDmpSender::createReport name, as in the production
// exports.def) backed by the host crash-capture unit, plus two test seams that
// exercise the host runtime-load state machine against the sibling nevr.dll.
//
// The crash capture never depends on the runtime load: a valid dump is written
// whether nevr.dll is absent, ABI-incompatible, missing its entry point, or
// failing to initialize.

#include "core/runtime_abi.h"
#include "runtime/host/bootstrap_diagnostics.h"
#include "runtime/host/crash_capture.h"
#include "runtime/host/runtime_load.h"

#include <windows.h>

#include <cstdint>

namespace {

NevrHost::CrashCapture g_capture;
NevrHost::BootstrapDiagnostics g_diagnostics;
NevrHost::RuntimeLoader g_loader;
bool g_prepared = false;

std::uint64_t DummyOriginalPreprocess(void*) { return 0; }

}  // namespace

extern "C" void BugSplat_createReport(void*, PEXCEPTION_POINTERS exception_pointers) {
  g_capture.Capture(exception_pointers);
}

extern "C" void NevrHostTest_Prepare(HMODULE shim_module) {
  g_diagnostics.Prepare();
  g_capture.Prepare(shim_module);
  g_loader.Prepare(shim_module);
  g_prepared = true;
}

extern "C" std::uint32_t NevrHostTest_LoadRuntime() {
  if (!g_prepared) return 0xFFFFFFFFu;
  NevrHostContext host{};
  host.header.struct_size = sizeof(NevrHostContext);
  host.header.abi_major = NevrAbi::kMajor;
  host.header.abi_minor = NevrAbi::kMinor;
  host.host_module = GetModuleHandleW(L"BugSplat64.dll");
  host.game_module = GetModuleHandleW(nullptr);
  host.original_preprocess = &DummyOriginalPreprocess;
  g_loader.Load(host, g_diagnostics);
  return static_cast<std::uint32_t>(g_loader.Result().outcome);
}

extern "C" void DetoursExportPlaceholder() {}
