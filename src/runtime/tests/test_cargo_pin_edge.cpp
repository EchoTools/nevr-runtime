// The PC edge of the cargo pin (runtime/content/cargo_pin.h, PLAN-v7 5.5 R11): the refetch marker that row 14 writes through the Env
// (its gate, its path, one write, and the failure Warning), the boot pin's hasher, and the three seams BindFeed binds to a run's config,
// driven in the order cdn_run.cpp calls them in a boot fetch. The Env is the shared fake (quest/tests/cdn_fake_env.h); the game is a
// fake GameAccess behind the tick. No game, no Win32.

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "cdn/cdn_manifest.h"
#include "cdn/cdn_plan.h"
#include "cdn/cdn_run.h"
#include "cdn/cdn_store.h"
#include "core/cargo_session_pin.h"
#include "core/combat_level_class.h"
#include "core/map_session_pin.h"
#include "quest/tests/cdn_fake_env.h"
#include "runtime/content/cargo_pin.h"

namespace nc = nevr_cdn;
namespace ncp = nevr_cargo_pin;
namespace sp = nevr_map_session_pin;

namespace {

constexpr const char* kPc = "windows-x86_64";
constexpr const char* kDir = "cdn-cache";
constexpr const char* kLabel = "combat_content";
constexpr const char* kCauseUndeliverable = "undeliverable";
constexpr const char* kCauseNone = "none";
constexpr const char* kReasonUnchecked = "unchecked";
constexpr const char* kReasonUndeliverable = "undeliverable";
constexpr const char* kCacheEventName = "cdn_boot_state";
constexpr const char* kCacheResultKey = "result";
constexpr const char* kCacheResultFailed = "write_failed";
constexpr const char* kCacheFileKey = "file";
constexpr const char* kCacheFileValue = "boot_refetch";
constexpr std::uint64_t kType1 = 0x991421b41de58370ULL;
constexpr std::uint64_t kName1 = 0x94ba7ca39fc97e1aULL;
constexpr std::uint64_t kType2 = 0x489bb35d53ca50e9ULL;
constexpr std::uint64_t kName2 = 0x3fa6d25bc24a7b01ULL;
constexpr std::uint64_t kEmptyType = 0xe2efe7289d5985b8ULL;
constexpr std::uint64_t kLobby = 0x7000;
constexpr std::uint64_t kNow = 1000;
constexpr std::int32_t kNetInGame = 9;
constexpr std::uint64_t kLevel = nevr_combat_level::kCombatMaps[0];
// The server's pin: not none, not absent, and not the pin of the boot set or the manifest the tests build.
constexpr std::uint64_t kServer = 0x2222222222222222ULL;
constexpr std::size_t kWholeNames = 3;     // the names of the whole-set mount in Whole()
constexpr std::size_t kDigestChars = 64;   // a SHA-256 hex digest's length
constexpr int kFrames = 3;                 // ticks in one session: the decision is made once for the key
constexpr std::size_t kOneWrite = 1;

// A fake Env that also records the path of every WriteFileAtomically, in order (the refetch marker is one of them).
class CountingEnv : public cdn_test::FakeEnv {
 public:
  std::vector<std::string> atomic_writes;
  bool WriteFileAtomically(const std::string& path, const std::string& data) override {
    atomic_writes.push_back(path);
    return cdn_test::FakeEnv::WriteFileAtomically(path, data);
  }
};

// The game behind the tick: a client in a combat session on kLobby, with the server's cargo pin in the owner JSON.
class FakeClient : public sp::GameAccess {
 public:
  std::map<std::string, std::uint64_t> owner;
  std::vector<std::pair<std::string, std::string>> notices;
  std::vector<sp::PinEvent> events;

