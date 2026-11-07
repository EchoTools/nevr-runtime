// #1424: the content pack the runtime serves (runtime/content/content_json.h, content_pack.h). Every pack and every "stock" file
// here is made by the test in its own temporary folder: a fake game folder (with _data and bin\win10\scripts) and a content folder.
// Nothing of the game's is read.
#include <winsock2.h>
#include <windows.h>

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include <functional>
#include <string>
#include <vector>

#include "runtime/content/content_json.h"
#include "runtime/content/content_pack.h"
#include "runtime/content/pack_rules.h"
#include "runtime/host/file_digest.h"

namespace {

using nevr_content::Decide;
using nevr_content::Decision;
using nevr_content::Inputs;
using nevr_content::Outcome;

constexpr const char* kSet = "48037dc70b0ecab2";
constexpr const char* kScript = "190ed14fe6c74dd3.dll";

std::string Sha(const std::string& bytes) {
  return nevr_host::Sha256Hex(std::vector<unsigned char>(bytes.begin(), bytes.end()));
}

std::string Narrow(const std::wstring& w) {
  std::string out;
  for (wchar_t c : w) out += static_cast<char>(c);
  return out;
}

void MakeDirs(const std::wstring& path) {
  for (std::size_t i = 3; i <= path.size(); ++i) {
    if (i == path.size() || path[i] == L'\\') CreateDirectoryW(path.substr(0, i).c_str(), nullptr);
  }
}

void WriteFileBytes(const std::wstring& path, const std::string& bytes) {
  MakeDirs(path.substr(0, path.find_last_of(L'\\')));
  HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  ASSERT_NE(h, INVALID_HANDLE_VALUE) << Narrow(path);
  DWORD wrote = 0;
  ASSERT_TRUE(WriteFile(h, bytes.data(), static_cast<DWORD>(bytes.size()), &wrote, nullptr));
  CloseHandle(h);
}

// One fake install and one content folder, removed at the end of the test.
class ContentPack : public ::testing::Test {
 protected:
  void SetUp() override {
    nevr_content::Forget();
    wchar_t tmp[MAX_PATH];
    ASSERT_NE(GetTempPathW(MAX_PATH, tmp), 0u);
    base_ = std::wstring(tmp) + L"nevr-content-pack-test-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
            std::to_wstring(GetTickCount64());
    game_ = base_ + L"\\echovr";
    exe_dir_ = game_ + L"\\bin\\win10";
    content_ = base_ + L"\\root\\content";
    data_ = game_ + L"\\_data\\5932408047\\rad15\\win10";
    stock_manifest_ = "stock manifest bytes";
    WriteFileBytes(data_ + L"\\manifests\\" + std::wstring(kSet, kSet + 16), stock_manifest_);
    WriteFileBytes(data_ + L"\\packages\\48037dc70b0ecab2_0", "stock package 0");
    WriteFileBytes(exe_dir_ + L"\\scripts\\" + std::wstring(kScript, kScript + 20), "stock script");
    MakeDirs(content_);
  }

  void TearDown() override {
    nevr_content::Forget();  // closes the held files so the folder can go
    Remove(base_);
  }

  static void Remove(const std::wstring& dir) {
    WIN32_FIND_DATAW f;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &f);
    if (h != INVALID_HANDLE_VALUE) {
      do {
        const std::wstring name = f.cFileName;
        if (name == L"." || name == L"..") continue;
        const std::wstring path = dir + L"\\" + name;
        if (f.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
          Remove(path);
        } else {
          DeleteFileW(path.c_str());
        }
      } while (FindNextFileW(h, &f));
      FindClose(h);
    }
    RemoveDirectoryW(dir.c_str());
  }

