// The update worker's threads (core/update/worker.h), on std::thread and std::condition_variable. The events of the first Win32
// build are flags under one mutex here: the mark and the stop wake the confirmation thread, and each thread's end is a done flag that
// Stop and the tests wait on. The decisions are Decide (policy.cpp); the two jobs are RunCheck and UpdateConfirm (engine_check.cpp).

#include "core/update/worker.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <mutex>
#include <thread>
#include <utility>

namespace nevr_update {
namespace {

// The destructor's bound on each thread. A thread still inside the core is detached and ends with the process.
constexpr std::uint32_t kDestructorStopMs = 500;
// `first_frame_ms` before the first OnFrame call: no start of the 10 s yet.
constexpr std::uint64_t kNoFrameYet = UINT64_MAX;

}  // namespace

struct Worker::Shared {
  Deps deps;
  std::atomic<bool> started{false};
  Disabled decision = Disabled::kNone;
  std::atomic<std::uint64_t> first_frame_ms{kNoFrameYet};
  std::atomic<std::uint64_t> mark_after_ms{0};
  std::atomic<bool> mark_set{false};
  bool threads_run = false;
  std::thread check_thread;
  std::thread confirm_thread;

  // The flags the threads wait on, all under `mu`. `mark` is set by OnFrame at the mark, `stop` by Stop; `check_done` and
  // `confirm_done` are set when a thread's work is over, or when there is none.
  std::mutex mu;
  std::condition_variable cv;
  bool mark = false;
  bool stop = false;
  bool check_done = false;
  bool confirm_done = false;

  explicit Shared(Deps d) : deps(std::move(d)) {}

  void Say(int level, const std::string& line) const {
    if (deps.log) deps.log(level, line);
  }

  // Sets one of the flags above and wakes every waiter.
  void Raise(bool* flag) {
    {
      std::lock_guard<std::mutex> lock(mu);
      *flag = true;
    }
    cv.notify_all();
  }

  // True when `flag` is set before `timeout_ms` passes.
  bool WaitFlag(const bool* flag, std::uint32_t timeout_ms) {
    std::unique_lock<std::mutex> lock(mu);
    return cv.wait_for(lock, std::chrono::milliseconds(timeout_ms), [flag] { return *flag; });
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
      env.root_unconvertible = s->deps.root_unconvertible;
      env.platform = s->deps.platform;
      env.version = version;
      env.runtime_sha256 = sha256;
      env.config_channel = s->deps.config_channel;
      env.source = s->deps.source;
      env.core = s->deps.core;
      env.verify = s->deps.verify;
      env.log = s->deps.log;
      env.now_ms = s->deps.now_ms;
      env.fs = s->deps.fs;
      RunCheck(env);
    }
  } catch (const std::exception&) {  // a thread of ours must never end the game's process: the check is tried again next launch
    s->Say(kWarning, "[NEVR.UPDATE] update check action=failed reason=internal_error");
  }
  s->Raise(&s->check_done);
}

// The confirmation, on its own thread, independent of the check: a check stuck inside the core must not keep a swapped release from
// being confirmed, or the host rolls it back (section 6). It waits for the mark that the dispatcher sets, or for the stop.
void ConfirmMain(const std::shared_ptr<Worker::Shared>& s) {
  bool marked = false;
  {
    std::unique_lock<std::mutex> lock(s->mu);
    s->cv.wait(lock, [&s] { return s->mark || s->stop; });
    marked = s->mark;  // the mark wins over a stop that came with it, as the first Win32 wait's lowest index did
  }
  if (marked) {
    try {
      std::string version, sha256, error;
      if (!s->deps.read_running || !s->deps.read_running(&version, &sha256, &error)) {
        s->Say(kInfo, "[NEVR.UPDATE] update confirm skipped reason=running_runtime_unreadable");
      } else {
        ConfirmEnv env;
        env.root = s->deps.root;
        env.root_unconvertible = s->deps.root_unconvertible;
        env.runtime_sha256 = sha256;
        env.role = s->deps.server ? "server" : "client";
        env.log = s->deps.log;
        env.fs = s->deps.fs;
        UpdateConfirm(env, s->mark_after_ms.load());
      }
    } catch (const std::exception&) {  // see CheckMain
      s->Say(kWarning, "[NEVR.UPDATE] update confirm skipped reason=internal_error");
    }
  }
  s->Raise(&s->confirm_done);
}

}  // namespace

Worker::Worker(Deps deps) : shared_(std::make_shared<Shared>(std::move(deps))) {}

Worker::~Worker() { Stop(kDestructorStopMs); }

Disabled Worker::Start() {
  Shared& s = *shared_;
  if (s.started.exchange(true)) return s.decision;
  Switches switches = s.deps.switches;
  // An empty root would put the update folder at the root of a drive. A root that is there but does not convert is not "no root":
  // the check and the confirmation run and fail closed.
  if (s.deps.root.empty() && !s.deps.root_unconvertible) switches.no_root = true;
  s.decision = Decide(switches);
  if (s.decision != Disabled::kNone) {
    s.Say(s.decision == Disabled::kNoRoot ? kWarning : kInfo, std::string("[NEVR.UPDATE] update disabled reason=") + DisabledWord(s.decision));
  }
  const bool can_confirm = s.decision != Disabled::kDevBuild && s.decision != Disabled::kNoRoot && s.decision != Disabled::kHostCannotSwap;
  if (s.decision != Disabled::kNone) s.Raise(&s.check_done);
  if (!can_confirm) s.Raise(&s.confirm_done);
  s.threads_run = can_confirm;
  try {
    if (can_confirm) s.confirm_thread = std::thread(ConfirmMain, shared_);
    if (s.decision == Disabled::kNone) s.check_thread = std::thread(CheckMain, shared_);
  } catch (const std::exception&) {  // no thread to be had: the game runs without an update check
    s.Say(kWarning, "[NEVR.UPDATE] update check action=failed reason=thread_not_started");
    s.Raise(&s.check_done);
    s.Raise(&s.confirm_done);
  }
  return s.decision;
}

void Worker::OnFrame(std::uint64_t now_ms) {
  Shared& s = *shared_;
  if (!s.threads_run || s.mark_set.load(std::memory_order_acquire)) return;
  std::uint64_t first = s.first_frame_ms.load(std::memory_order_acquire);
  if (first == kNoFrameYet) {
    s.first_frame_ms.compare_exchange_strong(first, now_ms);  // the first call fixes the start of the 10 s
    first = s.first_frame_ms.load(std::memory_order_acquire);
  }
  if (!ConfirmDue(first, now_ms)) return;
  if (!s.mark_set.exchange(true)) {
    s.mark_after_ms.store(now_ms - first);
    s.Raise(&s.mark);
  }
}

bool Worker::WaitCheckDone(std::uint32_t timeout_ms) { return shared_->WaitFlag(&shared_->check_done, timeout_ms); }
bool Worker::WaitConfirmDone(std::uint32_t timeout_ms) { return shared_->WaitFlag(&shared_->confirm_done, timeout_ms); }

void Worker::Stop(std::uint32_t timeout_ms) {
  Shared& s = *shared_;
  s.Raise(&s.stop);
  for (std::thread* t : {&s.check_thread, &s.confirm_thread}) {
    if (!t->joinable()) continue;
    const bool* done = t == &s.check_thread ? &s.check_done : &s.confirm_done;
    if (s.WaitFlag(done, timeout_ms)) {
      t->join();
    } else {
      t->detach();  // inside the core, which cannot be cancelled: the thread holds its own reference and ends with the process
    }
  }
}

}  // namespace nevr_update
