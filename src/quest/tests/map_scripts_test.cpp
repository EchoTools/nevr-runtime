// Map scripts on Quest (docs/adr/0048 decision 6): the ELF rules (one must-fail per rule), the map-script table, Register
// against a temporary library directory, and that the stock redirect's answers do not change.
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "core/cargo_format.h"
#include "core/script_image_rules.h"
#include "core/tests/script_image_builders.h"
#include "hook_log.h"
#include "quest/data/sha256.h"
#include "quest/plugin/script_redirect.h"
#include "quest/tests/test_check.h"

namespace {

using namespace nevr_script_image_fixture;

// The byte limit the run gives the table: a device share, set once before the first Register.
constexpr std::uint64_t kTestByteLimit = 1ULL << 20;

// A token from the table, compared without trusting it to be non-null: a null answer is a mismatch that names its check.
bool TokenIs(const char* got, const char* want) { return got != nullptr && std::strcmp(got, want) == 0; }

namespace sr = nevr_quest::script_redirect;
namespace ms = nevr_quest::script_redirect::map_scripts;
using nevr_script_image::CheckElfScript;

std::string g_lines;
sentinel::LogLevel g_level = sentinel::LogLevel::kInfo;  // the level of the last record the sink received
void Capture(sentinel::LogLevel level, const char* line) {
  g_level = level;
  g_lines += line;
  g_lines += '\n';
}

std::string MakeDir() {
  char dir[] = "/tmp/nevr_map_scripts_XXXXXX";
  QCHECK(mkdtemp(dir) != nullptr);
  return dir;
}

std::string WriteLib(const std::string& dir, const char* name, const std::vector<std::uint8_t>& bytes) {
  const std::string path = dir + "/" + name;
  FILE* f = std::fopen(path.c_str(), "wb");
  QCHECK(f != nullptr);
  std::fwrite(bytes.data(), 1, bytes.size(), f);
  std::fclose(f);
  return nevr_quest::data::Sha256Hex(bytes.data(), bytes.size());
}

constexpr std::uint64_t kMap = 0x0123456789abcdefULL;
const char kMapName[] = "lib0123456789abcdef.so";

// The other hashes the checks use, named for what each one tests.
constexpr std::uint64_t kWithdrawnOwnerHash = 2;   // Admit by an owner withdrawn in this process
constexpr std::uint64_t kOverTableHash = 999;      // Admit into a full table
constexpr std::uint64_t kMissingFileHash = 0x1111; // no library file for this hash in the directory
constexpr std::uint64_t kBadElfHash = 0xaa;        // the library file that fails the ELF rules
constexpr std::uint64_t kEmptyFileHash = 1;        // the library file with no bytes
constexpr std::uint64_t kNoHash = 0;               // names no script (bad_hash)
constexpr std::size_t kDigestChars = nevr_cargo::kSha256HexChars;  // a sha-256 in hex, the digest of no file here
const char kBadElfName[] = "lib00000000000000aa.so";  // kBadElfHash's file name
const char kEmptyName[] = "lib0000000000000001.so";   // kEmptyFileHash's file name

void TableRules() {
  ms::ResetForTest();
  QCHECK(ms::Admit("map:a", kMap) == nullptr);
  QCHECK(TokenIs(ms::Admit("map:b", kMap), "duplicate"));
  QCHECK(TokenIs(ms::Admit("map:a", kNoHash), "bad_hash"));
  QCHECK(TokenIs(ms::Admit("", kEmptyFileHash), "bad_owner"));
  QCHECK(ms::Holds("map:a", kMap) && !ms::Holds("map:b", kMap));
  QCHECK(ms::Withdraw("map:a") == 1 && !ms::Holds("map:a", kMap) && ms::HeldCount() == 0);
  QCHECK(TokenIs(ms::Admit("map:a", kWithdrawnOwnerHash), "withdrawn"));
  QCHECK(ms::Admit("map:b", kMap) == nullptr);  // the hash is free for another map
  g_lines.clear();
  ms::NoteLoad(kMapName);
  ms::NoteLoad(kMapName);
  QCHECK(g_lines.find("map_script_served") != std::string::npos);
  QCHECK(g_lines.find("map_script_served") == g_lines.rfind("map_script_served"));
  QCHECK(ms::Withdraw("map:b") == 0 && ms::Holds("map:b", kMap));  // loaded: never pulled
  QCHECK(g_lines.find("withdraw_after_load") != std::string::npos);

  ms::ResetForTest();
  for (std::uint64_t i = 1; i <= ms::kMaxMapScripts; ++i) QCHECK(ms::Admit("map:c", i) == nullptr);
  QCHECK(TokenIs(ms::Admit("map:c", kOverTableHash), "table_full"));
  ms::ResetForTest();
}

void StockRedirectUnchanged() {
  ms::ResetForTest();
  sr::ResetForTest();
  const std::string dir = MakeDir();
  WriteLib(dir, "lib0123456789abcdef_nevr.so", {1});
  QCHECK(sr::RegisterIn(dir.c_str(), "combat", &kMap, 1) == NEVR_SCRIPT_REDIRECT_OK);
  QCHECK(TokenIs(ms::Admit("map:a", kMap), "stock_hash"));
  char buf[sr::kNameCapacity];
  QCHECK(TokenIs(sr::RewriteLoadName(kMapName, buf, sizeof buf), "lib0123456789abcdef_nevr.so"));
  g_lines.clear();
  ms::NoteLoad(kMapName);
  QCHECK(g_lines.empty());  // nothing held: the handler's extra step says nothing
  sr::ResetForTest();
}

// A withdrawn owner that held nothing is barred all the same (as on PC); the marker slot does not hold a hash.
void WithdrawnOwnerWithNothingHeldIsBarred() {
  ms::ResetForTest();
  QCHECK(ms::Withdraw("map:x") == 0);
  QCHECK(TokenIs(ms::Admit("map:x", kMap), "withdrawn"));
  QCHECK(ms::Admit("map:y", kMap) == nullptr);
  QCHECK(ms::HeldCount() == 1);
  ms::ResetForTest();
}

// The device's share is the limit (no fact refuses every script), and a package index's pinned size must match the file.
void DeviceLimitAndPinnedSize() {
  ms::ResetForTest();
  const std::string dir = MakeDir();
  const std::string sha = WriteLib(dir, kMapName, Image{}.Build());
  ms::SetByteLimit(0);
  QCHECK(TokenIs(ms::RegisterIn(dir.c_str(), "map:a", kMap, sha.c_str()), "not_configured"));
  ms::SetByteLimit(kTestByteLimit);
  QCHECK(TokenIs(ms::RegisterIn(dir.c_str(), "map:a", kMap, sha.c_str(), 1), "bad_size"));
  QCHECK(ms::RegisterIn(dir.c_str(), "map:a", kMap, sha.c_str(), Image{}.Build().size()) == nullptr);
  ms::ResetForTest();
}

void RegisterChecksThePackagedFile() {
  ms::ResetForTest();
  const std::string dir = MakeDir();
  const std::string sha = WriteLib(dir, kMapName, Image{}.Build());
  const std::string zeros(kDigestChars, '0');
  QCHECK(TokenIs(ms::RegisterIn(dir.c_str(), "a", kMap, sha.c_str()), "bad_owner"));
  QCHECK(TokenIs(ms::RegisterIn(dir.c_str(), "map:Arena", kMap, sha.c_str()), "bad_owner"));
  QCHECK(TokenIs(ms::RegisterIn(dir.c_str(), "map:a", kMap, "ABC"), "bad_sha256"));
  QCHECK(TokenIs(ms::RegisterIn(dir.c_str(), "map:a", kMissingFileHash, sha.c_str()), "missing"));
  QCHECK(TokenIs(ms::RegisterIn(dir.c_str(), "map:a", kMap, zeros.c_str()), "sha_mismatch"));
  Image bad;
  bad.needed = "liblog.s";
  const std::string badSha = WriteLib(dir, kBadElfName, bad.Build());
  QCHECK(TokenIs(ms::RegisterIn(dir.c_str(), "map:a", kBadElfHash, badSha.c_str()), "elf_needed"));
  QCHECK(g_level == sentinel::LogLevel::kWarn);  // a refused script is a Warning, as on PC
  QCHECK(g_lines.find("\"reason\":\"elf_needed\"") != std::string::npos);
  // The shared rules, in the shared order: a zero hash, an empty file and an over-long owner are refused before the table.
  QCHECK(TokenIs(ms::RegisterIn(dir.c_str(), "map:a", kNoHash, sha.c_str()), "bad_hash"));
  const std::string emptySha = WriteLib(dir, kEmptyName, {});
  QCHECK(TokenIs(ms::RegisterIn(dir.c_str(), "map:a", kEmptyFileHash, emptySha.c_str()), "bad_size"));
  const std::string longOwner = "map:" + std::string(ms::kOwnerCapacity, 'x');
  QCHECK(TokenIs(ms::RegisterIn(dir.c_str(), longOwner.c_str(), kMap, sha.c_str()), "bad_owner"));
  QCHECK(ms::HeldCount() == 0);
  QCHECK(ms::RegisterIn(dir.c_str(), "map:a", kMap, sha.c_str()) == nullptr);
  QCHECK(ms::Holds("map:a", kMap));
  ms::ResetForTest();
}

}  // namespace

int main() {
  sentinel::SetLogSink(&Capture);
  ms::SetByteLimit(kTestByteLimit);
  TableRules();
  StockRedirectUnchanged();
  RegisterChecksThePackagedFile();
  WithdrawnOwnerWithNothingHeldIsBarred();
  DeviceLimitAndPinnedSize();
  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "map_scripts_test: %d check(s) failed\n", quest_test::Failures());
    return 1;
  }
  std::printf("map_scripts_test: all checks pass\n");
  return 0;
}
