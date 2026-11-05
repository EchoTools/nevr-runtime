// Host tests for the stock-data step as a whole (src/quest/obb/stock_data.cpp, docs/adr/0042): a synthetic store OBB is built
// here (FAT16 image, Twofish-CBC, footer), pinned from the bytes that went in, and extracted into a scratch cache. Covers the
// extraction, the cache hit that does nothing, and every refusal that must leave no cache directory behind.

#include <sys/stat.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "quest/data/sha256.h"
#include "quest/obb/stock_data.h"
#include "quest/tests/mini_test.h"
#include "quest/tests/obb_test_support.h"

namespace fs = std::filesystem;
using namespace nevr_quest::obb;
using nevr_quest::data::Sha256Hex;
using obb_test::Pattern;
using Json = nlohmann::json;

namespace {

struct Rig {
  fs::path dir;
  std::string obb_path, pins_path, cache_root;
  std::vector<std::uint8_t> obb;
  std::string obb_sha;
  std::string pins_text;
  std::vector<std::pair<std::string, std::vector<std::uint8_t>>> files;  // path -> content
  std::vector<Json> lines;
};

std::string Slurp(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}
void Spit(const std::string& path, const std::string& text) {
  std::ofstream out(path, std::ios::binary);
  out << text;
}

std::string FileLine(const std::string& path, const std::vector<std::uint8_t>& content) {
  return "file " + Sha256Hex(content.data(), content.size()) + " " + std::to_string(content.size()) + " " + path + "\n";
}

Rig MakeRig(const std::string& name, const obb_test::ObbSpec& obb_spec = obb_test::ObbSpec(), bool duplicate_name = false) {
  Rig r;
  const char* tmp = std::getenv("NEVR_TEST_TMP");
  r.dir = fs::path(tmp ? tmp : ".") / ("obb-flow-" + name);
  fs::remove_all(r.dir);
  fs::create_directories(r.dir);
  r.obb_path = (r.dir / "store.obb").string();
  r.pins_path = (r.dir / "stock.pin").string();
  r.cache_root = (r.dir / "cache").string();

  const std::string android = "_data/5932408047/rad15/android/";
  r.files = {{android + "manifests/48037dc70b0ecab2", Pattern(3000, 21)},
             {android + "packages/48037dc70b0ecab2_0", Pattern(70000, 22)},
             {android + "manifests/2b47aab238f60515", Pattern(1461, 23)}};
  obb_test::FatSpec spec;
  spec.fragmented = true;
  spec.root.push_back(obb_test::Dir("_data", {obb_test::Dir("5932408047", {obb_test::Dir("rad15", {obb_test::Dir("android", {
      obb_test::Dir("manifests", duplicate_name
          ? std::vector<obb_test::Node>{obb_test::File("48037dc70b0ecab2", r.files[0].second), obb_test::File("48037dc70b0ecab2", r.files[2].second)}
          : std::vector<obb_test::Node>{obb_test::File("48037dc70b0ecab2", r.files[0].second), obb_test::File("2b47aab238f60515", r.files[2].second)}),
      obb_test::Dir("packages", {obb_test::File("48037dc70b0ecab2_0", r.files[1].second)})})})})}));
  r.obb = obb_test::BuildObb(obb_test::BuildFat16(spec).bytes, obb_spec);
  Spit(r.obb_path, std::string(r.obb.begin(), r.obb.end()));
  r.obb_sha = Sha256Hex(r.obb.data(), r.obb.size());
  r.pins_text = "obb_sha256 " + r.obb_sha + "\nobb_size " + std::to_string(r.obb.size()) + "\n";
  for (const auto& f : r.files) r.pins_text += FileLine(f.first, f.second);
  Spit(r.pins_path, r.pins_text);
  return r;
}

StockDataOptions Options(const Rig& r) {
  StockDataOptions o;
  o.obb_path = r.obb_path;
  o.pins_path = r.pins_path;
  o.cache_root = r.cache_root;
  return o;
}

Status Run(Rig& r, const StockDataOptions& options, StockDataResult* result = nullptr) {
  r.lines.clear();
  BuildLog log([&r](const std::string& line) { r.lines.push_back(Json::parse(line)); });
  StockDataResult local;
  return BuildStockData(options, log, result ? result : &local);
}

std::vector<std::string> Events(const Rig& r) {
  std::vector<std::string> events;
  for (const Json& l : r.lines) events.push_back(l.at("event").get<std::string>());
  return events;
}
int Count(const Rig& r, const std::string& event) {
  int n = 0;
  for (const std::string& e : Events(r)) n += e == event ? 1 : 0;
  return n;
}
bool Exists(const std::string& p) { return fs::exists(p); }
std::vector<std::string> CacheEntries(const Rig& r) {
  std::vector<std::string> out;
  if (!fs::exists(r.cache_root)) return out;
  for (const auto& e : fs::directory_iterator(r.cache_root)) out.push_back(e.path().filename().string());
  return out;
}

}  // namespace

