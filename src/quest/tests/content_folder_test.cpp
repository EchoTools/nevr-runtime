// #2312: the content pack installed as a folder on Quest (quest/data/content_folder.h), over the shared decision
// (runtime/content/content_core.h). Every pack and every "stock" file is made by the test in its own scratch directory: an engine
// root with the Android data folder, and a content folder. Nothing of the game's is read.
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "mini_test.h"
#include "data_test_support.h"
#include "quest/data/content_folder.h"
#include "quest/data/sha256.h"
#include "runtime/content/content_core.h"
#include "runtime/tests/content_line_golden.h"

namespace {

using data_test::ReadFile;
using data_test::Scratch;
using nevr_content::Decision;
using nevr_content::LineLevel;
using nevr_content::Outcome;

constexpr const char* kSet = "48037dc70b0ecab2";
constexpr const char* kDataDir = "_data/5932408047/rad15/android";

std::string Sha(const std::string& bytes) { return nevr_quest::data::Sha256Hex(bytes.data(), bytes.size()); }

void MakeDirs(const std::string& path) {
  for (std::size_t i = 1; i <= path.size(); ++i) {
    if (i == path.size() || path[i] == '/') ::mkdir(path.substr(0, i).c_str(), 0755);
  }
}

void Write(const std::string& path, const std::string& bytes) {
  MakeDirs(path.substr(0, path.rfind('/')));
  data_test::WriteFile(path, bytes);
}

// What the decision wrote, and whether the hook was asked to go in.
struct Seen {
  std::vector<std::pair<LineLevel, std::string>> lines;
  int installs = 0;
  const char* refuse = nullptr;  // what the installer answers
  std::string probe;             // a game path the installer looks up while it runs
  bool probe_served = false;     // whether the table served it then
};
Seen* g_seen = nullptr;

void Sink(LineLevel level, const char* line) { g_seen->lines.emplace_back(level, line); }
const char* Installer() {
  ++g_seen->installs;
  // The hook may run the moment it is in: the table it reads must already be there.
  if (!g_seen->probe.empty()) g_seen->probe_served = nevr_quest::content::ServedFor(g_seen->probe.c_str(), 1) != nullptr;
  return g_seen->refuse;
}

// One engine root with the stock manifest and package 0, and a content folder.
struct Rig {
  Scratch scratch;
  Seen seen;
  std::string root, content, data, stock_manifest = "stock manifest bytes";

  Rig() {
    g_seen = &seen;
    nevr_quest::content::Forget();
    root = scratch.Path("files");
    content = nevr_quest::content::ContentDir(root);
    data = root + "/" + kDataDir;
    Write(data + "/manifests/" + kSet, stock_manifest);
    Write(data + "/packages/48037dc70b0ecab2_0", "stock package 0");
    MakeDirs(content);
  }
  ~Rig() {
    nevr_quest::content::Forget();
    g_seen = nullptr;
  }

  // A pack folder with a manifest and one added package (and a script when asked), and its content.json; `edit` changes the
  // JSON first.
  std::string MakePack(const std::string& id, bool with_script = false,
                       const std::function<void(nlohmann::json&)>& edit = nullptr) {
    const std::string dir = content + "/" + id;
    const std::string manifest = "pack manifest", package = "pack package 1", script = "pack script";
    Write(dir + "/manifests/" + kSet, manifest);
    Write(dir + "/packages/48037dc70b0ecab2_1", package);
    nlohmann::json j = {{"format", 1}, {"id", id}, {"version", "1.2.3"}, {"data_dir", kDataDir}};
    j["manifests"] = nlohmann::json::array({{{"package", kSet}, {"stock_sha256", Sha(stock_manifest)}, {"stock_packages", 1},
                                             {"file", std::string("manifests/") + kSet},
                                             {"packages", {"packages/48037dc70b0ecab2_1"}}}});
    j["files"] = {{std::string("manifests/") + kSet, Sha(manifest)}, {"packages/48037dc70b0ecab2_1", Sha(package)}};
    if (with_script) {
      Write(dir + "/scripts/190ed14fe6c74dd3.dll", script);
      j["scripts"] = {"scripts/190ed14fe6c74dd3.dll"};
      j["files"]["scripts/190ed14fe6c74dd3.dll"] = Sha(script);
    }
    if (edit) edit(j);
    Write(dir + "/content.json", j.dump());
    return dir;
  }

