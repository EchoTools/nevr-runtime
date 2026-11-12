// CDN_DESIGN Q2b: the `_lowspec` level vectors, the Quest's session pin check (map_pin.h) and the POSIX stock source
// (stock_source_posix.h) over a stock tree written under NEVR_TEST_TMP. No game file is read.
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zstd.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "nevr/symbol_hash.h"
#include "quest/cargo/map_pin.h"
#include "quest/cargo/stock_source_posix.h"
#include "quest/tests/test_check.h"

namespace {

namespace pin = nevr_quest::maps::pin;

std::string Tmp() {
  const char* dir = std::getenv("NEVR_TEST_TMP");
  return std::string(dir != nullptr ? dir : "/tmp") + "/map_quest";
}

void MakeDirs(const std::string& path) {
  for (std::size_t at = 1; at <= path.size(); ++at) {
    if (at == path.size() || path[at] == '/') ::mkdir(path.substr(0, at).c_str(), 0700);
  }
}

void WriteFile(const std::string& path, const std::vector<std::uint8_t>& bytes) {
  const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
  QCHECK(fd >= 0);
  QCHECK(::write(fd, bytes.data(), bytes.size()) == static_cast<ssize_t>(bytes.size()));
  ::close(fd);
}

void Put(std::vector<std::uint8_t>* out, std::size_t at, std::uint64_t value, unsigned width) {
  for (unsigned i = 0; i < width; ++i) (*out)[at + i] = static_cast<std::uint8_t>(value >> (8 * i));
}

std::vector<std::uint8_t> Compress(const std::vector<std::uint8_t>& in) {
  std::vector<std::uint8_t> out(ZSTD_compressBound(in.size()));
  const std::size_t n = ZSTD_compress(out.data(), out.size(), in.data(), in.size(), 3);
  QCHECK(!ZSTD_isError(n));
  out.resize(n);
  return out;
}

struct Rec {
  std::uint64_t type, name;
  std::uint32_t offset, size;
};

// A one-package manifest (docs/reference/package-manifest-format.md) whose records all sit in chunk 0 of `compressed` bytes.
std::vector<std::uint8_t> Manifest(const std::vector<Rec>& recs, std::uint32_t chunk_bytes, std::uint32_t compressed) {
  std::vector<std::uint8_t> body(0xc0 + recs.size() * 0x20 + 0x10, 0);
  Put(&body, 0, 1, 4);
  Put(&body, 0x38, recs.size(), 8);
  Put(&body, 0x78, 0, 8);
  Put(&body, 0xb8, 1, 8);
  for (std::size_t i = 0; i < recs.size(); ++i) {
    const std::size_t at = 0xc0 + i * 0x20;
    Put(&body, at, recs[i].type, 8);
    Put(&body, at + 8, recs[i].name, 8);
    Put(&body, at + 0x10, 0, 4);
    Put(&body, at + 0x14, recs[i].offset, 4);
    Put(&body, at + 0x18, recs[i].size, 4);
  }
  const std::size_t chunk_at = 0xc0 + recs.size() * 0x20;
  Put(&body, chunk_at, 0, 4);
  Put(&body, chunk_at + 4, 0, 4);
  Put(&body, chunk_at + 8, compressed, 4);
  Put(&body, chunk_at + 12, chunk_bytes, 4);
  const std::vector<std::uint8_t> frame = Compress(body);
  std::vector<std::uint8_t> file(24, 0);
  std::memcpy(file.data(), "ZSTD", 4);
  Put(&file, 4, 0x10, 4);
  Put(&file, 8, body.size(), 8);
  Put(&file, 16, frame.size(), 8);
  file.insert(file.end(), frame.begin(), frame.end());
  return file;
}

std::string g_root;
const char* Root() noexcept { return g_root.empty() ? nullptr : g_root.c_str(); }

void TheLowspecLevelsAreTheGamesNames() {
  // The game's own level table (combat/combat_level.cpp): the plain and the `_lowspec` name of one level.
  QCHECK(EchoVR::CSymbol64Hash("mpl_lobby_b2") == 0xd09afd15b1c75c04ULL);
  QCHECK(EchoVR::CSymbol64Hash("mpl_lobby_b2_lowspec") == 0x05361c73bb73db19ULL);
  QCHECK(EchoVR::CSymbol64Hash("mpl_arena_a_lowspec") == 0x7691d78d414c5701ULL);
  QCHECK(pin::SessionLevelOf("mpl_lobby_b2") == 0xd09afd15b1c75c04ULL);
  QCHECK(pin::LowspecLevelOf("mpl_lobby_b2") == 0x05361c73bb73db19ULL);
  QCHECK(pin::LowspecLevelOf("mpl_arena_a") == 0x7691d78d414c5701ULL);
}

void ThePinIsThePcsAndNamesTheDocument() {
  const pin::Pin p{"arena2", "1.0", "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"};
  QCHECK(pin::PinValue(p) == EchoVR::CSymbol64Hash("arena2@1.0@0123456789abcdef"));  // PC9's test vector
  pin::Pin other = p;
  other.source_sha256 = "f" + p.source_sha256.substr(1);
  QCHECK(pin::PinValue(other) != pin::PinValue(p));
}

void TheDecisionFollowsPcNinesTable() {
  pin::ResetForTest();
  nevr_maps::BootReport boot;
  boot.listed = {{pin::SessionLevelOf("arena2"), "arena2"}, {pin::SessionLevelOf("bad"), "bad"},
                 {pin::SessionLevelOf("gone"), "gone"}, {pin::SessionLevelOf("other"), "other"}};
  nevr_maps::MountedMap staged;
  staged.id = "arena2";
  staged.version = "1.0";
  staged.source_sha256 = std::string(64, 'a');
  boot.staged.push_back(staged);
  boot.not_mounted = {{"bad", "stock_changed"}, {"gone", "derived_unsupported"}};
  pin::SetView(pin::ViewFromBoot(boot, {"arena2", "bad", "gone"}));
  const std::uint64_t level = pin::SessionLevelOf("arena2");
  const std::uint64_t good = pin::PinValue({"arena2", "1.0", staged.source_sha256});

  QCHECK(pin::Decide(0, good).action == pin::Action::kNone);
  QCHECK(pin::Decide(pin::SessionLevelOf("stock_level"), 7).action == pin::Action::kNone);
  QCHECK(pin::Decide(level, 0).reason == "map_unpinned");
  QCHECK(pin::Decide(level, good).action == pin::Action::kNone);                         // staged match
  QCHECK(pin::Decide(pin::LowspecLevelOf("arena2"), good).action == pin::Action::kNone);  // the Quest's own level name
  const pin::Decision version = pin::Decide(level, good + 1);
  QCHECK(version.action == pin::Action::kLeave && version.reason == "version" && version.request_fetch);
  const pin::Decision changed = pin::Decide(pin::SessionLevelOf("bad"), 5);
  QCHECK(changed.reason == "not_mounted" && changed.request_fetch);
  const pin::Decision unsupported = pin::Decide(pin::SessionLevelOf("gone"), 5);
  QCHECK(unsupported.reason == "not_mounted" && !unsupported.request_fetch);
  const pin::Decision unsubscribed = pin::Decide(pin::SessionLevelOf("other"), 5);
  QCHECK(unsubscribed.reason == "not_subscribed" && !unsubscribed.request_fetch);

  pin::SetMounted({{"arena2", "1.1", staged.source_sha256}});  // the mounted pin counts too
  QCHECK(pin::Decide(level, pin::PinValue({"arena2", "1.1", staged.source_sha256})).action == pin::Action::kNone);
  QCHECK(pin::Mounted().size() == 1);
  pin::ResetForTest();
}

struct FakeGame final : pin::GameAccess {
  bool callable = true;
  std::uint64_t lobby = 0x1000, owner_level = 0, owner_pin = 0, main_level = 0, now = 0;
  std::int32_t state = 9;
  bool leave_ok = true;
  int leaves = 0, notices = 0;
  std::string notice;
  std::vector<std::string> reasons;
  bool Callable() override { return callable; }
  std::uint64_t Lobby() override { return lobby; }
  std::int32_t NetState() override { return state; }
  std::uint64_t OwnerSymbol(std::uint64_t, const char* key) override {
    return std::string(key) == pin::kOwnerPinKey ? owner_pin : owner_level;
  }
  std::uint64_t MainLevel() override { return main_level; }
  void WriteNotice(const char*, const char* line) override {
    ++notices;
    notice = line;
  }
  bool ScheduleLeave() override {
    ++leaves;
    return leave_ok;
  }
  std::uint64_t NowMs() override { return now; }
  void LogSkew(const char* reason, const std::string&, const std::string&) override { reasons.push_back(reason); }
};

std::vector<std::string> g_fetched;
void RecordFetch(const std::string& id) { g_fetched.push_back(id); }

void TheClientTickLeavesOncePerSessionAndRetriesForTenSeconds() {
  pin::ResetForTest();
  g_fetched.clear();
  nevr_maps::BootReport boot;
  boot.listed = {{pin::SessionLevelOf("arena2"), "arena2"}};
  nevr_maps::MountedMap staged;
  staged.id = "arena2";
  staged.version = "1.0";
  staged.source_sha256 = std::string(64, 'a');
  boot.staged.push_back(staged);
  pin::SetView(pin::ViewFromBoot(boot, {"arena2"}));
  pin::SetFetchRequest(&RecordFetch);
  const std::uint64_t good = pin::PinValue({"arena2", "1.0", staged.source_sha256});

  FakeGame game;  // before the load: the owner level is present, the main level 0
  game.owner_level = pin::SessionLevelOf("arena2");
  game.owner_pin = good;
  for (int i = 0; i < 100; ++i) pin::TickWith(game);
  QCHECK(game.leaves == 0 && game.notices == 0);

  game.owner_pin = good + 1;
  for (int i = 0; i < 100; ++i) pin::TickWith(game);
  QCHECK(game.leaves == 1 && game.notices == 1 && g_fetched.size() == 1);
  QCHECK(game.notice == "Needs arena2 (not your 1.0); updating for next launch.");

  game.state = 5;  // back in the lobby: the next session is decided again
  pin::TickWith(game);
  game.state = 7;
  game.owner_level = 0;  // after the load: the main level only, the Quest's `_lowspec` name
  game.main_level = pin::LowspecLevelOf("arena2");
  game.leave_ok = false;
  pin::TickWith(game);
  QCHECK(game.leaves == 2);
  for (game.now = 0; game.now <= pin::kLeaveRetryMs + 500; game.now += 100) pin::TickWith(game);
  QCHECK(!game.reasons.empty() && game.reasons.back() == "leave_failed");
  const int after = game.leaves;
  for (int i = 0; i < 10; ++i) pin::TickWith(game);
  QCHECK(game.leaves == after);  // given up: silence for this key

  FakeGame off;
  off.callable = false;
  off.owner_level = pin::SessionLevelOf("arena2");
  off.owner_pin = 5;
  pin::TickWith(off);
  QCHECK(off.leaves == 0 && off.notices == 0);
  pin::ResetForTest();
}

void TheNoticeFitsTheErrorLineAndTheLoginListsTheMaps() {
  pin::ResetForTest();
  const std::string long_id = "m" + std::string(54, 'x');
  for (const char* reason : {"version", "not_subscribed", "not_mounted"}) {
    pin::Decision d;
    d.action = pin::Action::kLeave;
    d.id = long_id;
    d.reason = reason;
    const std::string text = pin::NoticeFor(d, std::string(20, '9'));
    QCHECK(text.size() <= pin::kNoticeBytes);
    QCHECK(text.find('~') != std::string::npos);
  }
  QCHECK(pin::LoginEntries().empty());
  nevr_maps::BootReport boot;
  nevr_maps::MountedMap a;
  nevr_maps::MountedMap b;
  a.id = "alpha";
  b.id = "beta";
  boot.staged = {a, b};
  pin::SetView(pin::ViewFromBoot(boot, {}));
  pin::SetMounted({{"beta", "2", ""}});
  const std::vector<pin::LoginEntry> entries = pin::LoginEntries();
  QCHECK(entries.size() == 2 && entries[0].pin.id == "beta" && entries[0].mounted);
  QCHECK(entries[1].pin.id == "alpha" && !entries[1].mounted);
  pin::ResetForTest();
}

void TheStockSourceReadsTheGamesTreeReadOnly() {
  g_root = Tmp() + "/root";
  const std::string android = g_root + "/_data/5932408047/rad15/android";
  MakeDirs(android + "/manifests");
  MakeDirs(android + "/packages");
  const std::string lib_dir = Tmp() + "/lib";
  MakeDirs(lib_dir);

  std::vector<std::uint8_t> chunk(300);
  for (std::size_t i = 0; i < chunk.size(); ++i) chunk[i] = static_cast<std::uint8_t>(i * 7);
  const std::vector<std::uint8_t> frame = Compress(chunk);
  const std::vector<Rec> recs = {{0x22, 0x9, 100, 50}, {0x11, 0x5, 0, 100}};
  const std::string manifest = android + "/manifests/48037dc70b0ecab2";
  WriteFile(manifest, Manifest(recs, static_cast<std::uint32_t>(chunk.size()), static_cast<std::uint32_t>(frame.size())));
  WriteFile(android + "/packages/48037dc70b0ecab2_0", frame);
  WriteFile(lib_dir + "/lib00000000000000ab.so", {1, 2, 3});

  std::unique_ptr<nevr_maps::StockSource> stock = nevr_quest::maps::MakePosixStockSource(&Root, lib_dir, "");
  std::vector<std::uint8_t> out;
  std::vector<nevr_maps::StockStamp> read_from;
  QCHECK(stock->ReadResource(0x22, 0x9, &out, &read_from));
  QCHECK(out == std::vector<std::uint8_t>(chunk.begin() + 100, chunk.begin() + 150));
  QCHECK(read_from.size() == 2 && read_from[0].path == manifest);
  QCHECK(stock->StampsCurrent(read_from));
  QCHECK(stock->ReadResource(0x11, 0x5, &out, &read_from) && out.size() == 100 && out[3] == 21);
  QCHECK(!stock->ReadResource(0x11, 0x6, &out, &read_from) && out.empty());

  // A pack's manifest replaced the stock one, which it kept as `.stock`: the rebuild reads the kept copy.
  WriteFile(manifest + ".stock", Manifest({{0x33, 0x1, 0, 300}}, static_cast<std::uint32_t>(chunk.size()),
                                          static_cast<std::uint32_t>(frame.size())));
  read_from.clear();
  QCHECK(stock->ReadResource(0x33, 0x1, &out, &read_from) && out == chunk);
  QCHECK(read_from[0].path == manifest + ".stock");
  QCHECK(!stock->ReadResource(0x22, 0x9, &out, &read_from));

  // A stock file that changed after the read is no longer current (must-fail for a stamp that is never compared).
  std::vector<nevr_maps::StockStamp> stamps;
  QCHECK(stock->ReadResource(0x33, 0x1, &out, &stamps));
  std::vector<std::uint8_t> longer = frame;
  longer.push_back(0);
  WriteFile(android + "/packages/48037dc70b0ecab2_0", longer);
  QCHECK(!stock->StampsCurrent(stamps));

  read_from.clear();
  QCHECK(stock->ReadScript(0xab, &out, &read_from) && out == std::vector<std::uint8_t>({1, 2, 3}));
  QCHECK(read_from.size() == 1 && read_from[0].path == lib_dir + "/lib00000000000000ab.so");
  QCHECK(!stock->ReadScript(0xac, &out, &read_from));

  g_root.clear();
  QCHECK(!stock->ReadResource(0x33, 0x1, &out, &read_from));  // no data root yet: nothing is found
}

}  // namespace

int main() {
  TheLowspecLevelsAreTheGamesNames();
  ThePinIsThePcsAndNamesTheDocument();
  TheDecisionFollowsPcNinesTable();
  TheClientTickLeavesOncePerSessionAndRetriesForTenSeconds();
  TheNoticeFitsTheErrorLineAndTheLoginListsTheMaps();
  TheStockSourceReadsTheGamesTreeReadOnly();
  return quest_test::Failures() == 0 ? 0 : 1;
}