TEST(AnObbIsExtractedAndEveryLineIsStructured) {
  Rig r = MakeRig("extract");
  StockDataResult result;
  const Status s = Run(r, Options(r), &result);
  CHECK(s.ok);
  CHECK(!result.cache_hit);
  CHECK_EQ(result.obb_sha256, r.obb_sha);
  CHECK_EQ(result.directory, r.cache_root + "/" + r.obb_sha);
  for (const auto& f : r.files) {
    CHECK_EQ(Slurp(result.directory + "/" + f.first), std::string(f.second.begin(), f.second.end()));
  }
  CHECK_EQ(result.files.size(), 3u);
  CHECK(Exists(result.directory + "/" + kStockDataManifestName));
  CHECK(!Exists(r.cache_root + "/" + r.obb_sha + ".partial"));
  CHECK_EQ(CacheEntries(r).size(), 1u);

  // The log: tag, schema and event on every line, the steps in order, no key.
  for (const Json& l : r.lines) {
    CHECK_EQ(l.at("tag").get<std::string>(), std::string("NEVR.QUESTBUILD"));
    CHECK(l.contains("ts") && l.contains("level") && l.contains("schema"));
    CHECK(l.dump().find("329f4bd7978590e66cdfb8e1c20aeb58") == std::string::npos);
    CHECK(l.dump().find("37c70a1635a1ad7a") == std::string::npos);
  }
  const std::vector<std::string> events = Events(r);
  CHECK_EQ(events.front(), std::string("pins_loaded"));
  CHECK_EQ(events.back(), std::string("stock_data_ready"));
  CHECK_EQ(Count(r, "obb_hashed"), 1);
  CHECK_EQ(Count(r, "obb_footer"), 1);
  CHECK_EQ(Count(r, "image_opened"), 1);
  CHECK_EQ(Count(r, "file_extracted"), 3);
  CHECK_EQ(Count(r, "cache_miss"), 1);
  CHECK_EQ(Count(r, "cache_hit"), 0);
  for (const Json& l : r.lines) {
    if (l.at("event") == "file_extracted") {
      CHECK(l.contains("path") && l.contains("sha256") && l.contains("bytes"));
    }
  }
}

TEST(ASecondRunWithTheSameObbDoesNothing) {
  Rig r = MakeRig("hit");
  CHECK(Run(r, Options(r)).ok);
  const std::string manifest = r.cache_root + "/" + r.obb_sha + "/" + kStockDataManifestName;
  const std::string file = r.cache_root + "/" + r.obb_sha + "/" + r.files[1].first;
  const auto manifest_time = fs::last_write_time(manifest);
  const auto file_time = fs::last_write_time(file);
  StockDataResult result;
  const Status s = Run(r, Options(r), &result);
  CHECK(s.ok);
  CHECK(result.cache_hit);
  CHECK_EQ(Count(r, "cache_hit"), 1);
  CHECK_EQ(Count(r, "file_extracted"), 0);
  CHECK_EQ(Count(r, "obb_footer"), 0);
  CHECK_EQ(Count(r, "image_opened"), 0);
  CHECK(fs::last_write_time(manifest) == manifest_time);
  CHECK(fs::last_write_time(file) == file_time);
  CHECK_EQ(result.files.size(), 3u);
  CHECK_EQ(Events(r).back(), std::string("stock_data_ready"));
}

