// The runtime's half of self-update (docs/adr/0024-pc-self-update.md, sections 3, 6 and 8; #1200 work item 5), on the shared engine
// (core/update: policy, engine_check, worker).
//
// What is driven for real: the decisions (policy), the check and the confirmation (engine_check: real files in a temp nevr root, the
// real update\lock through the platform's UpdateFs), and the worker's threads (worker). What is faked: the core (its C ABI is the
// `Core` struct), the verifier and the clock. #1200's falsifier lines this item owns:
//   * a staged file that fails its signature is loaded                  -> VerifyFailureStagesNothing, a file the verifier refuses is never published
//   * a download cut off at any byte is installed                       -> CutOffDownloadPublishesNothing (the core's failure publishes nothing)
//   * a run writes a file under the game folder                         -> every test lists the root after the run: only the update folder
// and ADR 0024's "Tests, failing first", item 3 (the worker is not started for a dev build, with the flag, with the config key
// off, or without the host capability; UpdateConfirm confirms the matching sha256 only and only after 10 s of frames).
// Every decision is one line, and every test asserts the line (the house logging rule).
//
// One source for two hosts: PC (the Win32 fs and folders, under Wine) and the Linux host (the POSIX fs, core/update/fs_posix.h).
// The Win32 folder, the wide conversions and the PC edge tests (the wide root and the wide verifier path) are under
// #if defined(_WIN32); every other test is the same on both hosts.

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <windows.h>

#include "runtime/lifecycle/update_fs_win32.h"
#include "runtime/lifecycle/update_run.h"
#include "runtime/tests/run_root.h"
#else
#include <unistd.h>

#include <filesystem>
#endif

#include <nlohmann/json.hpp>

#include "core/update/engine_check.h"
#include "core/update/fs.h"
#include "core/update/policy.h"
#include "core/update/worker.h"

using nlohmann::json;
using namespace nevr_update;

namespace {

constexpr char kOldSha[] = "1111111111111111111111111111111111111111111111111111111111111111";
constexpr std::uint32_t kHolderStepMs = 10;  // the other holder of update\lock retries every 10 ms
constexpr char kPluginBytes[] = "PLUGIN";
constexpr std::size_t kStagedFilesWithAPlugin = 2;  // nevr-runtime.dll and plugins/a.dll
constexpr char kNewSha[] = "2222222222222222222222222222222222222222222222222222222222222222";

// ---- the platform seams ------------------------------------------------------------------------------------------------------

// The fs under test: the Win32 one on PC, the POSIX one on the Linux host.
const UpdateFs& TestFs() noexcept {
#if defined(_WIN32)
  return Win32UpdateFs();
#else
  // Every test root is a folder of the system's temporary directory (class Root), so one POSIX fs rooted there reaches all of them.
  static const std::unique_ptr<UpdateFs> posix = MakePosixUpdateFs(std::filesystem::temp_directory_path().string());
  return *posix;
#endif
}

#if defined(_WIN32)
constexpr char kSeparator = '\\';
std::wstring WideOf(const std::string& utf8) {
  std::wstring out;
  Utf8ToWide(utf8, &out);  // the test's paths are UTF-8 and convert strictly
  return out;
}
// Every folder above a file, made once each. The first three characters are the drive ("C:\").
void MakeDirsOfWide(const std::wstring& file) {
  for (std::size_t i = 3; i < file.size(); ++i) {
    if (file[i] == L'\\') CreateDirectoryW(file.substr(0, i).c_str(), nullptr);
  }
}
void WalkWide(const std::wstring& base, const std::wstring& rel, std::vector<std::string>* out) {
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
    if (f.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) WalkWide(base, r, out);
  } while (FindNextFileW(h, &f));
  FindClose(h);
}
std::string ReadAllFile(const std::string& path) {
  std::ifstream in(WideOf(path).c_str(), std::ios::binary);
  std::stringstream s;
  s << in.rdbuf();
  return s.str();
}
#else
constexpr char kSeparator = '/';
std::string ReadAllFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  std::stringstream s;
  s << in.rdbuf();
  return s.str();
}
#endif

// A relative path in these tests is written with backslashes, as a Windows path; this is the platform's spelling of it.
std::string Native(std::string rel) {
#if defined(_WIN32)
  return rel;
#else
  std::replace(rel.begin(), rel.end(), '\\', '/');
  return rel;
#endif
}

// A nevr root in a temp folder, removed with the fixture. Paths are UTF-8.
class Root {
 public:
  // Win32: a fresh folder under NEVR_TEST_RUN_ROOT (run_root.h), which the test never removes.
  explicit Root(const char* suffix = "") {
#if defined(_WIN32)
    std::wstring label = L"nevr-update-test";
    for (const char* p = suffix; *p != '\0'; ++p) label += static_cast<wchar_t>(*p);
    const std::wstring dir = nevr_test::NewDir(label.c_str());
    if (dir.empty()) std::abort();
    RootToUtf8(dir.c_str(), &path_);
#else
    static std::atomic<int> n{0};
    path_ = (std::filesystem::temp_directory_path() /
             ("nevr-update-test-" + std::to_string(::getpid()) + "-" + std::to_string(n.fetch_add(1)) + suffix))
                .string();
    std::filesystem::create_directories(path_);
#endif
  }
  ~Root() {
#if !defined(_WIN32)
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
#endif
  }
  Root(const Root&) = delete;
  Root& operator=(const Root&) = delete;

  const std::string& path() const { return path_; }
  /// A path under the root; `rel` is written with backslashes.
  std::string File(const std::string& rel) const { return path_ + kSeparator + Native(rel); }
  void Write(const std::string& rel, const std::string& text) const {
    const std::string full = File(rel);
#if defined(_WIN32)
    const std::wstring wide = WideOf(full);
    MakeDirsOfWide(wide);
    std::ofstream out(wide.c_str(), std::ios::binary | std::ios::trunc);
#else
    std::filesystem::create_directories(std::filesystem::path(full).parent_path());
    std::ofstream out(full, std::ios::binary | std::ios::trunc);
#endif
    out << text;
  }
  std::string Read(const std::string& rel) const { return ReadAllFile(File(rel)); }
  void MakeDir(const std::string& rel) const {
#if defined(_WIN32)
    CreateDirectoryW(WideOf(File(rel)).c_str(), nullptr);
#else
    std::filesystem::create_directories(File(rel));
#endif
  }
  bool Exists(const std::string& rel) const {
#if defined(_WIN32)
    return GetFileAttributesW(WideOf(File(rel)).c_str()) != INVALID_FILE_ATTRIBUTES;
#else
    return std::filesystem::exists(File(rel));
#endif
  }
  /// Every file and folder under the root, as a path relative to it, with forward slashes, sorted.
  std::vector<std::string> List() const {
    std::vector<std::string> out;
#if defined(_WIN32)
    WalkWide(WideOf(path_), L"", &out);
#else
    for (const auto& entry : std::filesystem::recursive_directory_iterator(path_)) {
      out.push_back(std::filesystem::relative(entry.path(), path_).string());
    }
#endif
    std::sort(out.begin(), out.end());
    return out;
  }