  // A pack folder with a manifest, one added package and one script, and its content.json; `edit` changes the JSON first.
  std::wstring MakePack(const std::string& id, bool with_script = true,
                        const std::function<void(nlohmann::json&)>& edit = nullptr) {
    const std::wstring dir = content_ + L"\\" + std::wstring(id.begin(), id.end());
    const std::string manifest = "pack manifest", package = "pack package 1", script = "pack script";
    WriteFileBytes(dir + L"\\manifests\\48037dc70b0ecab2", manifest);
    WriteFileBytes(dir + L"\\packages\\48037dc70b0ecab2_1", package);
    nlohmann::json j = {{"format", 1}, {"id", id}, {"name", "Test"}, {"version", "1.2.3"},
                        {"data_dir", "_data/5932408047/rad15/win10"}};
    j["manifests"] = nlohmann::json::array({{{"package", kSet}, {"stock_sha256", Sha(stock_manifest_)}, {"stock_packages", 1},
                                             {"file", "manifests/48037dc70b0ecab2"},
                                             {"packages", {"packages/48037dc70b0ecab2_1"}}}});
    j["files"] = {{"manifests/48037dc70b0ecab2", Sha(manifest)}, {"packages/48037dc70b0ecab2_1", Sha(package)}};
    if (with_script) {
      WriteFileBytes(dir + L"\\scripts\\" + std::wstring(kScript, kScript + 20), script);
      j["scripts"] = {std::string("scripts/") + kScript};
      j["files"][std::string("scripts/") + kScript] = Sha(script);
    }
    if (edit) edit(j);
    WriteFileBytes(dir + L"\\content.json", j.dump());
    return dir;
  }

  Inputs In() const {
    Inputs in;
    in.content_dir = content_;
    in.game_dir = game_;
    in.exe_dir = exe_dir_;
    return in;
  }

  std::string GamePath(const std::wstring& path) const { return Narrow(path); }

  std::wstring base_, game_, exe_dir_, content_, data_;
  std::string stock_manifest_;
};

TEST_F(ContentPack, NoPackInstalledServesNothing) {
  const Decision d = Decide(In());
  EXPECT_EQ(d.outcome, Outcome::kNone);
  std::string out;
  EXPECT_FALSE(nevr_content::ServeOpen(GamePath(data_ + L"\\manifests\\48037dc70b0ecab2").c_str(), 1, &out));
}

TEST_F(ContentPack, AMissingContentFolderIsNoPack) {
  Inputs in = In();
  in.content_dir = base_ + L"\\no-such-folder";
  EXPECT_EQ(Decide(in).outcome, Outcome::kNone);
}

