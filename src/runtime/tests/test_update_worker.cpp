// The runtime's half of PC self-update (docs/adr/0024-pc-self-update.md, sections 3, 6 and 8; #1200 work item 5).
//
// What is driven for real: the decisions (update_policy), the check and the confirmation (update_run: real files in a
// temp nevr root, the real update\lock), and the worker's threads (update_worker). What is faked: the core (its C ABI is
// the `Core` struct), the Authenticode verifier and the clock. #1200's falsifier lines this item owns:
//   * a staged file that fails its signature is loaded                  -> VerifyFailureStagesNothing, a file the verifier refuses is never published
//   * a download cut off at any byte is installed                       -> CutOffDownloadPublishesNothing (the core's failure publishes nothing)
//   * a run writes a file under the game folder                         -> every test lists the root after the run: only the update folder
// and ADR 0024's "Tests, failing first", item 3 (the worker is not started for a dev build, with the flag, with the config key
// off, or without the host capability; UpdateConfirm confirms the matching sha256 only and only after 10 s of frames).
// Every decision is one line, and every test asserts the line (the house logging rule).

#include <gtest/gtest.h>

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "runtime/lifecycle/update_policy.h"
#include "runtime/lifecycle/update_run.h"
#include "runtime/lifecycle/update_worker.h"

using nlohmann::json;
using namespace nevr_update;

namespace {

constexpr char kOldSha[] = "1111111111111111111111111111111111111111111111111111111111111111";
constexpr char kNewSha[] = "2222222222222222222222222222222222222222222222222222222222222222";

// A nevr root in a temp folder, removed with the fixture.
class Root {
 public:
  explicit Root(const std::wstring& suffix = L"") {
    wchar_t temp[MAX_PATH];
    GetTempPathW(MAX_PATH, temp);
    static std::atomic<int> n{0};
    path_ = std::wstring(temp) + L"nevr-update-test-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
            std::to_wstring(n.fetch_add(1)) + suffix;
    CreateDirectoryW(path_.c_str(), nullptr);
  }
  ~Root() { Remove(path_); }
  const std::wstring& path() const { return path_; }
  std::wstring File(const std::wstring& rel) const { return path_ + L"\\" + rel; }
  void Write(const std::wstring& rel, const std::string& text) const {
    const std::wstring full = File(rel);
    MakeDirs(full);
    std::ofstream out(full.c_str(), std::ios::binary | std::ios::trunc);
    out << text;
  }
  std::string Read(const std::wstring& rel) const {
    const std::wstring full = File(rel);
    std::ifstream in(full.c_str(), std::ios::binary);
    std::stringstream s;
    s << in.rdbuf();
    return s.str();
  }
  bool Exists(const std::wstring& rel) const { return GetFileAttributesW(File(rel).c_str()) != INVALID_FILE_ATTRIBUTES; }
  /// Every file and folder under the root, as a path relative to it, with forward slashes.
  std::vector<std::string> List() const {
    std::vector<std::string> out;
    Walk(path_, L"", &out);
    std::sort(out.begin(), out.end());
    return out;
  }

 private:
  static void MakeDirs(const std::wstring& file) {
    for (std::size_t i = 3; i < file.size(); ++i) {
      if (file[i] == L'\\') CreateDirectoryW(file.substr(0, i).c_str(), nullptr);
    }
  }
  static void Walk(const std::wstring& base, const std::wstring& rel, std::vector<std::string>* out) {
    WIN32_FIND_DATAW f;
    HANDLE h = FindFirstFileW((base + (rel.empty() ? L"" : L"\\" + rel) + L"\\*").c_str(), &f);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
      const std::wstring name = f.cFileName;
      if (name == L"." || name == L"..") continue;
      const std::wstring r = rel.empty() ? name : rel + L"\\" + name;
      std::string narrow(r.begin(), r.end());
      std::replace(narrow.begin(), narrow.end(), '\\', '/');
      out->push_back(narrow);
      if (f.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) Walk(base, r, out);
    } while (FindNextFileW(h, &f));
    FindClose(h);
  }
  static void Remove(const std::wstring& dir) {
    WIN32_FIND_DATAW f;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &f);
    if (h != INVALID_HANDLE_VALUE) {
      do {
        const std::wstring name = f.cFileName;
        if (name == L"." || name == L"..") continue;
        const std::wstring p = dir + L"\\" + name;
        if (f.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
          Remove(p);
        } else {
          SetFileAttributesW(p.c_str(), FILE_ATTRIBUTE_NORMAL);
          DeleteFileW(p.c_str());
        }
      } while (FindNextFileW(h, &f));
      FindClose(h);
    }
    RemoveDirectoryW(dir.c_str());
  }
  std::wstring path_;
};