 private:
  std::string path_;
};

// The lock is released: the same folder's lock is taken again. (Whether the lock file remains is the implementation's: Win32 deletes
// it with the last handle, POSIX keeps it; the proof is that the next holder gets it.)
void ExpectLockFree(const Root& root) {
  int err = 0;
  const std::unique_ptr<FsLock> again = TestFs().TakeLock(root.File("update"), 0, kHolderStepMs, &err);
  EXPECT_NE(again, nullptr) << "the lock is released (error " << err << ")";
}

// The lines the worker's threads write. The check thread, the confirmation thread and the test's own thread all use it, so every
// access is under one mutex: the log function is called from several threads (worker.h, Deps::log).
struct Lines {
  LogFn Fn() {
    return [this](int level, const std::string& line) {
      std::lock_guard<std::mutex> lock(mu_);
      all_.push_back(line);
      levels_.push_back(level);
    };
  }
  /// The level of the line Find would return, or -1 when there is none.
  int LevelOf(const std::string& event, const std::vector<std::string>& fields = {}) const {
    std::lock_guard<std::mutex> lock(mu_);
    const std::string line = FindLocked(event, fields);
    for (std::size_t i = 0; i < all_.size(); ++i) {
      if (all_[i] == line) return levels_[i];
    }
    return -1;
  }
  /// The first line that starts with `[NEVR.UPDATE] <event>` and holds every `key=value` word in `fields`.
  std::string Find(const std::string& event, const std::vector<std::string>& fields = {}) const {
    std::lock_guard<std::mutex> lock(mu_);
    return FindLocked(event, fields);
  }
  int Count(const std::string& event) const {
    std::lock_guard<std::mutex> lock(mu_);
    int n = 0;
    for (const std::string& line : all_) n += line.rfind("[NEVR.UPDATE] " + event, 0) == 0 ? 1 : 0;
    return n;
  }
  std::string Dump() const {
    std::lock_guard<std::mutex> lock(mu_);
    std::string s;
    for (const std::string& l : all_) s += l + "\n";
    return s;
  }

 private:
  std::string FindLocked(const std::string& event, const std::vector<std::string>& fields) const {
    for (const std::string& line : all_) {
      if (line.rfind("[NEVR.UPDATE] " + event, 0) != 0) continue;
      if (line.size() > 14 + event.size() && line[14 + event.size()] != ' ') continue;
      bool ok = true;
      for (const std::string& f : fields) ok = ok && (" " + line + " ").find(" " + f + " ") != std::string::npos;
      if (ok) return line;
    }
    return "";
  }

  mutable std::mutex mu_;
  std::vector<std::string> all_;
  std::vector<int> levels_;  // the level each line was written at, in step with `all_`
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
  bool discard_ok = true;
  bool with_plugin = false;  // the release also lists plugins/a.dll
  int open_calls = 0, manifest_calls = 0, stage_calls = 0, end_calls = 0, close_calls = 0, discard_calls = 0;
  std::string discard_version, discard_sha, discard_reason;
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
        root->Write("update\\staging\\" + version + "\\nevr-runtime.dll", runtime_bytes);
        files.push_back({{"path", "nevr-runtime.dll"}, {"size", runtime_bytes.size()}, {"sha256", kNewSha}});
        if (with_plugin) {
          root->Write("update\\staging\\" + version + "\\plugins\\a.dll", kPluginBytes);
          files.push_back({{"path", "plugins/a.dll"}, {"size", sizeof(kPluginBytes) - 1}, {"sha256", kOldSha}});
        }
      }
      *result = json{{"action", action}, {"version", version}, {"direction", "upgrade"}, {"reason", reason}, {"files", files}}.dump();
      return true;
    };
    c.run_end = [this](const std::string& state, const std::string& why) {
      ++end_calls;
      end_state = state;
      end_reason = why;
    };
    c.discard = [this](const std::string&, const std::string& version, const std::string& sha, const std::string& why, std::string* error) {
      ++discard_calls;
      discard_version = version;
      discard_sha = sha;
      discard_reason = why;
      if (!discard_ok) *error = "cannot write";
      return discard_ok;
    };
    c.close = [this] { ++close_calls; };
    return c;
  }
};

