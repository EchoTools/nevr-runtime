// BugSplat64.dll shim for the rendezvous harness: the production host entry points
// (runtime/host/host_entry.h) behind the DLL's DllMain and the NEVR_SetGameModule
// export, with two test seams resolved from the executable:
//   - RendezvousProbeGameImage supplies a synthetic game image (the harness cannot be
//     echovr.exe), where production passes the process image;
//   - RendezvousProbeLauncherMode, when 1, makes DllMain skip the direct-import install
//     so the test can prove NEVR_SetGameModule does the full install itself.
//
//   - DLL_PROCESS_ATTACH prepares the diagnostics and the capture, validates the game
//     image and installs ONE GameMainWrapper detour; it loads nothing else (no
//     nevr-runtime.dll load in attach);
//   - the first GameMainWrapper call (after attach) loads the sibling nevr-runtime.dll
//     through RuntimeLoader::Load, calls initialize and returns game_main's result,
//     stopping the game (exit code 78) when the runtime did not load; the harness installs the
//     host's stop seam so a scenario can look on, unless the child asks for the real stop;
//   - NEVR_SetGameModule is the launcher-mode external contract.
// An attach failure is journalled in fixed memory; nevr_host_test_FlushDeferred stands
// for the crash-reporter constructor, the trigger that reports it when no detour exists.

#include "runtime/host/host_entry.h"
#include "runtime/host/path_buffer.h"
#include "runtime/host/runtime_load.h"
#include "runtime/tests/rendezvous/abi.h"

#include <windows.h>

#include <cstdint>

namespace {

using namespace nevr_rendezvous_probe;

RecordEventFn g_record = nullptr;

void Record(Event event, std::uint64_t payload = 0) {
  if (g_record != nullptr) g_record(event, payload);
}

void RecordStop(std::uint32_t exit_code) { Record(kHostStopGame, exit_code); }

bool RealStop() {
  HMODULE exe = GetModuleHandleW(nullptr);
  const auto fn = exe == nullptr ? nullptr : reinterpret_cast<RealStopFn>(GetProcAddress(exe, kRealStopExport));
  return fn != nullptr && fn() != 0;
}

bool VerifyRuntime() {
  HMODULE exe = GetModuleHandleW(nullptr);
  const auto fn = exe == nullptr ? nullptr : reinterpret_cast<VerifyRuntimeFn>(GetProcAddress(exe, kVerifyRuntimeExport));
  return fn != nullptr && fn() != 0;
}

bool LauncherMode() {
  HMODULE exe = GetModuleHandleW(nullptr);
  const auto fn = exe == nullptr
                      ? nullptr
                      : reinterpret_cast<LauncherModeFn>(GetProcAddress(exe, kLauncherModeExport));
  return fn != nullptr && fn() != 0;
}

}  // namespace

extern "C" __declspec(dllexport) void NEVR_SetGameModule(HMODULE game_module) {
  nevr_host::SetGameModule(game_module);
}

// Test seam: stands for MiniDmpSender::MiniDmpSender, the trigger that reports the
// attach decisions when the game has no detour to trigger them earlier.
extern "C" __declspec(dllexport) void nevr_host_test_FlushDeferred() {
  nevr_host::OnCrashReporterConstructed();
}

// Test seam: waits for the thread an unhooked attach starts to write the attach journal. It flushes nothing
// itself: the record must reach the disk by the attach's own doing.
extern "C" __declspec(dllexport) int nevr_host_test_WaitFailureFlush() {
  return nevr_host::WaitForAttachFailureFlush(10000) ? 1 : 0;
}

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID) {
  if (reason != DLL_PROCESS_ATTACH) return TRUE;
  HMODULE exe = GetModuleHandleW(nullptr);
  g_record = exe == nullptr ? nullptr
                           : reinterpret_cast<RecordEventFn>(
                                 GetProcAddress(exe, kRecordEventExport));
  Record(kHostAttachEnter);

  const bool launcher = LauncherMode();
  HMODULE game = nullptr;
  if (!launcher && exe != nullptr) {
    const auto image = reinterpret_cast<GameImageFn>(GetProcAddress(exe, kGameImageExport));
    if (image != nullptr) game = image();
  }
  const auto fail_create = exe == nullptr
                               ? nullptr
                               : reinterpret_cast<FailHookCreateFn>(GetProcAddress(exe, kFailHookCreateExport));
  if (fail_create != nullptr && fail_create() != 0) {
    nevr_host::RuntimeRendezvous::Seams().fail_hook_create = true;
  }
  if (!RealStop()) nevr_host::RuntimeRendezvous::Seams().stop_game = &RecordStop;
  // The test targets load the fixture runtime without a check; a scenario can turn the host's own check on, with the
  // production pins, to watch the refusal of that unsigned file.
  if (VerifyRuntime()) nevr_host::SetVerifyRuntimeForTest(true);
  // The fixture runtime is staged beside this shim; the production rule (the nEVR root) is the subject of test_host_runtime_load.
  static wchar_t runtime_dir[MAX_PATH];
  if (nevr_host::DirOfModule(runtime_dir, MAX_PATH, module)) nevr_host::SetRuntimeDirForTest(runtime_dir);
  nevr_host::OnProcessAttach(module, game, launcher);

  if (!launcher) {
    if (nevr_host::Rendezvous().Installed()) {
      Record(kHostHookEnabled,
             static_cast<std::uint64_t>(*(reinterpret_cast<const unsigned char*>(game) +
                                          nevr_host::kGameMainWrapperRva)));
    } else {
      Record(kHostHookFailed);
    }
  }
  Record(kHostAttachExit, GetModuleHandleW(L"nevr-runtime.dll") != nullptr ? 1 : 0);
  return TRUE;  // the game statically imports this DLL; a FALSE would abort start
}