  bool Callable() override { return true; }
  bool IsServer() override { return false; }
  std::uint64_t Lobby() override { return kLobby; }
  std::int32_t NetState() override { return kNetInGame; }
  std::uint64_t OwnerSymbol(std::uint64_t, const char* key) override {
    const auto it = owner.find(key);
    return it == owner.end() ? 0 : it->second;
  }
  void SetOwnerSymbol(std::uint64_t, const char* key, std::uint64_t value) override { owner[key] = value; }
  std::uint64_t MainLevel() override { return 0; }
  bool WriteNotice(const char* title, const char* line) override {
    notices.emplace_back(title, line);
    return true;
  }
  bool ScheduleLeave() override { return true; }
  std::uint64_t NowMs() override { return kNow; }
  void Event(const sp::PinEvent& event) override { events.push_back(event); }
};

// The game behind the tick on a dedicated server: a lobby whose owner JSON the tick writes the server's cargo pin to.
class FakeServer : public FakeClient {
 public:
  bool IsServer() override { return true; }
};

nc::Entry Ent(std::uint64_t type, std::uint64_t name, char fill) {
  nc::Entry e;
  e.label = kLabel;
  e.platform = kPc;
  e.type = type;
  e.name = name;
  e.sha256 = std::string(kDigestChars, fill);
  return e;
}

nc::EmptyRecord Empty(std::uint64_t type, std::uint64_t name) {
  nc::EmptyRecord r;
  r.label = kLabel;
  r.platform = kPc;
  r.type = type;
  r.name = name;
  return r;
}

// The set the boot mount and the fetch both see: two entries and an empty record, all for the PC.
nc::Manifest Golden() {
  nc::Manifest m;
  m.entries = {Ent(kType1, kName1, 'a'), Ent(kType2, kName2, 'b')};
  m.empties = {Empty(kEmptyType, kName2)};
  return m;
}

nc::Policy PolicyOn() {
  nc::Policy policy;
  policy.overrides[kLabel] = nc::LabelState::kOn;
  return policy;
}

nc::Mounted Whole() {
  nc::Mounted m;
  m.per_label.push_back(nc::LabelMount{kLabel, kWholeNames, 0, 0});
  return m;
}

nc::BootMount BootWith(nc::Manifest manifest, nc::Mounted mounted, const char* fetch) {
  nc::BootMount boot;
  boot.boot = std::move(manifest);
  boot.mounted = std::move(mounted);
  boot.boot_fetch = fetch;
  return boot;
}

std::string Sha(const std::string& bytes) { return cdn_test::Sha256Of(bytes); }

// The run's config the way cdn_start binds it: the policy, the platform, the marker's directory, then BindFeed.
nc::RunConfig BoundConfig(nc::Env* env, bool boot_state) {
  nc::RunConfig config;
  config.dir = kDir;
  config.platform = nc::kWindowsX64;
  config.policy = PolicyOn();
  config.boot_state = boot_state;
  ncp::BindFeed(config, env);
  return config;
}

FakeClient ClientAt(std::uint64_t server_pin) {
  FakeClient game;
  game.owner["level"] = kLevel;
  game.owner[sp::kOwnerCargoPinKey] = server_pin;
  return game;
}

void Tick(FakeClient& game) {
  for (int i = 0; i < kFrames; ++i) sp::TickWith(game);
}

// The cargo leaves the tick made for `reason` (kCargoSkew is written once per leave).
std::size_t CargoSkews(const FakeClient& game, const std::string& reason) {
  std::size_t n = 0;
  for (const sp::PinEvent& e : game.events) {
    if (e.kind == sp::PinEventKind::kCargoSkew && e.reason == reason) ++n;
  }
  return n;
}

const sp::PinEvent* FindCargoSkew(const FakeClient& game) {
  for (const sp::PinEvent& e : game.events) {
    if (e.kind == sp::PinEventKind::kCargoSkew) return &e;
  }
  return nullptr;
}

class CargoPinEdge : public ::testing::Test {
 protected:
  void SetUp() override { sp::ResetForTest(); }
};

}  // namespace

// Test 9 (PLAN-v7 5.5 R11, "row 14 writes the marker once"): a whole set of this run and no manifest yet is row 14. The leave stands, and the
// marker is written once for the key however many frames the session lasts.
TEST_F(CargoPinEdge, RowFourteenWritesTheMarkerOnceAndLeavesUnchecked) {
  CountingEnv env;
  nc::RunConfig config = BoundConfig(&env, /*boot_state=*/true);
  config.on_boot(BootWith(Golden(), Whole(), kCauseNone));
  FakeClient game = ClientAt(kServer);
  Tick(game);

  const std::string marker = nc::BootRefetchPath(kDir);
  EXPECT_EQ(env.atomic_writes, std::vector<std::string>{marker});
  EXPECT_EQ(env.fs_->files.count(marker), kOneWrite);
  EXPECT_EQ(CargoSkews(game, kReasonUnchecked), kOneWrite);
  EXPECT_EQ(game.notices.size(), kOneWrite);
}

