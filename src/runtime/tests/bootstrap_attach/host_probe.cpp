// Probe of the proposed BugSplat64.dll host attach path
// (docs/design/2026-10-05-bugsplat-nevr-split.md, "Early bootstrap without doing
// the full runtime under the loader lock" + Implementation step 2).
//
// DLL_PROCESS_ATTACH does exactly: validate the game image (the production
// GameImageGuard), validate the PreprocessCommandLine prologue, compute the
// sibling nevr.dll path into a fixed buffer, and install ONE MinHook detour. It
// loads nothing and does no file I/O. Any validation or MinHook failure leaves
// the game unhooked and still returns TRUE: the game statically imports this DLL,
// so a FALSE return would abort process start.
//
// The first PreprocessCommandLine call (after attach has returned) loads nevr.dll
// once under InitOnce, validates its descriptor (core/runtime_abi.h), initializes
// it, and dispatches preprocess_before -> original -> preprocess_after. A
// same-thread re-entry while the runtime is still loading calls the original
// directly instead of deadlocking on its own InitOnce.
//
// Test seams (resolved from the executable; absent in production):
//   - the game image comes from BootstrapProbeGameImage instead of
//     GetModuleHandleW(nullptr), because the harness cannot be echovr.exe;
//   - BootstrapProbeAttachHoldMs can keep DllMain open after the hook is live,
//     to force a concurrent detour entry while the loader lock is held.

#include "runtime/tests/bootstrap_attach/abi.h"

#include "core/runtime_abi.h"
#include "runtime/lifecycle/game_image_guard.h"

#include <MinHook.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <cwchar>