  Decision Run(bool data_read_already = false) {
    nevr_quest::content::Config c;
    c.content_dir = content;
    c.engine_root = root;
    c.data_read_already = data_read_already;
    c.install_hook = &Installer;
    c.log = &Sink;
    return nevr_quest::content::Run(c);
  }

  std::string Stock(const std::string& rel) const { return data + "/" + rel; }
};

std::string ReadThrough(const char* served_path) { return served_path != nullptr ? ReadFile(served_path) : std::string(); }

const char* ServedPath(const std::string& game_path, unsigned flags) {
  const nevr_content::Served* s = nevr_quest::content::ServedFor(game_path.c_str(), flags);
  return s != nullptr ? s->serve_path.c_str() : nullptr;
}

bool Contains(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

// The one decision line of a refused pack names the token, at Warning, and the hook was not asked to go in.
void CheckRefused(Rig& rig, const Decision& d, const char* reason) {
  CHECK(d.outcome == Outcome::kRefused);
  CHECK_EQ(d.reason, std::string(reason));
  CHECK_EQ(rig.seen.installs, 0);
  CHECK_EQ(rig.seen.lines.size(), std::size_t{1});
  if (!rig.seen.lines.empty()) {
    CHECK(rig.seen.lines[0].first == LineLevel::kWarning);
    CHECK(Contains(rig.seen.lines[0].second, std::string("[NEVR.CONTENT] content pack refused id=")));
    CHECK(Contains(rig.seen.lines[0].second, std::string("reason=") + reason + " "));
  }
  CHECK(ServedPath(rig.Stock(std::string("manifests/") + kSet), 1) == nullptr);
}

}  // namespace

TEST(NoPackInstalledServesNothing) {
  Rig rig;
  const Decision d = rig.Run();
  CHECK(d.outcome == Outcome::kNone);
  CHECK_EQ(rig.seen.installs, 0);
  CHECK_EQ(rig.seen.lines.size(), std::size_t{1});
  CHECK(rig.seen.lines[0].first == LineLevel::kInfo);
  CHECK_EQ(rig.seen.lines[0].second,
           "[NEVR.CONTENT] no content pack installed dir=" + rig.content + " (the game runs on its stock data)");
  CHECK(ServedPath(rig.Stock(std::string("manifests/") + kSet), 1) == nullptr);
}

TEST(TheContentFolderIsBesideTheSentinelLogs) {
  CHECK_EQ(nevr_quest::content::ContentDir("/sdcard/Android/media/com.readyatdawn.r15/files"),
           std::string("/sdcard/Android/media/com.readyatdawn.r15/files/nevr/content"));
}

TEST(ACheckedPackServesItsManifestAndPackageThroughTheHeldFiles) {
  Rig rig;
  rig.MakePack("echocombat");
  rig.seen.probe = rig.Stock(std::string("manifests/") + kSet);
  const Decision d = rig.Run();
  CHECK(d.outcome == Outcome::kServed);
  CHECK(rig.seen.probe_served);  // the table was published before the hook went in
  CHECK_EQ(d.id, std::string("echocombat"));
  CHECK_EQ(d.manifests, std::size_t{1});
  CHECK_EQ(d.packages, std::size_t{1});
  CHECK_EQ(d.scripts, std::size_t{0});
  CHECK_EQ(d.files_checked, std::size_t{2});
  CHECK_EQ(rig.seen.installs, 1);
  CHECK_EQ(rig.seen.lines.size(), std::size_t{1});
  CHECK(rig.seen.lines[0].first == LineLevel::kInfo);
  CHECK_EQ(rig.seen.lines[0].second,
           "[NEVR.CONTENT] content pack served id=echocombat version=1.2.3 manifests=1 packages=1 scripts=0 files_checked=2 dir=" +
               rig.content +
               " (the game reads these files in place of its stock ones; removing the pack's folder runs the game stock)");
  const char* manifest = ServedPath(rig.Stock(std::string("manifests/") + kSet), 1);
  const char* package = ServedPath(rig.Stock("packages/48037dc70b0ecab2_1"), 1);
  CHECK(manifest != nullptr && std::strncmp(manifest, "/proc/self/fd/", 14) == 0);
  CHECK(package != nullptr && std::strncmp(package, "/proc/self/fd/", 14) == 0);
  CHECK_EQ(ReadThrough(manifest), std::string("pack manifest"));
  CHECK_EQ(ReadThrough(package), std::string("pack package 1"));
}

TEST(OnlyReadsOfThePathsThePackReplacesAreServed) {
  Rig rig;
  rig.MakePack("echocombat");
  CHECK(rig.Run().outcome == Outcome::kServed);
  const std::string manifest = rig.Stock(std::string("manifests/") + kSet);
  CHECK(ServedPath(manifest, 1) != nullptr);
  CHECK(ServedPath(manifest, 1 | 0x20) != nullptr);  // O_SYNC on a read is still a read
  CHECK(ServedPath(manifest, 0) == nullptr);         // not a read
  CHECK(ServedPath(manifest, 2) == nullptr);         // write
  CHECK(ServedPath(manifest, 3) == nullptr);         // read and write
  CHECK(ServedPath(manifest, 1 | 0x4) == nullptr);   // create
  CHECK(ServedPath(manifest, 1 | 0x8) == nullptr);
  CHECK(ServedPath(manifest, 1 | 0x10) == nullptr);  // append
  CHECK(ServedPath(rig.Stock("packages/48037dc70b0ecab2_0"), 1) == nullptr);  // a stock package the pack does not replace
  CHECK(ServedPath(rig.Stock("manifests/2b47aab238f60515"), 1) == nullptr);
  CHECK(nevr_quest::content::ServedFor(nullptr, 1) == nullptr);
}

TEST(TheGameIsHandedTheCheckedFileEvenAfterARenameOverIt) {
  Rig rig;
  const std::string dir = rig.MakePack("echocombat");
  CHECK(rig.Run().outcome == Outcome::kServed);
  // POSIX has no share-deny mode: another file renamed over the pack's path after the check is not what the game gets.
  Write(dir + "/swap", "swapped bytes");
  CHECK_EQ(::rename((dir + "/swap").c_str(), (dir + "/manifests/" + kSet).c_str()), 0);
  CHECK_EQ(ReadFile(dir + "/manifests/" + kSet), std::string("swapped bytes"));
  const char* served = ServedPath(rig.Stock(std::string("manifests/") + kSet), 1);
  CHECK_EQ(ReadThrough(served), std::string("pack manifest"));
  // Each open of the served path has its own offset: a second reader starts at the beginning too.
  const int a = ::open(served, O_RDONLY);
  const int b = ::open(served, O_RDONLY);
  char ca[4] = {}, cb[4] = {};
  CHECK(::read(a, ca, 4) == 4);
  CHECK(::read(b, cb, 4) == 4);
  CHECK_EQ(std::string(ca, 4), std::string("pack"));
  CHECK_EQ(std::string(cb, 4), std::string("pack"));
  ::close(a);
  ::close(b);
}

TEST(TwoPacksAreBothRefused) {
  Rig rig;
  rig.MakePack("echocombat");
  rig.MakePack("other");
  const Decision d = rig.Run();
  CheckRefused(rig, d, "several_packs");
  CHECK_EQ(d.detail, std::string("echocombat,other"));
  CHECK(!rig.seen.lines.empty() && Contains(rig.seen.lines[0].second, "refused id=none reason=several_packs "));  // no one id
}

TEST(APackCheckedAfterTheGameReadItsDataIsRefused) {
  Rig rig;
  rig.MakePack("echocombat");
  CheckRefused(rig, rig.Run(/*data_read_already=*/true), "loaded_too_late");
}

TEST(AFolderWithoutContentJsonIsRefused) {
  Rig rig;
  MakeDirs(rig.content + "/echocombat");
  const Decision d = rig.Run();
  CheckRefused(rig, d, "no_content_json");
  CHECK_EQ(d.detail, rig.content + "/echocombat/content.json");
}

TEST(AMalformedContentJsonIsRefusedWithItsReason) {
  Rig rig;
  rig.MakePack("echocombat", false, [](nlohmann::json& j) { j["format"] = 2; });
  CheckRefused(rig, rig.Run(), "bad_format");
}

TEST(APackWithScriptsIsRefusedWhole) {
  Rig rig;
  rig.MakePack("echocombat", /*with_script=*/true);
  const Decision d = rig.Run();
  CheckRefused(rig, d, "scripts_unsupported");
  CHECK_EQ(d.detail, std::string("the pack carries 1 scripts and this platform loads no script from a pack folder"));
  CHECK(ServedPath(rig.Stock("packages/48037dc70b0ecab2_1"), 1) == nullptr);
}

TEST(AManifestABundledPackReplacedIsRefused) {
  Rig rig;
  rig.MakePack("echocombat");
  // ADR 0029's step kept the stock manifest as <manifest>.stock: the manifest in place is the bundled pack's.
  Write(rig.Stock(std::string("manifests/") + kSet + ".stock"), rig.stock_manifest);
  const Decision d = rig.Run();
  CheckRefused(rig, d, "bundled_pack_placed");
  CHECK_EQ(d.detail, std::string(kSet) + " (a bundled pack has replaced this manifest and kept the stock one as " + kSet + ".stock)");
}

TEST(AStockManifestThePackWasNotBuiltFromIsRefused) {
  Rig rig;
  rig.MakePack("echocombat");
  Write(rig.Stock(std::string("manifests/") + kSet), "another stock manifest");
  const Decision d = rig.Run();
  CheckRefused(rig, d, "stock_mismatch");
  CHECK_EQ(d.detail, std::string(kSet) + " sha256=" + Sha("another stock manifest") + " built_from=" + Sha(rig.stock_manifest));
}

TEST(AMissingStockManifestIsRefusedAsMissing) {
  Rig rig;
  rig.MakePack("echocombat");
  CHECK_EQ(::unlink(rig.Stock(std::string("manifests/") + kSet).c_str()), 0);
  const Decision d = rig.Run();
  CheckRefused(rig, d, "stock_mismatch");
  CHECK_EQ(d.detail, std::string(kSet) + " sha256=missing built_from=" + Sha(rig.stock_manifest));
}

TEST(ANullKeyFindsNothing) {
  std::vector<std::unique_ptr<nevr_content::Served>> table;
  table.push_back(std::make_unique<nevr_content::Served>());
  CHECK(nevr_content::FindKey(table, nullptr) == nullptr);
}

TEST(AChangedPackFileIsRefused) {
  Rig rig;
  const std::string dir = rig.MakePack("echocombat");
  Write(dir + "/packages/48037dc70b0ecab2_1", "changed");
  const Decision d = rig.Run();
  CheckRefused(rig, d, "file_changed");
  CHECK(Contains(d.detail, "packages/48037dc70b0ecab2_1 sha256=" + Sha("changed")));
}

TEST(AMissingPackFileIsRefused) {
  Rig rig;
  const std::string dir = rig.MakePack("echocombat");
  CHECK_EQ(::unlink((dir + "/packages/48037dc70b0ecab2_1").c_str()), 0);
  const Decision d = rig.Run();
  CheckRefused(rig, d, "file_missing");
  CHECK_EQ(d.detail, std::string("packages/48037dc70b0ecab2_1 error=2"));  // ENOENT
}

TEST(AnEmptyPackFileIsRefused) {
  Rig rig;
  const std::string dir = rig.MakePack("echocombat", false, [](nlohmann::json& j) {
    j["files"]["packages/48037dc70b0ecab2_1"] = Sha("");
  });
  Write(dir + "/packages/48037dc70b0ecab2_1", "");
  CheckRefused(rig, rig.Run(), "file_empty");
}

TEST(AHookThatDoesNotGoInRefusesThePack) {
  Rig rig;
  rig.MakePack("echocombat");
  rig.seen.refuse = "slot_not_found";
  const Decision d = rig.Run();
  CHECK(d.outcome == Outcome::kRefused);
  CHECK_EQ(d.reason, std::string("hook_failed"));
  CHECK_EQ(d.detail, std::string("CSysFile::Open slot 0x36f6450 status=slot_not_found"));
  CHECK_EQ(rig.seen.installs, 1);
  CHECK(ServedPath(rig.Stock(std::string("manifests/") + kSet), 1) == nullptr);
  CHECK(rig.seen.lines.size() == 1 && rig.seen.lines[0].first == LineLevel::kWarning);
}

TEST(AServedFilesLineIsInfoFirstDebugAfterAndWarningWhenTheOpenFailed) {
  Rig rig;
  rig.MakePack("echocombat");
  CHECK(rig.Run().outcome == Outcome::kServed);
  const std::string game = rig.Stock(std::string("manifests/") + kSet);
  const nevr_content::Served* s = nevr_quest::content::ServedFor(game.c_str(), 1);
  CHECK(s != nullptr);
  if (s == nullptr) return;
  char line[640];
  CHECK(nevr_quest::content::ServedLine(*s, game.c_str(), 0, line, sizeof(line)) == LineLevel::kInfo);
  CHECK_EQ(std::string(line), "[NEVR.CONTENT] manifest served id=echocombat name=" + std::string(kSet) + " game_path=" + game +
                                  " pack_file=" + rig.content + "/echocombat/manifests/" + kSet + " count=1");
  CHECK(nevr_quest::content::ServedLine(*s, game.c_str(), 0, line, sizeof(line)) == LineLevel::kDebug);
  CHECK(Contains(line, " count=2"));
  CHECK(nevr_quest::content::ServedLine(*s, game.c_str(), 13, line, sizeof(line)) == LineLevel::kWarning);
  CHECK(Contains(line, "manifest open failed id=echocombat") && Contains(line, " result=0xd (the game gets the open's failure)"));
  // The game's result is an int: a negative one is written as its 32 bits, as the register holds it.
  CHECK(nevr_quest::content::ServedLine(*s, game.c_str(), -1, line, sizeof(line)) == LineLevel::kWarning);
  CHECK(Contains(line, " result=0xffffffff ("));
}

TEST(AServedLineWithNoRoomStillCountsTheServe) {
  nevr_content::Served s;
  content_golden::FillServedManifest(&s);
  CHECK(nevr_content::FormatServed(s, "echocombat", content_golden::kGamePath, 0, nullptr, 0) == LineLevel::kDebug);
  char line[640];
  nevr_content::FormatServed(s, "echocombat", content_golden::kGamePath, 0, line, sizeof(line));
  CHECK_EQ(std::string(line), std::string(content_golden::kSecondServe));
}

TEST(AServedLineNamesTheKindOfFile) {
  nevr_content::Served s;
  content_golden::FillServedManifest(&s);
  char line[640];
  for (const auto& [kind, name] : {std::pair{nevr_content::Kind::kManifest, "manifest"},
                                   std::pair{nevr_content::Kind::kPackage, "package"},
                                   std::pair{nevr_content::Kind::kScript, "script"}}) {
    s.kind = kind;
    nevr_content::FormatServed(s, "echocombat", content_golden::kGamePath, 0, line, sizeof(line));
    CHECK(std::strncmp(line, (std::string("[NEVR.CONTENT] ") + name + " served id=").c_str(), 15 + std::strlen(name) + 11) == 0);
  }
}

TEST(PathsAreJoinedWithOneSlash) {
  nevr_quest::content::PosixPackFiles files;
  CHECK_EQ(files.Join("a", "b/c"), std::string("a/b/c"));
  CHECK_EQ(files.Join("a/", "b"), std::string("a/b"));
  CHECK_EQ(files.Join("", "b"), std::string("b"));
}

TEST(AFileBesideThePackFolderIsNoPack) {
  Rig rig;
  rig.MakePack("echocombat");
  Write(rig.content + "/README.txt", "notes");
  const Decision d = rig.Run();
  CHECK(d.outcome == Outcome::kServed);
  CHECK_EQ(d.id, std::string("echocombat"));
}

// A refusal after the files were held leaves none held, for a caller that keeps its PackFiles.
TEST(ARefusedPackHoldsNoFile) {
  const std::vector<std::pair<const char*, std::function<void(Rig&, const std::string&)>>> cases = {
      {"file_missing", [](Rig&, const std::string& dir) { ::unlink((dir + "/packages/48037dc70b0ecab2_1").c_str()); }},
      {"file_empty", [](Rig&, const std::string& dir) { Write(dir + "/packages/48037dc70b0ecab2_1", ""); }},
      {"file_changed", [](Rig&, const std::string& dir) { Write(dir + "/packages/48037dc70b0ecab2_1", "pack package 2"); }},
      {"bundled_pack_placed", [](Rig& r, const std::string&) { Write(r.Stock(std::string("manifests/") + kSet + ".stock"), "s"); }},
      {"stock_mismatch", [](Rig& r, const std::string&) { Write(r.Stock(std::string("manifests/") + kSet), "another build"); }},
  };
  for (const auto& [reason, spoil] : cases) {
    Rig rig;
    spoil(rig, rig.MakePack("echocombat"));
    nevr_content::CoreInputs in;
    in.content_dir = rig.content;
    in.game_dir = rig.root;
    in.refuse_placed_bundle = true;
    nevr_quest::content::PosixPackFiles files;
    std::unique_ptr<nevr_content::Pack> pack;
    const Decision d = nevr_content::Decide(in, files, &pack);
    CHECK_EQ(d.reason, std::string(reason));
    CHECK_EQ(files.HeldCount(), std::size_t{0});
    CHECK(pack == nullptr);
  }
}

// A served pack keeps each checked file open (its manifest and its package) until it is released.
TEST(AServedPackHoldsEachOfItsFiles) {
  Rig rig;
  rig.MakePack("echocombat");
  nevr_content::CoreInputs in;
  in.content_dir = rig.content;
  in.game_dir = rig.root;
  in.refuse_placed_bundle = true;
  nevr_quest::content::PosixPackFiles files;
  std::unique_ptr<nevr_content::Pack> pack;
  const Decision d = nevr_content::Decide(in, files, &pack);
  CHECK_EQ(d.reason, std::string());
  CHECK_EQ(files.HeldCount(), std::size_t{2});
  files.ReleaseAll();
  CHECK_EQ(files.HeldCount(), std::size_t{0});
}

TEST(AFileIsHashedInChunksToTheSameDigestWithoutMovingItsOffset) {
  Scratch scratch;
  std::string bytes;
  for (int i = 0; i < 1000; ++i) bytes += static_cast<char>(i * 7);
  const std::string path = scratch.Path("f");
  data_test::WriteFile(path, bytes);
  const int fd = ::open(path.c_str(), O_RDONLY);
  CHECK(::lseek(fd, 5, SEEK_SET) == 5);
  CHECK_EQ(nevr_quest::content::StreamSha256(fd, 7), Sha(bytes));
  CHECK_EQ(nevr_quest::content::StreamSha256(fd, nevr_quest::content::kDigestChunkBytes), Sha(bytes));
  CHECK(::lseek(fd, 0, SEEK_CUR) == 5);
  CHECK_EQ(nevr_quest::content::StreamSha256(fd, 0), std::string());  // no chunk: no digest, not the empty file's
  ::close(fd);
  CHECK_EQ(nevr_quest::content::StreamSha256(-1, 7), std::string());
}

TEST(TheLinesAreThePcsText) {
  LineLevel level = LineLevel::kDebug;
  CHECK_EQ(nevr_content::FormatDecision(content_golden::ServedDecision(), content_golden::kDir, &level),
           std::string(content_golden::kServed));
  CHECK(level == LineLevel::kInfo);
  CHECK_EQ(nevr_content::FormatDecision(Decision{}, content_golden::kDir, &level), std::string(content_golden::kNone));
  CHECK(level == LineLevel::kInfo);
  CHECK_EQ(nevr_content::FormatDecision(content_golden::RefusedDecision(), content_golden::kDir, &level),
           std::string(content_golden::kRefused));
  CHECK(level == LineLevel::kWarning);
  nevr_content::Served s;
  content_golden::FillServedManifest(&s);
  char line[640];
  CHECK(nevr_content::FormatServed(s, "echocombat", content_golden::kGamePath, 0, line, sizeof(line)) == LineLevel::kInfo);
  CHECK_EQ(std::string(line), std::string(content_golden::kFirstServe));
  CHECK(nevr_content::FormatServed(s, "echocombat", content_golden::kGamePath, 0, line, sizeof(line)) == LineLevel::kDebug);
  CHECK_EQ(std::string(line), std::string(content_golden::kSecondServe));
  CHECK(nevr_content::FormatServed(s, "echocombat", content_golden::kGamePath, 2, line, sizeof(line)) ==
        LineLevel::kWarning);
  CHECK_EQ(std::string(line), std::string(content_golden::kFailedServe));
}

int main(int argc, char** argv) { return mini_test::RunAll(argc, argv); }