struct Lines {
  std::vector<std::string> all;
  LogFn Fn() {
    return [this](int, const std::string& line) { all.push_back(line); };
  }
  /// The first line that starts with `[NEVR.UPDATE] <event>` and holds every `key=value` word in `fields`.
  std::string Find(const std::string& event, const std::vector<std::string>& fields = {}) const {
    for (const std::string& line : all) {
      if (line.rfind("[NEVR.UPDATE] " + event, 0) != 0) continue;
      if (line.size() > 14 + event.size() && line[14 + event.size()] != ' ') continue;
      bool ok = true;
      for (const std::string& f : fields) ok = ok && (" " + line + " ").find(" " + f + " ") != std::string::npos;
      if (ok) return line;
    }
    return "";
  }
  int Count(const std::string& event) const {
    int n = 0;
    for (const std::string& line : all) n += line.rfind("[NEVR.UPDATE] " + event, 0) == 0 ? 1 : 0;
    return n;
  }
  std::string Dump() const {
    std::string s;
    for (const std::string& l : all) s += l + "\n";
    return s;
  }
};

// The core as a test sees it: stage() writes the files a real stage would and answers with the result the real one gives.
struct FakeCore {
  bool open_ok = true;
  bool manifest_ok = true;
  std::string manifest_error = "could not fetch the manifest";
  bool stage_ok = true;
  std::string stage_error = "the download was cut off";
  std::string action = "staged";
  std::string version = "5.99.0";
  std::string reason;
  std::string runtime_bytes = "NEW-RUNTIME";
  int open_calls = 0, manifest_calls = 0, stage_calls = 0, end_calls = 0, close_calls = 0;
  std::string open_config, current_json, end_state, end_reason;
  const Root* root = nullptr;
  std::function<void()> during_stage;  // runs inside stage(), for a clock that moves

  Core Make() {
    Core c;
    c.open = [this](const std::string& cfg, std::string* error) {
      ++open_calls;
      open_config = cfg;
      if (!open_ok) *error = "cannot open";
      return open_ok;
    };
    c.manifest_fetch = [this](std::string* error) {
      ++manifest_calls;
      if (!manifest_ok) *error = manifest_error;
      return manifest_ok;
    };
    c.stage = [this](const std::string&, const std::string& current, std::string* result, std::string* error) {
      ++stage_calls;
      current_json = current;
      if (during_stage) during_stage();
      if (!stage_ok) {
        *error = stage_error;
        return false;
      }
      json files = json::array();
      if (action == "staged") {
        root->Write(L"update\\staging\\" + std::wstring(version.begin(), version.end()) + L"\\nevr-runtime.dll", runtime_bytes);
        files.push_back({{"path", "nevr-runtime.dll"}, {"size", runtime_bytes.size()}, {"sha256", kNewSha}});
      }
      *result = json{{"action", action}, {"version", version}, {"direction", "upgrade"}, {"reason", reason}, {"files", files}}.dump();
      return true;
    };
    c.run_end = [this](const std::string& state, const std::string& why) {
      ++end_calls;
      end_state = state;
      end_reason = why;
    };
    c.close = [this] { ++close_calls; };
    return c;
  }
};

CheckEnv MakeEnv(const Root& root, FakeCore& core, Lines& lines) {
  core.root = &root;
  CheckEnv env;
  env.root = root.path();
  env.version = "5.16.1";
  env.runtime_sha256 = kOldSha;
  env.core = core.Make();
  env.verify = [](const std::wstring& path) {
    std::ifstream in(path.c_str(), std::ios::binary);
    std::stringstream s;
    s << in.rdbuf();
    VerifyOutcome v;
    v.ok = s.str().find("BAD") == std::string::npos;
    v.reason = v.ok ? "accepted" : "Unsigned: no embedded signature";
    return v;
  };
  env.log = lines.Fn();
  static std::uint64_t clock = 1000;
  env.now_ms = [] { return clock; };
  return env;
}

}  // namespace

// ---- policy ----------------------------------------------------------------------------------------------------------

TEST(UpdatePolicy, DecideNamesTheFirstReasonInTheLogsOrder) {
  Switches s;
  s.host_can_swap = true;
  EXPECT_EQ(Decide(s), Disabled::kNone);
  s.config_enabled = true;
  EXPECT_EQ(Decide(s), Disabled::kNone) << "update.enabled: true is the default, not a switch";
  s.config_enabled = false;
  EXPECT_EQ(Decide(s), Disabled::kConfig);
  s.flag_no_update = true;
  EXPECT_EQ(Decide(s), Disabled::kFlag) << "the flag outranks the config key";
  s.dev_build = true;
  EXPECT_EQ(Decide(s), Disabled::kDevBuild) << "a developer's runtime is never replaced, whatever else is set";
  s.host_can_swap = false;
  EXPECT_EQ(Decide(s), Disabled::kDevBuild);
  Switches old_host;  // everything else on, no capability bit
  EXPECT_EQ(Decide(old_host), Disabled::kHostCannotSwap) << "F16: a host that cannot swap gets no download";
  EXPECT_STREQ(DisabledWord(Disabled::kDevBuild), "dev_build");
  EXPECT_STREQ(DisabledWord(Disabled::kFlag), "flag");
  EXPECT_STREQ(DisabledWord(Disabled::kConfig), "config");
  EXPECT_STREQ(DisabledWord(Disabled::kHostCannotSwap), "host_cannot_swap");
}