CheckEnv MakeEnv(const Root& root, FakeCore& core, Lines& lines) {
  core.root = &root;
  CheckEnv env;
  env.root = root.path();
  env.platform = "pc";
  env.version = "5.16.1";
  env.runtime_sha256 = kOldSha;
  env.core = core.Make();
  env.fs = &TestFs();
  env.verify = [](const std::string& path, const ExpectedFile&) {
    const std::string text = ReadAllFile(path);
    VerifyOutcome v;
    v.ok = text.find("BAD") == std::string::npos;
    v.refused = !v.ok && text.find("UNREADABLE") == std::string::npos;  // a file the verifier could not read is not a refusal
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
  Switches no_root;
  no_root.host_can_swap = true;
  no_root.no_root = true;
  no_root.flag_no_update = true;
  EXPECT_EQ(Decide(no_root), Disabled::kNoRoot) << "no root stops the confirmation too, so it outranks the switches";
  no_root.dev_build = true;
  EXPECT_EQ(Decide(no_root), Disabled::kDevBuild);
  EXPECT_STREQ(DisabledWord(Disabled::kNoRoot), "no_root");
  Switches old_host;  // everything else on, no capability bit
  EXPECT_EQ(Decide(old_host), Disabled::kHostCannotSwap) << "F16: a host that cannot swap gets no download";
  EXPECT_STREQ(DisabledWord(Disabled::kDevBuild), "dev_build");
  EXPECT_STREQ(DisabledWord(Disabled::kFlag), "flag");
  EXPECT_STREQ(DisabledWord(Disabled::kConfig), "config");
  EXPECT_STREQ(DisabledWord(Disabled::kHostCannotSwap), "host_cannot_swap");
}

// The development update source (policy.h, ResolveSource): the one decision of base and pins.
namespace {
constexpr char kTestPinKey[] = "RWQf6LRCGA9i53mlYecO4IzT51TGPpvWucNSCh1CBM0QTaLn73Y7GFO3";  // 56 base64 characters
constexpr char kTestPins[] = "test-key RWQf6LRCGA9i53mlYecO4IzT51TGPpvWucNSCh1CBM0QTaLn73Y7GFO3";
}  // namespace

TEST(UpdateSource, AReleaseBuildIgnoresBothKeysAndSaysSo) {
  const SourceResolution none = ResolveSource(false, std::nullopt, std::nullopt);
  EXPECT_EQ(none.verdict, SourceVerdict::kNone);
  const SourceResolution base = ResolveSource(false, std::string("https://example.test/u"), std::nullopt);
  EXPECT_EQ(base.verdict, SourceVerdict::kIgnoredRelease);
  EXPECT_EQ(base.key, "update.base");
  EXPECT_EQ(base.reason, "release_build");
  EXPECT_TRUE(base.use.base.empty()) << "a release build carries nothing to the core";
  const SourceResolution pins = ResolveSource(false, std::nullopt, std::string(kTestPins));
  EXPECT_EQ(pins.verdict, SourceVerdict::kIgnoredRelease);
  EXPECT_EQ(pins.key, "update.pins");
  EXPECT_TRUE(pins.use.pins.empty());
  EXPECT_STREQ(SourceVerdictWord(SourceVerdict::kIgnoredRelease), "ignored");
}

TEST(UpdateSource, ADevBuildUsesAValidBaseAndNormalizesIt) {
  const SourceResolution https = ResolveSource(true, std::string("https://example.test/nevr/"), std::nullopt);
  EXPECT_EQ(https.verdict, SourceVerdict::kUsed);
  EXPECT_EQ(https.use.base, "https://example.test/nevr") << "the core joins \"<base>/<name>\": no trailing slash";
  EXPECT_TRUE(https.use.pins.empty());
  EXPECT_FALSE(https.pins_given);
  const SourceResolution http = ResolveSource(true, std::string("http://127.0.0.1:8080"), std::nullopt);
  EXPECT_EQ(http.verdict, SourceVerdict::kUsed) << "adb reverse serves plain http, in a dev build";
  EXPECT_EQ(http.use.base, "http://127.0.0.1:8080");
  EXPECT_STREQ(SourceVerdictWord(SourceVerdict::kUsed), "used");
}

TEST(UpdateSource, ADevBuildRefusesABaseThatIsNotUsable) {
  const std::pair<const char*, const char*> bad[] = {
      {"", "empty"},
      {"ftp://example.test/u", "not_http_or_https"},
      {"file:///sdcard/u", "not_http_or_https"},
      {"example.test/u", "not_http_or_https"},
      {"https://exa mple.test/u", "control_character"},
      {"https://example.test/u\n", "control_character"},
      {"https://example.test/\x01", "control_character"},
      {"https://user:pw@example.test/u", "credentials_query_or_fragment"},
      {"https://example.test/u?x=1", "credentials_query_or_fragment"},
      {"https://example.test/u#f", "credentials_query_or_fragment"},
      {"https://", "no_host"},
      {"https:///u", "no_host"},
  };
  for (const auto& [text, reason] : bad) {
    const SourceResolution r = ResolveSource(true, std::string(text), std::nullopt);
    EXPECT_EQ(r.verdict, SourceVerdict::kRejected) << text;
    EXPECT_EQ(r.reason, reason) << text;
    EXPECT_EQ(r.key, "update.base") << text;
    EXPECT_TRUE(r.use.base.empty()) << text;
  }
  const SourceResolution tooLong = ResolveSource(true, "https://example.test/" + std::string(kMaxSourceBaseBytes, 'a'), std::nullopt);
  EXPECT_EQ(tooLong.reason, "too_long");
  EXPECT_STREQ(SourceVerdictWord(SourceVerdict::kRejected), "rejected");
}

TEST(UpdateSource, PinsNeedABaseAndAreCheckedLineByLine) {
  const SourceResolution used = ResolveSource(true, std::string("https://example.test/u"), std::string("# a comment\n\n") + kTestPins + "\n");
  EXPECT_EQ(used.verdict, SourceVerdict::kUsed);
  EXPECT_EQ(used.pins_count, 1u);
  EXPECT_TRUE(used.pins_given);
  EXPECT_EQ(used.use.pins, std::string("# a comment\n\n") + kTestPins + "\n") << "the core parses the text; it is passed as given";
  EXPECT_EQ(ResolveSource(true, std::nullopt, std::string(kTestPins)).reason, "pins_without_base");
  const std::pair<const char*, const char*> bad[] = {
      {"", "empty"},
      {"# only a comment\n", "no_keys"},
      {"label-only\n", "malformed_line"},
      {"label notbase64key\n", "malformed_key"},
      {"label RWQf6LRCGA9i53mlYecO4IzT51TGPpvWucNSCh1CBM0QTaLn73Y7GFO\n", "malformed_key"},
      {"label RWQf6LRCGA9i53mlYecO4IzT51TGPpvWucNSCh1CBM0QTaLn73Y7GFO3\t\n", "control_character"},
  };
  for (const auto& [text, reason] : bad) {
    const SourceResolution r = ResolveSource(true, std::string("https://example.test/u"), std::string(text));
    EXPECT_EQ(r.verdict, SourceVerdict::kRejected) << text;
    EXPECT_EQ(r.reason, reason) << text;
    EXPECT_EQ(r.key, "update.pins") << text;
    EXPECT_TRUE(r.use.base.empty()) << "one bad part uses neither: " << text;
  }
  EXPECT_EQ(std::string(kTestPinKey).size(), kPinKeyBase64Chars);
}

TEST(UpdateSource, ADevBuildWithASourceMayUpdateAndOneWithoutStaysDisabled) {
  Switches s;
  s.host_can_swap = true;
  s.dev_build = true;
  EXPECT_EQ(Decide(s), Disabled::kDevBuild) << "a dev build with no base stays disabled, with the existing reason";
  s.dev_source_override = true;
  EXPECT_EQ(Decide(s), Disabled::kNone);
  s.flag_no_update = true;
  EXPECT_EQ(Decide(s), Disabled::kFlag) << "the override lifts only the dev_build reason; the other switches still apply";
  s.flag_no_update = false;
  s.no_root = true;
  EXPECT_EQ(Decide(s), Disabled::kNoRoot);
  Switches release;
  release.host_can_swap = true;
  EXPECT_EQ(Decide(release), Disabled::kNone) << "a release build is decided as before";
}

TEST(UpdateSource, BaseAndPinsReachTheCoreOnlyWhenTheCheckEnvHoldsThem) {
  Lines lines;
  FakeCore core;
  Root root;
  CheckEnv env = MakeEnv(root, core, lines);
  RunCheck(env);
  ASSERT_EQ(core.open_calls, 1);
  const json plain = json::parse(core.open_config);
  EXPECT_FALSE(plain.contains("base")) << core.open_config;
  EXPECT_FALSE(plain.contains("pins")) << core.open_config;
  EXPECT_EQ(lines.Find("update check", {"source_override"}), "") << lines.Dump();

  SourceOverride source;
  source.base = "http://127.0.0.1:8080";
  source.pins = kTestPins;
  env.source = source;
  RunCheck(env);
  const json with = json::parse(core.open_config);
  EXPECT_EQ(with.value("base", ""), "http://127.0.0.1:8080") << core.open_config;
  EXPECT_EQ(with.value("pins", ""), kTestPins) << core.open_config;
  EXPECT_NE(lines.Find("update check", {"source_override=true"}), "") << lines.Dump();

  env.source->pins.clear();
  RunCheck(env);
  const json baseOnly = json::parse(core.open_config);
  EXPECT_TRUE(baseOnly.contains("base"));
  EXPECT_FALSE(baseOnly.contains("pins")) << "no pins given: the core keeps its built-in keys";
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

TEST(UpdateCheck, EachStagedFileIsVerifiedAtItsPathInTheReleasesFolder) {
  Root root;
  Lines lines;
  FakeCore core;
  core.with_plugin = true;
  CheckEnv env = MakeEnv(root, core, lines);
  std::vector<std::string> seen;
  std::vector<ExpectedFile> expected_seen;
  const auto inner = env.verify;
  env.verify = [&seen, &expected_seen, inner](const std::string& path, const ExpectedFile& expected) {
    seen.push_back(path);
    expected_seen.push_back(expected);
    return inner(path, expected);
  };
  const CheckOutcome out = RunCheck(env);
  EXPECT_EQ(out.action, CheckAction::kStaged) << lines.Dump();
  ASSERT_EQ(seen.size(), kStagedFilesWithAPlugin);
  EXPECT_EQ(seen[0], root.File("update\\staging\\5.99.0\\nevr-runtime.dll"));
  EXPECT_EQ(seen[1], root.File("update\\staging\\5.99.0\\plugins\\a.dll")) << "a plugin's member path has a separator of its own";
  // The verifier is given the entry the stage result listed for each file (the Quest verifier checks the bytes against it).
  ASSERT_EQ(expected_seen.size(), kStagedFilesWithAPlugin);
  EXPECT_EQ(expected_seen[0].size, std::string("NEW-RUNTIME").size());
  EXPECT_EQ(expected_seen[0].sha256, kNewSha);
  EXPECT_EQ(expected_seen[1].size, sizeof(kPluginBytes) - 1);
  EXPECT_EQ(expected_seen[1].sha256, kOldSha);
}

namespace {
// The fs with every call written down, so a test sees which operations the engine asked for and in what order. The spelling in
// `calls` is always backslashes, whatever the platform's separator is.
class RecordingFs final : public UpdateFs {
 public:
  explicit RecordingFs(std::string root) : root_(std::move(root)) {}
  mutable std::vector<std::string> calls;
  std::vector<std::string> links;  // paths (relative to the root, as `calls` spells them) that IsLink answers true for
  char Separator() const noexcept override { return TestFs().Separator(); }
  ReadResult ReadSmallFile(const std::string& p, std::string* out) const noexcept override {
    Note("ReadSmallFile", p);
    return TestFs().ReadSmallFile(p, out);
  }
  bool WriteByRename(const std::string& p, const std::string& t) const noexcept override {
    Note("WriteByRename", p);
    return TestFs().WriteByRename(p, t);
  }
  bool RemoveFile(const std::string& p) const noexcept override {
    Note("RemoveFile", p);
    return TestFs().RemoveFile(p);
  }
  void RemoveTree(const std::string& p) const noexcept override {
    Note("RemoveTree", p);
    TestFs().RemoveTree(p);
  }
  bool IsLink(const std::string& p) const noexcept override {
    Note("IsLink", p);
    return std::find(links.begin(), links.end(), Relative(p)) != links.end() || TestFs().IsLink(p);
  }
  bool Exists(const std::string& p) const noexcept override {
    Note("Exists", p);
    return TestFs().Exists(p);
  }
  std::unique_ptr<FsLock> TakeLock(const std::string& d, std::uint32_t t, std::uint32_t st, int* e) const noexcept override {
    Note("TakeLock", d);
    return TestFs().TakeLock(d, t, st, e);
  }
  bool Move(const std::string& from, const std::string& to, int* e) const noexcept override {
    Note("Move", from);
    return TestFs().Move(from, to, e);
  }

 private:
  std::string Relative(const std::string& path) const {
    std::string rel = path.compare(0, root_.size(), root_) == 0 ? path.substr(root_.size()) : path;
    std::replace(rel.begin(), rel.end(), '/', '\\');
    return rel;
  }
  void Note(const char* op, const std::string& path) const { calls.push_back(std::string(op) + " " + Relative(path)); }
  std::string root_;
};
}  // namespace

TEST(UpdateCheck, TheCheckAsksTheFsForEachOperationInTheOrderOfTheAdr) {
  Root root;
  Lines lines;
  FakeCore core;
  CheckEnv env = MakeEnv(root, core, lines);
  RecordingFs fs(root.path());
  env.fs = &fs;
  const auto inner = env.verify;
  env.verify = [&fs, inner](const std::string& path, const ExpectedFile& expected) {
    fs.calls.push_back("verify");
    return inner(path, expected);
  };
  const CheckOutcome out = RunCheck(env);
  EXPECT_EQ(out.action, CheckAction::kStaged) << lines.Dump();
  const std::vector<std::string> expected = {
      "IsLink \\update",
      "IsLink \\update\\staging",
      "Exists \\update\\pending.json",
      "ReadSmallFile \\update\\state.json",
      "IsLink \\update\\staging\\5.99.0",
      "verify",
      "TakeLock \\update",
      "Exists \\update\\pending.json",
      "WriteByRename \\update\\staged.json",
  };
  EXPECT_EQ(fs.calls, expected) << "the lock is taken after the verifier, the pending record is looked at again under it, staged.json goes last";
}

TEST(UpdateCheck, ALinkAtUpdateStagingOrTheReleasesFolderBlocksTheCheck) {
  struct Case {
    const char* link;
    int stage_calls;  // the core is asked to stage only when the two folders above it are not links
    bool verified;
  };
  const Case cases[] = {{"\\update", 0, false}, {"\\update\\staging", 0, false}, {"\\update\\staging\\5.99.0", 1, false}};
  for (const Case& c : cases) {
    Root root;
    Lines lines;
    FakeCore core;
    CheckEnv env = MakeEnv(root, core, lines);
    RecordingFs fs(root.path());
    fs.links = {c.link};
    env.fs = &fs;
    int verified = 0;
    const auto inner = env.verify;
    env.verify = [&verified, inner](const std::string& path, const ExpectedFile& expected) {
      ++verified;
      return inner(path, expected);
    };
    const CheckOutcome out = RunCheck(env);
    EXPECT_EQ(out.action, CheckAction::kBlocked) << c.link << "\n" << lines.Dump();
    EXPECT_EQ(out.reason, "reparse_point") << c.link;
    EXPECT_EQ(core.stage_calls, c.stage_calls) << c.link;
    EXPECT_EQ(verified, 0) << c.link;
    EXPECT_FALSE(root.Exists("update\\staged.json")) << c.link << ": nothing is published through a link";
  }
}

TEST(UpdateCheck, AnUnconvertibleRootReadsNothingAndEndsAsRootUnconvertible) {
  Lines lines;
  FakeCore core;
  Root root;
  CheckEnv env = MakeEnv(root, core, lines);
  env.root = "";
  env.root_unconvertible = true;  // a root whose text does not convert to UTF-8 (a lone surrogate on PC)
  env.config_channel = "beta";
  RecordingFs fs("");
  env.fs = &fs;
  const CheckOutcome out = RunCheck(env);
  EXPECT_EQ(out.action, CheckAction::kFailed);
  EXPECT_EQ(out.reason, "root_unconvertible");
  EXPECT_TRUE(fs.calls.empty()) << "nothing is read from a root that cannot be given to the fs";
  EXPECT_EQ(core.open_calls, 0);
  EXPECT_NE(lines.Find("update check", {"action=failed", "reason=root_unconvertible", "channel=beta", "channel_source=config"}), "") << lines.Dump();
  env.root_unconvertible = false;
  EXPECT_EQ(RunCheck(env).reason, "root_unconvertible") << "no root at all ends the same way";
}

TEST(UpdateCheck, AVerifiedReleaseIsPublishedLastAndEveryDecisionIsALine) {
  Root root;
  Lines lines;
  FakeCore core;
  CheckEnv env = MakeEnv(root, core, lines);
  const CheckOutcome out = RunCheck(env);

  EXPECT_EQ(out.action, CheckAction::kStaged) << lines.Dump();
  ASSERT_TRUE(root.Exists("update\\staged.json"));
  const json staged = json::parse(root.Read("update\\staged.json"));
  EXPECT_EQ(staged["version"], "5.99.0");
  EXPECT_EQ(staged["files"][0]["path"], "nevr-runtime.dll");
  EXPECT_EQ(staged["files"][0]["sha256"], kNewSha);
  EXPECT_TRUE(root.Exists("update\\staging\\5.99.0\\nevr-runtime.dll")) << "the staged files stay for the host's swap";
#if defined(_WIN32)
  // Win32 deletes the lock file with its last handle (FILE_FLAG_DELETE_ON_CLOSE in update_fs_win32.cpp).
  EXPECT_FALSE(root.Exists("update\\lock")) << "the lock is released (delete on close)";
#endif
  ExpectLockFree(root);

  EXPECT_NE(lines.Find("update verify", {"path=nevr-runtime.dll", "ok=true"}), "") << lines.Dump();
  EXPECT_NE(lines.Find("update published", {"version=5.99.0", "files=1"}), "") << lines.Dump();
  EXPECT_EQ(lines.LevelOf("update verify", {"ok=true"}), kInfo);
  EXPECT_EQ(lines.LevelOf("update published"), kInfo);
  EXPECT_EQ(lines.LevelOf("update check", {"action=staged"}), kInfo);
  EXPECT_NE(lines.Find("update check", {"action=staged", "target=5.99.0", "current=5.16.1", "direction=upgrade",
                                        "channel_source=default"}), "") << lines.Dump();
  EXPECT_NE(lines.Find("update check").find("elapsed_ms="), std::string::npos);
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
  EXPECT_EQ(cfg["nevr_root"], root.path()) << "an ASCII root reaches the core as it is";
  EXPECT_FALSE(cfg.contains("channel")) << "no channel anywhere: the core's default";
  // Nothing outside update\ and the core's own folders is written.
  for (const std::string& p : root.List()) {
    EXPECT_EQ(p.rfind("update", 0), 0u) << p << " is outside update\\";
  }
}

TEST(UpdateCheck, ARootWithANonAsciiLetterReachesTheCoreAsUtf8) {
  Root root("-Zo\xC3\xAB");  // a profile folder named for the player
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
  EXPECT_FALSE(root.Exists("update\\staged.json")) << "falsifier: a staged file that fails its signature must not be offered to the host";
  EXPECT_FALSE(root.Exists("update\\staging\\5.99.0")) << "staging removed";
  EXPECT_NE(lines.Find("update verify", {"path=nevr-runtime.dll", "ok=false"}), "") << lines.Dump();
  EXPECT_EQ(lines.LevelOf("update verify", {"ok=false"}), kWarning);
  EXPECT_EQ(lines.LevelOf("update check", {"action=blocked"}), kInfo);
  EXPECT_EQ(core.discard_calls, 1) << "the verifier refused these bytes: the core is told";
  EXPECT_EQ(core.discard_version, "5.99.0");
  EXPECT_EQ(core.discard_sha, kNewSha);
  EXPECT_EQ(core.discard_reason, "verify_failed");
  EXPECT_NE(lines.Find("update discard", {"version=5.99.0", "recorded=true"}), "") << lines.Dump();
  EXPECT_EQ(lines.LevelOf("update discard"), kInfo);
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
  EXPECT_FALSE(root.Exists("update\\staged.json"));
  EXPECT_NE(lines.Find("update check", {"action=failed", "reason=stage_failed"}), "") << lines.Dump();
  EXPECT_EQ(lines.LevelOf("update check", {"action=failed"}), kWarning);
  EXPECT_EQ(core.discard_calls, 0) << "a download that failed is not a refused release";
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
    EXPECT_FALSE(root.Exists("update\\staged.json"));
    EXPECT_NE(lines.Find("update check", {std::string("action=") + action, "reason=" + core.reason}), "") << lines.Dump();
  }
}

TEST(UpdateCheck, AReleaseWaitsWhileAnotherIsPendingAndNothingIsFetched) {
  Root root;
  root.Write("update\\pending.json", R"({"to":{"version":"5.98.0","sha256":")" + std::string(kNewSha) + R"("},"from":null,"boot_attempts":1,"failed_reason":null})");
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
  core.during_stage = [clock] { *clock += kCheckBudgetMs + 1000; };  // past the 120 s budget
  env.core = core.Make();
  const CheckOutcome out = RunCheck(env);
  EXPECT_EQ(out.action, CheckAction::kFailed);
  EXPECT_EQ(out.reason, "timeout");
  EXPECT_FALSE(root.Exists("update\\staged.json")) << "bounded: 120 s for the whole stage";
  EXPECT_EQ(lines.Count("update verify"), 0) << "a late result is not even verified";
  EXPECT_FALSE(root.Exists("update\\staging\\5.99.0")) << "what a late stage wrote is removed, not left on disk";
  EXPECT_EQ(core.discard_calls, 0) << "a late result is not a refused one";
  EXPECT_NE(lines.Find("update check", {"action=failed", "reason=timeout"}), "") << lines.Dump();
}

TEST(UpdateCheck, AReleaseThatTurnsLateWhileItIsBeingVerifiedIsRemovedToo) {
  Root root;
  Lines lines;
  FakeCore core;
  CheckEnv env = MakeEnv(root, core, lines);
  auto clock = std::make_shared<std::uint64_t>(1000);
  env.now_ms = [clock] { return *clock; };
  const auto verify = env.verify;
  env.verify = [clock, verify](const std::string& path, const ExpectedFile& expected) {
    *clock += kCheckBudgetMs + 1000;  // the verification itself runs past the budget
    return verify(path, expected);
  };
  const CheckOutcome out = RunCheck(env);
  EXPECT_EQ(out.reason, "timeout");
  EXPECT_FALSE(root.Exists("update\\staged.json"));
  EXPECT_FALSE(root.Exists("update\\staging\\5.99.0")) << "staging removed on the second budget check as on the first";
  EXPECT_EQ(core.discard_calls, 0);
}

TEST(UpdateCheck, AFileThatCouldNotBeReadIsNotDiscardedAndAFailedRecordIsAWarning) {
  {
    Root root;
    Lines lines;
    FakeCore core;
    core.runtime_bytes = "BAD-UNREADABLE";
    CheckEnv env = MakeEnv(root, core, lines);
    EXPECT_EQ(RunCheck(env).reason, "verify_failed");
    EXPECT_EQ(core.discard_calls, 0) << "a scanner's lock is not a verdict: the next launch tries again";
    EXPECT_FALSE(root.Exists("update\\staging\\5.99.0"));
    EXPECT_EQ(lines.Count("update discard"), 0);
  }
  {
    Root root;
    Lines lines;
    FakeCore core;
    core.runtime_bytes = "BAD";
    core.discard_ok = false;
    CheckEnv env = MakeEnv(root, core, lines);
    EXPECT_EQ(RunCheck(env).reason, "verify_failed");
    EXPECT_NE(lines.Find("update discard", {"recorded=false"}), "") << lines.Dump();
    EXPECT_EQ(lines.LevelOf("update discard"), kWarning);
  }
}

TEST(UpdateCheck, TheChannelAndItsSourceReachTheCoreAndTheLine) {
  Root root;
  root.Write("update\\state.json", R"({"channel":"stable","owned":[]})");
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
  root.Write("update\\state.json", json{{"quarantine", json::array({{{"version", "5.99.0"}, {"runtime_sha256", kNewSha}}})}}.dump());
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
  root.MakeDir("update");
  int err = 0;
  const std::unique_ptr<FsLock> other = TestFs().TakeLock(root.File("update"), 0, kHolderStepMs, &err);
  ASSERT_NE(other, nullptr) << err;
  Lines lines;
  FakeCore core;
  CheckEnv env = MakeEnv(root, core, lines);
  env.lock_timeout_ms = 200;
  const CheckOutcome out = RunCheck(env);
  EXPECT_EQ(out.action, CheckAction::kFailed);
  EXPECT_EQ(out.reason, "lock_unavailable");
  EXPECT_FALSE(root.Exists("update\\staged.json"));
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
  root.Write("update\\pending.json", PendingJson("5.99.0", kNewSha));
  root.Write("update\\crashed", "");
  root.Write("update\\state.json", R"({"channel":"beta","owned":[{"path":"nevr-runtime.dll","sha256":")" + std::string(kNewSha) + R"("}]})");
  Lines lines;
  ConfirmEnv env;
  env.root = root.path();
  env.runtime_sha256 = kNewSha;
  env.role = "server";
  env.log = lines.Fn();
  env.fs = &TestFs();
  EXPECT_EQ(UpdateConfirm(env, 10042), ConfirmResult::kConfirmed) << lines.Dump();
  EXPECT_FALSE(root.Exists("update\\pending.json"));
  EXPECT_FALSE(root.Exists("update\\crashed"));
  const json state = json::parse(root.Read("update\\state.json"));
  EXPECT_EQ(state["current"]["version"], "5.99.0");
  EXPECT_EQ(state["current"]["runtime_sha256"], kNewSha);
  EXPECT_EQ(state["channel"], "beta");
  EXPECT_NE(lines.Find("update confirmed", {"version=5.99.0", "after_ms=10042", "role=server"}), "") << lines.Dump();
  EXPECT_EQ(lines.LevelOf("update confirmed"), kInfo);
#if defined(_WIN32)
  // Win32 deletes the lock file with its last handle (FILE_FLAG_DELETE_ON_CLOSE in update_fs_win32.cpp).
  EXPECT_FALSE(root.Exists("update\\lock")) << "the lock is released (delete on close)";
#endif
  ExpectLockFree(root);
}

TEST(UpdateConfirm, TheConfirmationWritesStateFirstThenRemovesPendingThenCrashedUnderTheLock) {
  Root root;
  root.Write("update\\pending.json", PendingJson("5.99.0", kNewSha));
  root.Write("update\\crashed", "");
  root.Write("update\\state.json", "{}");
  Lines lines;
  RecordingFs fs(root.path());
  ConfirmEnv env;
  env.root = root.path();
  env.runtime_sha256 = kNewSha;
  env.log = lines.Fn();
  env.fs = &fs;
  EXPECT_EQ(UpdateConfirm(env, 1), ConfirmResult::kConfirmed) << lines.Dump();
  const std::vector<std::string> expected = {
      "IsLink \\update",
      "ReadSmallFile \\update\\pending.json",
      "TakeLock \\update",
      "ReadSmallFile \\update\\pending.json",
      "ReadSmallFile \\update\\state.json",
      "WriteByRename \\update\\state.json",
      "RemoveFile \\update\\pending.json",
      "RemoveFile \\update\\crashed",
  };
  EXPECT_EQ(fs.calls, expected) << "state.json first: a kill between the writes leaves a pending record the host drops as stale";
}

TEST(UpdateConfirm, ALinkAtUpdateIsNotWrittenThrough) {
  Root root;
  root.Write("update\\pending.json", PendingJson("5.99.0", kNewSha));
  Lines lines;
  RecordingFs fs(root.path());
  fs.links = {"\\update"};
  ConfirmEnv env;
  env.root = root.path();
  env.runtime_sha256 = kNewSha;
  env.log = lines.Fn();
  env.fs = &fs;
  EXPECT_EQ(UpdateConfirm(env, 1), ConfirmResult::kPendingUnreadable);
  EXPECT_TRUE(root.Exists("update\\pending.json"));
}

TEST(UpdateConfirm, AnUnconvertibleRootFailsClosedAndNoRootIsNoPending) {
  Lines lines;
  ConfirmEnv env;
  env.runtime_sha256 = kNewSha;
  env.log = lines.Fn();
  env.fs = &TestFs();
  env.root = "";
  env.root_unconvertible = true;
  EXPECT_EQ(UpdateConfirm(env, 1), ConfirmResult::kPendingUnreadable);
  env.root_unconvertible = false;
  EXPECT_EQ(UpdateConfirm(env, 1), ConfirmResult::kNotPending);
}

TEST(UpdateConfirm, ARuntimeThatIsNotThePendingOneConfirmsNothing) {
  Root root;
  root.Write("update\\pending.json", PendingJson("5.99.0", kNewSha));
  root.Write("update\\state.json", "{}");
  Lines lines;
  ConfirmEnv env;
  env.root = root.path();
  env.runtime_sha256 = kOldSha;  // the old runtime, e.g. a second process that still maps it
  env.log = lines.Fn();
  env.fs = &TestFs();
  EXPECT_EQ(UpdateConfirm(env, 10000), ConfirmResult::kNotThePendingRelease);
  EXPECT_TRUE(root.Exists("update\\pending.json"));
  EXPECT_EQ(root.Read("update\\state.json"), "{}");
  EXPECT_EQ(lines.Count("update confirmed"), 0);
  EXPECT_NE(lines.Find("update confirm skipped", {"reason=not_the_pending_release"}), "") << lines.Dump();
  EXPECT_EQ(lines.LevelOf("update confirm skipped"), kInfo);
}

TEST(UpdateConfirm, NoPendingRecordMeansNothingToConfirm) {
  Root root;
  Lines lines;
  ConfirmEnv env;
  env.root = root.path();
  env.runtime_sha256 = kNewSha;
  env.log = lines.Fn();
  env.fs = &TestFs();
  EXPECT_EQ(UpdateConfirm(env, 10000), ConfirmResult::kNotPending);
  EXPECT_NE(lines.Find("update confirm skipped", {"reason=no_pending"}), "") << lines.Dump();
  EXPECT_EQ(lines.LevelOf("update confirm skipped", {"reason=no_pending"}), kDebug);
  EXPECT_TRUE(root.List().empty()) << "a run with no update folder makes none";
}

TEST(UpdateConfirm, AStateFileThatIsDamagedIsNotWrittenOver) {
  Root root;
  root.Write("update\\pending.json", PendingJson("5.99.0", kNewSha));
  root.Write("update\\state.json", "this is not json");
  Lines lines;
  ConfirmEnv env;
  env.root = root.path();
  env.runtime_sha256 = kNewSha;
  env.log = lines.Fn();
  env.fs = &TestFs();
  EXPECT_EQ(UpdateConfirm(env, 10000), ConfirmResult::kStateUnreadable);
  EXPECT_TRUE(root.Exists("update\\pending.json")) << "the host rebuilds state; the record of the trial stays";
  EXPECT_EQ(root.Read("update\\state.json"), "this is not json");
  EXPECT_NE(lines.Find("update confirm skipped", {"reason=state_unreadable"}), "") << lines.Dump();
}

#if defined(_WIN32)
// ---- the PC edge: the wide root and the wide verifier path (update_run.cpp) ----------------------------------------------

TEST(UpdateEdgePc, ALoneSurrogateDoesNotConvertAndNoRootIsEmpty) {
  std::string out = "x";
  EXPECT_FALSE(RootToUtf8(L"C:\\nevr-\xD800", &out));
  EXPECT_TRUE(out.empty()) << "a root that does not convert leaves nothing behind";
  EXPECT_TRUE(RootToUtf8(L"", &out));
  EXPECT_TRUE(out.empty());
  EXPECT_TRUE(RootToUtf8(L"C:\\nevr", &out));
  EXPECT_EQ(out, "C:\\nevr");
}

TEST(UpdateEdgePc, APathThatDoesNotConvertIsNotAVerdictAndTheWideVerifierIsNotRun) {
  bool ran = false;
  const auto verify = WidenVerifier([&ran](const std::wstring&) {
    ran = true;
    return VerifyOutcome{true, false, "accepted"};
  });
  const VerifyOutcome out = verify(std::string("C:\\x\xFF"), ExpectedFile{});  // not UTF-8
  EXPECT_FALSE(out.ok);
  EXPECT_FALSE(out.refused) << "a path that does not convert is not a refusal: nothing is discarded for it";
  EXPECT_EQ(out.reason, "path_unconvertible");
  EXPECT_FALSE(ran);
  EXPECT_TRUE(verify(std::string("C:\\ok"), ExpectedFile{}).ok) << "a path that converts reaches the wide verifier";
  EXPECT_TRUE(ran);
}

TEST(UpdateLevels, TheGamesLogLevelBitsAreTheOnesTheLinesAreWrittenAt) {
  EXPECT_EQ(EchoLevelBits(kDebug), 0x1);
  EXPECT_EQ(EchoLevelBits(kInfo), 0x2);
  EXPECT_EQ(EchoLevelBits(kWarning), 0x4);
  EXPECT_EQ(EchoLevelBits(kError), 0x8);
  EXPECT_EQ(EchoLevelBits(42), 0x2) << "an unknown level is Info";
}
#endif

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
    deps.platform = "pc";
    deps.root = root.path();
    deps.read_running = [](std::string* v, std::string* s, std::string*) {
      *v = "5.16.1";
      *s = kNewSha;
      return true;
    };
    deps.core = core.Make();
    deps.verify = [](const std::string&, const ExpectedFile&) { return VerifyOutcome{true, false, "accepted"}; };
    deps.log = lines.Fn();
    deps.now_ms = [this] { return clock.load(); };
    deps.fs = &TestFs();
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
  EXPECT_EQ(f.lines.LevelOf("update disabled"), kInfo);
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

TEST(UpdateWorker, WithNoNevrRootNothingStartsAndTheLineIsAWarning) {
  WorkerFixture f;
  f.deps.root.clear();
  f.deps.switches.flag_no_update = true;  // even with checks off the confirmation must not look for the update folder at the root of a drive
  Worker w(f.deps);
  EXPECT_EQ(w.Start(), Disabled::kNoRoot);
  w.OnFrame(1000);
  w.OnFrame(20000);
  w.Stop(3000);
  EXPECT_EQ(f.core.open_calls, 0);
  EXPECT_NE(f.lines.Find("update disabled", {"reason=no_root"}), "") << f.lines.Dump();
  EXPECT_EQ(f.lines.LevelOf("update disabled"), kWarning) << "a launch that cannot update at all is not a routine switch";
  EXPECT_EQ(f.lines.Count("update confirm skipped"), 0) << "no thread ran";
}

TEST(UpdateWorker, AnUnconvertibleRootIsNotNoRootSoTheCheckRunsAndFailsClosed) {
  WorkerFixture f;
  f.deps.root.clear();
  f.deps.root_unconvertible = true;  // the root exists but its text does not convert: not "no root", so the check runs and fails closed
  Worker w(f.deps);
  EXPECT_EQ(w.Start(), Disabled::kNone);
  ASSERT_TRUE(w.WaitCheckDone(10000)) << f.lines.Dump();
  w.Stop(2000);
  EXPECT_EQ(f.core.open_calls, 0) << "nothing is read from a root that does not convert";
  EXPECT_NE(f.lines.Find("update check", {"action=failed", "reason=root_unconvertible"}), "") << f.lines.Dump();
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
    f.root.Write("update\\pending.json", PendingJson("5.99.0", kNewSha));
    f.root.Write("update\\state.json", "{}");
    Worker w(f.deps);
    w.Start();
    w.OnFrame(1000);
    w.OnFrame(20000);
    w.Stop(3000);
    EXPECT_TRUE(f.root.Exists("update\\pending.json")) << "no capability bit, so no swap ever made this record: nothing confirms it";
    EXPECT_EQ(f.lines.Count("update confirmed"), 0);
  }
}