TEST(AnObbThePinsDoNotNameIsRefusedBeforeTheFooterIsRead) {
  // The pins name another OBB: refused before the footer is read, and the cache is untouched.
  Rig r = MakeRig("wrongobb");
  std::string other = r.pins_text;
  other.replace(other.find(r.obb_sha), 64, std::string(64, '0'));
  Spit(r.pins_path, other);
  const Status s = Run(r, Options(r));
  CHECK(!s.ok);
  CHECK_EQ(s.code, std::string("pin_obb_mismatch"));
  CHECK_EQ(Count(r, "obb_footer"), 0);
  CHECK_EQ(Count(r, "file_extracted"), 0);
  CHECK(CacheEntries(r).empty());
  CHECK_EQ(Events(r).back(), std::string("refused"));
  CHECK_EQ(r.lines.back().at("code").get<std::string>(), std::string("pin_obb_mismatch"));
  CHECK_EQ(r.lines.back().at("level").get<std::string>(), std::string("error"));
}

TEST(ATreeThatDoesNotMatchThePinsLeavesNoCache) {
  Rig r = MakeRig("badpin");
  std::string text = r.pins_text;
  const std::size_t at = text.find("file ") + 5;
  text[at] = text[at] == '0' ? '1' : '0';  // first pinned digest differs
  Spit(r.pins_path, text);
  const Status s = Run(r, Options(r));
  CHECK(!s.ok);
  CHECK_EQ(s.code, std::string("pin_digest_mismatch"));
  CHECK(CacheEntries(r).empty());  // neither <digest> nor <digest>.partial
  CHECK_EQ(r.lines.back().at("code").get<std::string>(), std::string("pin_digest_mismatch"));
}

TEST(AFileThePinsDoNotListLeavesNoCache) {
  Rig r = MakeRig("unlisted");
  std::string text = r.pins_text.substr(0, r.pins_text.rfind("file "));  // drop the last pinned file
  Spit(r.pins_path, text);
  const Status s = Run(r, Options(r));
  CHECK_EQ(s.code, std::string("pin_unlisted_file"));
  CHECK(CacheEntries(r).empty());
}

TEST(APinnedFileTheTreeLacksLeavesNoCache) {
  Rig r = MakeRig("missing");
  Spit(r.pins_path, r.pins_text + FileLine("_data/not/there", Pattern(10, 1)));
  const Status s = Run(r, Options(r));
  CHECK_EQ(s.code, std::string("pin_missing_file"));
  CHECK(CacheEntries(r).empty());
}

TEST(AWrongKeyIsARefusedImageNotAGuess) {
  Rig r = MakeRig("wrongkey");
  StockDataOptions o = Options(r);
  o.key_string = "not-the-game-key";
  const Status s = Run(r, o);
  CHECK(!s.ok);
  CHECK_EQ(s.code, std::string("fat_bad_boot_sector"));  // the decrypted first sector has no 0x55AA mark
  CHECK(CacheEntries(r).empty());
}

TEST(AnUnsaltedObbIsRefusedBeforeAnyDecryption) {
  obb_test::ObbSpec spec;
  spec.flags = 0;
  Rig r = MakeRig("unsalted", spec);
  const Status s = Run(r, Options(r));
  CHECK_EQ(s.code, std::string("obb_unsalted"));
  CHECK_EQ(Count(r, "image_opened"), 0);
  CHECK(CacheEntries(r).empty());
}

