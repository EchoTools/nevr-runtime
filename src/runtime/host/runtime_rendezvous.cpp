#include "runtime/host/runtime_rendezvous.h"

#include <MinHook.h>

#include "runtime/host/crash_report_host.h"
#include "runtime/host/crash_send_host.h"
#include "runtime/host/host_identity.h"
#include "runtime/host/host_json.h"
#include "runtime/host/loader_lock.h"
#include "runtime/host/stop_line.h"
#include "runtime/host/stop_notice.h"

#include <cstring>
#include <exception>

namespace nevr_host {

#if defined(NEVR_HOST_TESTING)
RuntimeRendezvous::TestSeams& RuntimeRendezvous::Seams() {
  static TestSeams seams;
  return seams;
}
#endif

namespace {

// The player's notice. user32 is not an import of the host (tools/verify_pe_contract.py allows kernel32 and the
// UCRT), so the box is reached by name, here, when it is needed. Nothing is shown if either step fails.
void ShowStopNotice(const wchar_t* text) noexcept {
#if defined(NEVR_HOST_TESTING)
  // A test never opens a dialog: with no seam set, nothing is shown.
  if (RuntimeRendezvous::Seams().show_stop_notice != nullptr) RuntimeRendezvous::Seams().show_stop_notice(text);
#else
  using MessageBoxWFn = int(WINAPI*)(HWND, LPCWSTR, LPCWSTR, UINT);
  HMODULE user32 = LoadLibraryExW(L"user32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
  if (user32 == nullptr) return;
  const auto box = reinterpret_cast<MessageBoxWFn>(GetProcAddress(user32, "MessageBoxW"));
  if (box == nullptr) return;
  box(nullptr, text, kStopNoticeTitle, MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
#endif
}

const char* StopCommandLine() {
#if defined(NEVR_HOST_TESTING)
  if (RuntimeRendezvous::Seams().command_line != nullptr) return RuntimeRendezvous::Seams().command_line();
#endif
  return GetCommandLineA();
}

// Production behaviour is the constants; the test targets can change them through the seams.
bool FailEventCreate() {
#if defined(NEVR_HOST_TESTING)
  return RuntimeRendezvous::Seams().fail_event_create;
#else
  return false;
#endif
}
bool FailHookBytesRead() {
#if defined(NEVR_HOST_TESTING)
  return RuntimeRendezvous::Seams().fail_hook_bytes_read;
#else
  return false;
#endif
}
bool FailHookCreate() {
#if defined(NEVR_HOST_TESTING)
  return RuntimeRendezvous::Seams().fail_hook_create;
#else
  return false;
#endif
}
DWORD InstallWaitMs() {
#if defined(NEVR_HOST_TESTING)
  return RuntimeRendezvous::Seams().install_wait_ms;
#else
  return RuntimeRendezvous::kInstallWaitMs;
#endif
}
DWORD ExitNoticeWaitMs() {
#if defined(NEVR_HOST_TESTING)
  return RuntimeRendezvous::Seams().exit_notice_wait_ms;
#else
  return RuntimeRendezvous::kExitNoticeWaitMs;
#endif
}

}  // namespace

RuntimeRendezvous::InstallWait RuntimeRendezvous::AwaitInstall() {
  const HANDLE install_done = install_done_.load(std::memory_order_acquire);
  if (install_done == nullptr) {
    Journal(AttachStep::kInstallWait, AttachOutcome::kFailed, 0, nullptr, HookStage::kNone,
            AttachNote::kInstallWaitUnavailable);
    return InstallWait::kUnavailable;
  }
  if (WaitForSingleObject(install_done, InstallWaitMs()) != WAIT_OBJECT_0) {
    Journal(AttachStep::kInstallWait, AttachOutcome::kFailed, 0, nullptr, HookStage::kNone,
            AttachNote::kInstallWaitTimeout);
    return InstallWait::kTimeout;
  }
  Journal(AttachStep::kInstallWait, AttachOutcome::kAccepted, 0, nullptr, HookStage::kNone,
          AttachNote::kInstallWaitSignalled);
  return InstallWait::kSignalled;
}

void RuntimeRendezvous::Journal(AttachStep step, AttachOutcome outcome, std::uint32_t detail,
                                const unsigned char* actual, HookStage stage, AttachNote note) {
  if (diagnostics_ != nullptr) diagnostics_->RecordAttach(step, outcome, detail, actual, stage, note);
}

namespace {

// The one rendezvous the detour body dispatches through. MinHook carries no
// context, so the static detour reaches the active instance here. It is published
// in Prepare(), before any detour can fire.
RuntimeRendezvous* g_active_rendezvous = nullptr;

}  // namespace

#if defined(NEVR_HOST_TESTING)
void RuntimeRendezvous::ResetActiveForTest() { g_active_rendezvous = nullptr; }
const NevrHostContext& RuntimeRendezvous::Context() const { return context_; }
void RuntimeRendezvous::SetInstallEventForTest(HANDLE event) {
  install_done_.store(event, std::memory_order_release);
}
#endif

void RuntimeRendezvous::Prepare(HMODULE host_module, HMODULE game_module,
                                BootstrapDiagnostics& diagnostics) {
  host_module_ = host_module;
  game_module_ = game_module;
  diagnostics_ = &diagnostics;
  loader_.Prepare();
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
  HANDLE install_done = FailEventCreate() ? nullptr : CreateEventW(nullptr, TRUE, FALSE, nullptr);
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
  if (FailHookCreate()) {  // the seam: a create that came back refused
    if (status == MH_OK) MH_RemoveHook(target);
    status = MH_ERROR_UNSUPPORTED_FUNCTION;
  }
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
    return nullptr;
  }
  // The first bytes at the target now that the hook is live: the jump the host wrote. Read here,
  // before the install event is signalled, so a detour thread (which waits for the event before
  // it touches the context) sees them. The flag is written last; 0 says the bytes mean nothing.
  SIZE_T got = 0;
  const bool read_call = ReadProcessMemory(GetCurrentProcess(), target, context_.wrapper_hook_bytes,
                                           sizeof(context_.wrapper_hook_bytes), &got) != FALSE &&
                         got == sizeof(context_.wrapper_hook_bytes);
  const bool read_ok = read_call && !FailHookBytesRead();  // the seam: a read that came back unusable
  if (!read_ok) std::memset(context_.wrapper_hook_bytes, 0, sizeof(context_.wrapper_hook_bytes));
  MemoryBarrier();
  context_.wrapper_hook_bytes_valid = read_ok ? 1u : 0u;
  Journal(AttachStep::kHookInstall, AttachOutcome::kAccepted, static_cast<std::uint32_t>(MH_OK),
          actual, HookStage::kEnable, read_ok ? AttachNote::kNone : AttachNote::kHookBytesUnread);
  return original_;
}

BOOL CALLBACK RuntimeRendezvous::LoadRuntimeOnce(PINIT_ONCE, PVOID parameter, PVOID*) {
  auto* self = static_cast<RuntimeRendezvous*>(parameter);
  if (self == nullptr) return TRUE;
  self->loading_thread_.store(GetCurrentThreadId(), std::memory_order_release);
  // A thread can be here before the thread that installed the hook has journalled it.
  const InstallWait waited = self->AwaitInstall();
  // The attach decisions are reported first, now that attach has returned, so the
  // load record that follows reads in order.
  self->diagnostics_->FlushAttachJournal();
  const RuntimeLoadResult result = self->loader_.Load(self->context_, *self->diagnostics_);
  if (result.Loaded()) self->api_.store(result.api, std::memory_order_release);
  // A load that did not wait for the install may have run ahead of its last journal entry.
  if (waited != InstallWait::kSignalled) self->diagnostics_->FlushAttachJournal();
  self->loading_thread_.store(0, std::memory_order_release);
  return TRUE;
}

std::int64_t RuntimeRendezvous::StopGame(std::int64_t command_line) {
  // One thread at a time (the others wait here, and the process ends under the first): every thread that was
  // waiting on the load arrives here when it fails, one record says why, and a player at the screen is told once.
  AcquireSRWLockExclusive(&stop_lock_);
  if (!stop_recorded_.exchange(true, std::memory_order_acq_rel)) {
    // Quiet when the command line cannot be read: a dialog that might hold a server is worse than none.
    StopNotice decision = StopNotice::kSuppressedServerOrHeadless;
    try {
      decision = DecideStopNotice(StopCommandLine(), LoaderLockHeldByThisThread());
    } catch (const std::exception&) {
      // decision stays suppressed
    }
    EmitStopDiagnostic(*diagnostics_, loader_.Result(), StopNoticeName(decision));
    // The one console line (#1252), after the record so its `log` names the file that took the record.
    WriteRuntimeStopLine(*diagnostics_, loader_.Result());
    const RuntimeLoadResult& result = loader_.Result();
    // The same cause in plain words (#1533): a -server run gets no dialog, so this line is what its console shows.
    const wchar_t* sentence = StopNoticeTextFor(result.outcome, result.verify_reason_code);
    const bool console_took = WriteRuntimeNoticeLine(sentence, *diagnostics_, result);
    // A person at a console that took the line gets a moment to read it; a dialog already held the process open.
    if (console_took && decision == StopNotice::kSuppressedServerOrHeadless) Sleep(ExitNoticeWaitMs());
    if (decision == StopNotice::kShow) {
      wchar_t log[MAX_PATH] = {};
      const bool has_log = diagnostics_->LogFilePath(log, MAX_PATH);
      // The sentence, then the host version and the log the record is in (stop_notice.h); the bare sentence when the
      // longer text cannot be built.
      try {
        ShowStopNotice(StopNoticeBody(sentence, Version(), has_log ? log : nullptr).c_str());
      } catch (const std::exception&) {
        ShowStopNotice(sentence);
      }
    }
  }
#if defined(NEVR_HOST_TESTING)
  if (Seams().stop_game != nullptr) {
    Seams().stop_game(kRuntimeStopExitCode);
    ReleaseSRWLockExclusive(&stop_lock_);  // production never gets here: the process ends holding the lock
    return original_(command_line);
  }
#else
  static_cast<void>(command_line);
#endif
  TerminateProcess(GetCurrentProcess(), kRuntimeStopExitCode);
  // TerminateProcess on the current process does not return; if it ever did, nothing may run the game.
  RaiseFailFastException(nullptr, nullptr, 0);
  __builtin_unreachable();
}

std::int64_t RuntimeRendezvous::GameMainWrapperDetour(std::int64_t command_line) {
  RuntimeRendezvous* self = g_active_rendezvous;
  // Defensive, unreachable in practice: Install publishes the trampoline before
  // MH_EnableHook, so the detour is never live with a null original_. If a defect
  // ever trips these guards there is no original to fall back to, so returning 0
  // is the only non-dereferencing option. This is NOT the stop path — that is the
  // api == nullptr branch below.
  if (self == nullptr) {
    // No rendezvous, so no journal: a fixed-buffer line on stderr (no allocation, no lock).
    HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
    if (err != nullptr && err != INVALID_HANDLE_VALUE) {
      char buffer[256];
      JsonLine line(buffer, sizeof(buffer), "detour_entry");
      line.Str("outcome", "defect").Str("reason", "no_rendezvous").Str("next", "returned_0_without_calling_the_original");
      const std::size_t length = line.Finish();
      DWORD written = 0;
      if (length != 0) WriteFile(err, buffer, static_cast<DWORD>(length), &written, nullptr);
    }
    return 0;
  }
  if (self->original_ == nullptr) {
    self->Journal(AttachStep::kDetourEntry, AttachOutcome::kFailed, 0, nullptr, HookStage::kNone,
                  AttachNote::kDetourWithoutHook);
    // Nothing else will report it: this thread returns 0 into the game and no load follows.
    if (self->diagnostics_ != nullptr) self->diagnostics_->FlushAttachJournal();
    return 0;
  }

  // Same-thread re-entry while the runtime is still loading: call the original
  // directly. InitOnceExecuteOnce recursion on the loading thread deadlocks.
  if (self->loading_thread_.load(std::memory_order_acquire) == GetCurrentThreadId()) {
    return self->original_(command_line);
  }

  // Before the runtime is loaded: the crash reports a past run left go out whether or not this run's runtime loads
  // (core/crash_send.h). On a thread of its own, so the load below never waits for the network. Once per process.
  // The module table the host's own crash report resolves addresses against (crash_report_host.h): taken here, off the loader lock.
  SnapshotModulesForCrashReport();
  if (self->diagnostics_ != nullptr) StartCrashSend(*self->diagnostics_);
  InitOnceExecuteOnce(&self->load_once_, &LoadRuntimeOnce, self, nullptr);
  const NevrRuntimeApi* api = self->api_.load(std::memory_order_acquire);
  if (api != nullptr) return api->game_main(command_line);
  return self->StopGame(command_line);
}

}  // namespace nevr_host