TEST(UpdatePolicy, ChannelIsTheConfigsThenTheInstallersThenTheCores) {
  Channel c = ResolveChannel("beta", "stable");
  EXPECT_EQ(c.name, "beta");
  EXPECT_EQ(c.source, ChannelSource::kConfig);
  c = ResolveChannel(std::nullopt, "stable");
  EXPECT_EQ(c.name, "stable");
  EXPECT_EQ(c.source, ChannelSource::kState);
  c = ResolveChannel(std::nullopt, std::nullopt);
  EXPECT_EQ(c.name, "");
  EXPECT_EQ(c.source, ChannelSource::kDefault);
  c = ResolveChannel("", "stable");
  EXPECT_EQ(c.source, ChannelSource::kState) << "an empty config value is absent";
  c = ResolveChannel("be ta/../x", "stable");
  EXPECT_EQ(c.source, ChannelSource::kState) << "a value that is not a channel name is never passed on";
  EXPECT_TRUE(c.config_rejected);
  EXPECT_STREQ(ChannelSourceWord(ChannelSource::kConfig), "config");
  EXPECT_STREQ(ChannelSourceWord(ChannelSource::kState), "state");
  EXPECT_STREQ(ChannelSourceWord(ChannelSource::kDefault), "default");
}

TEST(UpdatePolicy, CurrentJsonCarriesTheRunningRuntimeAndTheQuarantine) {
  const json j = json::parse(BuildCurrentJson("5.16.1", kOldSha, {{"5.17.0", kNewSha}}));
  EXPECT_EQ(j["version"], "5.16.1");
  EXPECT_EQ(j["runtime_sha256"], kOldSha);
  ASSERT_EQ(j["quarantine"].size(), 1u);
  EXPECT_EQ(j["quarantine"][0]["version"], "5.17.0");
  EXPECT_EQ(j["quarantine"][0]["runtime_sha256"], kNewSha);
}

TEST(UpdatePolicy, AStageResultOutsideTheHostsVocabularyIsNeverPublished) {
  StageResult r;
  std::string err;
  const std::string good = json{{"action", "staged"}, {"version", "5.99.0"}, {"direction", "upgrade"}, {"reason", ""},
                                {"files", json::array({{{"path", "nevr-runtime.dll"}, {"size", 11}, {"sha256", kNewSha}}})}}
                               .dump();
  ASSERT_TRUE(ParseStageResult(good, &r, &err)) << err;
  EXPECT_EQ(r.action, "staged");
  ASSERT_EQ(r.files.size(), 1u);
  EXPECT_EQ(r.files[0].sha256, kNewSha);

  auto with = [&](const char* key, const json& value) {
    json j = json::parse(good);
    if (std::string(key) == "path") j["files"].push_back({{"path", value}, {"size", 3}, {"sha256", kOldSha}});  // the runtime stays valid
    else if (std::string(key) == "sha256") j["files"][0]["sha256"] = value;
    else j[key] = value;
    return j.dump();
  };
  EXPECT_TRUE(ParseStageResult(with("path", "plugins/a.dll"), &r, &err)) << err << " (the control: the same shape with a good path)";
  EXPECT_FALSE(ParseStageResult(with("path", "../evil.dll"), &r, &err)) << "a path that leaves the folder";
  EXPECT_FALSE(ParseStageResult(with("path", "plugins\\x.dll"), &r, &err)) << "a backslash";
  EXPECT_FALSE(ParseStageResult(with("path", "C:/x.dll"), &r, &err)) << "a drive";
  EXPECT_FALSE(ParseStageResult(with("sha256", "abc"), &r, &err)) << "a hash that is not 64 hex";
  EXPECT_FALSE(ParseStageResult(with("version", "5/../9"), &r, &err)) << "a version that is a path";
  EXPECT_FALSE(ParseStageResult(with("action", "staged\n"), &r, &err));
  EXPECT_FALSE(ParseStageResult("{\"action\":\"staged\",\"version\":\"5.99.0\",\"files\":[]}", &r, &err)) << "staged with no file";
  EXPECT_FALSE(ParseStageResult("not json", &r, &err));
  EXPECT_FALSE(err.empty());
  StageResult none;
  EXPECT_TRUE(ParseStageResult(json{{"action", "none"}, {"version", "5.16.1"}, {"direction", "replace"}, {"reason", "current"},
                                    {"files", json::array()}}.dump(), &none, &err))
      << "none and blocked carry no files";
}

TEST(UpdatePolicy, StagedJsonIsTheSpellingTheHostReads) {
  StageResult r;
  r.action = "staged";
  r.version = "5.99.0";
  r.files = {{"nevr-runtime.dll", 11, kNewSha}, {"plugins/a.dll", 3, kOldSha}};
  const json j = json::parse(BuildStagedJson(r));
  EXPECT_EQ(j["version"], "5.99.0");
  ASSERT_EQ(j["files"].size(), 2u);
  EXPECT_EQ(j["files"][0]["path"], "nevr-runtime.dll");
  EXPECT_EQ(j["files"][0]["size"], 11u);
  EXPECT_EQ(j["files"][1]["path"], "plugins/a.dll");
  EXPECT_EQ(j.size(), 2u) << "version and files, nothing else";
}

