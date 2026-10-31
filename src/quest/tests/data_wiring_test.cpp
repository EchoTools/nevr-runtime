// Host test for the first-run data step as the game reaches it (src/quest/sentinel/data_wiring.cpp): through the real
// permission-wait handlers and their thunk entries (RequestPermissions and CheckPermission), with stand-ins for the
// game's functions, the toast, the process end and the network. Covers: data already there is not fetched; a
// development build fetches into the root the engine reads (the legacy root without an OBB file, unless the data is under
// the media root and the data-root hook is installed: then the media root, with or without the storage grant); a release build
// never fetches and shows the missing-data message; Storage not granted writes nothing and shows the Storage message,
// on both the request path and the microphone-granted path; the step runs once across both paths; a failed fetch shows
// the floor message and ends the process after holding it; the probe file acts once; and the bundled content pack
// (docs/adr/0029-quest-bundled-content-pack.md) after the data step: none, placed after the data was used and after it was
// fetched, in a release build too, stock_mismatch, a failed placement that ends the process, a kill between the writes, and no
// pack step at all when the data step failed or the storage message ended the process.
//
// Built and run by `just test-quest-data-net`.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <thread>
#include <dirent.h>
#include <sys/stat.h>
#include <vector>

#include <nlohmann/json.hpp>

#include "data_wiring.h"
#include "hook_log.h"
#include "obb_skip_hook.h"
#include "permission_wait_hook.h"
#include "probe_command.h"
#include "process_end_hook.h"
#include "quest/data/data_step.h"
#include "quest/data/pack.h"
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
std::string g_served_root = nevr_quest::data_root::kLegacyRoot;  // where the fetch's tree "is" once served
bool g_serving_done = false;  // the transport has served the last byte: the fetch's tree is "there"

