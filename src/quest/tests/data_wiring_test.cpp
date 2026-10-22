// Host test for the first-run data step as the game reaches it (src/quest/sentinel/data_wiring.cpp): through the real
// permission-wait handler and its thunk entry, with stand-ins for the game's RequestPermissions, the toast, the
// process end and the network. Covers: data already there is not fetched, a fetch serves the new root to the game, a
// failed fetch shows the floor message and ends the process after holding it, the probe file acts once, and nothing
// runs on a request that is not the boot request.
//
// Built and run by `just test-quest-data-net`.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <sys/stat.h>
#include <vector>

#include "data_wiring.h"
#include "obb_skip_hook.h"
#include "permission_wait_hook.h"
#include "process_end_hook.h"
#include "quest/data/data_step.h"
#include "quest/data/sha256.h"
#include "quest/tests/data_test_support.h"
#include "quest/tests/test_check.h"

namespace {

namespace dw = nevr_quest::data_wiring;
namespace pw = nevr_quest::permission_wait;
using namespace data_test;

constexpr const char* kBuild = "b243509c08ce677aeb95fa348016949b3fc45230";
constexpr const char* kManifestRel = "_data/5932408047/rad15/android/manifests/48037dc70b0ecab2";

std::set<std::string> g_present;
bool g_serving_done = false;  // the transport has served the last byte: the fetch's tree is "there"

bool FakeExists(const char* path) noexcept {
  if (g_present.count(path) != 0) return true;
  if (g_serving_done) {
    const std::string media = nevr_quest::data_root::kMediaRoot;
    return std::string(path) == media || std::string(path) == media + "/_data" ||
           std::string(path) == media + "/" + kManifestRel;
  }
  return false;
}

std::vector<std::string> g_shown;
std::vector<std::string> g_log;  // "show", "sleep:<ms>", "end", in order
int g_ends = 0;
int g_requests = 0;

const char* FakeShow(void*, const char* text, bool* prepared) noexcept {
  g_shown.push_back(text);
  g_log.push_back("show");
  *prepared = true;
  return "requested";
}
int g_exit_code = 0;
std::string g_exit_reason;
void FakeEnd(int exitCode, const char* reason) noexcept {
  ++g_ends;
  g_exit_code = exitCode;
  g_exit_reason = reason != nullptr ? reason : "";
  g_log.push_back("end");
}
void FakeSleep(unsigned ms) noexcept { g_log.push_back("sleep:" + std::to_string(ms)); }
// A stand-in android_app: only destroyRequested (offset 0x64) is read by the step.
unsigned char g_app[0x100];
bool g_have_app = false;
void* FakeApp() noexcept { return g_have_app ? static_cast<void*>(g_app) : nullptr; }
void SetDestroyRequested(int value) { std::memcpy(g_app + 0x64, &value, sizeof(value)); }

void RealRequestPermissions(const char* const*, std::uint64_t) { ++g_requests; }
bool GrantedCheck(const char*) noexcept { return true; }
bool DeniedCheck(const char*) noexcept { return false; }

class ServingTransport : public nevr_quest::data::Transport {
 public:
  ServingTransport(std::string payload, bool network) : payload_(std::move(payload)), network_(network) {}
  nevr_quest::data::TransportResult GetRange(const std::string&, std::uint64_t offset, const nevr_quest::data::BodyFn& on_body,
                                             const nevr_quest::data::KeepGoingFn& keep_going) override {
    ++calls;
    nevr_quest::data::TransportResult r;
    keep_going_seen = keep_going();
    if (!keep_going_seen) {
      r.interrupted = true;
      return r;
    }
    if (!network_) {
      r.code = 6;
      return r;
    }
    r.http_status = 206;
    for (std::size_t at = static_cast<std::size_t>(offset); at < payload_.size();) {
      const std::size_t n = std::min<std::size_t>(payload_.size() - at, 8192);
      if (!on_body(206, payload_.data() + at, n)) return r;
      at += n;
    }
    g_serving_done = true;
    r.completed = true;
    return r;
  }
  int calls = 0;
  bool keep_going_seen = true;  // what keep_going answered at the start of the last request