TEST(UpdatePolicy, TheMarkIsTenSecondsAfterTheFirstFrame) {
  EXPECT_FALSE(ConfirmDue(5000, 5000));
  EXPECT_FALSE(ConfirmDue(5000, 14999));
  EXPECT_TRUE(ConfirmDue(5000, 15000));
  EXPECT_TRUE(ConfirmDue(5000, 99999));
  EXPECT_FALSE(ConfirmDue(5000, 4000)) << "a clock that went backwards is not due";
}

TEST(UpdatePolicy, StateReadersAndConfirmState) {
  const std::string state = json{{"channel", "beta"},
                                 {"owned", json::array({{{"path", "nevr-runtime.dll"}, {"sha256", kNewSha}}})},
                                 {"quarantine", json::array({{{"version", "5.17.0"}, {"runtime_sha256", kOldSha}}, {{"version", "x"}}})}}
                                .dump();
  EXPECT_EQ(StateChannel(state).value_or(""), "beta");
  const auto q = StateQuarantine(state);
  ASSERT_EQ(q.size(), 1u) << "an entry with no sha is skipped";
  EXPECT_EQ(q[0].version, "5.17.0");
  EXPECT_FALSE(StateChannel("{}").has_value());
  EXPECT_FALSE(StateChannel("garbage").has_value());

  std::string out;
  ASSERT_TRUE(ConfirmState(state, "5.99.0", kNewSha, &out));
  const json j = json::parse(out);
  EXPECT_EQ(j["current"]["version"], "5.99.0");
  EXPECT_EQ(j["current"]["runtime_sha256"], kNewSha);
  EXPECT_EQ(j["channel"], "beta") << "every other key is kept";
  EXPECT_EQ(j["owned"].size(), 1u);
  EXPECT_EQ(j["quarantine"].size(), 2u);
  EXPECT_FALSE(ConfirmState("[1]", "5.99.0", kNewSha, &out));

  PendingTarget t;
  ASSERT_TRUE(ParsePendingTarget(R"({"to":{"version":"5.99.0","sha256":")" + std::string(kNewSha) + R"("},"from":null,"boot_attempts":1,"failed_reason":null})", &t));
  EXPECT_EQ(t.version, "5.99.0");
  EXPECT_EQ(t.sha256, kNewSha);
  EXPECT_FALSE(ParsePendingTarget("{}", &t));
}

// ---- the check ---------------------------------------------------------------------------------------------------------

TEST(UpdateCheck, AVerifiedReleaseIsPublishedLastAndEveryDecisionIsALine) {
  Root root;
  Lines lines;
  FakeCore core;
  CheckEnv env = MakeEnv(root, core, lines);
  const CheckOutcome out = RunCheck(env);

  EXPECT_EQ(out.action, CheckAction::kStaged) << lines.Dump();
  ASSERT_TRUE(root.Exists(L"update\\staged.json"));
  const json staged = json::parse(root.Read(L"update\\staged.json"));
  EXPECT_EQ(staged["version"], "5.99.0");
  EXPECT_EQ(staged["files"][0]["path"], "nevr-runtime.dll");
  EXPECT_EQ(staged["files"][0]["sha256"], kNewSha);
  EXPECT_TRUE(root.Exists(L"update\\staging\\5.99.0\\nevr-runtime.dll")) << "the staged files stay for the host's swap";
  EXPECT_FALSE(root.Exists(L"update\\lock")) << "the lock is released (delete on close)";

  EXPECT_NE(lines.Find("update verify", {"path=nevr-runtime.dll", "ok=true"}), "") << lines.Dump();
  EXPECT_NE(lines.Find("update published", {"version=5.99.0", "files=1"}), "") << lines.Dump();
  EXPECT_NE(lines.Find("update check", {"action=staged", "target=5.99.0", "current=5.16.1", "direction=upgrade",
                                        "channel_source=default"}), "") << lines.Dump();
  EXPECT_NE(lines.Find("update check")  .find("elapsed_ms="), std::string::npos);
  // The core was driven in order and its run was ended.
  EXPECT_EQ(core.open_calls, 1);
  EXPECT_EQ(core.manifest_calls, 1);
  EXPECT_EQ(core.stage_calls, 1);
  EXPECT_EQ(core.end_calls, 1);
  EXPECT_EQ(core.close_calls, 1);
  const json cur = json::parse(core.current_json);
  EXPECT_EQ(cur["version"], "5.16.1");
  EXPECT_EQ(cur["runtime_sha256"], kOldSha);
  const json cfg = json::parse(core.open_config);
  EXPECT_EQ(cfg["platform"], "pc");
  EXPECT_EQ(cfg["nevr_root"], std::string(root.path().begin(), root.path().end())) << "an ASCII root reaches the core as it is";
  EXPECT_FALSE(cfg.contains("channel")) << "no channel anywhere: the core's default";
  // Nothing outside update\ and the core's own folders is written.
  for (const std::string& p : root.List()) {
    EXPECT_EQ(p.rfind("update", 0), 0u) << p << " is outside update\\";
  }
}