bool FakeExists(const char* path) noexcept {
  if (g_present.count(path) != 0) return true;
  if (g_serving_done) {
    const std::string root = g_served_root;
    return std::string(path) == root || std::string(path) == root + "/_data" ||
           std::string(path) == root + "/" + kManifestRel;
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

std::vector<std::string> g_lines;  // the sentinel log lines of the run
void CaptureSink(sentinel::LogLevel, const char* line) { g_lines.emplace_back(line); }

// The first captured line of `event` (empty when none).
std::string LineOf(const char* event) {
  const std::string key = std::string("\"event\":\"") + event + "\"";
  for (const std::string& l : g_lines) {
    if (l.find(key) != std::string::npos) return l;
  }
  return "";
}
bool Has(const std::string& text, const std::string& needle) { return text.find(needle) != std::string::npos; }

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

namespace obb = nevr_quest::obb_skip;

void RealMount(void*, const char*, const char*, void (*)(const char*, int, void*), void*) {}
void NoCallback(const char*, std::int32_t, void*) {}

// The game found an OBB file and mounted through the OBB skip: its callback is answered and the root it serves is
// `root` (a directory that exists; the skip serves the media directory when it exists). Without this call the skip never
// ran (CSysFile::Init returned at "OBB not found"): the engine's root is the legacy literal. Call it after the Rig is
// made and before the boot request, as the game's mount precedes CR15Game::Initialize.
void ObbSkipsTheMountServing(const char* root) {
  obb::ArmForTest();
  *obb::MountThunk::OriginalOut() = reinterpret_cast<void*>(&RealMount);
  using MountFn = obb::MountThunk::Fn;
  reinterpret_cast<MountFn>(obb::MountThunk::EntryAddress())(nullptr, "x.obb", "k", &NoCallback, nullptr);
  g_present.insert(root);
  obb::ResolveRoot(&FakeExists);
}

pw::RequestThunk::Fn RequestEntry() {
  return reinterpret_cast<pw::RequestThunk::Fn>(pw::RequestThunk::EntryAddress());
}
pw::CheckThunk::Fn CheckEntry() {
  return reinterpret_cast<pw::CheckThunk::Fn>(pw::CheckThunk::EntryAddress());
}

// The game's CheckPermission: the microphone and each storage permission answer as the test sets them.
bool g_micGranted = true;
bool g_storageGranted = true;
bool GameCheck(const char* permission) noexcept {
  if (std::strcmp(permission, "android.permission.RECORD_AUDIO") == 0) return g_micGranted;
  return g_storageGranted;
}

struct Rig {
  Scratch scratch;
  std::string zip = GoodZip();
  ServingTransport net;
  dw::Options options;
  explicit Rig(bool network = true) : net(zip, network) {
    g_present.clear();
    g_served_root = nevr_quest::data_root::kLegacyRoot;
    g_serving_done = false;
    g_shown.clear();
    g_log.clear();
    g_ends = 0;
    g_requests = 0;
    g_exit_code = 0;
    g_exit_reason.clear();
    g_have_app = false;
    g_lines.clear();
    g_micGranted = true;
    g_storageGranted = true;
    sentinel::SetLogSink(&CaptureSink);
    SetDestroyRequested(0);
    nevr_quest::obb_skip::ResetForTest();
    pw::ResetForTest();
    pw::RequestThunk::Reset();
    pw::CheckThunk::Reset();
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
    options.allow_fetch = true;  // a development build; the release cases clear it
    options.zip_sha256 = nevr_quest::data::Sha256Hex(zip.data(), zip.size());
    options.zip_bytes = zip.size();
    options.extracted_bytes = 1000;
  }
  ~Rig() { sentinel::SetLogSink(nullptr); }
  // The game's boot request, through the real hook.
  void BootRequest(bool (*check)(const char*) noexcept = &GrantedCheck) {
    dw::Install(options);
    pw::ArmForTest(check, 5, 100);
    static const char* const names[2] = {"android.permission.READ_EXTERNAL_STORAGE",
                                         "android.permission.WRITE_EXTERNAL_STORAGE"};
    RequestEntry()(names, 2);
  }
  // The game's first CheckPermission (the microphone), through the real hook, on a headset that already holds the
  // microphone permission: no request follows.
  bool MicCheck() {
    dw::Install(options);
    pw::ArmForTest(&GameCheck, 5, 100);
    *pw::CheckThunk::OriginalOut() = reinterpret_cast<void*>(&GameCheck);
    return CheckEntry()("android.permission.RECORD_AUDIO");
  }
  void LaterBootRequest() {
    static const char* const names[3] = {"android.permission.RECORD_AUDIO", "android.permission.READ_EXTERNAL_STORAGE",
                                         "android.permission.WRITE_EXTERNAL_STORAGE"};
    RequestEntry()(names, 3);
  }
};

std::string Legacy(const char* rel = "") {
  return std::string(nevr_quest::data_root::kLegacyRoot) + (*rel != '\0' ? std::string("/") + rel : std::string());
}
std::string Media(const char* rel = "") {
  return std::string(nevr_quest::data_root::kMediaRoot) + (*rel != '\0' ? std::string("/") + rel : std::string());
}
void DataIn(const std::string& root) { g_present.insert({root, root + "/_data", root + "/" + kManifestRel}); }
// A `_data` directory with no manifest: a partial copy, or the data of another build.
void StaleDataIn(const std::string& root) { g_present.insert({root, root + "/_data"}); }

void DataAlreadyThereIsNotFetchedAndTheProcessContinues() {
  Rig rig;
  DataIn(Legacy());  // no OBB file: the engine reads the legacy root
  rig.BootRequest();
  QCHECK(g_requests == 1);        // the game's own call ran
  QCHECK(rig.net.calls == 0);
  QCHECK(g_ends == 0);
  QCHECK(g_shown.empty());
  QCHECK(Has(LineOf("data_check"), "\"action\":\"use\""));
  // The sentinel log says which data it found, not only where: the version folder and the manifest hash fields.
  QCHECK(Has(LineOf("data_check"), "\"data_version\":") && Has(LineOf("data_check"), "\"manifest_sha256\":"));
}

void AFetchPlacesTheDataInTheRootTheEngineReads() {
  Rig rig;
  rig.BootRequest();
  QCHECK(rig.net.calls == 1);
  QCHECK(Exists(rig.options.fetch_root_override + "/" + kManifestRel));
  QCHECK(g_ends == 0);
  // The download notice is shown once, before the download starts: nothing else.
  QCHECK(g_shown.size() == 1 && g_shown[0] == nevr_quest::data::kDownloadNotice);
  // A sideloaded headset has no OBB file, the skip never ran, and the engine reads the legacy root: that is the target.
  const std::string check = LineOf("data_check");
  QCHECK(Has(check, "\"action\":\"fetch\"") && Has(check, "\"root\":\"" + Legacy() + "\""));
  QCHECK(Has(LineOf("data_step"), "\"outcome\":\"fetched\""));
}

void AFetchWithAnObbServingTheMediaRootTargetsTheMediaRoot() {
  Rig rig;
  ObbSkipsTheMountServing(nevr_quest::data_root::kMediaRoot);  // the game recorded the media root
  g_served_root = nevr_quest::data_root::kMediaRoot;
  rig.BootRequest();
  QCHECK(rig.net.calls == 1 && g_ends == 0);
  const std::string check = LineOf("data_check");
  QCHECK(Has(check, "\"action\":\"fetch\"") && Has(check, "\"root\":\"" + Media() + "\""));
}

void DataInTheMediaRootDoesNotCountWhenTheEngineReadsTheLegacyRoot() {
  Rig rig;
  DataIn(Media());  // this build's data, in a root nothing reads
  rig.BootRequest();
  QCHECK(rig.net.calls == 1);  // the engine's root has none: it is fetched
  QCHECK(Has(LineOf("data_check"), "\"action\":\"fetch\""));
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
  DataIn(Legacy());
  rig.options.probe_enabled = true;
  WriteFile(rig.options.probe_path, "toast probe text 1\nignored line\n");
  rig.BootRequest();
  QCHECK(g_shown.size() == 1 && g_shown[0] == "probe text 1");
  QCHECK(!Exists(rig.options.probe_path));
  QCHECK(Exists(rig.options.probe_path + ".done"));
  rig.BootRequest();  // the next launch: no probe
  QCHECK(g_shown.size() == 1);
}

// ---- the probe's kinds (#829) ----

void TheProbeLineParsesIntoItsKindAndArgument() {
  namespace dwp = nevr_quest::data_wiring;
  struct Case {
    const char* line;
    dwp::ProbeKind kind;
    const char* arg;
  };
  const Case cases[] = {
      {"toast hello", dwp::ProbeKind::kToast, "hello"},
      {"ui-toast hello world", dwp::ProbeKind::kUiToast, "hello world"},
      {"ui-post hello world", dwp::ProbeKind::kUiPost, "hello world"},
      {"ui-pos hello", dwp::ProbeKind::kUnknown, ""},
      {"dialog a dialog", dwp::ProbeKind::kDialog, "a dialog"},
      {"block 3", dwp::ProbeKind::kBlock, "3"},
      {"ui-toast  two spaces", dwp::ProbeKind::kUiToast, " two spaces"},  // one space ends the kind; the rest is the text
      {"ui-toast ", dwp::ProbeKind::kUiToast, ""},
      {"toasty hello", dwp::ProbeKind::kUnknown, ""},   // a kind is matched whole
      {"ui-toast", dwp::ProbeKind::kUnknown, ""},       // and needs its space
      {"UI-TOAST hello", dwp::ProbeKind::kUnknown, ""}, // and is case-sensitive
      {"xui-toast hello", dwp::ProbeKind::kUnknown, ""},
      {"dialogs hello", dwp::ProbeKind::kUnknown, ""},
      {"", dwp::ProbeKind::kUnknown, ""},
  };
  for (const Case& c : cases) {
    const dwp::ProbeCommand parsed = dwp::ParseProbeCommand(c.line);
    QCHECK(parsed.kind == c.kind);
    QCHECK(parsed.arg != nullptr && std::strcmp(parsed.arg, c.arg) == 0);
  }
  // The argument points into the parsed line, after the kind's word and one space.
  const char* line = "ui-toast abc";
  QCHECK(dwp::ParseProbeCommand(line).arg == line + 9);
}

std::vector<std::string> g_ui_shown;
std::vector<std::string> g_dialog_shown;
bool g_ui_prepared = false;
const char* g_ui_result = "requested";
const char* FakeShowUi(void*, const char* text, bool* prepared) noexcept {
  g_ui_shown.push_back(text);
  *prepared = g_ui_prepared;
  return g_ui_result;
}
std::vector<std::string> g_post_shown;
const char* g_post_result = "posted";
const char* FakeShowPost(void*, const char* text, bool* prepared) noexcept {
  g_post_shown.push_back(text);
  *prepared = false;
  return g_post_result;
}
const char* FakeShowDialog(void*, const char* text, bool* prepared) noexcept {
  g_dialog_shown.push_back(text);
  *prepared = false;
  return "requested";
}
void ResetProbeSeams() {
  g_ui_shown.clear();
  g_dialog_shown.clear();
  g_post_shown.clear();
  g_post_result = "posted";
  g_ui_prepared = false;
  g_ui_result = "requested";
}

std::vector<std::string> ProbeLines() {
  std::vector<std::string> out;
  for (const std::string& l : g_lines) {
    if (Has(l, "\"event\":\"data_probe\"")) out.push_back(l);
  }
  return out;
}

void AUiToastProbeGoesToTheUiPresenterOnceAndSaysWhichThreadAndLooper() {
  ResetProbeSeams();
  Rig rig;
  DataIn(Legacy());
  rig.options.probe_enabled = true;
  rig.options.show_ui = &FakeShowUi;
  rig.options.show_dialog = &FakeShowDialog;
  WriteFile(rig.options.probe_path, "ui-toast can you read this\n");
  rig.BootRequest();
  QCHECK(g_ui_shown.size() == 1 && g_ui_shown[0] == "can you read this");
  QCHECK(g_shown.empty() && g_dialog_shown.empty());  // not the floor presenter, not the dialog
  QCHECK(!Exists(rig.options.probe_path) && Exists(rig.options.probe_path + ".done"));
  const std::vector<std::string> lines = ProbeLines();
  QCHECK(lines.size() == 1);
  if (lines.size() == 1) {
    QCHECK(Has(lines[0], "\"kind\":\"ui-toast\"") && Has(lines[0], "\"result\":\"requested\""));
    QCHECK(Has(lines[0], "\"looper_prepared\":\"false\"") && Has(lines[0], "\"tid\":"));
    QCHECK(Has(lines[0], "\"main_thread\":\"true\""));  // the host test runs the step on the process's main thread
    QCHECK(Has(lines[0], "\"level\":\"info\""));
  }
  rig.BootRequest();  // the next launch: no probe
  QCHECK(g_ui_shown.size() == 1);
}

void TheProbeSaysWhenItRanOffTheMainThreadAndWhenALooperWasPrepared() {
  ResetProbeSeams();
  g_ui_prepared = true;
  Rig rig;
  DataIn(Legacy());
  rig.options.probe_enabled = true;
  rig.options.show_ui = &FakeShowUi;
  WriteFile(rig.options.probe_path, "ui-toast x\n");
  std::thread worker([&rig] { rig.BootRequest(); });  // the game's android_main is not the process's main thread
  worker.join();
  const std::vector<std::string> lines = ProbeLines();
  QCHECK(lines.size() == 1);
  if (lines.size() == 1) {
    QCHECK(Has(lines[0], "\"main_thread\":\"false\"") && Has(lines[0], "\"looper_prepared\":\"true\""));
  }
}

void AUiPostProbeGoesToThePostPresenterAndIsInfoWhenPosted() {
  ResetProbeSeams();
  Rig rig;
  DataIn(Legacy());
  rig.options.probe_enabled = true;
  rig.options.show_ui = &FakeShowUi;
  rig.options.show_post = &FakeShowPost;
  WriteFile(rig.options.probe_path, "ui-post posted text\n");
  rig.BootRequest();
  QCHECK(g_post_shown.size() == 1 && g_post_shown[0] == "posted text");
  QCHECK(g_ui_shown.empty() && g_shown.empty() && g_dialog_shown.empty());
  const std::vector<std::string> lines = ProbeLines();
  QCHECK(lines.size() == 1);
  if (lines.size() == 1) {
    QCHECK(Has(lines[0], "\"kind\":\"ui-post\"") && Has(lines[0], "\"result\":\"posted\""));
    QCHECK(Has(lines[0], "\"level\":\"info\""));
  }
  // The same probe with no looper captured: a warning with the token.
  ResetProbeSeams();
  g_post_result = "no_ui_looper";
  Rig again;
  DataIn(Legacy());
  again.options.probe_enabled = true;
  again.options.show_post = &FakeShowPost;
  WriteFile(again.options.probe_path, "ui-post x\n");
  g_lines.clear();
  again.BootRequest();
  const std::vector<std::string> warned = ProbeLines();
  QCHECK(warned.size() == 1 && Has(warned[0], "\"result\":\"no_ui_looper\"") && Has(warned[0], "\"level\":\"warn\""));
  // And with no presenter at all.
  ResetProbeSeams();
  Rig none;
  DataIn(Legacy());
  none.options.probe_enabled = true;
  WriteFile(none.options.probe_path, "ui-post x\n");
  g_lines.clear();
  none.BootRequest();
  const std::vector<std::string> bare = ProbeLines();
  QCHECK(bare.size() == 1 && Has(bare[0], "\"result\":\"no_presenter\""));
}

void AFailedUiToastIsAWarningWithItsToken() {
  ResetProbeSeams();
  g_ui_result = "looper_overload_unavailable";
  Rig rig;
  DataIn(Legacy());
  rig.options.probe_enabled = true;
  rig.options.show_ui = &FakeShowUi;
  WriteFile(rig.options.probe_path, "ui-toast x\n");
  rig.BootRequest();
  const std::vector<std::string> lines = ProbeLines();
  QCHECK(lines.size() == 1);
  if (lines.size() == 1) {
    QCHECK(Has(lines[0], "\"result\":\"looper_overload_unavailable\"") && Has(lines[0], "\"level\":\"warn\""));
  }
}

void ADialogProbeGoesToTheDialogPresenterAndWithoutOneSaysNoPresenter() {
  ResetProbeSeams();
  {
    Rig rig;
    DataIn(Legacy());
    rig.options.probe_enabled = true;
    rig.options.show_ui = &FakeShowUi;
    rig.options.show_dialog = &FakeShowDialog;
    WriteFile(rig.options.probe_path, "dialog read me\n");
    rig.BootRequest();
    QCHECK(g_dialog_shown.size() == 1 && g_dialog_shown[0] == "read me" && g_ui_shown.empty() && g_shown.empty());
    const std::vector<std::string> lines = ProbeLines();
    QCHECK(lines.size() == 1 && Has(lines[0], "\"kind\":\"dialog\"") && Has(lines[0], "\"result\":\"requested\""));
  }
  ResetProbeSeams();
  {
    Rig rig;
    DataIn(Legacy());
    rig.options.probe_enabled = true;
    WriteFile(rig.options.probe_path, "dialog read me\n");
    rig.BootRequest();
    const std::vector<std::string> lines = ProbeLines();
    QCHECK(lines.size() == 1 && Has(lines[0], "\"kind\":\"dialog\"") && Has(lines[0], "\"result\":\"no_presenter\""));
    QCHECK(Has(lines[0], "\"level\":\"warn\""));
    QCHECK(g_shown.empty());  // nothing fell back to the floor presenter
    QCHECK(Exists(rig.options.probe_path + ".done"));
  }
}

void AUiToastProbeWithoutAPresenterOrWithoutTextShowsNothing() {
  ResetProbeSeams();
  {
    Rig rig;
    DataIn(Legacy());
    rig.options.probe_enabled = true;
    WriteFile(rig.options.probe_path, "ui-toast x\n");
    rig.BootRequest();
    const std::vector<std::string> lines = ProbeLines();
    QCHECK(lines.size() == 1 && Has(lines[0], "\"result\":\"no_presenter\"") && g_shown.empty());
  }
  ResetProbeSeams();
  {
    Rig rig;
    DataIn(Legacy());
    rig.options.probe_enabled = true;
    rig.options.show_ui = &FakeShowUi;
    WriteFile(rig.options.probe_path, "ui-toast \n");
    rig.BootRequest();
    const std::vector<std::string> lines = ProbeLines();
    QCHECK(g_ui_shown.empty());
    QCHECK(lines.size() == 1 && Has(lines[0], "\"result\":\"empty_text\""));
  }
}

void TheToastProbeStillUsesTheFloorPresenterAndNotTheUiOne() {
  ResetProbeSeams();
  Rig rig;
  DataIn(Legacy());
  rig.options.probe_enabled = true;
  rig.options.show_ui = &FakeShowUi;
  WriteFile(rig.options.probe_path, "toast plain\n");
  rig.BootRequest();
  QCHECK(g_shown.size() == 1 && g_shown[0] == "plain" && g_ui_shown.empty());
}

void ABlockProbeHoldsTheThreadForItsSeconds() {
  Rig rig;
  DataIn(Legacy());
  rig.options.probe_enabled = true;
  WriteFile(rig.options.probe_path, "block 3\n");
  rig.BootRequest();
  QCHECK(std::count(g_log.begin(), g_log.end(), std::string("sleep:1000")) == 3);
  QCHECK(g_requests == 1);  // and then the game's call returned
}

void ABlockProbeIsCapped() {
  Rig rig;
  DataIn(Legacy());
  rig.options.probe_enabled = true;
  WriteFile(rig.options.probe_path, "block 99999\n");
  rig.BootRequest();
  QCHECK(std::count(g_log.begin(), g_log.end(), std::string("sleep:1000")) == 900);
}

// ---- Storage not granted ----

// Without the grant the legacy root cannot be read, so with no OBB file (the engine reads the legacy root) nothing is
// looked at, fetched or written: the player is told how to allow Storage and the process ends.
void StorageNotGrantedWithoutAnObbShowsTheStorageMessageAndWritesNothing() {
  Rig rig;
  DataIn(Legacy());  // present or not, it cannot be read
  rig.BootRequest(&DeniedCheck);
  QCHECK(g_requests == 1);  // the game's own call still ran
  QCHECK(rig.net.calls == 0);
  QCHECK(!Exists(rig.options.fetch_root_override));  // nothing was created
  QCHECK(g_ends == 1 && g_exit_code != 0 && g_exit_reason == "no_storage_grant");
  QCHECK(g_shown.size() == 2);  // the message, and again after the first hold
  QCHECK(g_shown[0] == nevr_quest::data::kNeedsStoragePermission && g_shown[1] == g_shown[0]);
  QCHECK(g_shown[0].find("Storage") != std::string::npos && g_shown[0].find("Settings") != std::string::npos);
  int holds = 0;
  for (const std::string& e : g_log) holds += e == "sleep:7" ? 1 : 0;
  QCHECK(holds == 2);
  QCHECK(LineOf("data_check").empty());  // the step never looked
}

void StorageNotGrantedOnTheMicGrantedPathShowsTheStorageMessageAndWritesNothing() {
  Rig rig;
  g_storageGranted = false;  // the microphone is granted, so the game never asks for Storage
  const bool answer = rig.MicCheck();
  QCHECK(answer);  // the game's own answer comes back untouched
  QCHECK(g_requests == 0);
  QCHECK(rig.net.calls == 0);
  QCHECK(!Exists(rig.options.fetch_root_override));
  QCHECK(g_ends == 1 && g_exit_code != 0 && g_exit_reason == "no_storage_grant");
  QCHECK(g_shown.size() == 2 && g_shown[0] == nevr_quest::data::kNeedsStoragePermission);
  QCHECK(Has(LineOf("data_step_start"), "\"storage\":\"denied_not_requested\""));
}

// With an OBB file the game recorded the root the skip served; the media root needs no permission.
void StorageNotGrantedWithAnObbServingTheMediaRootStillUsesItsData() {
  Rig rig;
  DataIn(Media());
  ObbSkipsTheMountServing(nevr_quest::data_root::kMediaRoot);
  rig.BootRequest(&DeniedCheck);
  QCHECK(g_requests == 1);
  QCHECK(rig.net.calls == 0 && g_ends == 0 && g_shown.empty());
  QCHECK(Has(LineOf("data_check"), "\"action\":\"use\""));
}

void StorageNotGrantedWithAnObbServingTheMediaRootFetchesIntoItAndSucceeds() {
  Rig rig;
  g_served_root = nevr_quest::data_root::kMediaRoot;
  ObbSkipsTheMountServing(nevr_quest::data_root::kMediaRoot);
  rig.BootRequest(&DeniedCheck);
  QCHECK(rig.net.calls == 1);
  QCHECK(Exists(rig.options.fetch_root_override + "/" + kManifestRel));
  QCHECK(g_ends == 0);
  QCHECK(g_shown.size() == 1 && g_shown[0] == nevr_quest::data::kDownloadNotice);  // no Storage message
}

void StorageNotGrantedWithAnUnwritableMediaRootSaysHowToAllowStorageAndEndsNonzero() {
  Rig rig;
  ObbSkipsTheMountServing(nevr_quest::data_root::kMediaRoot);
  WriteFile(rig.scratch.Path("afile"), "x");
  rig.options.fetch_root_override = rig.scratch.Path("afile/root");  // under a file: the root cannot be made, bad_root
  rig.BootRequest(&DeniedCheck);
  QCHECK(rig.net.calls == 0);
  QCHECK(g_ends == 1 && g_exit_code != 0 && g_exit_reason == "no_storage_grant");
  QCHECK(!g_shown.empty() && g_shown.back() == nevr_quest::data::kNeedsStoragePermission);
  // One message, held once (shown, then again after the first hold): not the step's own and then this one.
  int storage = 0;
  for (const std::string& m : g_shown) {
    QCHECK(m == nevr_quest::data::kNeedsStoragePermission || m == nevr_quest::data::kDownloadNotice);
    storage += m == nevr_quest::data::kNeedsStoragePermission ? 1 : 0;
  }
  QCHECK(storage == 2);
  int holds = 0;
  for (const std::string& e : g_log) holds += e == "sleep:7" ? 1 : 0;
  QCHECK(holds == 2);
}

void AGrantedUnwritableRootEndsWithTheStepsOwnReason() {
  Rig rig;
  WriteFile(rig.scratch.Path("afile"), "x");
  rig.options.fetch_root_override = rig.scratch.Path("afile/root");
  rig.BootRequest();  // granted
  QCHECK(g_ends == 1 && g_exit_reason == "data_step_failed");
  for (const std::string& m : g_shown) QCHECK(m != nevr_quest::data::kNeedsStoragePermission);
}

// ---- no OBB file, the data under the media root (#866) ----

// The launch the tester's headset had: no OBB file (the skip's mount hook never runs), the resolver finds the game's
// `_data` under Android/media, and the data-root hook is installed, so the engine's GetDataRootDir question is answered
// with the media root. Call it after the Rig is made and before the boot request.
void NoObbFileAndTheMediaRootHoldsTheData() {
  DataIn(Media());
  obb::ResolveRoot(&FakeExists);
  obb::ArmForTest();  // arms the handlers and marks the data-root hook installed; no mount is skipped
}

void DataOnlyInTheMediaRootIsUsedWithNoObbFileAndNoStorageGrant() {
  Rig rig;
  NoObbFileAndTheMediaRootHoldsTheData();
  rig.BootRequest(&DeniedCheck);
  QCHECK(g_requests == 1);  // the game's own call still ran
  QCHECK(rig.net.calls == 0 && g_ends == 0 && g_shown.empty());
  QCHECK(g_exit_reason.empty());
  const std::string check = LineOf("data_check");
  QCHECK(Has(check, "\"action\":\"use\"") && Has(check, "\"root\":\"" + Media() + "\""));
  QCHECK(LineOf("data_step").find("no_storage_grant") == std::string::npos);
  // The line says which root the engine reads and why, so a headset log can be read against it.
  const std::string start = LineOf("data_step_start");
  QCHECK(Has(start, "\"engine_root\":\"" + Media() + "\"") && Has(start, "\"engine_root_source\":\"data_root_hook\""));
}

void DataOnlyInTheMediaRootIsUsedWithNoObbFileWhenStorageIsGranted() {
  Rig rig;  // a development build: allow_fetch is true, so a wrong root would download 937 MB
  NoObbFileAndTheMediaRootHoldsTheData();
  rig.BootRequest();
  QCHECK(rig.net.calls == 0 && g_ends == 0 && g_shown.empty());
  QCHECK(Has(LineOf("data_check"), "\"action\":\"use\"") && Has(LineOf("data_check"), "\"root\":\"" + Media() + "\""));
}

void AReleaseBuildFindsTheDataInTheMediaRootWithNoObbFileAndSaysNothing() {
  for (bool granted : {true, false}) {
    Rig rig;
    rig.options.allow_fetch = false;
    rig.options.hold_ms = 0;
    NoObbFileAndTheMediaRootHoldsTheData();
    rig.BootRequest(granted ? &GrantedCheck : &DeniedCheck);
    QCHECK(rig.net.calls == 0 && g_ends == 0 && g_shown.empty());
    QCHECK(g_exit_reason.empty());
    QCHECK(Has(LineOf("data_check"), "\"action\":\"use\""));
  }
}

void DataOnlyInTheLegacyRootStillNeedsTheGrantWithNoObbFile() {
  Rig rig;
  DataIn(Legacy());
  obb::ResolveRoot(&FakeExists);  // legacy wins: the engine's own answer is already that root
  obb::ArmForTest();
  rig.BootRequest(&DeniedCheck);
  QCHECK(g_ends == 1 && g_exit_reason == "no_storage_grant");
  QCHECK(g_shown.size() == 2 && g_shown[0] == nevr_quest::data::kNeedsStoragePermission);
  QCHECK(Has(LineOf("data_step_start"), "\"engine_root_source\":\"data_root_hook\""));  // the hook answers the legacy root too (C1)
}

// A `_data` under the media root that is not this version's (no manifest) is not what the engine is sent to read: with
// complete data in the legacy root it keeps reading that (#932).
void StaleMediaDataAndCompleteLegacyDataKeepsTheEngineOnTheLegacyRoot() {
  for (const bool granted : {true, false}) {
    Rig rig;  // a development build: a wrong root would download 937 MB
    StaleDataIn(Media());
    DataIn(Legacy());
    obb::ResolveRoot(&FakeExists);
    obb::ArmForTest();
    rig.BootRequest(granted ? &GrantedCheck : &DeniedCheck);
    const std::string start = LineOf("data_step_start");
    QCHECK(Has(start, "\"engine_root\":\"" + Legacy() + "\"") && Has(start, "\"engine_root_source\":\"data_root_hook\""));
    if (granted) {
      QCHECK(rig.net.calls == 0 && g_ends == 0 && g_shown.empty());
      QCHECK(Has(LineOf("data_check"), "\"action\":\"use\"") && Has(LineOf("data_check"), "\"root\":\"" + Legacy() + "\""));
    } else {
      QCHECK(g_ends == 1 && g_exit_reason == "no_storage_grant");  // the engine reads shared storage, so the grant is needed
    }
  }
}

void CompleteMediaDataIsTheEnginesRootEvenWhenLegacyDataIsCompleteToo() {
  Rig rig;
  DataIn(Media());
  DataIn(Legacy());
  obb::ResolveRoot(&FakeExists);
  obb::ArmForTest();
  rig.BootRequest(&DeniedCheck);
  QCHECK(g_ends == 0 && g_shown.empty() && rig.net.calls == 0);
  const std::string start = LineOf("data_step_start");
  QCHECK(Has(start, "\"engine_root\":\"" + Media() + "\"") && Has(start, "\"engine_root_source\":\"data_root_hook\""));
}

// Neither root holds complete data (a `_data` without the manifest in each). The engine is answered the media root:
// docs/adr/0007 and 0013, "the media root unless the legacy root alone holds the data". The storage gate in AfterStorage
// fires only when the engine root is the legacy root, so it does not apply here, even without the grant. The data step
// then reads the media root: its `_data` is not this version's (no manifest), so Decide replaces it (kReplace), which is a
// fetch that keeps the old tree aside. The rig's fetch writes to its scratch root (fetch_root_override), where no `_data`
// exists, so nothing is set aside and the fetch completes. The process continues: no end, no exit reason.
void NeitherRootCompleteIsTheEnginesMediaRoot() {
  Rig rig;
  StaleDataIn(Media());
  StaleDataIn(Legacy());
  obb::ResolveRoot(&FakeExists);
  obb::ArmForTest();
  rig.BootRequest(&DeniedCheck);
  const std::string start = LineOf("data_step_start");
  QCHECK(Has(start, "\"engine_root\":\"" + Media() + "\"") && Has(start, "\"engine_root_source\":\"data_root_hook\""));
  QCHECK(Has(LineOf("data_check"), "\"action\":\"replace\"") && Has(LineOf("data_check"), "\"root\":\"" + Media() + "\""));
  bool replacing = false;
  for (const std::string& l : g_lines) {
    if (Has(l, "\"event\":\"data_fetch\"") && Has(l, "\"replacing\":\"true\"")) replacing = true;
  }
  QCHECK(replacing);
  QCHECK(rig.net.calls == 1);  // one download of the zip, as the other fetch cases show
  QCHECK(Has(LineOf("data_step"), "\"outcome\":\"fetched\"") && Has(LineOf("data_step"), "\"root\":\"" + Media() + "\""));
  QCHECK(g_ends == 0 && g_exit_reason.empty());
}

// The hook is what makes the engine read the media root. When it did not install (a refused GOT slot), the engine still
// reads the legacy root, so the data under the media root does not count and the grant is asked for.
void WithoutTheDataRootHookTheMediaRootIsNotTheEngineRoot() {
  Rig rig;
  DataIn(Media());
  obb::ResolveRoot(&FakeExists);  // resolved, but the hook was never armed (Install did not take it)
  rig.BootRequest(&DeniedCheck);
  QCHECK(g_ends == 1 && g_exit_reason == "no_storage_grant");
  QCHECK(Has(LineOf("data_step_start"), "\"engine_root\":\"" + Legacy() + "\""));
}

// The predicate the permission-wait hook asks before it requests the storage permissions (#1737 C2): storage is needed
// exactly when the engine reads the legacy (shared storage) root.
void StorageNeededFollowsTheEngineRoot() {
  {
    Rig rig;  // the media root holds the data and the data-root hook is armed: the engine reads media, no storage needed
    NoObbFileAndTheMediaRootHoldsTheData();
    QCHECK(!dw::StorageNeeded());
  }
  {
    Rig rig;  // the legacy root is the engine's root
    DataIn(Legacy());
    QCHECK(dw::StorageNeeded());
  }
  {
    Rig rig;  // the data-root hook was never armed: the engine reads the legacy root, whatever is in media
    DataIn(Media());
    obb::ResolveRoot(&FakeExists);
    QCHECK(dw::StorageNeeded());
  }
}

// ---- the step runs once across both paths ----

// Mic denied: the game checks, sees "denied", asks (RequestPermissions) and the step runs there. A later CheckPermission
// for the microphone must not run it again.
void TheStepRunsOnceWhenTheMicrophoneIsDeniedAndAnotherCheckFollows() {
  Rig rig;
  dw::Install(rig.options);
  pw::ArmForTest(&GrantedCheck, 5, 100);
  *pw::CheckThunk::OriginalOut() = reinterpret_cast<void*>(&GameCheck);
  g_micGranted = false;
  QCHECK(!CheckEntry()("android.permission.RECORD_AUDIO"));  // denied: the request path will run the step
  QCHECK(rig.net.calls == 0 && g_shown.empty());
  rig.LaterBootRequest();                                    // the game's request: the step runs here
  QCHECK(rig.net.calls == 1 && g_shown.size() == 1);
  g_micGranted = true;
  QCHECK(CheckEntry()("android.permission.RECORD_AUDIO"));   // a later microphone check
  rig.LaterBootRequest();                                    // and a later request
  QCHECK(rig.net.calls == 1 && g_shown.size() == 1 && g_ends == 0);
  QCHECK(g_requests == 2);
}

// Mic granted: the step runs at the check. A request that follows anyway (a script node) must not run it again.
void TheStepRunsOnceWhenTheMicrophoneIsGrantedAndARequestFollows() {
  Rig rig;
  QCHECK(rig.MicCheck());
  QCHECK(rig.net.calls == 1 && g_shown.size() == 1);
  rig.LaterBootRequest();
  QCHECK(rig.net.calls == 1 && g_shown.size() == 1 && g_ends == 0);
}

// ---- a release build never fetches ----

void AReleaseBuildWithoutDataShowsTheMissingMessageHoldsNothingAndEndsNonzero() {
  Rig rig;
  rig.options.allow_fetch = false;
  rig.options.hold_ms = 0;  // entry.cpp: a release build holds nothing while no presenter is known to draw
  rig.BootRequest();
  QCHECK(rig.net.calls == 0);                       // no network at all
  QCHECK(!Exists(rig.options.fetch_root_override));  // and nothing written
  QCHECK(g_ends == 1 && g_exit_code != 0 && g_exit_reason == "game_data_missing");
  QCHECK(g_shown.size() == 1 && g_shown[0] == nevr_quest::data::kGameDataMissing);
  // shown once, then the process ends at once: no hold, no second show.
  const std::vector<std::string> want = {"show", "end"};
  std::vector<std::string> got;
  for (const std::string& e : g_log) {
    if (e == "sleep:7" || e == "show" || e == "end") got.push_back(e);
  }
  QCHECK(got == want);
  QCHECK(Has(LineOf("data_step"), "\"outcome\":\"missing\""));
  QCHECK(Has(LineOf("data_check"), "\"root\":\"" + Legacy() + "\""));
}

void AReleaseBuildWithoutDataEndsTheSameOnTheMicGrantedPath() {
  Rig rig;
  rig.options.allow_fetch = false;
  rig.options.hold_ms = 0;
  QCHECK(rig.MicCheck());
  QCHECK(rig.net.calls == 0 && g_requests == 0);
  QCHECK(g_ends == 1 && g_exit_reason == "game_data_missing");
  QCHECK(g_shown.size() == 1 && g_shown[0] == nevr_quest::data::kGameDataMissing);
}

void AReleaseBuildWithoutTheStorageGrantShowsTheStorageMessageHoldsNothingAndEndsNonzero() {
  Rig rig;
  rig.options.allow_fetch = false;
  rig.options.hold_ms = 0;
  rig.BootRequest(&DeniedCheck);
  QCHECK(rig.net.calls == 0 && !Exists(rig.options.fetch_root_override));
  QCHECK(g_ends == 1 && g_exit_code != 0 && g_exit_reason == "no_storage_grant");
  QCHECK(g_shown.size() == 1 && g_shown[0] == nevr_quest::data::kNeedsStoragePermission);
  for (const std::string& e : g_log) QCHECK(e.rfind("sleep:", 0) != 0);  // nothing is held and nothing is fetched
}

void AReleaseBuildWithDataUsesItAndSaysNothing() {
  Rig rig;
  rig.options.allow_fetch = false;
  DataIn(Legacy());
  rig.BootRequest();
  QCHECK(rig.net.calls == 0 && g_ends == 0 && g_shown.empty());
  QCHECK(g_requests == 1);
  QCHECK(Has(LineOf("data_check"), "\"action\":\"use\""));
}

void AReleaseBuildWithAnObbServingTheMediaRootLooksThereAndWritesNothing() {
  Rig rig;
  rig.options.allow_fetch = false;
  ObbSkipsTheMountServing(nevr_quest::data_root::kMediaRoot);
  rig.BootRequest(&DeniedCheck);  // the media root needs no Storage permission
  QCHECK(rig.net.calls == 0 && !Exists(rig.options.fetch_root_override));
  QCHECK(g_ends == 1 && g_exit_reason == "game_data_missing");
  QCHECK(Has(LineOf("data_check"), "\"root\":\"" + Media() + "\""));
}

// ---- the bundled content pack, after the data step (ADR 0029) ----

const std::string kPackManifestBytes = "PACK-MANIFEST-" + Pattern(4000);
const std::string kPackPackageBytes = "PACK-PACKAGE-" + Pattern(30000);
// The manifest the data step's own fetch writes (GoodZip), and the stock manifest the pack below names.
const std::string kStockManifestBytes = Pattern(20000);
constexpr const char* kAndroidRel = "_data/5932408047/rad15/android";

// An APK (a zip with hundreds of entries in life, a hundred here) carrying a pack that requires `stock_sha256`.
std::string PackApk(bool with_pack, const std::string& stock_sha256, const std::string& package = kPackPackageBytes) {
  std::vector<SrcEntry> entries;
  entries.emplace_back("AndroidManifest.xml", "xml");
  for (int i = 0; i < 100; ++i) entries.emplace_back("res/f" + std::to_string(i), "r" + std::to_string(i));
  if (with_pack) {
    const std::string hash = nevr_quest::data::Sha256Hex(kPackPackageBytes.data(), kPackPackageBytes.size());  // what pack.json says
    nlohmann::json files = nlohmann::json::array();
    files.push_back({{"path", "packages/48037dc70b0ecab2_1"}, {"sha256", hash}, {"size", kPackPackageBytes.size()}});
    files.push_back({{"path", "manifests/48037dc70b0ecab2"},
                     {"sha256", nevr_quest::data::Sha256Hex(kPackManifestBytes.data(), kPackManifestBytes.size())},
                     {"size", kPackManifestBytes.size()}});
    nlohmann::json pack = {{"schema", 1},
                           {"id", "combat-pack"},
                           {"version", "1.0.0"},
                           {"stock_data_version", "5932408047"},
                           {"requires_stock_manifest_sha256", nlohmann::json::array({stock_sha256})},
                           {"files", files},
                           {"source_zip_sha256", std::string(64, 'b')},
                           {"source_zip_size", 99}};
    entries.emplace_back("assets/nevr-pack/pack.json", pack.dump());
    entries.emplace_back("assets/nevr-pack/files/packages/48037dc70b0ecab2_1", package, 0);
    entries.emplace_back("assets/nevr-pack/files/manifests/48037dc70b0ecab2", kPackManifestBytes, 0);
  }
  return BuildZip(entries);
}

std::string StockSha() { return nevr_quest::data::Sha256Hex(kStockManifestBytes.data(), kStockManifestBytes.size()); }

// The data already there (the data step uses it) as real files under the rig's root, and the APK in the scratch directory.
void PackRig(Rig& rig, bool with_pack = true, const std::string& manifest = kStockManifestBytes) {
  const std::string root = rig.options.fetch_root_override;
  ::mkdir(root.c_str(), 0755);
  nevr_quest::data::MakeDirsBelow(root, std::string(kAndroidRel) + "/manifests");
  nevr_quest::data::MakeDirsBelow(root, std::string(kAndroidRel) + "/packages");
  WriteFile(root + "/" + kManifestRel, manifest);
  WriteFile(rig.scratch.Path("game.apk"), PackApk(with_pack, StockSha()));
  rig.options.apk_path = rig.scratch.Path("game.apk");
  DataIn(Legacy());
}
std::string PackManifestPath(const Rig& rig) { return rig.options.fetch_root_override + "/" + kManifestRel; }
std::string PackPackagePath(const Rig& rig) {
  return rig.options.fetch_root_override + "/" + kAndroidRel + "/packages/48037dc70b0ecab2_1";
}

void ThePackIsPlacedAfterTheDataStepUsedTheDataAndTheGameContinues() {
  Rig rig;
  PackRig(rig);
  rig.BootRequest();
  QCHECK(g_requests == 1 && g_ends == 0 && g_shown.empty() && rig.net.calls == 0);
  QCHECK(ReadFile(PackManifestPath(rig)) == kPackManifestBytes);
  QCHECK(ReadFile(PackPackagePath(rig)) == kPackPackageBytes);
  QCHECK(ReadFile(PackManifestPath(rig) + ".stock") == kStockManifestBytes);
  const std::string check = LineOf("pack_check");
  QCHECK(Has(check, "\"outcome\":\"place\"") && Has(check, "\"id\":\"combat-pack\"") && Has(check, "\"level\":\"info\""));
  QCHECK(Has(LineOf("pack_place"), "\"path\":\"packages/48037dc70b0ecab2_1\"") && Has(LineOf("pack_place"), "\"result\":\"placed\""));
  const std::string step = LineOf("pack_step");
  QCHECK(Has(step, "\"outcome\":\"placed\"") && Has(step, "\"duration_ms\":") && Has(step, "\"version\":\"1.0.0\""));
  QCHECK(Has(step, "\"root\":\"" + rig.options.fetch_root_override + "\""));
  QCHECK(Has(LineOf("data_version"), nevr_quest::data::Sha256Hex(kPackManifestBytes.data(), kPackManifestBytes.size())));
  // The data step's own lines come first: the pack goes over data the step found.
  std::size_t at_check = std::string::npos, at_pack = std::string::npos;
  for (std::size_t i = 0; i < g_lines.size(); ++i) {
    if (Has(g_lines[i], "\"event\":\"data_step\"") && at_check == std::string::npos) at_check = i;
    if (Has(g_lines[i], "\"event\":\"pack_check\"") && at_pack == std::string::npos) at_pack = i;
  }
  QCHECK(at_check != std::string::npos && at_pack != std::string::npos && at_check < at_pack);
}

void ThePackIsPlacedOverDataTheDataStepFetched() {
  Rig rig;  // a development build, no data: the step fetches (a manifest of Pattern(20000)) and the pack goes over it
  rig.options.apk_path = rig.scratch.Path("game.apk");
  WriteFile(rig.options.apk_path, PackApk(true, StockSha()));
  rig.BootRequest();
  QCHECK(rig.net.calls == 1 && g_ends == 0);
  QCHECK(Has(LineOf("data_step"), "\"outcome\":\"fetched\""));
  QCHECK(ReadFile(PackManifestPath(rig)) == kPackManifestBytes);
  QCHECK(ReadFile(PackPackagePath(rig)) == kPackPackageBytes);
  QCHECK(ReadFile(PackManifestPath(rig) + ".stock") == kStockManifestBytes);
  QCHECK(Has(LineOf("pack_step"), "\"outcome\":\"placed\""));
  // `data_version` is reported after the fetch and again after the pack: the last one is the pack's manifest.
  std::string last;
  int versions = 0;
  for (const std::string& l : g_lines) {
    if (Has(l, "\"event\":\"data_version\"")) {
      last = l;
      ++versions;
    }
  }
  QCHECK(versions == 2 && Has(last, nevr_quest::data::Sha256Hex(kPackManifestBytes.data(), kPackManifestBytes.size())));
}

void AReleaseBuildPlacesThePackToo() {
  Rig rig;
  rig.options.allow_fetch = false;  // entry.cpp: a release build only looks at the data; the variant APK is a release build
  rig.options.hold_ms = 0;
  PackRig(rig);
  rig.BootRequest();
  QCHECK(g_ends == 0 && rig.net.calls == 0);
  QCHECK(ReadFile(PackManifestPath(rig)) == kPackManifestBytes && ReadFile(PackPackagePath(rig)) == kPackPackageBytes);
  QCHECK(Has(LineOf("pack_step"), "\"outcome\":\"placed\""));
}

void AnApkWithNoPackLogsOneLineAndChangesNothing() {
  for (const bool release : {false, true}) {
    Rig rig;
    rig.options.allow_fetch = !release;
    if (release) rig.options.hold_ms = 0;
    PackRig(rig, /*with_pack=*/false);
    rig.BootRequest();
    QCHECK(g_ends == 0 && g_shown.empty());
    int pack_lines = 0;
    for (const std::string& l : g_lines) pack_lines += Has(l, "\"event\":\"pack_") ? 1 : 0;
    QCHECK(pack_lines == 1);  // one pack_check, no pack_step
    QCHECK(Has(LineOf("pack_check"), "\"outcome\":\"none\"") && Has(LineOf("pack_check"), "\"level\":\"info\""));
    QCHECK(ReadFile(PackManifestPath(rig)) == kStockManifestBytes);
    QCHECK(!Exists(PackPackagePath(rig)) && !Exists(PackManifestPath(rig) + ".stock"));
  }
}

void WithoutAnApkPathThePackStepSaysSoAndTheGameContinues() {
  Rig rig;
  PackRig(rig);
  rig.options.apk_path.clear();  // the host test executable is not an APK: what dladdr names gives no APK
  rig.BootRequest();
  QCHECK(g_ends == 0);
  QCHECK(Has(LineOf("pack_check"), "\"outcome\":\"none\"") && Has(LineOf("pack_check"), "\"reason\":\"apk_path_unknown\""));
  QCHECK(ReadFile(PackManifestPath(rig)) == kStockManifestBytes);
}

void AStockManifestThePackDoesNotNameIsLeftAloneAndTheGameContinues() {
  Rig rig;
  PackRig(rig, true, "A-MANIFEST-OF-ANOTHER-BUILD");
  rig.BootRequest();
  QCHECK(g_ends == 0 && g_shown.empty());
  QCHECK(ReadFile(PackManifestPath(rig)) == "A-MANIFEST-OF-ANOTHER-BUILD");
  QCHECK(!Exists(PackPackagePath(rig)));
  const std::string check = LineOf("pack_check");
  QCHECK(Has(check, "\"outcome\":\"stock_mismatch\"") && Has(check, "\"level\":\"warn\""));
  QCHECK(Has(check, nevr_quest::data::Sha256Hex("A-MANIFEST-OF-ANOTHER-BUILD", 27)));
  QCHECK(Has(check, StockSha()));
}

void ASecondLaunchOverAPlacedPackIsCurrent() {
  Rig first;
  PackRig(first);
  first.BootRequest();
  QCHECK(ReadFile(PackManifestPath(first)) == kPackManifestBytes);
  Rig second;
  second.options.fetch_root_override = first.options.fetch_root_override;
  second.options.apk_path = first.options.apk_path;
  DataIn(Legacy());
  second.BootRequest();
  QCHECK(g_ends == 0);
  QCHECK(Has(LineOf("pack_check"), "\"outcome\":\"current\"") && Has(LineOf("pack_step"), "\"outcome\":\"current\""));
  QCHECK(LineOf("data_version").empty());
}

void NoSpaceForThePackShowsTheMessageHoldsItAndEndsTheProcessWithReasonPackFailed() {
  Rig rig;
  PackRig(rig);
  rig.options.pack_available_bytes = [](const std::string&) { return std::uint64_t{1} * 1024 * 1024; };
  rig.BootRequest();
  QCHECK(g_ends == 1 && g_exit_code != 0 && g_exit_reason == "pack_failed");
  QCHECK(g_shown.size() == 2 && g_shown[0] == g_shown[1]);  // shown, held, shown again, held
  QCHECK(g_shown[0].find("1 MiB free") != std::string::npos && g_shown[0].find("MiB needed") != std::string::npos);
  const std::vector<std::string> want = {"show", "sleep:7", "show", "sleep:7", "end"};
  std::vector<std::string> got;
  for (const std::string& e : g_log) {
    if (e == "sleep:7" || e == "show" || e == "end") got.push_back(e);
  }
  QCHECK(got == want);
  QCHECK(ReadFile(PackManifestPath(rig)) == kStockManifestBytes && !Exists(PackPackagePath(rig)));
  const std::string step = LineOf("pack_step");
  QCHECK(Has(step, "\"outcome\":\"failed\"") && Has(step, "\"status\":\"no_space\"") && Has(step, "\"level\":\"error\""));
}

void APackWhoseBytesAreNotItsDigestEndsTheProcessAndWritesNothing() {
  Rig rig;
  PackRig(rig);
  // The APK carries a package that is not the one pack.json hashes (same size).
  std::string tampered = kPackPackageBytes;
  tampered[100] = static_cast<char>(tampered[100] ^ 0x20);
  WriteFile(rig.options.apk_path, PackApk(true, StockSha(), tampered));
  rig.BootRequest();
  QCHECK(g_ends == 1 && g_exit_reason == "pack_failed");
  QCHECK(!Exists(PackPackagePath(rig)) && ReadFile(PackManifestPath(rig)) == kStockManifestBytes);
  QCHECK(Has(LineOf("pack_step"), "\"status\":\"hash_mismatch\""));
}

void AKillBetweenThePackageAndTheManifestIsFinishedByTheNextLaunch() {
  Rig first;
  PackRig(first);
  first.options.pack_probe = [&first](const char* point, const std::string& path) {
    return !(std::string(point) == "tmp_written" && path == PackManifestPath(first));
  };
  first.BootRequest();
  QCHECK(g_ends == 1 && g_exit_reason == "pack_failed");  // the seam's stop is a failed step; a real kill ends nothing
  QCHECK(ReadFile(PackPackagePath(first)) == kPackPackageBytes && ReadFile(PackManifestPath(first)) == kStockManifestBytes);
  const std::string tmp = PackManifestPath(first) + nevr_quest::data::kPackTmpSuffix;
  QCHECK(Exists(tmp) && nevr_quest::data::IsOwned(tmp));

  Rig second;  // the next launch, over the same root and APK
  second.options.fetch_root_override = first.options.fetch_root_override;
  second.options.apk_path = first.options.apk_path;
  DataIn(Legacy());
  second.BootRequest();
  QCHECK(g_ends == 0);
  QCHECK(!Exists(tmp) && !Exists(nevr_quest::data::OwnerMarkerPath(tmp)));
  QCHECK(ReadFile(PackManifestPath(second)) == kPackManifestBytes && ReadFile(PackManifestPath(second) + ".stock") == kStockManifestBytes);
  QCHECK(Has(LineOf("data_recover"), "\"cleaned\":\"1\""));
  QCHECK(Has(LineOf("pack_step"), "\"outcome\":\"placed\""));
}

void NoPackStepRunsWhenTheDataStepFailedOrWasMissingOrTheStorageMessageEndedTheProcess() {
  {
    Rig rig(false);  // a development build, no network: the fetch fails
    PackRig(rig);
    g_present.clear();  // and no data is there
    rig.BootRequest();
    QCHECK(g_ends == 1 && g_exit_reason == "data_step_failed");
    QCHECK(LineOf("pack_check").empty());
    QCHECK(ReadFile(PackManifestPath(rig)) == kStockManifestBytes && !Exists(PackPackagePath(rig)));
  }
  {
    Rig rig;  // a release build with no data
    rig.options.allow_fetch = false;
    rig.options.hold_ms = 0;
    PackRig(rig);
    g_present.clear();
    rig.BootRequest();
    QCHECK(g_ends == 1 && g_exit_reason == "game_data_missing");
    QCHECK(LineOf("pack_check").empty() && !Exists(PackPackagePath(rig)));
  }
  {
    Rig rig;  // no storage grant, the engine reads the legacy root
    PackRig(rig);
    rig.BootRequest(&DeniedCheck);
    QCHECK(g_ends == 1 && g_exit_reason == "no_storage_grant");
    QCHECK(LineOf("pack_check").empty() && !Exists(PackPackagePath(rig)));
  }
}

// ---- the production exists check (obb_skip::PathExists), over two launches ----

std::string g_map_root;  // where the legacy root (the engine's root without an OBB file) lives in this test

// The data roots are fixed absolute paths: this rewrites the legacy root's prefix to a scratch directory and then asks
// the production check about the real file. Nothing else is stood in for.
bool MappedPathExists(const char* path) noexcept {
  const std::string legacy = nevr_quest::data_root::kLegacyRoot;
  const std::string p = path;
  if (p.compare(0, legacy.size(), legacy) != 0) return false;  // the media root: nothing there
  return nevr_quest::obb_skip::PathExists((g_map_root + p.substr(legacy.size())).c_str());
}

void ThePathCheckAcceptsDirectoriesAndNonEmptyFilesOnly() {
  Scratch scratch;
  const std::string dir = scratch.Path("d");
  ::mkdir(dir.c_str(), 0755);
  WriteFile(scratch.Path("full"), "manifest");
  WriteFile(scratch.Path("empty"), "");
  QCHECK(nevr_quest::obb_skip::PathExists(dir.c_str()));
  QCHECK(nevr_quest::obb_skip::PathExists(scratch.Path("full").c_str()));  // the manifest is a regular file
  QCHECK(!nevr_quest::obb_skip::PathExists(scratch.Path("empty").c_str()));
  QCHECK(!nevr_quest::obb_skip::PathExists(scratch.Path("missing").c_str()));
  QCHECK(!nevr_quest::obb_skip::DirectoryExists(scratch.Path("full").c_str()));  // why the old default failed
}

void ADefaultExistsSeesTheFetchedDataOnTheNextLaunchAndFetchesNothingAgain() {
  Rig first;
  g_map_root = first.options.fetch_root_override;
  first.options.exists = &MappedPathExists;
  first.BootRequest();
  QCHECK(first.net.calls == 1);
  QCHECK(Exists(g_map_root + "/" + kManifestRel));
  QCHECK(g_ends == 0);

  // Launch 2 and 3: the data is there as the real files the fetch wrote, so the step uses it.
  for (int launch = 2; launch <= 3; ++launch) {
    Rig again;
    again.options.fetch_root_override = g_map_root;
    again.options.exists = &MappedPathExists;
    again.BootRequest();
    QCHECK(again.net.calls == 0);
    QCHECK(g_ends == 0 && g_shown.empty());
  }
  // Nothing was set aside: the root holds `_data` and no `_data.replaced-*`.
  QCHECK(Exists(g_map_root + "/_data"));
  int aside = 0;
  if (DIR* d = ::opendir(g_map_root.c_str())) {
    while (const dirent* e = ::readdir(d)) aside += std::strncmp(e->d_name, "_data.replaced-", 15) == 0 ? 1 : 0;
    ::closedir(d);
  }
  QCHECK(aside == 0);
}

void TheNewProbeKindsAreNotReadInAShippedBuildOrWithoutTheGrant() {
  ResetProbeSeams();
  for (const bool devBuild : {false, true}) {
    Rig rig;
    DataIn(Legacy());
    rig.options.probe_enabled = devBuild;
    rig.options.show_ui = &FakeShowUi;
    rig.options.show_dialog = &FakeShowDialog;
    WriteFile(rig.options.probe_path, "ui-toast x\n");
    rig.BootRequest(devBuild ? &DeniedCheck : &GrantedCheck);  // shipped with the grant, or a development build without it
    QCHECK(g_ui_shown.empty() && g_dialog_shown.empty());
    QCHECK(Exists(rig.options.probe_path) && !Exists(rig.options.probe_path + ".done"));
  }
}

void TheProbeIsNotReadInAShippedBuild() {
  Rig rig;
  DataIn(Legacy());
  rig.options.probe_enabled = false;
  WriteFile(rig.options.probe_path, "toast probe text 1\n");
  rig.BootRequest();
  QCHECK(g_shown.empty());
  QCHECK(Exists(rig.options.probe_path) && !Exists(rig.options.probe_path + ".done"));
}

// The probe reads shared storage's neighbour: it waits for the grant like the legacy root does.
void TheProbeIsNotReadWithoutTheGrant() {
  Rig rig;
  DataIn(Legacy());
  rig.options.probe_enabled = true;
  WriteFile(rig.options.probe_path, "toast probe text 1\n");
  rig.BootRequest(&DeniedCheck);
  QCHECK(g_shown.size() == 2 && g_shown[0] == nevr_quest::data::kNeedsStoragePermission);  // the Storage message only
  QCHECK(Exists(rig.options.probe_path) && !Exists(rig.options.probe_path + ".done"));
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
  AFetchPlacesTheDataInTheRootTheEngineReads();
  AFetchWithAnObbServingTheMediaRootTargetsTheMediaRoot();
  DataInTheMediaRootDoesNotCountWhenTheEngineReadsTheLegacyRoot();
  AFailedFetchShowsTheMessageHoldsItAndEndsTheProcess();
  ARequestThatIsNotTheBootRequestRunsNothing();
  AToastProbeShowsItsTextOnceAndIsConsumed();
  TheProbeLineParsesIntoItsKindAndArgument();
  AUiToastProbeGoesToTheUiPresenterOnceAndSaysWhichThreadAndLooper();
  TheProbeSaysWhenItRanOffTheMainThreadAndWhenALooperWasPrepared();
  AUiPostProbeGoesToThePostPresenterAndIsInfoWhenPosted();
  AFailedUiToastIsAWarningWithItsToken();
  ADialogProbeGoesToTheDialogPresenterAndWithoutOneSaysNoPresenter();
  AUiToastProbeWithoutAPresenterOrWithoutTextShowsNothing();
  TheToastProbeStillUsesTheFloorPresenterAndNotTheUiOne();
  TheNewProbeKindsAreNotReadInAShippedBuildOrWithoutTheGrant();
  ABlockProbeHoldsTheThreadForItsSeconds();
  ABlockProbeIsCapped();
  StorageNotGrantedWithoutAnObbShowsTheStorageMessageAndWritesNothing();
  StorageNotGrantedOnTheMicGrantedPathShowsTheStorageMessageAndWritesNothing();
  StorageNotGrantedWithAnObbServingTheMediaRootStillUsesItsData();
  StorageNotGrantedWithAnObbServingTheMediaRootFetchesIntoItAndSucceeds();
  StorageNotGrantedWithAnUnwritableMediaRootSaysHowToAllowStorageAndEndsNonzero();
  DataOnlyInTheMediaRootIsUsedWithNoObbFileAndNoStorageGrant();
  DataOnlyInTheMediaRootIsUsedWithNoObbFileWhenStorageIsGranted();
  AReleaseBuildFindsTheDataInTheMediaRootWithNoObbFileAndSaysNothing();
  DataOnlyInTheLegacyRootStillNeedsTheGrantWithNoObbFile();
  WithoutTheDataRootHookTheMediaRootIsNotTheEngineRoot();
  StorageNeededFollowsTheEngineRoot();
  StaleMediaDataAndCompleteLegacyDataKeepsTheEngineOnTheLegacyRoot();
  CompleteMediaDataIsTheEnginesRootEvenWhenLegacyDataIsCompleteToo();
  NeitherRootCompleteIsTheEnginesMediaRoot();
  AGrantedUnwritableRootEndsWithTheStepsOwnReason();
  TheStepRunsOnceWhenTheMicrophoneIsDeniedAndAnotherCheckFollows();
  TheStepRunsOnceWhenTheMicrophoneIsGrantedAndARequestFollows();
  AReleaseBuildWithoutDataShowsTheMissingMessageHoldsNothingAndEndsNonzero();
  AReleaseBuildWithoutDataEndsTheSameOnTheMicGrantedPath();
  AReleaseBuildWithoutTheStorageGrantShowsTheStorageMessageHoldsNothingAndEndsNonzero();
  AReleaseBuildWithDataUsesItAndSaysNothing();
  AReleaseBuildWithAnObbServingTheMediaRootLooksThereAndWritesNothing();
  ThePackIsPlacedAfterTheDataStepUsedTheDataAndTheGameContinues();
  ThePackIsPlacedOverDataTheDataStepFetched();
  AReleaseBuildPlacesThePackToo();
  AnApkWithNoPackLogsOneLineAndChangesNothing();
  WithoutAnApkPathThePackStepSaysSoAndTheGameContinues();
  AStockManifestThePackDoesNotNameIsLeftAloneAndTheGameContinues();
  ASecondLaunchOverAPlacedPackIsCurrent();
  NoSpaceForThePackShowsTheMessageHoldsItAndEndsTheProcessWithReasonPackFailed();
  APackWhoseBytesAreNotItsDigestEndsTheProcessAndWritesNothing();
  AKillBetweenThePackageAndTheManifestIsFinishedByTheNextLaunch();
  NoPackStepRunsWhenTheDataStepFailedOrWasMissingOrTheStorageMessageEndedTheProcess();
  ThePathCheckAcceptsDirectoriesAndNonEmptyFilesOnly();
  ADefaultExistsSeesTheFetchedDataOnTheNextLaunchAndFetchesNothingAgain();
  TheProbeIsNotReadInAShippedBuild();
  TheProbeIsNotReadWithoutTheGrant();
  TheRetryWaitUsesTheFetchsDefaultBackoffInSlices();
  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "data_wiring_test: %d check(s) failed\n", quest_test::Failures());
    return 1;
  }
  std::printf("data_wiring_test: all checks pass\n");
  return 0;
}