TEST(AMissingObbOrPinsFileIsNamed) {
  Rig r = MakeRig("missinginput");
  StockDataOptions o = Options(r);
  o.obb_path = r.dir.string() + "/absent.obb";
  CHECK_EQ(Run(r, o).code, std::string("source_open_failed"));
  o = Options(r);
  o.pins_path = r.dir.string() + "/absent.pin";
  CHECK_EQ(Run(r, o).code, std::string("pins_unreadable"));
  Spit(r.pins_path, "obb_sha256 nothex\n");
  CHECK_EQ(Run(r, Options(r)).code, std::string("pin_syntax"));
  CHECK(CacheEntries(r).empty());
}

TEST(ACacheMissingAFileIsExtractedAgain) {
  Rig r = MakeRig("damaged");
  CHECK(Run(r, Options(r)).ok);
  const std::string dir = r.cache_root + "/" + r.obb_sha;
  fs::remove(dir + "/" + r.files[1].first);
  StockDataResult result;
  CHECK(Run(r, Options(r), &result).ok);
  CHECK(!result.cache_hit);
  CHECK_EQ(Count(r, "cache_invalid"), 1);
  CHECK_EQ(Count(r, "file_extracted"), 3);
  CHECK_EQ(Slurp(dir + "/" + r.files[1].first), std::string(r.files[1].second.begin(), r.files[1].second.end()));
}

TEST(ACacheWithAFileOfAnotherSizeIsExtractedAgain) {
  Rig r = MakeRig("truncated");
  CHECK(Run(r, Options(r)).ok);
  const std::string path = r.cache_root + "/" + r.obb_sha + "/" + r.files[0].first;
  fs::resize_file(path, 100);
  StockDataResult result;
  CHECK(Run(r, Options(r), &result).ok);
  CHECK(!result.cache_hit);
  CHECK_EQ(Count(r, "cache_invalid"), 1);
  CHECK_EQ(fs::file_size(path), 3000u);
}

TEST(ACacheWithAnExtraFileIsExtractedAgain) {
  Rig r = MakeRig("extrafile");
  CHECK(Run(r, Options(r)).ok);
  Spit(r.cache_root + "/" + r.obb_sha + "/_data/stray.bin", "x");
  StockDataResult result;
  CHECK(Run(r, Options(r), &result).ok);
  CHECK(!result.cache_hit);
  CHECK(!Exists(r.cache_root + "/" + r.obb_sha + "/_data/stray.bin"));
}

TEST(RehashFindsAChangeThatKeepsTheSize) {
  Rig r = MakeRig("rehash");
  CHECK(Run(r, Options(r)).ok);
  const std::string path = r.cache_root + "/" + r.obb_sha + "/" + r.files[0].first;
  std::string bytes = Slurp(path);
  bytes[10] = static_cast<char>(bytes[10] ^ 1);
  Spit(path, bytes);
  // Without --rehash the cache is trusted by size: a hit. This is the documented boundary of the cache check.
  StockDataResult hit;
  CHECK(Run(r, Options(r), &hit).ok);
  CHECK(hit.cache_hit);
  StockDataOptions o = Options(r);
  o.rehash_cache = true;
  StockDataResult again;
  CHECK(Run(r, o, &again).ok);
  CHECK(!again.cache_hit);
  CHECK_EQ(Count(r, "cache_invalid"), 1);
  CHECK_EQ(Slurp(path), std::string(r.files[0].second.begin(), r.files[0].second.end()));
}

TEST(RehashOfAGoodCacheIsAHit) {
  Rig r = MakeRig("rehashgood");
  CHECK(Run(r, Options(r)).ok);
  StockDataOptions o = Options(r);
  o.rehash_cache = true;
  StockDataResult result;
  CHECK(Run(r, o, &result).ok);
  CHECK(result.cache_hit);
  CHECK_EQ(Count(r, "cache_hit"), 1);
}

