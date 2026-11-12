// CDN_DESIGN Q2d: a Quest map's route-B scripts, its set and its aliases are granted together or not at all, and the mount
// observer's half (ApplyMount) withdraws what did not mount and arms the rest. The calls replay BootMount's order (scripts, set,
// aliases; Withdraw on a refusal). Every file is a temp file; no game file is read.
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include <zstd.h>

#include "core/config_alias.h"
#include "core/resource_set_registry.h"
#include "nevr/symbol_hash.h"
#include "quest/cargo/map_mount.h"
#include "quest/data/sha256.h"
#include "quest/plugin/script_redirect.h"
#include "quest/sentinel/hook_log.h"
#include "quest/sentinel/map_sets.h"
#include "quest/tests/test_check.h"

std::vector<std::string> g_warnings;

namespace nevr_config_alias {
void PlatformWarn(const char* line) noexcept { g_warnings.emplace_back(line); }
}  // namespace nevr_config_alias

namespace {

namespace rs = nevr_resource_sets;
namespace ms = nevr_quest::map_sets;
namespace scripts = nevr_quest::script_redirect::map_scripts;
namespace qm = nevr_quest::maps;
namespace ca = nevr_config_alias;

constexpr std::uint64_t kScript = 0x0123456789abcdefULL;
constexpr char kScriptName[] = "lib0123456789abcdef.so";
constexpr std::uint64_t kUnheldScript = 0x00000000000000aaULL;
constexpr std::uint64_t kLevel = 0x7691d78d414c5701ULL;  // mpl_arena_a_lowspec
constexpr std::uint64_t kAlias = 0x2222333344445555ULL;  // the map's own <id>_arena_rules
constexpr char kOwner[] = "map:arena";
// The types of stock mp_arena_rules and of the global_mp registry, read from the stock Android manifest 48037dc70b0ecab2.
constexpr std::uint64_t kStockJsonResourceAndroid = 0x921e9761b45fc6e4ULL;
constexpr std::uint64_t kStockJsonConfigCRAndroid = 0x4cccf0f046efb89eULL;

void QuietSink(sentinel::LogLevel, const char*) {}

void PutLe(std::vector<std::uint8_t>& b, std::size_t at, std::uint64_t value, int bytes) {
  if (b.size() < at + bytes) b.resize(at + bytes);
  for (int i = 0; i < bytes; ++i) b[at + i] = static_cast<std::uint8_t>(value >> (8 * i));
}

void PutLe(std::string* out, std::size_t at, unsigned width, std::uint64_t value) {
  for (unsigned i = 0; i < width; ++i) (*out)[at + i] = static_cast<char>(value >> (8 * i));
}

// map_scripts_test's stock-shaped script: an RX load, an RW load holding the dynamic table, DT_HASH with two symbols, the
// strings "\0libc.so\0setup_bindings\0", and setup_bindings a GLOBAL FUNC in the RX load.
std::vector<std::uint8_t> ScriptImage() {
  std::vector<std::uint8_t> b(0x2000, 0);
  std::memcpy(b.data(), "\x7f" "ELF", 4);
  b[4] = 2;
  b[5] = 1;
  PutLe(b, 16, 3, 2);
  PutLe(b, 18, 183, 2);
  PutLe(b, 32, 64, 8);
  PutLe(b, 54, 56, 2);
  PutLe(b, 56, 3, 2);
  const std::uint64_t ph[3][6] = {{1, 5, 0, 0, 0x1000, 0x1000}, {1, 6, 0x1000, 0x1000, 0x1000, 0x1000},
                                  {2, 6, 0x1000, 0x1000, 0x200, 0x200}};
  for (int i = 0; i < 3; ++i) {
    const std::size_t at = 64 + 56 * i;
    PutLe(b, at, ph[i][0], 4);
    PutLe(b, at + 4, ph[i][1], 4);
    PutLe(b, at + 8, ph[i][2], 8);
    PutLe(b, at + 16, ph[i][3], 8);
    PutLe(b, at + 32, ph[i][4], 8);
    PutLe(b, at + 40, ph[i][5], 8);
  }
  std::memcpy(b.data() + 0x101, "libc.so", 7);
  std::memcpy(b.data() + 0x109, "setup_bindings", 14);
  PutLe(b, 0x204, 2, 4);
  PutLe(b, 0x300 + 24, 9, 4);
  b[0x300 + 24 + 4] = 0x12;
  PutLe(b, 0x300 + 24 + 6, 10, 2);
  PutLe(b, 0x300 + 24 + 8, 0x400, 8);
  const std::pair<std::uint64_t, std::uint64_t> dynamic[] = {{1, 1}, {4, 0x200}, {5, 0x100}, {6, 0x300}, {10, 32}};
  std::size_t at = 0x1000;
  for (const auto& d : dynamic) {
    PutLe(b, at, d.first, 8);
    PutLe(b, at + 8, d.second, 8);
    at += 16;
  }
  return b;
}

using Records = std::vector<std::pair<std::pair<std::uint64_t, std::uint64_t>, std::string>>;

// map_sets_test's NEVRRES1 builder: each record in its own zstd chunk, the records sorted by key.
std::string BuildSet(Records records) {
  std::sort(records.begin(), records.end());
  std::vector<std::string> frames;
  for (const auto& r : records) {
    std::string frame(ZSTD_compressBound(r.second.size()), '\0');
    frame.resize(ZSTD_compress(&frame[0], frame.size(), r.second.data(), r.second.size(), 1));
    frames.push_back(frame);
  }
  const std::size_t n = records.size();
  std::string set(48 + n * 0x20 + n * 0x10, '\0');
  std::memcpy(&set[0], "NEVRRES1", 8);
  PutLe(&set, 8, 4, 48);
  PutLe(&set, 0x0c, 4, n);
  PutLe(&set, 0x10, 4, n);
  PutLe(&set, 0x18, 8, 48);
  PutLe(&set, 0x20, 8, 48 + n * 0x20);
  PutLe(&set, 0x28, 8, set.size());
  std::uint64_t at = set.size();
  for (std::size_t i = 0; i < n; ++i) {
    const std::size_t r = 48 + i * 0x20, c = 48 + n * 0x20 + i * 0x10;
    PutLe(&set, r, 8, records[i].first.first);
    PutLe(&set, r + 8, 8, records[i].first.second);
    PutLe(&set, r + 0x10, 4, i);
    PutLe(&set, r + 0x18, 4, records[i].second.size());
    PutLe(&set, r + 0x1c, 4, 16);
    PutLe(&set, c + 4, 4, at);
    PutLe(&set, c + 8, 4, frames[i].size());
    PutLe(&set, c + 12, 4, records[i].second.size());
    at += frames[i].size();
  }
  for (const std::string& f : frames) set += f;
  return set;
}

// An Android level with its archive, the map's alias record typed `alias_type`, and a script record for `script`.
std::string MapSet(std::uint64_t script, std::uint64_t alias_type) {
  const std::uint64_t component = 0x1111222233334444ULL, part = 0x55;
  std::string archive(4 + 4 + 16, '\0');
  PutLe(&archive, 0, 4, 0);
  PutLe(&archive, 4, 4, 1);
  PutLe(&archive, 8, 8, component);
  PutLe(&archive, 16, 8, part);
  return BuildSet({{{rs::kAndroidTypes.archive, kLevel}, archive},
                   {{component, part}, std::string(300, 'c')},
                   {{rs::kAndroidTypes.level_root, kLevel}, std::string(64, 'r')},
                   {{alias_type, kAlias}, std::string("{}")},
                   {{rs::kAndroidTypes.script, script}, std::string(16, 's')}});
}

std::string TempDir() {
  const char* base = std::getenv("NEVR_TEST_TMP");
  std::string dir = std::string(base != nullptr ? base : "/tmp") + "/map_mount_XXXXXX";
  QCHECK(::mkdtemp(&dir[0]) != nullptr);
  return dir;
}

std::string WriteFile(const std::string& path, const void* bytes, std::size_t size) {
  const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
  QCHECK(fd >= 0);
  QCHECK(::write(fd, bytes, size) == static_cast<ssize_t>(size));
  ::close(fd);
  return path;
}

bool StillOpen(int fd) { return ::fcntl(fd, F_GETFD) != -1; }
bool NoStock(void*, std::uint64_t, std::uint64_t) { return false; }
bool LevelIsStock(void*, std::uint64_t type, std::uint64_t name) {
  return type == rs::kAndroidTypes.level_root && name == kLevel;
}

struct Map {
  std::string dir, script_sha;
  std::uint64_t set_script = kScript;  // the script record the set carries
  std::uint64_t alias_key = ca::kArenaRulesKey;
  std::uint64_t second_alias_key = 0;  // a second pair when non-zero
  std::uint64_t alias_type = kStockJsonResourceAndroid;  // the type the set holds its alias record under
};

Map ArenaMap() {
  Map map;
  map.dir = TempDir();
  const std::vector<std::uint8_t> image = ScriptImage();
  WriteFile(map.dir + "/" + kScriptName, image.data(), image.size());
  map.script_sha = nevr_quest::data::Sha256Hex(image.data(), image.size());
  return map;
}

void Reset() {
  ms::ResetForTest();
  scripts::ResetForTest();
  ca::Unstage(kOwner);
  ca::Arm({}, nullptr, nullptr);
  rs::SetScriptWaiver(&qm::HoldsMapScript);
}

// BootMount's calls for one map (map_run.cpp MountOne): the scripts, the set, the aliases; Withdraw on any refusal.
// Returns the first refusal, "" when everything was granted.
std::string MountLikeTheBoot(qm::QuestMounter& mounter, const Map& map) {
  const int script_fd = ::open((map.dir + "/" + kScriptName).c_str(), O_RDONLY);
  QCHECK(script_fd >= 0);
  nevr_maps::Grant grant = mounter.Script(kOwner, kScript, script_fd, map.script_sha);
  if (!grant.ok) {
    ::close(script_fd);
    mounter.Withdraw(kOwner);
    return "script:" + grant.reason;
  }
  QCHECK(!StillOpen(script_fd));
  const std::string bytes = MapSet(map.set_script, map.alias_type);
  const int set_fd = ::open(WriteFile(map.dir + "/set.nevrres", bytes.data(), bytes.size()).c_str(), O_RDONLY);
  QCHECK(set_fd >= 0);
  grant = mounter.Set(kOwner, set_fd, std::string(kOwner) + "@1.0", "", true);
  if (!grant.ok) {
    ::close(set_fd);
    mounter.Withdraw(kOwner);
    return "set:" + grant.reason;
  }
  std::vector<nevr_maps::ConfigAlias> aliases(1);
  aliases[0].key = map.alias_key;
  aliases[0].alias = kAlias;
  if (map.second_alias_key != 0) {
    aliases.emplace_back();
    aliases[1].key = map.second_alias_key;
    aliases[1].alias = kAlias + 1;
  }
  return nevr_maps::StageAliasesOrWithdraw(mounter, kOwner, "arena", kLevel, aliases);
}

void HeldNone() {
  QCHECK(!scripts::Holds(kOwner, kScript));
  QCHECK(!ca::Global().AnyStaged());
  QCHECK(ms::Held() == 0);
}

void TheAliasRecordTypeIsStockArenaRulesType() {
  QCHECK(ca::kAliasRecordTypeAndroid == kStockJsonResourceAndroid);
  QCHECK(EchoVR::CSymbol64Hash("CJsonConfigCRAndroid") == kStockJsonConfigCRAndroid);
}

// A set that holds its alias record under the registry's type (CJsonConfigCR) mounts, but arms no alias, and says so:
// the stock rules are read with a [NEVR.MAPS] alias_missing warning, never silently.
void AnAliasRecordOfTheWrongTypeArmsNothingAndWarns() {
  Reset();
  Map map = ArenaMap();
  map.alias_type = kStockJsonConfigCRAndroid;
  qm::QuestMounter mounter(map.dir);
  QCHECK(MountLikeTheBoot(mounter, map).empty());
  rs::StockView stock;
  stock.has = &NoStock;
  stock.records = 1;
  const rs::MountReport report = ms::Mount(stock, nullptr);
  QCHECK(report.sets == 1 && report.refused == 0);
  g_warnings.clear();
  const qm::MountOutcome outcome = qm::ApplyMount({kOwner}, report);
  QCHECK(outcome.mounted.size() == 1 && outcome.withdrawn.empty());
  QCHECK(outcome.armed == 0);
  QCHECK(ca::Decide(0, {kLevel}).empty());
  const std::string expected = "[NEVR.MAPS] config_alias refused=alias_missing map=arena key=mp_arena_rules";
  QCHECK(std::count_if(g_warnings.begin(), g_warnings.end(),
                       [&](const std::string& line) { return line.find(expected) != std::string::npos; }) == 1);
}

void AMapWithAScriptAndAnAliasMountsAllOfThem() {
  Reset();
  const Map map = ArenaMap();
  qm::QuestMounter mounter(map.dir);
  QCHECK(MountLikeTheBoot(mounter, map).empty());
  QCHECK(scripts::Holds(kOwner, kScript));
  QCHECK(ca::Global().AnyStaged());
  QCHECK(ms::Held() == 1);

  rs::StockView stock;
  stock.has = &NoStock;
  stock.records = 1;
  const rs::MountReport report = ms::Mount(stock, nullptr);
  QCHECK(report.sets == 1 && report.refused == 0);
  const qm::MountOutcome outcome = qm::ApplyMount({kOwner}, report);
  QCHECK(outcome.mounted.size() == 1 && outcome.withdrawn.empty());
  QCHECK(outcome.armed == 1);
  QCHECK(scripts::Holds(kOwner, kScript));
  const std::vector<ca::Winner> winners = ca::Decide(0, {kLevel});
  QCHECK(winners.size() == 1 && winners[0].key == ca::kArenaRulesKey && winners[0].alias == kAlias &&
         winners[0].owner == kOwner);
  QCHECK(ca::Decide(0, {}).empty());  // no level of the map loaded: stock rules
}

void AScriptRefusalWithdrawsTheMap() {
  Reset();
  Map map = ArenaMap();
  map.script_sha = std::string(64, '0');
  qm::QuestMounter mounter(map.dir);
  QCHECK(MountLikeTheBoot(mounter, map) == "script:sha_mismatch");
  HeldNone();
}

void ASetRefusalWithdrawsTheHeldScript() {
  Reset();
  Map map = ArenaMap();
  map.set_script = kUnheldScript;
  qm::QuestMounter mounter(map.dir);
  QCHECK(MountLikeTheBoot(mounter, map) == "set:" + std::string(rs::RefusalName(rs::Refusal::kScript)));
  HeldNone();
}

void AnAliasRefusalWithdrawsTheScriptAndTheSet() {
  Reset();
  Map map = ArenaMap();
  map.alias_key = 0x3333ULL;  // not an allowed key: Stage keeps none
  qm::QuestMounter mounter(map.dir);
  QCHECK(MountLikeTheBoot(mounter, map) == "alias_partial");
  HeldNone();
}

// D8 (OI-4): one of two pairs staged is a refusal, and the map keeps nothing it registered.
void AMapWithOneOfTwoAliasesRefusedHoldsNothing() {
  Reset();
  Map map = ArenaMap();
  map.second_alias_key = 0x4444ULL;  // not an allowed key: Stage keeps 1 of 2
  qm::QuestMounter mounter(map.dir);
  QCHECK(MountLikeTheBoot(mounter, map) == "alias_partial");
  HeldNone();
}

void ASetTheMountRefusesWithdrawsTheScriptAndTheAliases() {
  Reset();
  const Map map = ArenaMap();
  qm::QuestMounter mounter(map.dir);
  QCHECK(MountLikeTheBoot(mounter, map).empty());
  rs::StockView stock;
  stock.has = &LevelIsStock;
  stock.records = 1;
  const rs::MountReport report = ms::Mount(stock, nullptr);
  QCHECK(report.sets == 0 && report.refused == 1);
  const qm::MountOutcome outcome = qm::ApplyMount({kOwner}, report);
  QCHECK(outcome.mounted.empty() && outcome.withdrawn.size() == 1 && outcome.armed == 0);
  HeldNone();
  QCHECK(ca::Decide(0, {kLevel}).empty());
}

void AnAbandonedMountWithdrawsEverything() {
  Reset();
  const Map map = ArenaMap();
  qm::QuestMounter mounter(map.dir);
  QCHECK(MountLikeTheBoot(mounter, map).empty());
  ms::Abandon();
  const qm::MountOutcome outcome = qm::ApplyMount({kOwner}, rs::MountReport{});
  QCHECK(outcome.withdrawn.size() == 1 && outcome.armed == 0);
  HeldNone();
}

}  // namespace

int main() {
  sentinel::SetLogSink(&QuietSink);
  TheAliasRecordTypeIsStockArenaRulesType();
  AnAliasRecordOfTheWrongTypeArmsNothingAndWarns();
  AMapWithAScriptAndAnAliasMountsAllOfThem();
  AScriptRefusalWithdrawsTheMap();
  ASetRefusalWithdrawsTheHeldScript();
  AnAliasRefusalWithdrawsTheScriptAndTheSet();
  AMapWithOneOfTwoAliasesRefusedHoldsNothing();
  ASetTheMountRefusesWithdrawsTheScriptAndTheAliases();
  AnAbandonedMountWithdrawsEverything();
  Reset();
  rs::SetScriptWaiver(nullptr);
  return quest_test::Failures() == 0 ? 0 : 1;
}
