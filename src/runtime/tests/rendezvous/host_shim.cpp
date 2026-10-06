// BugSplat64.dll shim for the step-4 rendezvous harness: a faithful preview of
// the stable host's DllMain. It drives the production RuntimeRendezvous:
//   - DLL_PROCESS_ATTACH prepares the bootstrap diagnostics and the rendezvous,
//     validates the game image, installs ONE PreprocessCommandLine detour, and
//     loads nothing else (no nevr.dll load in attach);
//   - the first PreprocessCommandLine call (after attach) loads the sibling
//     nevr.dll through RuntimeLoader::Load and dispatches
//     preprocess_before -> original -> preprocess_after, degrading to
//     original-only on any failure;
//   - NEVR_SetGameModule is exported as the launcher-mode external contract: it
//     validates the game module and drives the same rendezvous install.
//
// The game image comes from the test executable's RendezvousProbeGameImage seam
// (the harness cannot be echovr.exe); production resolves GetModuleHandleW(nullptr).

#include "runtime/lifecycle/game_image_guard.h"
#include "runtime/tests/rendezvous/abi.h"

#include "runtime/host/bootstrap_diagnostics.h"
#include "runtime/host/runtime_rendezvous.h"

#include <windows.h>

#include <cstdint>

namespace {

using namespace RendezvousProbe;

NevrHost::RuntimeRendezvous g_rendezvous;
NevrHost::BootstrapDiagnostics g_diagnostics;
HMODULE g_shim_module = nullptr;
RecordEventFn g_record = nullptr;

void Record(Event event, std::uint64_t payload = 0) {
  if (g_record != nullptr) g_record(event, payload);
}

void* RendezvousTarget(HMODULE game_module) {
  return reinterpret_cast<unsigned char*>(game_module) + NevrHost::kPreprocessRva;
}

}  // namespace

extern "C" __declspec(dllexport) void NEVR_SetGameModule(HMODULE game_module) {
  // External launcher-mode contract (design step 4): validate the game image,
  // then drive the same rendezvous install the direct-import DllMain uses. An
  // unsupported module terminates with the stable game-image-guard exit code.
  GameImageGuard::RequireSupportedGameModule(game_module);
  g_rendezvous.Prepare(g_shim_module, game_module, g_diagnostics);
  g_rendezvous.Install(RendezvousTarget(game_module));
}

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID) {
  if (reason != DLL_PROCESS_ATTACH) return TRUE;
  g_shim_module = module;
  HMODULE exe = GetModuleHandleW(nullptr);
  g_record = exe == nullptr ? nullptr
                           : reinterpret_cast<RecordEventFn>(
                                 GetProcAddress(exe, kRecordEventExport));
  Record(kHostAttachEnter);

  // Fixed-memory only under the loader lock: precompute the diagnostic paths and
  // the sibling nevr.dll path. No directory creation, no file I/O, no load.
  g_diagnostics.Prepare();

  HMODULE game = exe == nullptr ? nullptr
                                : reinterpret_cast<GameImageFn>(
                                      GetProcAddress(exe, kGameImageExport))();
  if (!GameImageGuard::IsSupportedGameModule(game)) {
    Record(kHostHookFailed);
    Record(kHostAttachExit, GetModuleHandleW(L"nevr.dll") != nullptr ? 1 : 0);
    return TRUE;  // the game statically imports this DLL; a FALSE would abort start
  }

  g_rendezvous.Prepare(module, game, g_diagnostics);
  void* target = RendezvousTarget(game);
  const NevrPreprocessCommandLineFn trampoline = g_rendezvous.Install(target);
  if (trampoline == nullptr) {
    Record(kHostHookFailed);
  } else {
    Record(kHostHookEnabled, static_cast<std::uint64_t>(
                                 *reinterpret_cast<const unsigned char*>(target)));
  }
  Record(kHostAttachExit, GetModuleHandleW(L"nevr.dll") != nullptr ? 1 : 0);
  return TRUE;
}