 private:
  std::string payload_;
  bool network_;
};

std::string GoodZip() {
  return BuildZip({{"_data/", "", 0},
                   {"_data/5932408047/", "", 0},
                   {"_data/5932408047/rad15/", "", 0},
                   {"_data/5932408047/rad15/android/", "", 0},
                   {"_data/5932408047/rad15/android/manifests/", "", 0},
                   {"_data/5932408047/rad15/android/manifests/48037dc70b0ecab2", Pattern(20000), 8}});
}

pw::RequestThunk::Fn RequestEntry() {
  return reinterpret_cast<pw::RequestThunk::Fn>(pw::RequestThunk::EntryAddress());
}

struct Rig {
  Scratch scratch;
  std::string zip = GoodZip();
  ServingTransport net;
  dw::Options options;
  explicit Rig(bool network = true) : net(zip, network) {
    g_present.clear();
    g_serving_done = false;
    g_shown.clear();
    g_log.clear();
    g_ends = 0;
    g_requests = 0;
    g_exit_code = 0;
    g_exit_reason.clear();
    g_have_app = false;
    SetDestroyRequested(0);
    nevr_quest::obb_skip::ResetForTest();
    pw::ResetForTest();
    pw::RequestThunk::Reset();
    *pw::RequestThunk::OriginalOut() = reinterpret_cast<void*>(&RealRequestPermissions);
    options.show = &FakeShow;
    options.app = &FakeApp;
    options.exists = &FakeExists;
    options.transport = &net;
    options.ender = &FakeEnd;
    options.sleep = &FakeSleep;
    options.probe_path = scratch.Path("nevr-data-probe");
    options.fetch_root_override = scratch.Path("root");
    options.hold_ms = 7;
    options.zip_sha256 = nevr_quest::data::Sha256Hex(zip.data(), zip.size());
    options.zip_bytes = zip.size();
    options.extracted_bytes = 1000;
  }
  // The game's boot request, through the real hook.
  void BootRequest(bool (*check)(const char*) noexcept = &GrantedCheck) {
    dw::Install(options);
    pw::ArmForTest(check, 5, 100);
    static const char* const names[2] = {"android.permission.READ_EXTERNAL_STORAGE",
                                         "android.permission.WRITE_EXTERNAL_STORAGE"};
    RequestEntry()(names, 2);
  }
};

void DataAlreadyThereIsNotFetchedAndTheProcessContinues() {
  Rig rig;
  const std::string media = nevr_quest::data_root::kMediaRoot;
  g_present = {media, media + "/_data", media + "/" + kManifestRel};
  rig.BootRequest();
  QCHECK(g_requests == 1);        // the game's own call ran
  QCHECK(rig.net.calls == 0);
  QCHECK(g_ends == 0);
  QCHECK(g_shown.empty());
}

void AFetchPlacesTheDataAndTheGameIsServedTheRoot() {
  Rig rig;
  QCHECK(std::strcmp(nevr_quest::obb_skip::CurrentRoot(), nevr_quest::data_root::kLegacyRoot) == 0);  // none yet
  rig.BootRequest();
  QCHECK(rig.net.calls == 1);
  QCHECK(Exists(rig.options.fetch_root_override + "/" + kManifestRel));
  QCHECK(g_ends == 0);
  // The download notice is shown once, before the download starts: nothing else.
  QCHECK(g_shown.size() == 1 && g_shown[0] == nevr_quest::data::kDownloadNotice);
  QCHECK(std::strcmp(nevr_quest::obb_skip::CurrentRoot(), nevr_quest::data_root::kMediaRoot) == 0);
}

void AFailedFetchShowsTheMessageHoldsItAndEndsTheProcess() {
  Rig rig(false);
  rig.BootRequest();
  QCHECK(g_requests == 1);
  QCHECK(g_ends == 1);
  QCHECK(g_exit_code != 0);  // a failure ends nonzero, with its reason
  QCHECK(g_exit_reason == "data_step_failed");
  QCHECK(g_shown.size() == 3);  // the download notice, the message, and the message again after the first hold
  QCHECK(g_shown[0] == nevr_quest::data::kDownloadNotice);
  QCHECK(g_shown[1].find(rig.options.fetch_root_override + "/_data") != std::string::npos);  // the path
  QCHECK(g_shown[1].find(kBuild) != std::string::npos);                                       // the build
  QCHECK(g_shown[1].find("start the game again") != std::string::npos);                       // the action
  QCHECK(g_shown[2] == g_shown[1]);
  // notice, message, hold, message again, hold, then the process ends: nothing after the end. The fetch's own backoff
  // sleeps (slices of at most 250 ms) are not the hold.
  const std::vector<std::string> want = {"show", "show", "sleep:7", "show", "sleep:7", "end"};
  std::vector<std::string> got;
  for (const std::string& e : g_log) {
    if (e == "sleep:7" || e == "show" || e == "end") got.push_back(e);
  }
  QCHECK(got == want);
}

void ARequestThatIsNotTheBootRequestRunsNothing() {
  Rig rig(false);
  rig.BootRequest();
  const int shown = static_cast<int>(g_shown.size());  // includes the notice from the boot request
  const int calls = rig.net.calls;
  static const char* const script[1] = {"android.permission.WRITE_EXTERNAL_STORAGE"};
  RequestEntry()(script, 1);
  QCHECK(static_cast<int>(g_shown.size()) == shown && rig.net.calls == calls && g_ends == 1);
}

void AToastProbeShowsItsTextOnceAndIsConsumed() {
  Rig rig;
  const std::string media = nevr_quest::data_root::kMediaRoot;
  g_present = {media, media + "/_data", media + "/" + kManifestRel};
  rig.options.probe_enabled = true;
  WriteFile(rig.options.probe_path, "toast probe text 1\nignored line\n");
  rig.BootRequest();
  QCHECK(g_shown.size() == 1 && g_shown[0] == "probe text 1");
  QCHECK(!Exists(rig.options.probe_path));
  QCHECK(Exists(rig.options.probe_path + ".done"));
  rig.BootRequest();  // the next launch: no probe
  QCHECK(g_shown.size() == 1);
}

void ABlockProbeHoldsTheThreadForItsSeconds() {
  Rig rig;
  const std::string media = nevr_quest::data_root::kMediaRoot;
  g_present = {media, media + "/_data", media + "/" + kManifestRel};
  rig.options.probe_enabled = true;
  WriteFile(rig.options.probe_path, "block 3\n");
  rig.BootRequest();
  QCHECK(std::count(g_log.begin(), g_log.end(), std::string("sleep:1000")) == 3);
  QCHECK(g_requests == 1);  // and then the game's call returned
}

void ABlockProbeIsCapped() {
  Rig rig;
  const std::string media = nevr_quest::data_root::kMediaRoot;
  g_present = {media, media + "/_data", media + "/" + kManifestRel};
  rig.options.probe_enabled = true;
  WriteFile(rig.options.probe_path, "block 99999\n");
  rig.BootRequest();
  QCHECK(std::count(g_log.begin(), g_log.end(), std::string("sleep:1000")) == 900);
}

void ADeniedGrantRunsNoDataStepAndEndsNonzeroWithTheHowToMessage() {
  Rig rig;
  const std::string media = nevr_quest::data_root::kMediaRoot;
  g_present = {media, media + "/_data", media + "/" + kManifestRel};  // even with data there, nothing is read
  rig.options.probe_enabled = true;
  WriteFile(rig.options.probe_path, "toast must not run\n");
  rig.BootRequest(&DeniedCheck);
  QCHECK(g_requests == 1);  // the game's own call still ran
  QCHECK(rig.net.calls == 0);
  QCHECK(g_ends == 1 && g_exit_code != 0 && g_exit_reason == "no_storage_grant");
  QCHECK(!g_shown.empty());
  for (const std::string& m : g_shown) {
    QCHECK(m == nevr_quest::data::kNeedsStoragePermission);
    QCHECK(m.find("Storage") != std::string::npos && m.find("Settings") != std::string::npos);
  }
  QCHECK(Exists(rig.options.probe_path));  // the probe did not run either
  QCHECK(!Exists(rig.options.fetch_root_override + "/" + kManifestRel));
}

void TheProbeIsNotReadInAShippedBuild() {
  Rig rig;
  const std::string media = nevr_quest::data_root::kMediaRoot;
  g_present = {media, media + "/_data", media + "/" + kManifestRel};
  rig.options.probe_enabled = false;
  WriteFile(rig.options.probe_path, "toast probe text 1\n");
  rig.BootRequest();
  QCHECK(g_shown.empty());
  QCHECK(Exists(rig.options.probe_path) && !Exists(rig.options.probe_path + ".done"));
}

void TheGamesDestroyRequestEndsTheDownload() {
  {
    Rig rig;
    g_have_app = true;
    rig.BootRequest();
    QCHECK(rig.net.calls == 1 && rig.net.keep_going_seen);  // a live game keeps the download going
  }
  {
    Rig rig;
    g_have_app = true;
    SetDestroyRequested(1);  // android_app::destroyRequested
    rig.BootRequest();
    QCHECK(rig.net.calls == 0);  // the fetch asks keep_going before every request and does not make one
    QCHECK(g_ends == 1);
    QCHECK(!Exists(rig.options.fetch_root_override + "/" + kManifestRel));
  }
}

void TheRetryWaitUsesTheFetchsDefaultBackoffInSlices() {
  Rig rig(false);
  rig.BootRequest();
  int slices = 0;
  for (const std::string& e : g_log) {
    if (e.rfind("sleep:", 0) == 0 && e != "sleep:7") {
      const unsigned ms = static_cast<unsigned>(std::stoul(e.substr(6)));
      QCHECK(ms >= 1 && ms <= 250);  // a cancel can end the wait within a slice
      ++slices;
    }
  }
  QCHECK(slices >= 8);  // the first backoff is 2 s
}

}  // namespace

int main() {
  DataAlreadyThereIsNotFetchedAndTheProcessContinues();
  AFetchPlacesTheDataAndTheGameIsServedTheRoot();
  AFailedFetchShowsTheMessageHoldsItAndEndsTheProcess();
  ARequestThatIsNotTheBootRequestRunsNothing();
  AToastProbeShowsItsTextOnceAndIsConsumed();
  ABlockProbeHoldsTheThreadForItsSeconds();
  ABlockProbeIsCapped();
  ADeniedGrantRunsNoDataStepAndEndsNonzeroWithTheHowToMessage();
  TheProbeIsNotReadInAShippedBuild();
  TheGamesDestroyRequestEndsTheDownload();
  TheRetryWaitUsesTheFetchsDefaultBackoffInSlices();
  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "data_wiring_test: %d check(s) failed\n", quest_test::Failures());
    return 1;
  }
  std::printf("data_wiring_test: all checks pass\n");
  return 0;
}
