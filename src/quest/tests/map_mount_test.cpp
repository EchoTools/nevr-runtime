// The Quest's map grants (quest/cdn/map_mount.h; docs/adr/0048 decisions 5, 6 and 7): what QuestMounter decides before it calls the
// script table, the map-set registry and the alias table (the signed-only rule, which refusals leave the held descriptor with the
// caller, rekeys and rebuilt scripts refused, the stamp that skips the re-hash, an alias pair the allowlist drops), what Withdraw
// undoes, and what ApplyMount does with the owners the boot granted once the registry has mounted. The shared core's decision rows
// (core/map_channel.h DecideMount) are tested in core/tests; these checks hold the edge's arguments to it. The scripts are checked in a
// temporary library directory (QuestMounter's `library_dir`), the sets are synthetic files under NEVR_TEST_TMP; no game file is read.

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

#include "core/config_alias.h"
#include "core/map_run.h"
#include "core/resource_set_registry.h"
#include "core/tests/script_image_builders.h"
#include "quest/sentinel/hook_log.h"
#include "quest/cdn/map_mount.h"
#include "quest/data/sha256.h"
#include "quest/plugin/script_redirect.h"
#include "quest/sentinel/map_sets.h"
#include "quest/tests/map_set_fixture.h"
#include "quest/tests/test_check.h"

namespace {

namespace rs = nevr_resource_sets;
namespace qm = nevr_quest::maps;
namespace ms = nevr_quest::map_sets;
namespace sr = nevr_quest::script_redirect::map_scripts;
using namespace map_set_fixture;

constexpr mode_t kFileMode = 0600;
constexpr std::uint64_t kScriptByteLimit = 1ULL << 20;  // the device's share the script table is given, as map_scripts_test does
constexpr std::uint64_t kScriptHash = 0x0123456789abcdefULL;
const char kScriptFile[] = "lib0123456789abcdef.so";
constexpr std::uint64_t kAliasName = 0x00000000a11a5001ULL;  // the <id>_arena_rules record a map's set carries
constexpr std::uint64_t kAliasLevel = kLevel;  // the level root the alias set is written for
constexpr std::size_t kSha256HexChars = 64;
const char kAlpha[] = "map:alpha";
const char kBeta[] = "map:beta";

std::vector<std::string> g_lines;
void Sink(sentinel::LogLevel, const char* line) { g_lines.emplace_back(line); }

bool StillOpen(int fd) { return ::fcntl(fd, F_GETFD) != -1; }

std::string TempPath(const char* name) {
  const char* dir = std::getenv("NEVR_TEST_TMP");
  return std::string(dir != nullptr ? dir : "/tmp") + "/map_mount_" + std::to_string(::getpid()) + "_" + name;
}

// A file with `bytes`, opened read-only: the descriptor a grant is handed. The name is unlinked at once; the descriptor keeps the file.
int OpenFileOf(const char* name, const std::string& bytes) {
  const std::string path = TempPath(name);
  const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, kFileMode);
  QCHECK(fd >= 0);
  QCHECK(::write(fd, bytes.data(), bytes.size()) == static_cast<ssize_t>(bytes.size()));
  ::close(fd);
  const int reader = ::open(path.c_str(), O_RDONLY);
  ::unlink(path.c_str());
  return reader;
}

std::string ShaOf(const std::string& bytes) { return nevr_quest::data::Sha256Hex(bytes.data(), bytes.size()); }

// A library directory holding the packaged script lib0123456789abcdef.so, and its digest; the directory is removed with the object.
struct Library {
  std::string dir;
  std::string sha;
  Library(std::string directory, std::string digest) : dir(std::move(directory)), sha(std::move(digest)) {}
  Library(const Library&) = delete;
  Library& operator=(const Library&) = delete;
  ~Library() {
    ::unlink((dir + "/" + kScriptFile).c_str());
    ::rmdir(dir.c_str());
  }
};
Library MakeLibrary() {
  const char* const root = std::getenv("NEVR_TEST_TMP");
  std::string pattern = std::string(root != nullptr ? root : "/tmp") + "/nevr_map_mount_XXXXXX";
  char* const dir = &pattern[0];
  QCHECK(::mkdtemp(dir) != nullptr);
  const std::vector<std::uint8_t> image = nevr_script_image_fixture::Image{}.Build();
  const std::string path = std::string(dir) + "/" + kScriptFile;
  FILE* file = std::fopen(path.c_str(), "wb");
  QCHECK(file != nullptr);
  std::fwrite(image.data(), 1, image.size(), file);
  std::fclose(file);
  return {dir, nevr_quest::data::Sha256Hex(image.data(), image.size())};
}