namespace {

using namespace BootstrapProbe;

constexpr wchar_t kRuntimeFileName[] = L"nevr.dll";

HMODULE g_host_module = nullptr;
HMODULE g_game_module = nullptr;
RecordEventFn g_record = nullptr;
NevrPreprocessCommandLineFn g_original = nullptr;
NevrHostContext g_context{};
wchar_t g_runtime_path[MAX_PATH] = {};
bool g_runtime_path_ok = false;

INIT_ONCE g_runtime_once = INIT_ONCE_STATIC_INIT;
std::atomic<const NevrRuntimeApi*> g_runtime_api{nullptr};
std::atomic<DWORD> g_loading_thread{0};

void Record(Event event, std::uint64_t payload = 0) {
  if (g_record != nullptr) g_record(event, payload);
}

std::uint64_t AsPayload(const void* pointer) {
  return reinterpret_cast<std::uintptr_t>(pointer);
}

// Fixed-buffer sibling path: <directory of this DLL>\nevr.dll. Attach-time safe:
// GetModuleFileNameW only, no heap, no I/O.
bool ComputeRuntimePath() {
  const DWORD length = GetModuleFileNameW(g_host_module, g_runtime_path, MAX_PATH);
  if (length == 0 || length >= MAX_PATH) return false;
  wchar_t* separator = wcsrchr(g_runtime_path, L'\\');
  if (separator == nullptr) return false;
  const std::size_t directory_length = static_cast<std::size_t>(separator - g_runtime_path) + 1;
  constexpr std::size_t name_length = sizeof(kRuntimeFileName) / sizeof(kRuntimeFileName[0]);
  if (directory_length + name_length > MAX_PATH) return false;
  std::memcpy(g_runtime_path + directory_length, kRuntimeFileName, sizeof(kRuntimeFileName));
  return true;
}

bool PrologueMatches(const void* target) {
  unsigned char bytes[sizeof(kPreprocessPrologue)] = {};
  SIZE_T read = 0;
  if (ReadProcessMemory(GetCurrentProcess(), target, bytes, sizeof(bytes), &read) == FALSE ||
      read != sizeof(bytes)) {
    return false;
  }
  return std::memcmp(bytes, kPreprocessPrologue, sizeof(bytes)) == 0;
}

std::uint64_t TryLoadRuntime() {
  if (!g_runtime_path_ok) return kLoadNoPath;
  HMODULE runtime = LoadLibraryExW(g_runtime_path, nullptr,
                                   LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
  if (runtime == nullptr) {
    return kLoadLibraryFailed | (static_cast<std::uint64_t>(GetLastError()) << 32);
  }
  // A failed runtime stays mapped: it may already have patched something, and
  // unmapping live code is worse than a leaked module for process lifetime.
  const auto get_api = reinterpret_cast<NevrGetRuntimeApiFn>(
      GetProcAddress(runtime, NevrAbi::kGetRuntimeApiExport));
  if (get_api == nullptr) return kLoadNoEntry;
  const NevrRuntimeApi* api = get_api();
  const NevrAbi::Status status = NevrAbi::ValidateRuntimeApi(api);
  if (status != NevrAbi::Status::kOk) {
    return kLoadAbiRejectedBase + static_cast<std::uint64_t>(status);
  }
  if (api->initialize(&g_context) != 0) return kLoadInitFailed;
  g_runtime_api.store(api, std::memory_order_release);
  return kLoadOk;
}

BOOL CALLBACK LoadRuntimeOnce(PINIT_ONCE, PVOID, PVOID*) {
  g_loading_thread.store(GetCurrentThreadId(), std::memory_order_release);
  Record(kHostLoadBegin);
  const std::uint64_t outcome = TryLoadRuntime();
  Record(kHostLoadEnd, outcome);
  g_loading_thread.store(0, std::memory_order_release);
  // TRUE on every outcome: a failed load is final, later calls run degraded.
  return TRUE;
}

std::uint64_t PreprocessCommandLineDetour(void* game) {
  if (g_loading_thread.load(std::memory_order_acquire) == GetCurrentThreadId()) {
    Record(kHostReentryBypass, AsPayload(game));
    return g_original(game);
  }
  InitOnceExecuteOnce(&g_runtime_once, &LoadRuntimeOnce, nullptr, nullptr);
  const NevrRuntimeApi* api = g_runtime_api.load(std::memory_order_acquire);
  if (api != nullptr) api->preprocess_before(game);
  const std::uint64_t result = g_original(game);
  if (api != nullptr) api->preprocess_after(game, result);
  return result;
}

void InstallRendezvous() {
  HMODULE exe = GetModuleHandleW(nullptr);
  const auto game_image = exe == nullptr ? nullptr
                                         : reinterpret_cast<GameImageFn>(
                                               GetProcAddress(exe, kGameImageExport));
  g_game_module = game_image == nullptr ? nullptr : game_image();
  if (!GameImageGuard::IsSupportedGameModule(g_game_module)) {
    Record(kHostImageRejected);
    return;
  }
  Record(kHostImageAccepted);

  void* target = reinterpret_cast<unsigned char*>(g_game_module) + kPreprocessRva;
  if (!PrologueMatches(target)) {
    Record(kHostPrologueRejected);
    return;
  }
  Record(kHostPrologueAccepted);
  g_runtime_path_ok = ComputeRuntimePath();

  MH_STATUS status = MH_Initialize();
  if (status != MH_OK) {
    Record(kHostHookFailed, static_cast<std::uint64_t>(status));
    return;
  }
  void* trampoline = nullptr;
  status = MH_CreateHook(target, reinterpret_cast<void*>(&PreprocessCommandLineDetour), &trampoline);
  if (status != MH_OK || trampoline == nullptr) {
    Record(kHostHookFailed, static_cast<std::uint64_t>(status));
    return;
  }
  // Publish everything a detour call can read before the jump goes live.
  g_original = reinterpret_cast<NevrPreprocessCommandLineFn>(trampoline);
  g_context.header.struct_size = sizeof(NevrHostContext);
  g_context.header.abi_major = NevrAbi::kMajor;
  g_context.header.abi_minor = NevrAbi::kMinor;
  g_context.host_module = g_host_module;
  g_context.game_module = g_game_module;
  g_context.original_preprocess = g_original;
  status = MH_EnableHook(target);
  if (status != MH_OK) {
    MH_RemoveHook(target);
    g_original = nullptr;
    Record(kHostHookFailed, static_cast<std::uint64_t>(status));
    return;
  }
  Record(kHostHookEnabled, *static_cast<const unsigned char*>(target));
}

}  // namespace

// Ordinal 1, as in the production exports.def. The static-import harness binds to it.
extern "C" __declspec(dllexport) void DetoursExportPlaceholder() {}

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    g_host_module = module;
    HMODULE exe = GetModuleHandleW(nullptr);
    g_record = exe == nullptr ? nullptr
                              : reinterpret_cast<RecordEventFn>(
                                    GetProcAddress(exe, kRecordEventExport));
    Record(kHostAttachEnter);
    InstallRendezvous();
    const auto hold = exe == nullptr ? nullptr
                                     : reinterpret_cast<AttachHoldFn>(
                                           GetProcAddress(exe, kAttachHoldExport));
    const DWORD hold_ms = hold == nullptr ? 0 : hold();
    if (hold_ms != 0) {
      Record(kHostAttachHeld, hold_ms);
      Sleep(hold_ms);
    }
    Record(kHostAttachExit, GetModuleHandleW(kRuntimeFileName) != nullptr ? 1 : 0);
  }
  return TRUE;
}