TEST(AStalePartialDirectoryIsRemoved) {
  Rig r = MakeRig("partial");
  fs::create_directories(r.cache_root + "/" + r.obb_sha + ".partial/_data");
  Spit(r.cache_root + "/" + r.obb_sha + ".partial/_data/junk", "junk");
  CHECK(Run(r, Options(r)).ok);
  CHECK_EQ(Count(r, "partial_removed"), 1);
  CHECK(!Exists(r.cache_root + "/" + r.obb_sha + ".partial"));
  CHECK(!Exists(r.cache_root + "/" + r.obb_sha + "/_data/junk"));
}

TEST(HashFileMatchesTheDigestOfItsBytes) {
  Rig r = MakeRig("hashfile");
  std::string sha;
  std::uint64_t size = 0;
  CHECK(HashFile(r.obb_path, &sha, &size).ok);
  CHECK_EQ(sha, r.obb_sha);
  CHECK_EQ(size, static_cast<std::uint64_t>(r.obb.size()));
  CHECK_EQ(HashFile(r.dir.string() + "/absent", &sha, &size).code, std::string("source_open_failed"));
}

TEST(AnImageThatNamesAFileTwiceIsRefused) {
  Rig r = MakeRig("duplicate", obb_test::ObbSpec(), /*duplicate_name=*/true);
  const Status s = Run(r, Options(r));
  CHECK_EQ(s.code, std::string("fat_duplicate_name"));
  CHECK(CacheEntries(r).empty());
}

TEST(ACacheWhoseManifestNamesAnotherObbIsExtractedAgain) {
  Rig r = MakeRig("manifestobb");
  CHECK(Run(r, Options(r)).ok);
  const std::string manifest = r.cache_root + "/" + r.obb_sha + "/" + kStockDataManifestName;
  Json j = Json::parse(Slurp(manifest));
  j["obb_sha256"] = std::string(64, '0');
  Spit(manifest, j.dump());
  StockDataResult result;
  CHECK(Run(r, Options(r), &result).ok);
  CHECK(!result.cache_hit);
  CHECK_EQ(Count(r, "cache_invalid"), 1);
}

TEST(ACacheWhoseManifestDisagreesWithThePinsIsExtractedAgain) {
  Rig r = MakeRig("manifestdigest");
  CHECK(Run(r, Options(r)).ok);
  const std::string manifest = r.cache_root + "/" + r.obb_sha + "/" + kStockDataManifestName;
  Json j = Json::parse(Slurp(manifest));
  j["files"][0]["sha256"] = std::string(64, 'f');
  Spit(manifest, j.dump());
  StockDataResult result;
  CHECK(Run(r, Options(r), &result).ok);
  CHECK(!result.cache_hit);
  CHECK_EQ(Count(r, "cache_invalid"), 1);
}

TEST(ACacheWithoutItsManifestOrWithAnUnreadableOneIsExtractedAgain) {
  Rig r = MakeRig("nomanifest");
  CHECK(Run(r, Options(r)).ok);
  const std::string manifest = r.cache_root + "/" + r.obb_sha + "/" + kStockDataManifestName;
  Spit(manifest, "{ not json");
  StockDataResult result;
  CHECK(Run(r, Options(r), &result).ok);
  CHECK(!result.cache_hit);
  fs::remove(manifest);
  CHECK(Run(r, Options(r), &result).ok);
  CHECK(!result.cache_hit);
  CHECK(Exists(manifest));
}

TEST(ASymbolicLinkInTheCacheIsNotAPinnedFile) {
  Rig r = MakeRig("symlink");
  CHECK(Run(r, Options(r)).ok);
  const std::string file = r.cache_root + "/" + r.obb_sha + "/" + r.files[0].first;
  const std::string copy = r.dir.string() + "/copy.bin";
  fs::copy_file(file, copy);
  fs::remove(file);
  fs::create_symlink(copy, file);
  StockDataResult result;
  CHECK(Run(r, Options(r), &result).ok);
  CHECK(!result.cache_hit);
  CHECK(fs::is_regular_file(fs::symlink_status(file)));
}

int main(int argc, char** argv) { return mini_test::RunAll(argc, argv, std::chrono::seconds(120)); }