// A set whose level root is `level`, whose archive lists the arena-rules alias record, and which carries that record.
std::string SetWithAlias(std::uint64_t level) {
  const std::uint64_t alias_type = nevr_config_alias::kAliasRecordTypeAndroid;
  const std::string archive = BuildArchive(alias_type, kAliasName);
  return BuildSet({{{rs::kAndroidTypes.archive, level}, archive},
                   {{alias_type, kAliasName}, std::string(kPartBytes, 'a')},
                   {{rs::kAndroidTypes.level_root, level}, std::string(kRootBytes, 'r')}});
}

bool NoStock(void*, std::uint64_t, std::uint64_t) { return false; }
rs::MountReport MountAll() {
  rs::StockView stock;
  stock.has = &NoStock;
  stock.records = 1;
  return ms::Mount(stock, [](const nevr_shadow::Shadow*) {});
}

void Reset() {
  ms::ResetForTest();
  sr::ResetForTest();
  sr::SetByteLimit(kScriptByteLimit);
  nevr_config_alias::Unstage(kAlpha);
  nevr_config_alias::Unstage(kBeta);
  g_lines.clear();
}

std::vector<nevr_maps::ConfigAlias> ArenaAlias() {
  nevr_maps::ConfigAlias alias;
  alias.key = nevr_config_alias::kArenaRulesKey;
  alias.alias = kAliasName;
  return {alias};
}

// What the tests create they remove: the grant's descriptor outlives its name, and the library directory goes with its object.
void TheTestsLeaveNoFilesBehind() {
  std::string library_dir;
  {
    const Library lib = MakeLibrary();
    library_dir = lib.dir;
    QCHECK(::access(library_dir.c_str(), F_OK) == 0);
  }
  QCHECK(::access(library_dir.c_str(), F_OK) != 0);
  const int fd = OpenFileOf("cleanup", "x");
  QCHECK(fd >= 0 && ::access(TempPath("cleanup").c_str(), F_OK) != 0);
  ::close(fd);
}

// ---- Script (route B) ---------------------------------------------------------------------------------------------------

// ADR 0048 decision 7: under signed-only an unofficial manifest's script is refused whatever the table would say; nothing is
// registered and the descriptor stays with the caller.
void SignedOnlyRefusesAnUnofficialScriptAndKeepsTheDescriptor() {
  Reset();
  const Library lib = MakeLibrary();
  const int fd = OpenFileOf("script_unofficial", "x");
  qm::QuestMounter mounter(/*signed_only=*/true, lib.dir);
  const nevr_maps::Grant grant = mounter.Script(kAlpha, kScriptHash, fd, lib.sha, /*official=*/false);
  QCHECK(!grant.ok && grant.reason == "signed_only");
  QCHECK(!qm::HoldsMapScript(kAlpha, kScriptHash));
  QCHECK(StillOpen(fd));
  ::close(fd);
}

void SignedOnlyAcceptsAnOfficialScript() {
  Reset();
  const Library lib = MakeLibrary();
  const int fd = OpenFileOf("script_official", "x");
  qm::QuestMounter mounter(/*signed_only=*/true, lib.dir);
  QCHECK(mounter.Script(kAlpha, kScriptHash, fd, lib.sha, /*official=*/true).ok);
  QCHECK(qm::HoldsMapScript(kAlpha, kScriptHash));
}

// Route B: the game opens the packaged library itself, so the downloaded copy only carries the pin and is closed on ok. Without
// signed-only the manifest's official flag does not matter.
void AGrantedScriptIsHeldForItsOwnerAndTheDownloadedCopyIsClosed() {
  Reset();
  const Library lib = MakeLibrary();
  const int fd = OpenFileOf("script_ok", "x");
  qm::QuestMounter mounter(/*signed_only=*/false, lib.dir);
  const nevr_maps::Grant grant = mounter.Script(kAlpha, kScriptHash, fd, lib.sha, /*official=*/false);
  QCHECK(grant.ok && grant.reason.empty());
  QCHECK(qm::HoldsMapScript(kAlpha, kScriptHash));
  QCHECK(!qm::HoldsMapScript(kBeta, kScriptHash));
  QCHECK(!StillOpen(fd));
}