TEST(UpdateCheck, ARootWithANonAsciiLetterReachesTheCoreAsUtf8) {
  Root root(L"-Zo\u00eb");  // a profile folder named for the player
  Lines lines;
  FakeCore core;
  CheckEnv env = MakeEnv(root, core, lines);
  RunCheck(env);
  const std::string sent = json::parse(core.open_config)["nevr_root"].get<std::string>();
  EXPECT_NE(sent.find("Zo\xC3\xAB"), std::string::npos) << "the e with a diaeresis is two UTF-8 bytes: " << sent;
  EXPECT_EQ(sent.find('\xEB'), std::string::npos) << "not the low byte of the UTF-16 unit";
}

TEST(UpdateCheck, AFileTheVerifierRefusesIsNeverPublished) {
  Root root;
  Lines lines;
  FakeCore core;
  core.runtime_bytes = "BAD-RUNTIME";
  CheckEnv env = MakeEnv(root, core, lines);
  const CheckOutcome out = RunCheck(env);

  EXPECT_EQ(out.action, CheckAction::kBlocked) << lines.Dump();
  EXPECT_EQ(out.reason, "verify_failed");
  EXPECT_FALSE(root.Exists(L"update\\staged.json")) << "falsifier: a staged file that fails its signature must not be offered to the host";
  EXPECT_FALSE(root.Exists(L"update\\staging\\5.99.0")) << "staging removed";
  EXPECT_NE(lines.Find("update verify", {"path=nevr-runtime.dll", "ok=false"}), "") << lines.Dump();
  EXPECT_NE(lines.Find("update check", {"action=blocked", "reason=verify_failed"}), "") << lines.Dump();
  EXPECT_EQ(lines.Count("update published"), 0);
}

TEST(UpdateCheck, ACutOffDownloadPublishesNothing) {
  Root root;
  Lines lines;
  FakeCore core;
  core.stage_ok = false;
  CheckEnv env = MakeEnv(root, core, lines);
  const CheckOutcome out = RunCheck(env);
  EXPECT_EQ(out.action, CheckAction::kFailed);
  EXPECT_EQ(out.reason, "stage_failed");
  EXPECT_FALSE(root.Exists(L"update\\staged.json"));
  EXPECT_NE(lines.Find("update check", {"action=failed", "reason=stage_failed"}), "") << lines.Dump();
  EXPECT_EQ(core.end_calls, 1) << "the core's run is ended on a failure too";
  EXPECT_EQ(core.close_calls, 1);
  EXPECT_EQ(lines.Dump().find("cut off"), std::string::npos) << "the core's own text is not a log field (logging Rule 4)";
}

TEST(UpdateCheck, AnUnreachableManifestIsNetworkAndABadOneIsManifestCheck) {
  {
    Root root;
    Lines lines;
    FakeCore core;
    core.manifest_ok = false;
    CheckEnv env = MakeEnv(root, core, lines);
    EXPECT_EQ(RunCheck(env).reason, "network");
    EXPECT_NE(lines.Find("update check", {"action=failed", "reason=network"}), "") << lines.Dump();
    EXPECT_EQ(core.stage_calls, 0) << "no manifest, nothing is staged";
    EXPECT_EQ(core.close_calls, 1);
  }
  {
    Root root;
    Lines lines;
    FakeCore core;
    core.manifest_ok = false;
    core.manifest_error = "FAIL step 2: signature does not verify";
    CheckEnv env = MakeEnv(root, core, lines);
    EXPECT_EQ(RunCheck(env).reason, "manifest_check");
    EXPECT_NE(lines.Find("update check", {"action=failed", "reason=manifest_check"}), "") << lines.Dump();
  }
  {
    Root root;
    Lines lines;
    FakeCore core;
    core.open_ok = false;
    CheckEnv env = MakeEnv(root, core, lines);
    EXPECT_EQ(RunCheck(env).reason, "core_open_failed");
    EXPECT_EQ(core.manifest_calls, 0);
  }
}

TEST(UpdateCheck, NoneAndBlockedFromTheCoreAreLoggedAndPublishNothing) {
  for (const char* action : {"none", "blocked"}) {
    Root root;
    Lines lines;
    FakeCore core;
    core.action = action;
    core.reason = std::string(action) == "none" ? "quarantined" : "no_windows_zip";
    CheckEnv env = MakeEnv(root, core, lines);
    const CheckOutcome out = RunCheck(env);
    EXPECT_EQ(std::string(CheckActionWord(out.action)), action);
    EXPECT_EQ(out.reason, core.reason);
    EXPECT_FALSE(root.Exists(L"update\\staged.json"));
    EXPECT_NE(lines.Find("update check", {std::string("action=") + action, "reason=" + core.reason}), "") << lines.Dump();
  }
}