// The marker's gate: with this run not writing boot state (the PC sets it in R6b-3), row 14 leaves the same way and writes no file.
TEST_F(CargoPinEdge, WithoutBootStateRowFourteenLeavesAndWritesNoMarker) {
  CountingEnv env;
  nc::RunConfig config = BoundConfig(&env, /*boot_state=*/false);
  config.on_boot(BootWith(Golden(), Whole(), kCauseNone));
  FakeClient game = ClientAt(kServer);
  Tick(game);

  EXPECT_TRUE(env.atomic_writes.empty());
  EXPECT_EQ(CargoSkews(game, kReasonUnchecked), kOneWrite);
}

// A failed marker write is one Warning (the record cdn_run writes for its own boot-state writes), and the leave still happens.
TEST_F(CargoPinEdge, AFailedMarkerWriteIsOneWarningAndTheLeaveStands) {
  CountingEnv env;
  const std::string marker = nc::BootRefetchPath(kDir);
  env.fail_writes.insert(marker);
  nc::RunConfig config = BoundConfig(&env, /*boot_state=*/true);
  config.on_boot(BootWith(Golden(), Whole(), kCauseNone));
  FakeClient game = ClientAt(kServer);
  Tick(game);

  EXPECT_EQ(env.atomic_writes, std::vector<std::string>{marker});
  EXPECT_EQ(env.CountRecords(kCacheEventName), kOneWrite);
  const cdn_test::Record* record = env.FindRecord(kCacheEventName, kCacheResultKey, kCacheResultFailed);
  ASSERT_NE(record, nullptr);
  EXPECT_EQ(record->level, nc::EventLevel::kWarn);
  EXPECT_EQ(record->Get(kCacheFileKey), kCacheFileValue);
  EXPECT_EQ(CargoSkews(game, kReasonUnchecked), kOneWrite);
}

// The boot pin is the SHA-256 of the records of the boot set, taken with the run's hasher: the leave reports it as `local`.
TEST_F(CargoPinEdge, TheBootPinIsTheDigestOfItsRecordsWithTheRunsHasher) {
  CountingEnv env;
  nc::RunConfig config = BoundConfig(&env, /*boot_state=*/false);
  config.on_boot(BootWith(Golden(), Whole(), kCauseNone));
  const std::uint64_t expected = ncp::CargoPinOf(Golden(), nc::kWindowsX64, PolicyOn(), Sha);
  ASSERT_NE(expected, ncp::kCargoPinNone);

  FakeClient game = ClientAt(kServer);
  Tick(game);
  const sp::PinEvent* skew = FindCargoSkew(game);
  ASSERT_NE(skew, nullptr);
  EXPECT_EQ(skew->local, expected);
}

// The three seams, in the order cdn_run.cpp calls them in a boot fetch: the manifest, the fetch's end, then the boot mount. Each seam is
// bound (a missing one leaves its input unset and the decision names another row), and the row is 11a with no marker.
TEST_F(CargoPinEdge, BindFeedBindsTheThreeSeamsInTheBootFetchOrder) {
  CountingEnv env;
  nc::RunConfig config = BoundConfig(&env, /*boot_state=*/true);
  ASSERT_TRUE(config.on_boot);
  ASSERT_TRUE(config.on_manifest);
  ASSERT_TRUE(config.on_end);
  const std::uint64_t golden = ncp::CargoPinOf(Golden(), nc::kWindowsX64, PolicyOn(), Sha);

  config.on_manifest(Golden());
  config.on_end(kCauseUndeliverable);
  config.on_boot(BootWith(nc::Manifest(), nc::Mounted(), kCauseUndeliverable));

  FakeClient game = ClientAt(golden);
  Tick(game);
  const sp::PinEvent* skew = FindCargoSkew(game);
  ASSERT_NE(skew, nullptr);
  EXPECT_EQ(skew->reason, kReasonUndeliverable);
  EXPECT_TRUE(skew->manifest_known);
  EXPECT_EQ(skew->manifest, golden);
  EXPECT_TRUE(env.atomic_writes.empty());
}

// The server's feed (PLAN-v7 5.5 "Feed", "Server"): BindServerFeed binds on_boot only, and the boot set's pin is what the server's tick
// writes to the lobby's owner JSON. A whole set is its pin, with the run's hasher.
TEST_F(CargoPinEdge, TheServersBootSetIsThePinItsTickWritesToTheLobby) {
  CountingEnv env;
  nc::RunConfig config;
  config.dir = kDir;
  config.platform = nc::kWindowsX64;
  config.policy = PolicyOn();
  ncp::BindServerFeed(config, &env);
  ASSERT_TRUE(config.on_boot);
  EXPECT_FALSE(config.on_manifest);
  EXPECT_FALSE(config.on_end);
  config.on_boot(BootWith(Golden(), Whole(), kCauseNone));
  const std::uint64_t expected = ncp::CargoPinOf(Golden(), nc::kWindowsX64, PolicyOn(), Sha);
  ASSERT_NE(expected, ncp::kCargoPinNone);

  FakeServer game;
  game.owner["level"] = kLevel;
  Tick(game);
  EXPECT_EQ(game.owner[sp::kOwnerCargoPinKey], expected);
}