// A refusal of the table passes through by its own token, and the descriptor stays with the caller (the core closes it).
void ARefusedScriptKeepsItsDescriptorAndNamesTheTablesToken() {
  Reset();
  const Library lib = MakeLibrary();
  const int fd = OpenFileOf("script_refused", "x");
  qm::QuestMounter mounter(/*signed_only=*/false, lib.dir);
  const std::string wrong_pin(kSha256HexChars, '0');
  const nevr_maps::Grant grant = mounter.Script(kAlpha, kScriptHash, fd, wrong_pin, /*official=*/true);
  QCHECK(!grant.ok && !grant.reason.empty() && grant.reason != "signed_only");
  QCHECK(!qm::HoldsMapScript(kAlpha, kScriptHash));
  QCHECK(StillOpen(fd));
  ::close(fd);
}

// Rekeys and rebuilt scripts need route A or recipes, which the Quest has not: both are refused and nothing is closed or held.
void RekeysAndRebuiltScriptsAreUnsupported() {
  Reset();
  const int fd = OpenFileOf("script_unsupported", "x");
  qm::QuestMounter mounter;
  const nevr_maps::Grant rekey = mounter.Rekey(kAlpha, kScriptHash, kScriptHash, "stock", "dir");
  const nevr_maps::Grant derived = mounter.DerivedScript(kAlpha, kScriptHash, fd, "sha", "stock", /*official=*/true);
  QCHECK(!rekey.ok && rekey.reason == "script_unsupported");
  QCHECK(!derived.ok && derived.reason == "script_unsupported");
  QCHECK(!qm::HoldsMapScript(kAlpha, kScriptHash));
  QCHECK(StillOpen(fd));
  ::close(fd);
}

// ---- Set ----------------------------------------------------------------------------------------------------------------

// The mount's stamp is the one the fetch recorded: a verified stamp skips the re-hash (the file is read, not hashed), and an
// unverified one asks for it. The same wrong pin is a format refusal in the first case and a hash refusal in the second.
void ASetIsRehashedOnlyWhenItsStampWasNotVerified() {
  Reset();
  const std::string wrong_pin(kSha256HexChars, '0');
  const std::string bytes = "abc";
  qm::QuestMounter mounter;
  const int stamped = OpenFileOf("set_stamped", bytes);
  const nevr_maps::Grant skipped = mounter.Set(kAlpha, stamped, "map:alpha@1", wrong_pin, /*verified_stamp=*/true);
  QCHECK(!skipped.ok && skipped.reason == rs::RefusalName(rs::Refusal::kFormat));
  const int unstamped = OpenFileOf("set_unstamped", bytes);
  const nevr_maps::Grant hashed = mounter.Set(kAlpha, unstamped, "map:alpha@1", wrong_pin, /*verified_stamp=*/false);
  QCHECK(!hashed.ok && hashed.reason == rs::RefusalName(rs::Refusal::kHash));
  QCHECK(ms::Held() == 0);
  QCHECK(StillOpen(stamped) && StillOpen(unstamped));  // a refusal leaves the descriptor with the caller
  ::close(stamped);
  ::close(unstamped);
}

void AValidSetIsRegisteredForItsOwnerAndTheRegistryOwnsTheDescriptor() {
  Reset();
  const std::string set = SetWithAlias(kAliasLevel);
  const int fd = OpenFileOf("set_valid", set);
  qm::QuestMounter mounter;
  const nevr_maps::Grant grant = mounter.Set(kAlpha, fd, "map:alpha@1", ShaOf(set), /*verified_stamp=*/true);
  QCHECK(grant.ok && grant.reason.empty());
  QCHECK(ms::Held() == 1);
  QCHECK(StillOpen(fd));
  QCHECK(ms::HoldsKey(rs::kAndroidTypes.level_root, kAliasLevel));
}

// ---- Aliases ------------------------------------------------------------------------------------------------------------

// A map with no alias pairs stages nothing: the alias table is not touched.
void NoAliasesAreGrantedWithoutTouchingTheTable() {
  Reset();
  qm::QuestMounter mounter;
  const std::uint64_t before = nevr_config_alias::Global().Generation();
  QCHECK(mounter.StageAliases(kAlpha, "alpha", kAliasLevel, {}).ok);
  QCHECK(nevr_config_alias::Global().Generation() == before);
}