TEST_F(ContentPack, ACheckedPackServesItsManifestPackageAndScript) {
  const std::wstring dir = MakePack("testpack");
  const Decision d = Decide(In());
  ASSERT_EQ(d.outcome, Outcome::kServed) << d.reason << " " << d.detail;
  EXPECT_EQ(d.id, "testpack");
  EXPECT_EQ(d.version, "1.2.3");
  EXPECT_EQ(d.manifests, 1u);
  EXPECT_EQ(d.packages, 1u);
  EXPECT_EQ(d.scripts, 1u);
  EXPECT_EQ(d.files_checked, 3u);

  std::string out;
  // The game's spellings: forward slashes, the \\?\ prefix, any case.
  const std::string manifest = GamePath(data_ + L"\\manifests\\48037dc70b0ecab2");
  ASSERT_TRUE(nevr_content::ServeOpen(manifest.c_str(), 1, &out));
  EXPECT_EQ(out, GamePath(dir + L"\\manifests\\48037dc70b0ecab2"));
  std::string slashed = manifest;
  for (char& c : slashed) c = c == '\\' ? '/' : c;
  EXPECT_TRUE(nevr_content::ServeOpen(slashed.c_str(), 1, &out));
  EXPECT_TRUE(nevr_content::ServeOpen(("\\\\?\\" + manifest).c_str(), 1, &out));
  std::string upper = manifest;
  for (char& c : upper) c = (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
  EXPECT_TRUE(nevr_content::ServeOpen(upper.c_str(), 1, &out));

  ASSERT_TRUE(nevr_content::ServeOpen(GamePath(data_ + L"\\packages\\48037dc70b0ecab2_1").c_str(), 1, &out));
  EXPECT_EQ(out, GamePath(dir + L"\\packages\\48037dc70b0ecab2_1"));

  std::wstring load;
  std::wstring lower = exe_dir_ + L"\\scripts\\190ed14fe6c74dd3.dll";
  for (wchar_t& c : lower) c = (c >= L'A' && c <= L'Z') ? static_cast<wchar_t>(c - L'A' + L'a') : c;
  ASSERT_TRUE(nevr_content::RewriteScript(lower.c_str(), &load));
  EXPECT_EQ(load, dir + L"\\scripts\\190ed14fe6c74dd3.dll");
}

TEST_F(ContentPack, OnlyReadsOfThePathsThePackReplacesAreServed) {
  MakePack("testpack");
  ASSERT_EQ(Decide(In()).outcome, Outcome::kServed);
  std::string out;
  const std::string manifest = GamePath(data_ + L"\\manifests\\48037dc70b0ecab2");
  for (const std::uint32_t write : {0x2u, 0x4u, 0x8u, 0x10u}) {
    EXPECT_FALSE(nevr_content::ServeOpen(manifest.c_str(), 1 | write, &out)) << write;
  }
  EXPECT_FALSE(nevr_content::ServeOpen(manifest.c_str(), 0x2, &out));  // a write alone
  EXPECT_FALSE(nevr_content::ServeOpen(manifest.c_str(), 0x0, &out));  // no read bit
  EXPECT_TRUE(nevr_content::ServeOpen(manifest.c_str(), 0x21, &out));  // a read with the async bit is still a read
  // A stock package the pack does not replace, another set and another script are the game's own.
  EXPECT_FALSE(nevr_content::ServeOpen(GamePath(data_ + L"\\packages\\48037dc70b0ecab2_0").c_str(), 1, &out));
  EXPECT_FALSE(nevr_content::ServeOpen(GamePath(data_ + L"\\manifests\\2b47aab238f60515").c_str(), 1, &out));
  std::wstring load;
  EXPECT_FALSE(nevr_content::RewriteScript(L"c:\\elsewhere\\scripts\\190ed14fe6c74dd3.dll", &load));
  std::wstring other = exe_dir_ + L"\\scripts\\0000000000000000.dll";
  for (wchar_t& c : other) c = (c >= L'A' && c <= L'Z') ? static_cast<wchar_t>(c - L'A' + L'a') : c;
  EXPECT_FALSE(nevr_content::RewriteScript(other.c_str(), &load));
}

TEST_F(ContentPack, TheDetourHandsTheGameThePacksFileAndPassesTheRestOn) {
  const std::wstring dir = MakePack("testpack");
  ASSERT_EQ(Decide(In()).outcome, Outcome::kServed);
  static std::string s_seen;
  static std::uint32_t s_flags = 0;
  const nevr_content::CFileOpenFn original = [](const char* path, std::uint32_t flags, HANDLE* out) -> std::uint64_t {
    s_seen = path;
    s_flags = flags;
    *out = nullptr;
    return 0;
  };
  HANDLE h = nullptr;
  const std::string manifest = GamePath(data_ + L"\\manifests\\48037dc70b0ecab2");
  EXPECT_EQ(nevr_content::OpenThrough(original, manifest.c_str(), 1, &h), 0u);
  EXPECT_EQ(s_seen, GamePath(dir + L"\\manifests\\48037dc70b0ecab2"));
  EXPECT_EQ(s_flags, 1u);
  const std::string other = GamePath(data_ + L"\\packages\\48037dc70b0ecab2_0");
  nevr_content::OpenThrough(original, other.c_str(), 1, &h);
  EXPECT_EQ(s_seen, other);
  nevr_content::OpenThrough(original, manifest.c_str(), 0x13, &h);  // a write is never redirected
  EXPECT_EQ(s_seen, manifest);
}

TEST_F(ContentPack, APackFileIsHeldSoItCannotBeChangedAfterTheCheck) {
  const std::wstring dir = MakePack("testpack");
  ASSERT_EQ(Decide(In()).outcome, Outcome::kServed);
  const std::wstring file = dir + L"\\packages\\48037dc70b0ecab2_1";
  HANDLE w = CreateFileW(file.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
  EXPECT_EQ(w, INVALID_HANDLE_VALUE);
  if (w != INVALID_HANDLE_VALUE) CloseHandle(w);
  EXPECT_FALSE(DeleteFileW(file.c_str()));
  // The game's own read (CFile::Open's share mode for a read is read and write) still opens it.
  HANDLE r = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
  EXPECT_NE(r, INVALID_HANDLE_VALUE);
  if (r != INVALID_HANDLE_VALUE) CloseHandle(r);
  nevr_content::Forget();
  std::string out;
  EXPECT_FALSE(nevr_content::ServeOpen(GamePath(data_ + L"\\manifests\\48037dc70b0ecab2").c_str(), 1, &out));
  // Forget closes what it held: the file can be written again.
  w = CreateFileW(file.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
  EXPECT_NE(w, INVALID_HANDLE_VALUE);
  if (w != INVALID_HANDLE_VALUE) CloseHandle(w);
}

TEST_F(ContentPack, TwoPacksAreBothRefused) {
  MakePack("alpha");
  MakePack("beta");
  const Decision d = Decide(In());
  EXPECT_EQ(d.outcome, Outcome::kRefused);
  EXPECT_EQ(d.reason, "several_packs");
  EXPECT_EQ(d.detail, "alpha,beta");
}

TEST_F(ContentPack, APackLoadedAfterTheGameReadItsDataIsRefused) {
  MakePack("testpack");
  Inputs in = In();
  in.data_read_already = true;
  const Decision d = Decide(in);
  EXPECT_EQ(d.outcome, Outcome::kRefused);
  EXPECT_EQ(d.reason, "loaded_too_late");
}

TEST_F(ContentPack, AFolderWithoutContentJsonIsRefused) {
  MakeDirs(content_ + L"\\empty");
  const Decision d = Decide(In());
  EXPECT_EQ(d.outcome, Outcome::kRefused);
  EXPECT_EQ(d.reason, "no_content_json");
}

TEST_F(ContentPack, AChangedPackFileIsRefused) {
  const std::wstring dir = MakePack("testpack");
  WriteFileBytes(dir + L"\\packages\\48037dc70b0ecab2_1", "changed");
  const Decision d = Decide(In());
  EXPECT_EQ(d.outcome, Outcome::kRefused);
  EXPECT_EQ(d.reason, "file_changed");
  std::string out;
  EXPECT_FALSE(nevr_content::ServeOpen(GamePath(data_ + L"\\manifests\\48037dc70b0ecab2").c_str(), 1, &out));
}

TEST_F(ContentPack, AnEmptyPackFileIsRefused) {
  const std::wstring dir = MakePack("testpack", true, [](nlohmann::json& j) {
    j["files"]["packages/48037dc70b0ecab2_1"] = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";  // sha-256 of no bytes
  });
  WriteFileBytes(dir + L"\\packages\\48037dc70b0ecab2_1", "");
  const Decision d = Decide(In());
  EXPECT_EQ(d.outcome, Outcome::kRefused);
  EXPECT_EQ(d.reason, "file_empty");
}

TEST_F(ContentPack, AMissingPackFileIsRefused) {
  const std::wstring dir = MakePack("testpack");
  ASSERT_TRUE(DeleteFileW((dir + L"\\scripts\\190ed14fe6c74dd3.dll").c_str()));
  const Decision d = Decide(In());
  EXPECT_EQ(d.outcome, Outcome::kRefused);
  EXPECT_EQ(d.reason, "file_missing");
}

TEST_F(ContentPack, AStockManifestThePackWasNotBuiltFromIsRefused) {
  MakePack("testpack");
  WriteFileBytes(data_ + L"\\manifests\\48037dc70b0ecab2", "another stock build");
  const Decision d = Decide(In());
  EXPECT_EQ(d.outcome, Outcome::kRefused);
  EXPECT_EQ(d.reason, "stock_mismatch");
}

TEST_F(ContentPack, ASignedOnlyRunRefusesAPackWithScriptsAndServesOneWithout) {
  MakePack("testpack");
  Inputs in = In();
  in.signed_only = true;
  Decision d = Decide(in);
  EXPECT_EQ(d.outcome, Outcome::kRefused);
  EXPECT_EQ(d.reason, "signed_only");
  Remove(content_ + L"\\testpack");
  MakePack("datapack", /*with_script=*/false);
  d = Decide(in);
  EXPECT_EQ(d.outcome, Outcome::kServed) << d.reason;
  EXPECT_EQ(d.scripts, 0u);
}

TEST_F(ContentPack, ScriptsOnAnotherGameBuildAreRefusedAndDataStillServes) {
  MakePack("testpack");
  Inputs in = In();
  in.game_supported = false;
  Decision d = Decide(in);
  EXPECT_EQ(d.outcome, Outcome::kRefused);
  EXPECT_EQ(d.reason, "unsupported_game");
  Remove(content_ + L"\\testpack");
  MakePack("datapack", /*with_script=*/false);
  EXPECT_EQ(Decide(in).outcome, Outcome::kServed);
}

TEST_F(ContentPack, AMalformedContentJsonIsRefusedWithItsReason) {
  MakePack("testpack", true, [](nlohmann::json& j) { j["manifests"][0]["packages"] = {"packages/48037dc70b0ecab2_0"}; });
  const Decision d = Decide(In());
  EXPECT_EQ(d.outcome, Outcome::kRefused);
  EXPECT_EQ(d.reason, "bad_package");  // _0 is a stock package: a pack never stands in for one
}

// content.json's own rules, without files.
struct JsonCase {
  const char* name;
  std::function<void(nlohmann::json&)> edit;
  const char* reason;
};

nlohmann::json GoodJson() {
  const std::string h(64, 'a');
  nlohmann::json j = {{"format", 1}, {"id", "pack"}, {"version", "1.0.0"}, {"data_dir", "_data/5932408047/rad15/win10"}};
  j["manifests"] = nlohmann::json::array({{{"package", kSet}, {"stock_sha256", h}, {"stock_packages", 3},
                                           {"file", "manifests/48037dc70b0ecab2"}, {"packages", {"packages/48037dc70b0ecab2_3"}}}});
  j["scripts"] = {"scripts/190ed14fe6c74dd3.dll"};
  j["files"] = {{"manifests/48037dc70b0ecab2", h}, {"packages/48037dc70b0ecab2_3", h}, {"scripts/190ed14fe6c74dd3.dll", h}};
  return j;
}

TEST(ContentJson, AWellFormedDescriptorParses) {
  nevr_content::Descriptor d;
  std::string reason, detail;
  ASSERT_TRUE(nevr_content::ParseContentJson(GoodJson().dump(), "pack", &d, &reason, &detail)) << reason << " " << detail;
  EXPECT_EQ(d.manifests.size(), 1u);
  EXPECT_EQ(d.manifests[0].stock_packages, 3u);
  EXPECT_EQ(d.manifests[0].packages, std::vector<std::string>{"packages/48037dc70b0ecab2_3"});
  EXPECT_EQ(d.scripts, std::vector<std::string>{"scripts/190ed14fe6c74dd3.dll"});
  EXPECT_EQ(d.files.size(), 3u);
}

TEST(ContentJson, EachBrokenRuleIsRefusedByName) {
  const std::string h(64, 'b');
  const std::vector<JsonCase> cases = {
      {"format 2", [](nlohmann::json& j) { j["format"] = 2; }, "bad_format"},
      {"id upper case", [](nlohmann::json& j) { j["id"] = "Pack"; }, "bad_id"},
      {"id dotted", [](nlohmann::json& j) { j["id"] = ".."; }, "bad_id"},
      {"id of another folder", [](nlohmann::json& j) { j["id"] = "other"; }, "id_mismatch"},
      {"no version", [](nlohmann::json& j) { j.erase("version"); }, "bad_version"},
      {"no files", [](nlohmann::json& j) { j["files"] = nlohmann::json::object(); }, "no_files"},
      {"a path that climbs out", [&](nlohmann::json& j) { j["files"]["../x"] = h; }, "bad_file_entry"},
      {"an absolute path", [&](nlohmann::json& j) { j["files"]["/x"] = h; }, "bad_file_entry"},
      {"a backslash path", [&](nlohmann::json& j) { j["files"]["scripts\\x"] = h; }, "bad_file_entry"},
      {"a short sha", [](nlohmann::json& j) { j["files"]["manifests/48037dc70b0ecab2"] = "abc"; }, "bad_file_entry"},
      {"two paths one file", [&](nlohmann::json& j) { j["files"]["Manifests/48037dc70b0ecab2"] = h; }, "bad_file_entry"},
      {"data_dir climbs out", [](nlohmann::json& j) { j["data_dir"] = "../_data"; }, "bad_data_dir"},
      {"no manifests", [](nlohmann::json& j) { j["manifests"] = nlohmann::json::array(); }, "no_manifests"},
      {"a set that is not 16 hex", [](nlohmann::json& j) { j["manifests"][0]["package"] = "48037DC70B0ECAB2"; }, "bad_manifest"},
      {"no stock count", [](nlohmann::json& j) { j["manifests"][0].erase("stock_packages"); }, "bad_manifest"},
      {"a manifest file not listed", [](nlohmann::json& j) { j["manifests"][0]["file"] = "manifests/other"; }, "unlisted_file"},
      {"the same set twice", [](nlohmann::json& j) { j["manifests"].push_back(j["manifests"][0]); }, "duplicate_manifest"},
      {"a stock package number", [](nlohmann::json& j) { j["manifests"][0]["packages"] = {"packages/48037dc70b0ecab2_2"}; },
       "bad_package"},
      {"a package of another set", [](nlohmann::json& j) { j["manifests"][0]["packages"] = {"packages/2b47aab238f60515_3"}; },
       "bad_package"},
      {"a package number with a zero first", [](nlohmann::json& j) { j["manifests"][0]["packages"] = {"packages/48037dc70b0ecab2_03"}; },
       "bad_package"},
      {"a package not listed", [](nlohmann::json& j) { j["manifests"][0]["packages"] = {"packages/48037dc70b0ecab2_4"}; },
       "unlisted_file"},
      {"a script not named by its hash", [&](nlohmann::json& j) {
         j["scripts"] = {"scripts/evil.dll"};
         j["files"]["scripts/evil.dll"] = h;
       }, "bad_script"},
      {"a script named by a non-hex name of the right length", [&](nlohmann::json& j) {
         j["scripts"] = {"scripts/zzzzzzzzzzzzzzzz.dll"};
         j["files"]["scripts/zzzzzzzzzzzzzzzz.dll"] = h;
       }, "bad_script"},
      {"a script outside scripts/", [&](nlohmann::json& j) {
         j["scripts"] = {"other/190ed14fe6c74dd3.dll"};
         j["files"]["other/190ed14fe6c74dd3.dll"] = h;
       }, "bad_script"},
      {"a script not listed", [](nlohmann::json& j) { j["scripts"] = {"scripts/190ed14fe6c74dd4.dll"}; }, "unlisted_file"},
      {"the same script twice", [](nlohmann::json& j) { j["scripts"].push_back(j["scripts"][0]); }, "duplicate_script"},
  };
  for (const JsonCase& c : cases) {
    nlohmann::json j = GoodJson();
    c.edit(j);
    nevr_content::Descriptor d;
    std::string reason, detail;
    EXPECT_FALSE(nevr_content::ParseContentJson(j.dump(), "pack", &d, &reason, &detail)) << c.name;
    EXPECT_EQ(reason, c.reason) << c.name;
  }
  nevr_content::Descriptor d;
  std::string reason, detail;
  EXPECT_FALSE(nevr_content::ParseContentJson("{not json", "pack", &d, &reason, &detail));
  EXPECT_EQ(reason, "not_json");
  EXPECT_FALSE(nevr_content::ParseContentJson(std::string(nevr_content::kMaxContentJsonBytes + 1, ' '), "pack", &d, &reason, &detail));
  EXPECT_EQ(reason, "too_large");
}

TEST(PackRules, TheSharedNamingRules) {
  EXPECT_TRUE(nevr_content::IsPackId("echocombat"));
  EXPECT_TRUE(nevr_content::IsPackId("0pack.v2_x-y"));
  EXPECT_FALSE(nevr_content::IsPackId(""));
  EXPECT_FALSE(nevr_content::IsPackId("-pack"));
  EXPECT_FALSE(nevr_content::IsPackId("Pack"));
  EXPECT_FALSE(nevr_content::IsPackId(std::string(65, 'a')));
  EXPECT_TRUE(nevr_content::IsPackId(std::string(64, 'a')));
  EXPECT_TRUE(nevr_content::IsPlainName("48037dc70b0ecab2_3"));
  EXPECT_FALSE(nevr_content::IsPlainName(".hidden"));
  EXPECT_FALSE(nevr_content::IsPlainName("a b"));
  EXPECT_FALSE(nevr_content::IsPlainName(std::string(65, 'a')));
  EXPECT_TRUE(nevr_content::IsLowerHex("0123456789abcdef", 16));
  EXPECT_FALSE(nevr_content::IsLowerHex("0123456789ABCDEF", 16));
  EXPECT_FALSE(nevr_content::IsLowerHex("0123", 16));
}

}  // namespace
