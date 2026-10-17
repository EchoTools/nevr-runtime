// BugSplat64.dll shim for the crash harness: the production host objects
// (runtime/host/) behind the real mangled MiniDmpSender::createReport export, plus
// test seams that run the host's runtime load/validate state machine against the
// sibling nevr.dll fixture and arm the capture's fault-injection hooks.
//
// The crash capture never depends on the runtime load: a valid dump is written
// whether nevr.dll is absent, ABI-incompatible, missing its entry point, or failing
// to initialize.

#include "core/runtime_abi.h"
#include "runtime/host/host_entry.h"
#include "runtime/host/runtime_load.h"

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cwchar>

namespace {

nevr_host::RuntimeLoader g_loader;

std::int64_t DummyOriginalWrapper(std::int64_t) { return 0; }

// Reentrancy probe: installed via CrashCapture::SetCaptureProbe and run while the
// host's atomic guard is held. It records its invocation count to a file (crash-safe:
// fixed buffer, raw WriteFile) and then raises a nested access violation so the harness
// can prove a second crash during reporting is suppressed rather than recursing or
// deadlocking.
volatile LONG g_probe_count = 0;
wchar_t g_probe_path[MAX_PATH] = {};

void WriteProbeCount(LONG count) {
  if (g_probe_path[0] == L'\0') return;
  HANDLE file = CreateFileW(g_probe_path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return;
  char buffer[32] = {};
  const int n = snprintf(buffer, sizeof(buffer), "%ld", static_cast<long>(count));
  DWORD written = 0;
  if (n > 0) WriteFile(file, buffer, static_cast<DWORD>(n), &written, nullptr);
  FlushFileBuffers(file);
  CloseHandle(file);
}

__declspec(noinline) void ReentrancyProbe() {
  const LONG count = InterlockedIncrement(&g_probe_count);
  WriteProbeCount(count);
  *reinterpret_cast<volatile int*>(0) = 0xDEAD;  // nested fault, guard held
}

}  // namespace

extern "C" void BugSplat_createReport(void*, PEXCEPTION_POINTERS exception_pointers) {
  nevr_host::CreateReport(exception_pointers);
}

extern "C" void nevr_host_test_SetCaptureProbe(const wchar_t* probe_path) {
  if (probe_path != nullptr && probe_path[0] != L'\0') {
    wcsncpy(g_probe_path, probe_path, MAX_PATH - 1);
    g_probe_path[MAX_PATH - 1] = L'\0';
  }
  nevr_host::CrashCapture::SetCaptureProbe(&ReentrancyProbe);
}

extern "C" void nevr_host_test_SetFailWrites() {
  nevr_host::CrashCapture::SetFailWrites(true);
}

extern "C" void nevr_host_test_Prepare(HMODULE host_module) {
  // Launcher mode prepares the diagnostics and the capture and installs nothing.
  nevr_host::OnProcessAttach(host_module, nullptr, true);
  g_loader.Prepare(host_module);
}

extern "C" std::uint32_t nevr_host_test_LoadRuntime() {
  NevrHostContext host{};
  host.header.struct_size = sizeof(NevrHostContext);
  host.header.abi_major = nevr_runtime_abi::kMajor;
  host.header.abi_minor = nevr_runtime_abi::kMinor;
  host.host_module = GetModuleHandleW(L"BugSplat64.dll");
  host.game_module = GetModuleHandleW(nullptr);
  host.original_game_main_wrapper = &DummyOriginalWrapper;
  host.wrapper_hook_target = reinterpret_cast<const void*>(&DummyOriginalWrapper);
  g_loader.Load(host, nevr_host::Diagnostics());
  return static_cast<std::uint32_t>(g_loader.Result().outcome);
}

extern "C" void DetoursExportPlaceholder() {}
