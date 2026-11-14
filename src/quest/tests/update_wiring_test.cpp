// Host test for the Quest runtime's start of the update worker (src/quest/runtime/update_wiring.h): the inputs the runtime builds for the
// worker, the start with a fake core, the frame call that reaches the worker's confirmation, and the read of the running runtime.

#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "core/update/fs.h"
#include "quest/data/sha256.h"
#include "quest/runtime/update_wiring.h"
#include "quest/tests/test_check.h"

namespace {

namespace uw = nevr_quest::update_wiring;
using nevr_update::Disabled;

constexpr char kVersion[] = "6.18.0";
constexpr char kInThisObject = 0;  // an address inside this executable, for dladdr
constexpr std::uint64_t kFirstFrameMs = 1000;
constexpr std::uint64_t kPastTheMarkMs = 12000;  // more than kConfirmAfterMs after the first frame
constexpr std::uint32_t kWaitMs = 10000;
constexpr std::uint32_t kStopMs = 2000;

// The lines the worker's threads write: under one mutex, because the worker logs from two threads (worker.h, Deps::log).
struct Lines {
  std::mutex mu;
  std::vector<std::string> all;
  nevr_update::LogFn Fn() {
    return [this](int, const std::string& line) {
      std::lock_guard<std::mutex> lock(mu);
      all.push_back(line);
    };
  }
  bool Has(const std::string& needle) {
    std::lock_guard<std::mutex> lock(mu);
    for (const std::string& l : all) {
      if (l.find(needle) != std::string::npos) return true;
    }
    return false;
  }
};

// A core whose every job answers "nothing to do": the check ends at once.
nevr_update::Core FakeCore() {
  nevr_update::Core core;
  core.open = [](const std::string&, std::string*) { return true; };
  core.manifest_fetch = [](std::string* error) {
    *error = "fake core";
    return false;
  };
  core.stage = [](const std::string&, const std::string&, std::string*, std::string* error) {
    *error = "fake core";
    return false;
  };
  core.run_end = [](const std::string&, const std::string&) {};
  core.discard = [](const std::string&, const std::string&, const std::string&, const std::string&, std::string*) { return true; };
  core.close = [] {};
  return core;
}

// A stand-in for frame_dispatch.h RegisterFrameWork that keeps what it was given.
struct FakeRegistry {
  uw::FrameWorkFn work = nullptr;
  void* context = nullptr;
  int calls = 0;
  bool accept = true;
  uw::RegisterFrameWorkFn Fn() {
    return [this](uw::FrameWorkFn fn, void* ctx) {
      ++calls;
      if (!accept) return false;
      work = fn;
      context = ctx;
      return true;
    };
  }
};

std::string MakeRoot() {
  const std::filesystem::path dir = std::filesystem::temp_directory_path() / ("nevr-update-wiring-" + std::to_string(::getpid()));
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  return dir.string();
}

uw::Inputs BaseInputs(const nevr_update::UpdateFs* fs, const std::string& root, Lines* lines) {
  uw::Inputs in;
  in.host_can_swap = true;
  in.root = root;
  in.fs = fs;
  in.read_running = [](std::string* version, std::string* sha256, std::string*) {
    *version = kVersion;
    *sha256 = std::string(64, 'a');
    return true;
  };
  in.core = FakeCore();
  in.verify = [](const std::string&) { return nevr_update::VerifyOutcome{true, false, "accepted"}; };
  in.log = lines->Fn();
  in.now_ms = [] { return std::uint64_t{0}; };
  return in;
}

// BuildDeps carries every input to the worker's switches, and Decide answers as the inputs say.
void TheInputsReachTheWorkersSwitches(const nevr_update::UpdateFs* fs, const std::string& root) {
  Lines lines;
  {
    const nevr_update::Deps deps = uw::BuildDeps(BaseInputs(fs, root, &lines));
    QCHECK(deps.platform == "quest");
    QCHECK(deps.root == root);
    QCHECK(deps.fs == fs);
    QCHECK(!deps.server);
    QCHECK(!deps.config_channel.has_value());
    QCHECK(nevr_update::Decide(deps.switches) == Disabled::kNone);
  }
  {
    uw::Inputs in = BaseInputs(fs, root, &lines);
    in.dev_build = true;
    QCHECK(nevr_update::Decide(uw::BuildDeps(std::move(in)).switches) == Disabled::kDevBuild);
  }
  {
    uw::Inputs in = BaseInputs(fs, root, &lines);
    in.flag_no_update = true;
    QCHECK(nevr_update::Decide(uw::BuildDeps(std::move(in)).switches) == Disabled::kFlag);
  }
  {
    uw::Inputs in = BaseInputs(fs, root, &lines);
    in.config_enabled = false;
    QCHECK(nevr_update::Decide(uw::BuildDeps(std::move(in)).switches) == Disabled::kConfig);
  }
  {
    uw::Inputs in = BaseInputs(fs, root, &lines);
    in.config_enabled = true;
    in.config_channel = "beta";
    const nevr_update::Deps deps = uw::BuildDeps(std::move(in));
    QCHECK(nevr_update::Decide(deps.switches) == Disabled::kNone);
    QCHECK(deps.config_channel.has_value() && *deps.config_channel == "beta");
  }
  {
    uw::Inputs in = BaseInputs(fs, root, &lines);
    in.host_can_swap = false;  // the capability bit the shim does not set yet
    QCHECK(nevr_update::Decide(uw::BuildDeps(std::move(in)).switches) == Disabled::kHostCannotSwap);
  }
  {
    uw::Inputs in = BaseInputs(fs, root, &lines);
    in.root.clear();
    QCHECK(nevr_update::Decide(uw::BuildDeps(std::move(in)).switches) == Disabled::kNoRoot);
  }
}

// Without a core the worker is not started, nothing is registered, and one record says why.
void WithoutACoreNothingStarts(const nevr_update::UpdateFs* fs, const std::string& root) {
  Lines lines;
  FakeRegistry registry;
  uw::Inputs in = BaseInputs(fs, root, &lines);
  in.core = nevr_update::Core();
  const uw::StartResult result = uw::Start(std::move(in), registry.Fn());
  QCHECK(result.outcome == uw::StartOutcome::kCoreNotLinked);
  QCHECK(result.worker == nullptr);
  QCHECK(registry.calls == 0);
  QCHECK(lines.Has("update wiring action=not_started reason=core_not_linked"));
}

// With a core the worker starts, the frame call is registered once, and the frame call reaches the confirmation: 10 s of frames later
// the confirmation runs (nothing is pending here, so it ends with "no_pending").
void TheFrameCallReachesTheConfirmation(const nevr_update::UpdateFs* fs, const std::string& root) {
  Lines lines;
  FakeRegistry registry;
  std::atomic<std::uint64_t> clock{kFirstFrameMs};
  uw::Inputs in = BaseInputs(fs, root, &lines);
  in.now_ms = [&clock] { return clock.load(); };
  const uw::StartResult result = uw::Start(std::move(in), registry.Fn());
  QCHECK(result.outcome == uw::StartOutcome::kStarted);
  QCHECK(result.worker != nullptr);
  QCHECK(result.decision == Disabled::kNone);
  QCHECK(registry.calls == 1);
  QCHECK(registry.work != nullptr && registry.context != nullptr);
  if (result.worker == nullptr || registry.work == nullptr) return;

  QCHECK(registry.work(registry.context));  // the first frame fixes the start of the 10 s
  clock.store(kPastTheMarkMs);
  QCHECK(registry.work(registry.context));  // past the mark: the confirmation thread is woken
  QCHECK(result.worker->WaitConfirmDone(kWaitMs));
  QCHECK(lines.Has("update confirm skipped reason=no_pending"));
  result.worker->Stop(kStopMs);
}

// A registry that refuses leaves the worker running and says the confirmation cannot fire.
void ARefusedFrameCallIsSaid(const nevr_update::UpdateFs* fs, const std::string& root) {
  Lines lines;
  FakeRegistry registry;
  registry.accept = false;
  const uw::StartResult result = uw::Start(BaseInputs(fs, root, &lines), registry.Fn());
  QCHECK(result.outcome == uw::StartOutcome::kFrameWorkRefused);
  QCHECK(result.worker != nullptr);
  QCHECK(registry.calls == 1);
  QCHECK(lines.Has("update wiring action=frame_work_refused reason=registry_full"));
  if (result.worker != nullptr) result.worker->Stop(kStopMs);
}

// The frame call of an unusable context says so and does nothing.
void TheFrameCallRefusesAnUnusableContext() {
  FakeRegistry registry;
  Lines lines;
  uw::Inputs in;
  in.core = FakeCore();
  in.fs = nullptr;
  in.log = lines.Fn();
  in.root.clear();  // no root: the worker starts no thread
  const uw::StartResult result = uw::Start(std::move(in), registry.Fn());
  QCHECK(result.worker != nullptr);
  QCHECK(registry.work != nullptr);
  QCHECK(!registry.work(nullptr));
  if (result.worker != nullptr) result.worker->Stop(kStopMs);
}

// The running runtime: the file the code is in, its sha256 (as an independent read of the same file gives it), and the given version.
void TheRunningRuntimeIsTheFileTheCodeIsIn() {
  std::string version, sha256, error;
  QCHECK(uw::ReadRunningRuntime(&kInThisObject, kVersion, &version, &sha256, &error));
  QCHECK(version == kVersion);
  QCHECK(sha256.size() == 64);

  std::ifstream self("/proc/self/exe", std::ios::binary);
  std::string bytes((std::istreambuf_iterator<char>(self)), std::istreambuf_iterator<char>());
  QCHECK(!bytes.empty());
  QCHECK(sha256 == nevr_quest::data::Sha256Hex(bytes.data(), bytes.size()));

  std::string v2, s2, e2;
  QCHECK(!uw::ReadRunningRuntime(nullptr, kVersion, &v2, &s2, &e2));
  QCHECK(e2 == "no_object");
  QCHECK(!uw::ReadRunningRuntime(&kInThisObject, "", &v2, &s2, &e2));
  QCHECK(e2 == "no_version");
}

}  // namespace

int main() {
  const std::string root = MakeRoot();
  const std::unique_ptr<nevr_update::UpdateFs> fs = nevr_update::MakePosixUpdateFs(std::filesystem::temp_directory_path().string());
  TheInputsReachTheWorkersSwitches(fs.get(), root);
  WithoutACoreNothingStarts(fs.get(), root);
  TheFrameCallReachesTheConfirmation(fs.get(), root);
  ARefusedFrameCallIsSaid(fs.get(), root);
  TheFrameCallRefusesAnUnusableContext();
  TheRunningRuntimeIsTheFileTheCodeIsIn();
  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "update_wiring_test: %d check(s) failed\n", quest_test::Failures());
    return 1;
  }
  std::printf("update_wiring_test: all checks passed\n");
  return 0;
}