TEST(UpdateCheck, AReleaseWaitsWhileAnotherIsPendingAndNothingIsFetched) {
  Root root;
  root.Write(L"update\\pending.json", R"({"to":{"version":"5.98.0","sha256":")" + std::string(kNewSha) + R"("},"from":null,"boot_attempts":1,"failed_reason":null})");
  Lines lines;
  FakeCore core;
  CheckEnv env = MakeEnv(root, core, lines);
  const CheckOutcome out = RunCheck(env);
  EXPECT_EQ(out.action, CheckAction::kNone);
  EXPECT_EQ(out.reason, "release_pending");
  EXPECT_EQ(core.open_calls, 0) << "a 6.5 MB download that cannot be swapped yet never happens (ADR 0024, section 6, Q1)";
  EXPECT_NE(lines.Find("update check", {"action=none", "reason=release_pending"}), "") << lines.Dump();
}

TEST(UpdateCheck, AResultThatArrivesAfterTheBudgetIsNotPublished) {
  Root root;
  Lines lines;
  FakeCore core;
  CheckEnv env = MakeEnv(root, core, lines);
  auto clock = std::make_shared<std::uint64_t>(1000);
  env.now_ms = [clock] { return *clock; };
  core.during_stage = [clock] { *clock += 121000; };
  env.core = core.Make();
  const CheckOutcome out = RunCheck(env);
  EXPECT_EQ(out.action, CheckAction::kFailed);
  EXPECT_EQ(out.reason, "timeout");
  EXPECT_FALSE(root.Exists(L"update\\staged.json")) << "bounded: 120 s for the whole stage";
  EXPECT_EQ(lines.Count("update verify"), 0) << "a late result is not even verified";
  EXPECT_NE(lines.Find("update check", {"action=failed", "reason=timeout"}), "") << lines.Dump();
}

TEST(UpdateCheck, TheChannelAndItsSourceReachTheCoreAndTheLine) {
  Root root;
  root.Write(L"update\\state.json", R"({"channel":"stable","owned":[]})");
  Lines lines;
  FakeCore core;
  CheckEnv env = MakeEnv(root, core, lines);
  RunCheck(env);
  EXPECT_EQ(json::parse(core.open_config)["channel"], "stable");
  EXPECT_NE(lines.Find("update check", {"channel=stable", "channel_source=state"}), "") << lines.Dump();

  Lines lines2;
  FakeCore core2;
  CheckEnv env2 = MakeEnv(root, core2, lines2);
  env2.config_channel = "beta";
  RunCheck(env2);
  EXPECT_EQ(json::parse(core2.open_config)["channel"], "beta");
  EXPECT_NE(lines2.Find("update check", {"channel=beta", "channel_source=config"}), "") << lines2.Dump();
}

TEST(UpdateCheck, TheQuarantineInStateReachesTheCore) {
  Root root;
  root.Write(L"update\\state.json", json{{"quarantine", json::array({{{"version", "5.99.0"}, {"runtime_sha256", kNewSha}}})}}.dump());
  Lines lines;
  FakeCore core;
  CheckEnv env = MakeEnv(root, core, lines);
  RunCheck(env);
  const json cur = json::parse(core.current_json);
  ASSERT_EQ(cur["quarantine"].size(), 1u);
  EXPECT_EQ(cur["quarantine"][0]["version"], "5.99.0");
}

TEST(UpdateCheck, TheLockHeldByAnotherProcessDefersThePublication) {
  Root root;
  CreateDirectoryW(root.File(L"update").c_str(), nullptr);
  UpdateFileLock other;
  DWORD err = 0;
  ASSERT_TRUE(other.Acquire(root.File(L"update"), 0, 10, &err));
  Lines lines;
  FakeCore core;
  CheckEnv env = MakeEnv(root, core, lines);
  env.lock_timeout_ms = 200;
  const CheckOutcome out = RunCheck(env);
  EXPECT_EQ(out.action, CheckAction::kFailed);
  EXPECT_EQ(out.reason, "lock_unavailable");
  EXPECT_FALSE(root.Exists(L"update\\staged.json"));
  EXPECT_NE(lines.Find("update check", {"action=failed", "reason=lock_unavailable"}), "") << lines.Dump();
}

// ---- the confirmation ----------------------------------------------------------------------------------------------------

namespace {
std::string PendingJson(const char* version, const char* sha) {
  return json{{"to", {{"version", version}, {"sha256", sha}}}, {"from", {{"version", "5.16.1"}, {"sha256", kOldSha}}},
              {"boot_attempts", 1}, {"failed_reason", nullptr}}.dump();
}
}  // namespace