TEST(UpdateWorker, TheCheckRunsOffTheCallersThreadAndEndsWithItsLine) {
  WorkerFixture f;
  struct StageThread {
    std::mutex mu;
    std::thread::id id;
  };
  auto stage_thread = std::make_shared<StageThread>();
  f.core.during_stage = [stage_thread] {
    std::lock_guard<std::mutex> lock(stage_thread->mu);
    stage_thread->id = std::this_thread::get_id();
  };
  const std::thread::id caller = std::this_thread::get_id();
  f.deps.core = f.core.Make();
  Worker w(f.deps);
  EXPECT_EQ(w.Start(), Disabled::kNone);
  ASSERT_TRUE(w.WaitCheckDone(10000)) << f.lines.Dump();
  std::thread::id ran_on;
  {
    std::lock_guard<std::mutex> lock(stage_thread->mu);
    ran_on = stage_thread->id;
  }
  EXPECT_NE(ran_on, std::thread::id()) << "the stage ran";
  EXPECT_NE(ran_on, caller) << "the game thread never waits for the core";
  EXPECT_TRUE(f.root.Exists("update\\staged.json"));
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
  f.root.Write("update\\pending.json", PendingJson("5.99.0", kNewSha));
  f.root.Write("update\\state.json", "{}");
  f.deps.switches.flag_no_update = true;  // checks off; the confirmation still runs
  Worker w(f.deps);
  EXPECT_EQ(w.Start(), Disabled::kFlag);
  w.OnFrame(50000);  // the first call fixes the start of the 10 s
  w.OnFrame(59999);
  EXPECT_FALSE(w.WaitConfirmDone(300)) << "9.999 s of frames is not the mark";
  EXPECT_TRUE(f.root.Exists("update\\pending.json"));
  w.OnFrame(60000);
  ASSERT_TRUE(w.WaitConfirmDone(10000)) << f.lines.Dump();
  EXPECT_FALSE(f.root.Exists("update\\pending.json"));
  EXPECT_NE(f.lines.Find("update confirmed", {"version=5.99.0", "role=client"}), "") << f.lines.Dump();
  w.Stop(2000);
}