// A server that holds no whole set (its boot set is empty, or the wait ran out) writes kCargoPinNone, not 0: "this server has no set" is a
// fact a client acts on, and 0 stays "this server predates the pin".
TEST_F(CargoPinEdge, AServerWithNoWholeSetWritesNone) {
  CountingEnv env;
  nc::RunConfig config;
  config.dir = kDir;
  config.platform = nc::kWindowsX64;
  config.policy = PolicyOn();
  ncp::BindServerFeed(config, &env);
  config.on_boot(nc::BootMount());

  FakeServer game;
  game.owner["level"] = kLevel;
  Tick(game);
  EXPECT_EQ(game.owner[sp::kOwnerCargoPinKey], ncp::kCargoPinNone);
}

// The server's feed arms no client decision: a client tick after it makes no cargo decision (no leave, no marker write).
TEST_F(CargoPinEdge, TheServersFeedArmsNoClientDecision) {
  CountingEnv env;
  nc::RunConfig config;
  config.dir = kDir;
  config.platform = nc::kWindowsX64;
  config.policy = PolicyOn();
  config.boot_state = true;
  ncp::BindServerFeed(config, &env);
  config.on_boot(BootWith(Golden(), Whole(), kCauseNone));

  FakeClient game = ClientAt(kServer);
  Tick(game);
  EXPECT_EQ(CargoSkews(game, kReasonUnchecked), std::size_t{0});
  EXPECT_TRUE(game.notices.empty());
  EXPECT_TRUE(env.atomic_writes.empty());
}

// PLAN-v7 5.5 "Consequences" (F7): a server holds a set only while its boot set pins one, and nothing holds a set before the boot set. A client
// never holds one (the server's features read this on a server only).
TEST_F(CargoPinEdge, AServerHoldsASetOnlyWhenItsBootSetPinsOne) {
  CountingEnv env;
  nc::RunConfig config;
  config.dir = kDir;
  config.platform = nc::kWindowsX64;
  config.policy = PolicyOn();
  ncp::BindServerFeed(config, &env);
  EXPECT_FALSE(ncp::ServerHoldsSet());
  config.on_boot(BootWith(Golden(), Whole(), kCauseNone));
  EXPECT_TRUE(ncp::ServerHoldsSet());
}

TEST_F(CargoPinEdge, AClientNeverHoldsASet) {
  CountingEnv env;
  nc::RunConfig config = BoundConfig(&env, /*boot_state=*/false);
  config.on_boot(BootWith(Golden(), Whole(), kCauseNone));
  EXPECT_FALSE(ncp::ServerHoldsSet());
}

// PLAN-v7 5.5 R11 test 11 (G2): a server whose cache is empty at its deadline. Its boot runs once with an empty Mounted: its pin is
// kCargoPinNone, it holds no set, and its tick writes kCargoPinNone with no map pin mounted. A client on a set leaves that server's combat
// session with server_none (row 10).
TEST_F(CargoPinEdge, AServerWithAnEmptyCacheAtItsDeadlineWritesNoneAndAClientWithASetLeaves) {
  CountingEnv server_env;
  nc::RunConfig server;
  server.dir = kDir;
  server.platform = nc::kWindowsX64;
  server.policy = PolicyOn();
  ncp::BindServerFeed(server, &server_env);
  server.on_boot(BootWith(nc::Manifest(), nc::Mounted(), "ceiling"));
  EXPECT_FALSE(ncp::ServerHoldsSet());

  FakeServer host;
  host.owner["level"] = kLevel;
  Tick(host);
  EXPECT_EQ(host.owner[sp::kOwnerCargoPinKey], ncp::kCargoPinNone);

  CountingEnv client_env;
  nc::RunConfig client = BoundConfig(&client_env, /*boot_state=*/false);
  client.on_boot(BootWith(Golden(), Whole(), kCauseNone));
  FakeClient game = ClientAt(ncp::kCargoPinNone);
  Tick(game);
  const sp::PinEvent* skew = FindCargoSkew(game);
  ASSERT_NE(skew, nullptr);
  EXPECT_EQ(skew->reason, "server_none");
}