// A pair the allowlist drops (a key other than mp_arena_rules) is `alias_partial`: the caller then withdraws the map whole.
void ADroppedAliasPairIsAliasPartial() {
  Reset();
  qm::QuestMounter mounter;
  nevr_maps::ConfigAlias not_allowed;
  not_allowed.key = nevr_config_alias::kArenaRulesKey + 1;
  not_allowed.alias = kAliasName;
  const nevr_maps::Grant grant = mounter.StageAliases(kAlpha, "alpha", kAliasLevel, {not_allowed});
  QCHECK(!grant.ok && grant.reason == "alias_partial");
  const nevr_maps::Grant mixed = mounter.StageAliases(kAlpha, "alpha", kAliasLevel, {ArenaAlias()[0], not_allowed});
  QCHECK(!mixed.ok && mixed.reason == "alias_partial");
  nevr_config_alias::Unstage(kAlpha);
}

void AnAllowedAliasPairIsStaged() {
  Reset();
  qm::QuestMounter mounter;
  const std::uint64_t before = nevr_config_alias::Global().Generation();
  const nevr_maps::Grant grant = mounter.StageAliases(kAlpha, "alpha", kAliasLevel, ArenaAlias());
  QCHECK(grant.ok && grant.reason.empty());
  QCHECK(nevr_config_alias::Global().Generation() != before);
  nevr_config_alias::Unstage(kAlpha);
}

// ---- Withdraw -----------------------------------------------------------------------------------------------------------

// Withdraw undoes all three grants of the owner and leaves another owner's alone.
void WithdrawUndoesTheScriptTheSetAndTheAliasesOfOneOwner() {
  Reset();
  const Library lib = MakeLibrary();
  qm::QuestMounter mounter(/*signed_only=*/false, lib.dir);
  const std::string set = SetWithAlias(kAliasLevel);
  const int script_fd = OpenFileOf("withdraw_script", "x");
  const int set_fd = OpenFileOf("withdraw_set", set);
  QCHECK(mounter.Script(kAlpha, kScriptHash, script_fd, lib.sha, true).ok);
  QCHECK(mounter.Set(kAlpha, set_fd, "map:alpha@1", ShaOf(set), true).ok);
  QCHECK(mounter.StageAliases(kAlpha, "alpha", kAliasLevel, ArenaAlias()).ok);
  const std::uint64_t staged = nevr_config_alias::Global().Generation();
  mounter.Withdraw(kBeta);  // another owner: nothing of alpha's moves
  QCHECK(qm::HoldsMapScript(kAlpha, kScriptHash) && ms::Held() == 1 && StillOpen(set_fd));
  mounter.Withdraw(kAlpha);
  QCHECK(!qm::HoldsMapScript(kAlpha, kScriptHash));
  QCHECK(ms::Held() == 0 && !StillOpen(set_fd));
  QCHECK(nevr_config_alias::Global().Generation() != staged);  // Unstage ran for the owner
}

// ---- ApplyMount ---------------------------------------------------------------------------------------------------------

// The owners the boot granted: one mounted with its alias record, one whose set did not mount. The unmounted one is withdrawn whole
// (its script, its aliases) and named; the mounted one keeps its script and arms its alias pair, because the mounted set holds the alias
// record under the Android record type.
void ApplyMountWithdrawsTheOwnersThatDidNotMountAndArmsTheOnesThatDid() {
  Reset();
  const Library lib = MakeLibrary();
  qm::QuestMounter mounter(/*signed_only=*/false, lib.dir);
  const std::string set = SetWithAlias(kAliasLevel);
  const int set_fd = OpenFileOf("apply_set", set);
  QCHECK(mounter.Set(kAlpha, set_fd, "map:alpha@1", ShaOf(set), true).ok);
  QCHECK(mounter.StageAliases(kAlpha, "alpha", kAliasLevel, ArenaAlias()).ok);
  const int script_fd = OpenFileOf("apply_script", "x");
  QCHECK(mounter.Script(kBeta, kScriptHash, script_fd, lib.sha, true).ok);
  QCHECK(mounter.StageAliases(kBeta, "beta", kAliasLevel, ArenaAlias()).ok);
  const rs::MountReport report = MountAll();
  QCHECK(report.sets == 1);
  const qm::MountOutcome outcome = qm::ApplyMount({kAlpha, kBeta}, report);
  QCHECK(outcome.mounted == std::vector<std::string>{kAlpha});
  QCHECK(outcome.withdrawn == std::vector<std::string>{kBeta});
  QCHECK(outcome.armed == 1);
  QCHECK(!qm::HoldsMapScript(kBeta, kScriptHash));  // the withdrawn owner's script is pulled
  nevr_config_alias::Unstage(kAlpha);
}