TEST(UpdateWorker, ARuntimeThatIsNotThePendingOneDoesNotConfirmAtTheMark) {
  WorkerFixture f;
  f.root.Write("update\\pending.json", PendingJson("5.99.0", kOldSha));  // the running one is kNewSha
  f.root.Write("update\\state.json", "{}");
  Worker w(f.deps);
  w.Start();
  w.OnFrame(1000);
  w.OnFrame(12000);
  ASSERT_TRUE(w.WaitConfirmDone(10000));
  EXPECT_TRUE(f.root.Exists("update\\pending.json"));
  EXPECT_EQ(f.lines.Count("update confirmed"), 0);
  EXPECT_NE(f.lines.Find("update confirm skipped", {"reason=not_the_pending_release"}), "") << f.lines.Dump();
  w.Stop(2000);
}

// The worker calls Deps::log from its check thread and its confirmation thread at the same time (worker.cpp, CheckMain and ConfirmMain),
// so the capture the tests give it must hold every line from both.
TEST(UpdateWorker, TheLogCaptureTakesLinesFromTwoThreadsAtOnce) {
  constexpr int kLinesPerThread = 2000;
  Lines lines;
  const LogFn log = lines.Fn();
  std::atomic<bool> go{false};
  const auto writer = [&] {
    while (!go.load()) std::this_thread::yield();
    for (int i = 0; i < kLinesPerThread; ++i) log(kInfo, "[NEVR.UPDATE] update check n=" + std::to_string(i));
  };
  std::thread first(writer);
  std::thread second(writer);
  go.store(true);
  first.join();
  second.join();
  EXPECT_EQ(lines.Count("update check"), 2 * kLinesPerThread);
}