TEST(UpdateConfirm, TheMatchingReleaseIsConfirmedAndThePendingRecordGoes) {
  Root root;
  root.Write(L"update\\pending.json", PendingJson("5.99.0", kNewSha));
  root.Write(L"update\\crashed", "");
  root.Write(L"update\\state.json", R"({"channel":"beta","owned":[{"path":"nevr-runtime.dll","sha256":")" + std::string(kNewSha) + R"("}]})");
  Lines lines;
  ConfirmEnv env;
  env.root = root.path();
  env.runtime_sha256 = kNewSha;
  env.role = "server";
  env.log = lines.Fn();
  EXPECT_EQ(UpdateConfirm(env, 10042), ConfirmResult::kConfirmed) << lines.Dump();
  EXPECT_FALSE(root.Exists(L"update\\pending.json"));
  EXPECT_FALSE(root.Exists(L"update\\crashed"));
  const json state = json::parse(root.Read(L"update\\state.json"));
  EXPECT_EQ(state["current"]["version"], "5.99.0");
  EXPECT_EQ(state["current"]["runtime_sha256"], kNewSha);
  EXPECT_EQ(state["channel"], "beta");
  EXPECT_NE(lines.Find("update confirmed", {"version=5.99.0", "after_ms=10042", "role=server"}), "") << lines.Dump();
  EXPECT_FALSE(root.Exists(L"update\\lock"));
}

TEST(UpdateConfirm, ARuntimeThatIsNotThePendingOneConfirmsNothing) {
  Root root;
  root.Write(L"update\\pending.json", PendingJson("5.99.0", kNewSha));
  root.Write(L"update\\state.json", "{}");
  Lines lines;
  ConfirmEnv env;
  env.root = root.path();
  env.runtime_sha256 = kOldSha;  // the old runtime, e.g. a second process that still maps it
  env.log = lines.Fn();
  EXPECT_EQ(UpdateConfirm(env, 10000), ConfirmResult::kNotThePendingRelease);
  EXPECT_TRUE(root.Exists(L"update\\pending.json"));
  EXPECT_EQ(root.Read(L"update\\state.json"), "{}");
  EXPECT_EQ(lines.Count("update confirmed"), 0);
  EXPECT_NE(lines.Find("update confirm skipped", {"reason=not_the_pending_release"}), "") << lines.Dump();
}

TEST(UpdateConfirm, NoPendingRecordMeansNothingToConfirm) {
  Root root;
  Lines lines;
  ConfirmEnv env;
  env.root = root.path();
  env.runtime_sha256 = kNewSha;
  env.log = lines.Fn();
  EXPECT_EQ(UpdateConfirm(env, 10000), ConfirmResult::kNotPending);
  EXPECT_NE(lines.Find("update confirm skipped", {"reason=no_pending"}), "") << lines.Dump();
  EXPECT_TRUE(root.List().empty()) << "a run with no update folder makes none";
}

TEST(UpdateConfirm, AStateFileThatIsDamagedIsNotWrittenOver) {
  Root root;
  root.Write(L"update\\pending.json", PendingJson("5.99.0", kNewSha));
  root.Write(L"update\\state.json", "this is not json");
  Lines lines;
  ConfirmEnv env;
  env.root = root.path();
  env.runtime_sha256 = kNewSha;
  env.log = lines.Fn();
  EXPECT_EQ(UpdateConfirm(env, 10000), ConfirmResult::kStateUnreadable);
  EXPECT_TRUE(root.Exists(L"update\\pending.json")) << "the host rebuilds state; the record of the trial stays";
  EXPECT_EQ(root.Read(L"update\\state.json"), "this is not json");
  EXPECT_NE(lines.Find("update confirm skipped", {"reason=state_unreadable"}), "") << lines.Dump();
}

// ---- the worker ---------------------------------------------------------------------------------------------------------

namespace {
struct WorkerFixture {
  Root root;
  Lines lines;
  FakeCore core;
  std::atomic<std::uint64_t> clock{1000};
  Deps deps;
  WorkerFixture() {
    core.root = &root;
    deps.switches.host_can_swap = true;
    deps.root = root.path();
    deps.read_running = [](std::string* v, std::string* s, std::string*) {
      *v = "5.16.1";
      *s = kNewSha;
      return true;
    };
    deps.core = core.Make();
    deps.verify = [](const std::wstring&) { return VerifyOutcome{true, "accepted"}; };
    deps.log = lines.Fn();
    deps.now_ms = [this] { return clock.load(); };
  }
};
}  // namespace

TEST(UpdateWorker, ADevBuildNeverChecksAndSaysSo) {
  WorkerFixture f;
  f.deps.switches.dev_build = true;
  Worker w(f.deps);
  EXPECT_EQ(w.Start(), Disabled::kDevBuild);
  w.Stop(3000);  // joins the threads there are: a thread that was wrongly started has run by now
  EXPECT_EQ(f.core.open_calls, 0);
  EXPECT_NE(f.lines.Find("update disabled", {"reason=dev_build"}), "") << f.lines.Dump();
  EXPECT_TRUE(f.root.List().empty());
}

TEST(UpdateWorker, TheFlagAndTheConfigKeyStopTheCheckAndEachSaysWhich) {
  {
    WorkerFixture f;
    f.deps.switches.flag_no_update = true;
    Worker w(f.deps);
    EXPECT_EQ(w.Start(), Disabled::kFlag);
    w.Stop(3000);
    EXPECT_EQ(f.core.open_calls, 0);
    EXPECT_NE(f.lines.Find("update disabled", {"reason=flag"}), "") << f.lines.Dump();
  }
  {
    WorkerFixture f;
    f.deps.switches.config_enabled = false;
    Worker w(f.deps);
    EXPECT_EQ(w.Start(), Disabled::kConfig);
    w.Stop(3000);
    EXPECT_EQ(f.core.open_calls, 0);
    EXPECT_NE(f.lines.Find("update disabled", {"reason=config"}), "") << f.lines.Dump();
  }
}