// A mounted set that does not hold the alias record arms nothing, and the pair is reported missing (the stock key is read).
void ApplyMountArmsNothingForASetWithoutItsAliasRecord() {
  Reset();
  qm::QuestMounter mounter;
  const std::string set = ValidSet(kAliasLevel);  // no alias record in it
  const int fd = OpenFileOf("apply_noalias", set);
  QCHECK(mounter.Set(kAlpha, fd, "map:alpha@1", ShaOf(set), true).ok);
  QCHECK(mounter.StageAliases(kAlpha, "alpha", kAliasLevel, ArenaAlias()).ok);
  const qm::MountOutcome outcome = qm::ApplyMount({kAlpha}, MountAll());
  QCHECK(outcome.mounted.size() == 1 && outcome.withdrawn.empty() && outcome.armed == 0);
  QCHECK(!g_lines.empty() && g_lines.back().find("alias_missing") != std::string::npos);
  nevr_config_alias::Unstage(kAlpha);
}

// The owners' levels are read only when the registry mounted a set: a report of no sets (the run was abandoned, or nothing was
// registered) withdraws every expected owner, even if the registry still holds a set.
void ApplyMountOfAnEmptyReportWithdrawsEveryExpectedOwner() {
  Reset();
  qm::QuestMounter mounter;
  const std::string set = SetWithAlias(kAliasLevel);
  const int fd = OpenFileOf("apply_empty", set);
  QCHECK(mounter.Set(kAlpha, fd, "map:alpha@1", ShaOf(set), true).ok);
  QCHECK(MountAll().sets == 1);
  const std::uint64_t before = nevr_config_alias::Global().Generation();
  const qm::MountOutcome outcome = qm::ApplyMount({kAlpha, kBeta}, rs::MountReport{});
  QCHECK(nevr_config_alias::Global().Generation() != before);  // the withdraw unstaged the owners' aliases
  QCHECK(outcome.mounted.empty());
  QCHECK(outcome.withdrawn == (std::vector<std::string>{kAlpha, kBeta}));
  QCHECK(outcome.armed == 0);
}

// A mounted set that no expected owner names is not the boot's grant: it is neither mounted nor withdrawn here.
void ApplyMountIgnoresAMountedSetOfAnOwnerThatWasNotExpected() {
  Reset();
  qm::QuestMounter mounter;
  const std::string set = SetWithAlias(kAliasLevel);
  const int fd = OpenFileOf("apply_unexpected", set);
  QCHECK(mounter.Set(kAlpha, fd, "map:alpha@1", ShaOf(set), true).ok);
  const qm::MountOutcome outcome = qm::ApplyMount({kBeta}, MountAll());
  QCHECK(outcome.mounted.empty() && outcome.withdrawn == std::vector<std::string>{kBeta} && outcome.armed == 0);
}

}  // namespace

// The alias table's Warning lines go to the sentinel's log on the Quest (quest/sentinel/config_alias_levels.cpp): here to the same sink.
namespace nevr_config_alias {
void PlatformWarn(const char* line) noexcept { g_lines.emplace_back(line); }
}  // namespace nevr_config_alias

int main() {
  sentinel::SetLogSink(&Sink);
  TheTestsLeaveNoFilesBehind();
  SignedOnlyRefusesAnUnofficialScriptAndKeepsTheDescriptor();
  SignedOnlyAcceptsAnOfficialScript();
  AGrantedScriptIsHeldForItsOwnerAndTheDownloadedCopyIsClosed();
  ARefusedScriptKeepsItsDescriptorAndNamesTheTablesToken();
  RekeysAndRebuiltScriptsAreUnsupported();
  ASetIsRehashedOnlyWhenItsStampWasNotVerified();
  AValidSetIsRegisteredForItsOwnerAndTheRegistryOwnsTheDescriptor();
  NoAliasesAreGrantedWithoutTouchingTheTable();
  ADroppedAliasPairIsAliasPartial();
  AnAllowedAliasPairIsStaged();
  WithdrawUndoesTheScriptTheSetAndTheAliasesOfOneOwner();
  ApplyMountWithdrawsTheOwnersThatDidNotMountAndArmsTheOnesThatDid();
  ApplyMountArmsNothingForASetWithoutItsAliasRecord();
  ApplyMountOfAnEmptyReportWithdrawsEveryExpectedOwner();
  ApplyMountIgnoresAMountedSetOfAnOwnerThatWasNotExpected();
  Reset();
  return quest_test::Failures() == 0 ? 0 : 1;
}
