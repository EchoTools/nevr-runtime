#include "runtime/host/runtime_rendezvous.h"

#include <MinHook.h>

#include <cstring>

namespace NevrHost {
namespace {

// The one rendezvous the detour body dispatches through. MinHook carries no
// context, so the static detour reaches the active instance here. It is published
// in Prepare(), before any detour can fire.
RuntimeRendezvous* g_active_rendezvous = nullptr;

}  // namespace

void RuntimeRendezvous::Prepare(HMODULE shim_module, HMODULE game_module,
                                BootstrapDiagnostics& diagnostics) {
  shim_module_ = shim_module;
  game_module_ = game_module;
  diagnostics_ = &diagnostics;
  loader_.Prepare(shim_module);
  g_active_rendezvous = this;
}

NevrPreprocessCommandLineFn RuntimeRendezvous::Install(void* target) {
  if (original_ != nullptr) return original_;  // idempotent

  // Prologue-validate before writing the detour: never blind-write into the game
  // image. ReadProcessMemory reports an unreadable target rather than faulting.
  unsigned char bytes[kPreprocessPrologueSize] = {};
  SIZE_T read = 0;
  if (target == nullptr ||
      ReadProcessMemory(GetCurrentProcess(), target, bytes, sizeof(bytes), &read) == FALSE ||
      read != sizeof(bytes) || std::memcmp(bytes, kPreprocessPrologue, sizeof(bytes)) != 0) {
    return nullptr;
  }

  MH_STATUS status = MH_Initialize();
  if (status != MH_OK) return nullptr;

  void* trampoline = nullptr;
  status = MH_CreateHook(target, reinterpret_cast<void*>(&PreprocessCommandLineDetour),
                         &trampoline);
  if (status != MH_OK || trampoline == nullptr) return nullptr;

  // Publish the trampoline and the host context BEFORE the jump goes live, so a
  // racing thread that enters the detour during MH_EnableHook can never read a
  // null original (observed in the step-2 proof gate).
  original_ = reinterpret_cast<NevrPreprocessCommandLineFn>(trampoline);
  context_.header.struct_size = sizeof(NevrHostContext);
  context_.header.abi_major = NevrAbi::kMajor;
  context_.header.abi_minor = NevrAbi::kMinor;
  context_.host_module = shim_module_;
  context_.game_module = game_module_;
  context_.original_preprocess = original_;

  status = MH_EnableHook(target);
  if (status != MH_OK) {
    MH_RemoveHook(target);
    original_ = nullptr;
    return nullptr;
  }
  return original_;
}

BOOL CALLBACK RuntimeRendezvous::LoadRuntimeOnce(PINIT_ONCE, PVOID parameter, PVOID*) {
  auto* self = static_cast<RuntimeRendezvous*>(parameter);
  if (self == nullptr) return TRUE;
  self->loading_thread_.store(GetCurrentThreadId(), std::memory_order_release);
  const RuntimeLoadResult result = self->loader_.Load(self->context_, *self->diagnostics_);
  if (result.Loaded()) self->api_.store(result.api, std::memory_order_release);
  self->loading_thread_.store(0, std::memory_order_release);
  return TRUE;
}

std::uint64_t RuntimeRendezvous::PreprocessCommandLineDetour(void* game) {
  RuntimeRendezvous* self = g_active_rendezvous;
  // The detour is only ever live with a published rendezvous and trampoline
  // (Install publishes both before MH_EnableHook); guard anyway so a defect
  // degrades to a call through the original rather than a null dereference.
  if (self == nullptr) return 0;
  if (self->original_ == nullptr) return 0;

  // Same-thread re-entry while the runtime is still loading: call the original
  // directly. InitOnceExecuteOnce recursion on the loading thread deadlocks.
  if (self->loading_thread_.load(std::memory_order_acquire) == GetCurrentThreadId()) {
    return self->original_(game);
  }

  InitOnceExecuteOnce(&self->load_once_, &LoadRuntimeOnce, self, nullptr);
  const NevrRuntimeApi* api = self->api_.load(std::memory_order_acquire);
  if (api != nullptr) api->preprocess_before(game);
  const std::uint64_t result = self->original_(game);
  if (api != nullptr) api->preprocess_after(game, result);
  return result;
}

}  // namespace NevrHost
