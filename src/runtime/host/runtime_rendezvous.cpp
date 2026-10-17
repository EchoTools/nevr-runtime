#include "runtime/host/runtime_rendezvous.h"

#include <MinHook.h>

#include <cstring>

namespace nevr_host {

void RuntimeRendezvous::Journal(AttachStep step, AttachOutcome outcome, std::uint32_t detail,
                                const unsigned char* actual, HookStage stage) {
  if (diagnostics_ != nullptr) diagnostics_->RecordAttach(step, outcome, detail, actual, stage);
}

namespace {

// The one rendezvous the detour body dispatches through. MinHook carries no
// context, so the static detour reaches the active instance here. It is published
// in Prepare(), before any detour can fire.
RuntimeRendezvous* g_active_rendezvous = nullptr;

}  // namespace

void RuntimeRendezvous::Prepare(HMODULE host_module, HMODULE game_module,
                                BootstrapDiagnostics& diagnostics) {
  host_module_ = host_module;
  game_module_ = game_module;
  diagnostics_ = &diagnostics;
  loader_.Prepare(host_module);
  g_active_rendezvous = this;
}

bool RuntimeRendezvous::ReadPrologue(const void* target,
                                     unsigned char (&out)[kGameMainWrapperPrologueSize]) {
  std::memset(out, 0, sizeof(out));
  SIZE_T read = 0;
  return target != nullptr &&
         ReadProcessMemory(GetCurrentProcess(), target, out, sizeof(out), &read) != FALSE &&
         read == sizeof(out);
}

bool RuntimeRendezvous::PrologueMatches(const void* target) {
  unsigned char bytes[kGameMainWrapperPrologueSize];
  return ReadPrologue(target, bytes) &&
         std::memcmp(bytes, kGameMainWrapperPrologue, sizeof(bytes)) == 0;
}

NevrGameMainFn RuntimeRendezvous::Install(void* target) {
  if (original_ != nullptr) return original_;  // idempotent

  // Prologue-validate before writing the detour: never blind-write into the game
  // image. ReadProcessMemory reports an unreadable target rather than faulting.
  unsigned char actual[kGameMainWrapperPrologueSize];
  const bool readable = ReadPrologue(target, actual);
  if (!readable || std::memcmp(actual, kGameMainWrapperPrologue, sizeof(actual)) != 0) {
    Journal(AttachStep::kPrologueCheck, AttachOutcome::kRejected, 0, readable ? actual : nullptr);
    return nullptr;
  }
  Journal(AttachStep::kPrologueCheck, AttachOutcome::kAccepted, 0, actual);

  // Created before the jump can go live; signalled by every way out of here.
  HANDLE install_done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  install_done_.store(install_done, std::memory_order_release);
  struct SignalOnExit {
    HANDLE event;
    ~SignalOnExit() {
      if (event != nullptr) SetEvent(event);
    }
  } signal_on_exit{install_done};

  MH_STATUS status = MH_Initialize();
  if (status != MH_OK) {
    Journal(AttachStep::kHookInstall, AttachOutcome::kFailed, static_cast<std::uint32_t>(status),
            actual, HookStage::kInitialize);
    return nullptr;
  }

  void* trampoline = nullptr;
  status = MH_CreateHook(target, reinterpret_cast<void*>(&GameMainWrapperDetour), &trampoline);
  if (status != MH_OK || trampoline == nullptr) {
    Journal(AttachStep::kHookInstall, AttachOutcome::kFailed, static_cast<std::uint32_t>(status),
            actual, HookStage::kCreate);
    MH_Uninitialize();
    return nullptr;
  }

  // Publish the trampoline and the host context BEFORE the jump goes live, so a
  // racing thread that enters the detour during MH_EnableHook can never read a
  // null original (the attach proof gate in tests/bootstrap_attach observed it).
  original_ = reinterpret_cast<NevrGameMainFn>(trampoline);
  target_ = target;
  context_.header.struct_size = sizeof(NevrHostContext);
  context_.header.abi_major = nevr_runtime_abi::kMajor;
  context_.header.abi_minor = nevr_runtime_abi::kMinor;
  context_.host_module = host_module_;
  context_.game_module = game_module_;
  context_.original_game_main_wrapper = original_;
  context_.wrapper_hook_target = target;

  status = MH_EnableHook(target);
  if (status != MH_OK) {
    Journal(AttachStep::kHookInstall, AttachOutcome::kFailed, static_cast<std::uint32_t>(status),
            actual, HookStage::kEnable);
    MH_RemoveHook(target);
    MH_Uninitialize();
    original_ = nullptr;
    target_ = nullptr;
    return nullptr;
  }
  Journal(AttachStep::kHookInstall, AttachOutcome::kAccepted, static_cast<std::uint32_t>(MH_OK),
          actual, HookStage::kEnable);
  return original_;
}

BOOL CALLBACK RuntimeRendezvous::LoadRuntimeOnce(PINIT_ONCE, PVOID parameter, PVOID*) {
  auto* self = static_cast<RuntimeRendezvous*>(parameter);
  if (self == nullptr) return TRUE;
  self->loading_thread_.store(GetCurrentThreadId(), std::memory_order_release);
  // A thread can be here before the thread that installed the hook has journalled it.
  const HANDLE install_done = self->install_done_.load(std::memory_order_acquire);
  if (install_done != nullptr) WaitForSingleObject(install_done, 5000);
  // The attach decisions are reported first, now that attach has returned, so the
  // load record that follows reads in order.
  self->diagnostics_->FlushAttachJournal();
  // The first bytes at the target now that the host's hook is live: the jump the host wrote.
  // Read here rather than at enable, because a thread can enter this detour before
  // MH_EnableHook returns; by the time any thread is here the patch is complete, and the
  // runtime has not loaded yet to change it. Only this InitOnce body writes the context.
  unsigned char hook_bytes[nevr_runtime_abi::kHookBytesSize] = {};
  SIZE_T read = 0;
  if (ReadProcessMemory(GetCurrentProcess(), self->target_, hook_bytes, sizeof(hook_bytes),
                        &read) != FALSE &&
      read == sizeof(hook_bytes)) {
    std::memcpy(self->context_.wrapper_hook_bytes, hook_bytes, sizeof(hook_bytes));
  }
  const RuntimeLoadResult result = self->loader_.Load(self->context_, *self->diagnostics_);
  if (result.Loaded()) {
    self->api_.store(result.api, std::memory_order_release);
    // A loaded runtime is never freed, so the handle is stable for the process
    // lifetime; callers that forward to runtime-owned symbols resolve them from it.
    self->runtime_module_.store(result.module, std::memory_order_release);
  }
  self->loading_thread_.store(0, std::memory_order_release);
  return TRUE;
}

std::int64_t RuntimeRendezvous::GameMainWrapperDetour(std::int64_t command_line) {
  RuntimeRendezvous* self = g_active_rendezvous;
  // Defensive, unreachable in practice: Install publishes the trampoline before
  // MH_EnableHook, so the detour is never live with a null original_. If a defect
  // ever trips these guards there is no original to fall back to, so returning 0
  // is the only non-dereferencing option. This is NOT the degrade-to-original
  // path — that is the api == nullptr branch below, which still calls original_.
  if (self == nullptr) return 0;
  if (self->original_ == nullptr) return 0;

  // Same-thread re-entry while the runtime is still loading: call the original
  // directly. InitOnceExecuteOnce recursion on the loading thread deadlocks.
  if (self->loading_thread_.load(std::memory_order_acquire) == GetCurrentThreadId()) {
    return self->original_(command_line);
  }

  InitOnceExecuteOnce(&self->load_once_, &LoadRuntimeOnce, self, nullptr);
  const NevrRuntimeApi* api = self->api_.load(std::memory_order_acquire);
  if (api != nullptr) return api->game_main(command_line);
  return self->original_(command_line);
}

}  // namespace nevr_host