TEST(UpdateWorker, AHostThatCannotSwapGetsNoDownloadAndNoConfirmation) {
  {
    WorkerFixture f;  // nothing pending, so a check that wrongly ran would reach the core
    f.deps.switches.host_can_swap = false;
    Worker w(f.deps);
    EXPECT_EQ(w.Start(), Disabled::kHostCannotSwap);
    w.Stop(3000);
    EXPECT_EQ(f.core.open_calls, 0) << "F16: a 6.5 MB download that nothing would apply never happens";
    EXPECT_NE(f.lines.Find("update disabled", {"reason=host_cannot_swap"}), "") << f.lines.Dump();
  }
  {
    WorkerFixture f;
    f.deps.switches.host_can_swap = false;
    f.root.Write(L"update\\pending.json", PendingJson("5.99.0", kNewSha));
    f.root.Write(L"update\\state.json", "{}");
    Worker w(f.deps);
    w.Start();
    w.OnFrame(1000);
    w.OnFrame(20000);
    w.Stop(3000);
    EXPECT_TRUE(f.root.Exists(L"update\\pending.json")) << "no capability bit, so no swap ever made this record: nothing confirms it";
    EXPECT_EQ(f.lines.Count("update confirmed"), 0);
  }
}

TEST(UpdateWorker, TheCheckRunsOffTheCallersThreadAndEndsWithItsLine) {
  WorkerFixture f;
  const DWORD caller = GetCurrentThreadId();
  auto stage_thread = std::make_shared<std::atomic<DWORD>>(0);
  f.core.during_stage = [stage_thread] { stage_thread->store(GetCurrentThreadId()); };
  f.deps.core = f.core.Make();
  Worker w(f.deps);
  EXPECT_EQ(w.Start(), Disabled::kNone);
  ASSERT_TRUE(w.WaitCheckDone(10000)) << f.lines.Dump();
  EXPECT_NE(stage_thread->load(), 0u);
  EXPECT_NE(stage_thread->load(), caller) << "the game thread never waits for the core";
  EXPECT_TRUE(f.root.Exists(L"update\\staged.json"));
  EXPECT_NE(f.lines.Find("update check", {"action=staged"}), "") << f.lines.Dump();
  w.Stop(2000);
}

TEST(UpdateWorker, AFileTheRunningRuntimeCannotBeReadAsStopsTheCheckWithAReason) {
  WorkerFixture f;
  f.deps.read_running = [](std::string*, std::string*, std::string* e) {
    *e = "cannot open";
    return false;
  };
  Worker w(f.deps);
  w.Start();
  ASSERT_TRUE(w.WaitCheckDone(5000));
  EXPECT_EQ(f.core.open_calls, 0);
  EXPECT_NE(f.lines.Find("update check", {"action=failed", "reason=running_runtime_unreadable"}), "") << f.lines.Dump();
}

TEST(UpdateWorker, TheConfirmationWaitsForTenSecondsOfFramesThenConfirms) {
  WorkerFixture f;
  f.root.Write(L"update\\pending.json", PendingJson("5.99.0", kNewSha));
  f.root.Write(L"update\\state.json", "{}");
  f.deps.switches.flag_no_update = true;  // checks off; the confirmation still runs
  Worker w(f.deps);
  EXPECT_EQ(w.Start(), Disabled::kFlag);
  w.OnFrame(50000);  // the first call fixes the start of the 10 s
  w.OnFrame(59999);
  EXPECT_FALSE(w.WaitConfirmDone(300)) << "9.999 s of frames is not the mark";
  EXPECT_TRUE(f.root.Exists(L"update\\pending.json"));
  w.OnFrame(60000);
  ASSERT_TRUE(w.WaitConfirmDone(10000)) << f.lines.Dump();
  EXPECT_FALSE(f.root.Exists(L"update\\pending.json"));
  EXPECT_NE(f.lines.Find("update confirmed", {"version=5.99.0", "role=client"}), "") << f.lines.Dump();
  w.Stop(2000);
}

TEST(UpdateWorker, ARuntimeThatIsNotThePendingOneDoesNotConfirmAtTheMark) {
  WorkerFixture f;
  f.root.Write(L"update\\pending.json", PendingJson("5.99.0", kOldSha));  // the running one is kNewSha
  f.root.Write(L"update\\state.json", "{}");
  Worker w(f.deps);
  w.Start();
  w.OnFrame(1000);
  w.OnFrame(12000);
  ASSERT_TRUE(w.WaitConfirmDone(10000));
  EXPECT_TRUE(f.root.Exists(L"update\\pending.json"));
  EXPECT_EQ(f.lines.Count("update confirmed"), 0);
  EXPECT_NE(f.lines.Find("update confirm skipped", {"reason=not_the_pending_release"}), "") << f.lines.Dump();
  w.Stop(2000);
}
