#include "runtime/lifecycle/update_worker.h"

#include <atomic>
#include <exception>
#include <thread>
#include <utility>

namespace nevr_update {

struct Worker::Shared {
  Deps deps;
  std::atomic<bool> started{false};
  Disabled decision = Disabled::kNone;
  std::atomic<std::uint64_t> first_frame_ms{UINT64_MAX};
  std::atomic<std::uint64_t> mark_after_ms{0};
  std::atomic<bool> mark_set{false};
  bool threads_run = false;
  HANDLE mark_event = nullptr;     // set by OnFrame at the mark
  HANDLE stop_event = nullptr;     // set by Stop
  HANDLE check_done = nullptr;     // set when the check thread's work is over, or when there is none
  HANDLE confirm_done = nullptr;   // the same for the confirmation
  std::thread check_thread;
  std::thread confirm_thread;

  explicit Shared(Deps d) : deps(std::move(d)) {
    mark_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    check_done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    confirm_done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  }
  ~Shared() {
    for (HANDLE h : {mark_event, stop_event, check_done, confirm_done}) {
      if (h != nullptr) CloseHandle(h);
    }
  }
  void Say(int level, const std::string& line) const {
    if (deps.log) deps.log(level, line);
  }
};

namespace {

// The check, on its own thread: the game never waits for it (ADR 0024, section 3). One run, then it ends.
void CheckMain(const std::shared_ptr<Worker::Shared>& s) {
  try {
    std::string version, sha256, error;
    if (!s->deps.read_running || !s->deps.read_running(&version, &sha256, &error)) {
      s->Say(kWarning, "[NEVR.UPDATE] update check action=failed reason=running_runtime_unreadable");
    } else {
      CheckEnv env;
      env.root = s->deps.root;
      env.version = version;
      env.runtime_sha256 = sha256;
      env.config_channel = s->deps.config_channel;
      env.core = s->deps.core;
      env.verify = s->deps.verify;
      env.log = s->deps.log;
      env.now_ms = s->deps.now_ms;
      RunCheck(env);
    }
  } catch (const std::exception&) {  // a thread of ours must never end the game's process: the check is tried again next launch
    s->Say(kWarning, "[NEVR.UPDATE] update check action=failed reason=internal_error");
  }
  SetEvent(s->check_done);
}

// The confirmation, on its own thread, independent of the check: a check stuck inside the core must not keep a swapped release from
// being confirmed, or the host rolls it back (section 6). It waits on the event the dispatcher sets at the mark.
void ConfirmMain(const std::shared_ptr<Worker::Shared>& s) {
  const HANDLE waits[2] = {s->mark_event, s->stop_event};
  if (WaitForMultipleObjects(2, waits, FALSE, INFINITE) == WAIT_OBJECT_0) {
    try {
      std::string version, sha256, error;
      if (!s->deps.read_running || !s->deps.read_running(&version, &sha256, &error)) {
        s->Say(kInfo, "[NEVR.UPDATE] update confirm skipped reason=running_runtime_unreadable");
      } else {
        ConfirmEnv env;
        env.root = s->deps.root;
        env.runtime_sha256 = sha256;
        env.role = s->deps.server ? "server" : "client";
        env.log = s->deps.log;
        UpdateConfirm(env, s->mark_after_ms.load());
      }
    } catch (const std::exception&) {  // see CheckMain
      s->Say(kWarning, "[NEVR.UPDATE] update confirm skipped reason=internal_error");
    }
  }
  SetEvent(s->confirm_done);
}

}  // namespace

Worker::Worker(Deps deps) : shared_(std::make_shared<Shared>(std::move(deps))) {}

Worker::~Worker() { Stop(500); }

Disabled Worker::Start() {
  Shared& s = *shared_;
  if (s.started.exchange(true)) return s.decision;
  Switches switches = s.deps.switches;
  if (s.deps.root.empty()) switches.no_root = true;  // an empty root would put the update folder at the root of a drive
  s.decision = Decide(switches);
  if (s.decision != Disabled::kNone) {
    s.Say(s.decision == Disabled::kNoRoot ? kWarning : kInfo, std::string("[NEVR.UPDATE] update disabled reason=") + DisabledWord(s.decision));
  }
  const bool can_confirm = s.decision != Disabled::kDevBuild && s.decision != Disabled::kNoRoot && s.decision != Disabled::kHostCannotSwap;
  if (s.decision != Disabled::kNone) SetEvent(s.check_done);
  if (!can_confirm) SetEvent(s.confirm_done);
  s.threads_run = can_confirm;
  try {
    if (can_confirm) s.confirm_thread = std::thread(ConfirmMain, shared_);
    if (s.decision == Disabled::kNone) s.check_thread = std::thread(CheckMain, shared_);
  } catch (const std::exception&) {  // no thread to be had: the game runs without an update check
    s.Say(kWarning, "[NEVR.UPDATE] update check action=failed reason=thread_not_started");
    SetEvent(s.check_done);
    SetEvent(s.confirm_done);
  }
  return s.decision;
}

void Worker::OnFrame(std::uint64_t now_ms) {
  Shared& s = *shared_;
  if (!s.threads_run || s.mark_set.load(std::memory_order_acquire)) return;
  std::uint64_t first = s.first_frame_ms.load(std::memory_order_acquire);
  if (first == UINT64_MAX) {
    s.first_frame_ms.compare_exchange_strong(first, now_ms);  // the first call fixes the start of the 10 s
    first = s.first_frame_ms.load(std::memory_order_acquire);
  }
  if (!ConfirmDue(first, now_ms)) return;
  if (!s.mark_set.exchange(true)) {
    s.mark_after_ms.store(now_ms - first);
    SetEvent(s.mark_event);
  }
}

bool Worker::WaitCheckDone(DWORD timeout_ms) { return WaitForSingleObject(shared_->check_done, timeout_ms) == WAIT_OBJECT_0; }
bool Worker::WaitConfirmDone(DWORD timeout_ms) { return WaitForSingleObject(shared_->confirm_done, timeout_ms) == WAIT_OBJECT_0; }

void Worker::Stop(DWORD timeout_ms) {
  Shared& s = *shared_;
  SetEvent(s.stop_event);
  for (std::thread* t : {&s.check_thread, &s.confirm_thread}) {
    if (!t->joinable()) continue;
    const HANDLE done = t == &s.check_thread ? s.check_done : s.confirm_done;
    if (WaitForSingleObject(done, timeout_ms) == WAIT_OBJECT_0) {
      t->join();
    } else {
      t->detach();  // inside the core, which cannot be cancelled: the thread holds its own reference and ends with the process
    }
  }
}

}  // namespace nevr_update
