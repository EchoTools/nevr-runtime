// Host test of the chassisfork module (src/quest/plugins/q15t/src/chassisfork.cpp): the fork naming rule, the level classifier and
// the scene rule, the package-table search over a fake sorted table, the scene tick over a fake CR15Game with a fake clock, and the
// BeginModelSwap wrapper installed on a fake slot page (real mprotect, real /proc/self/maps) and called through the slot.
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "chassisfork.h"
#include "module_switch.h"
#include "nevr/plugin_interface.h"
#include "plugin_log.h"

static int g_fail = 0;
#define CHECK(cond, ...)                               \
  do {                                                 \
    if (!(cond)) {                                     \
      ++g_fail;                                        \
      std::printf("FAIL %s:%d: ", __FILE__, __LINE__); \
      std::printf(__VA_ARGS__);                        \
      std::printf("\n");                               \
    }                                                  \
  } while (0)

struct Line {
  uint32_t level;
  std::string text;
};
static std::vector<Line> g_lines;
static void FakeLog(const char* id, uint32_t level, const char* msg, const NevrLogField* f, uint32_t n) {
  std::string t = std::string("plugin=") + id + " " + msg;
  for (uint32_t i = 0; i < n; ++i) t += std::string(" ") + f[i].key + "=" + f[i].value;
  g_lines.push_back({level, t});
}
static int Count(const char* needle, int level = -1) {
  int c = 0;
  for (const auto& l : g_lines)
    if (l.text.find(needle) != std::string::npos && (level < 0 || static_cast<int>(l.level) == level)) ++c;
  return c;
}

// ids used below
constexpr uint64_t kBodyFirst = 0x86a566d23f13a808ull;    // rwd_chassis_body_s11_a, combat model
constexpr uint64_t kBodyLast = 0x1a0dcb36399391a8ull;     // rwd_chassis_shark_a_tropical, combat model
constexpr uint64_t kSocialFirst = 0xbd16ff8db4cf18b8ull;  // rwd_chassis_body_s11_a, social model
constexpr uint64_t kSocialLast = 0xe6dbddbaf8ab6f76ull;   // rwd_chassis_shark_a_tropical, social model
constexpr uint64_t kGlobalMp = 0x3f9915d3001dc28eull;     // neutral
constexpr uint64_t kCombatMap = 0x43e2da7914642604ull;    // combat
constexpr uint64_t kLobby = 0xd09afd15b1c75c04ull;        // mpl_lobby_b2
constexpr uint64_t kArena = 0x1234567890abcdefull;        // not listed: other

// ---- package table: rows of 0x28 bytes, (type, id) sorted as signed values ----
struct Row {
  int64_t type, id;
  uint8_t pad[0x18];
};
static_assert(sizeof(Row) == 0x28, "row size");
static std::vector<Row> g_rows;
static uint8_t g_db[0x80];
static uint8_t g_mgr[0x10];
static uintptr_t g_mgr_ptr = 0;  // stands in for the global
static void AddRow(uint64_t type, uint64_t id) { g_rows.push_back({static_cast<int64_t>(type), static_cast<int64_t>(id), {}}); }
static void PublishTable() {
  std::sort(g_rows.begin(), g_rows.end(), [](const Row& a, const Row& b) { return a.type != b.type ? a.type < b.type : a.id < b.id; });
  const uintptr_t rows = reinterpret_cast<uintptr_t>(g_rows.data());
  const uint64_t n = g_rows.size();
  std::memcpy(g_db + q15t::kPkgRows, &rows, 8);
  std::memcpy(g_db + q15t::kPkgCount, &n, 8);
  const uintptr_t db = reinterpret_cast<uintptr_t>(g_db);
  std::memcpy(g_mgr + q15t::kResDb, &db, 8);
  g_mgr_ptr = reinterpret_cast<uintptr_t>(g_mgr);
}
static void AddFork(uint64_t fork) {
  AddRow(q15t::kTypeScene, fork);
  AddRow(q15t::kTypeMeshList, fork);
  AddRow(q15t::kTypeArchive, fork);
}

// ---- a fake CR15Game with one gamespace table ----
static uint8_t g_game[0x2000];
static uint8_t g_gs[8][0x60];
static uint8_t g_table[8][16];
static void SetLevels(std::vector<uint64_t> levels) {
  std::memset(g_game, 0, sizeof g_game);
  const uintptr_t game = reinterpret_cast<uintptr_t>(g_game);
  for (size_t i = 0; i < levels.size(); ++i) {
    std::memset(g_gs[i], 0, sizeof g_gs[i]);
    std::memcpy(g_gs[i] + q15t::kGsGame, &game, 8);
    std::memcpy(g_gs[i] + q15t::kGsLevel, &levels[i], 8);
    const uintptr_t gs = reinterpret_cast<uintptr_t>(g_gs[i]);
    std::memcpy(g_table[i] + 8, &gs, 8);
  }
  const uintptr_t tab = reinterpret_cast<uintptr_t>(g_table);
  const uint64_t n = levels.size();
  std::memcpy(g_game + q15t::kGameTables[0][0], &tab, 8);
  std::memcpy(g_game + q15t::kGameTables[0][1], &n, 8);
}
static double g_clock = 100.0;
static double FakeNow() { return g_clock; }
static void Tick() { q15t::ChassisForkTick(reinterpret_cast<uintptr_t>(g_game)); }

// ---- the stock BeginModelSwap and the slot page ----
static std::vector<uint64_t> g_swaps;
extern "C" __attribute__((noinline)) uint64_t StockSwap(uintptr_t, uint64_t, uint64_t target) {
  g_swaps.push_back(target);
  return 0;
}
enum : uintptr_t { kSlot = 0x10, kGlobal = 0x18, kWord = 0x20 };
struct World {
  uint8_t* mem = nullptr;
  uintptr_t base = 0;
  q15t::ChassisForkPins pins{};
  q15t::WordPin word{};
};
static World MakeWorld() {
  World w;
  w.mem = static_cast<uint8_t*>(mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
  if (w.mem == MAP_FAILED) std::abort();
  w.base = reinterpret_cast<uintptr_t>(w.mem);
  w.pins = {kSlot, reinterpret_cast<uintptr_t>(&StockSwap) - w.base, kGlobal};
  const uintptr_t stock = reinterpret_cast<uintptr_t>(&StockSwap);
  std::memcpy(w.mem + kSlot, &stock, 8);
  std::memcpy(w.mem + kGlobal, &g_mgr_ptr, 8);
  const uint32_t word = 0xd10183ff;
  std::memcpy(w.mem + kWord, &word, 4);
  w.word = {kWord, word};
  mprotect(w.mem, 4096, PROT_READ);
  return w;
}
static uintptr_t SlotWord(const World& w) { return *reinterpret_cast<const uintptr_t*>(w.mem + kSlot); }
static void Swap(const World& w, uint64_t target) {
  reinterpret_cast<uint64_t (*)(uintptr_t, uint64_t, uint64_t)>(SlotWord(w))(0x1000, 7, target);
}

static std::string g_dir;
static void Touch(const char* name) {
  FILE* f = std::fopen((g_dir + "/" + name).c_str(), "w");
  if (f) std::fclose(f);
}
static void ClearDir() {
  for (const char* n : {"chassisfork.off", "chassisfork_lobby.off", "chassisfork_soc.off"}) unlink((g_dir + "/" + n).c_str());
}
static void Reset() {
  g_lines.clear();
  g_swaps.clear();
  q15t::ResetChassisForkForTest();
  ClearDir();
  g_clock = 100.0;
}
// two looks at the same levels, 0.5 s apart: enough to publish a combat scene
static void Settle(const std::vector<uint64_t>& levels) {
  SetLevels(levels);
  Tick();
  g_clock += 0.6;
  Tick();
}

int main() {
  char tmpl[] = "/tmp/q15t-chf-XXXXXX";
  if (mkdtemp(tmpl) == nullptr) return 2;
  g_dir = tmpl;
  NvrGameContext ctx{};
  ctx.ctx_size = sizeof ctx;
  ctx.plugin_id = "q15t";
  ctx.log = &FakeLog;
  q15t::SetLogContext(&ctx);
  q15t::SetModuleSwitchDirForTest(g_dir.c_str());
  q15t::SetChassisForkClockForTest(&FakeNow);

  // ---------------------------------------------------------------- pins and the fork naming rule
  {
    const q15t::ChassisForkPins& p = q15t::kChassisForkPinned;
    CHECK(p.swap_slot == 0x3700470 && p.swap_fn == 0x1634f7c && p.res_mgr == 0x376ed60, "pins");
    CHECK(sizeof q15t::kChassisForkWords / sizeof q15t::kChassisForkWords[0] == 12, "word pins");
    CHECK(q15t::kPkgRow == 0x28 && q15t::kPkgRows == 0x40 && q15t::kPkgCount == 0x70 && q15t::kResDb == 0x8, "package layout");
    // the content pack's fork keys for the first and last body of each list (CSymbol64 of "<stock hex>_cbt" / "_soc")
    CHECK(q15t::ForkIdCombat(kBodyFirst) == 0xc7325a0293536ea6ull, "combat fork rule %016llx",
          static_cast<unsigned long long>(q15t::ForkIdCombat(kBodyFirst)));
    CHECK(q15t::ForkIdCombat(kBodyLast) == 0x7518fc812521b164ull, "combat fork rule (last)");
    CHECK(q15t::ForkIdSocial(kSocialFirst) == 0xb196816b458abbe9ull, "social fork rule %016llx",
          static_cast<unsigned long long>(q15t::ForkIdSocial(kSocialFirst)));
    CHECK(q15t::ForkIdSocial(kSocialLast) == 0x6f7661712cf3d825ull, "social fork rule (last)");
    CHECK(q15t::ForkFor(kBodyFirst, false) == q15t::ForkIdCombat(kBodyFirst), "combat lookup");
    CHECK(q15t::ForkFor(kSocialFirst, true) == q15t::ForkIdSocial(kSocialFirst), "social lookup");
    CHECK(q15t::ForkFor(kSocialFirst, false) == 0, "social lookup with social off");
    CHECK(q15t::ForkFor(kArena, true) == 0, "unlisted id");
  }

  // ---------------------------------------------------------------- the scene rule
  {
    using q15t::LevelKind;
    CHECK(q15t::ClassifyLevel(kCombatMap, false) == LevelKind::kCombat, "combat map");
    CHECK(q15t::ClassifyLevel(kGlobalMp, false) == LevelKind::kNeutral, "global_mp");
    CHECK(q15t::ClassifyLevel(kLobby, true) == LevelKind::kLobby, "lobby on");
    CHECK(q15t::ClassifyLevel(kLobby, false) == LevelKind::kOther, "lobby off");
    const uint64_t combat[] = {kGlobalMp, kCombatMap}, neutral[] = {kGlobalMp}, mixed[] = {kGlobalMp, kCombatMap, kArena},
                   lobby[] = {kGlobalMp, kLobby};
    CHECK(q15t::SceneIsCombat(combat, 2, false, false), "combat scene");
    CHECK(!q15t::SceneIsCombat(combat, 2, true, false), "a read failure is off");
    CHECK(!q15t::SceneIsCombat(neutral, 1, false, true), "neutral alone is off");
    CHECK(!q15t::SceneIsCombat(mixed, 3, false, true), "an unlisted level makes it off");
    CHECK(q15t::SceneIsCombat(lobby, 2, false, true), "lobby with lobby on");
    CHECK(!q15t::SceneIsCombat(lobby, 2, false, false), "lobby with lobby off");
    CHECK(!q15t::SceneIsCombat(nullptr, 0, false, true), "no level is off");
  }

  // ---------------------------------------------------------------- the package table search (signed order)
  {
    g_rows.clear();
    AddRow(0x8000000000000001ull, 5);  // a negative type sorts first
    AddRow(q15t::kTypeScene, 0xffffffffffffff00ull);
    AddRow(q15t::kTypeScene, 3);
    AddRow(q15t::kTypeArchive, 9);
    PublishTable();
    const uintptr_t global = reinterpret_cast<uintptr_t>(&g_mgr_ptr);
    CHECK(q15t::PackageHas(global, q15t::kTypeScene, 3), "row found");
    CHECK(q15t::PackageHas(global, q15t::kTypeScene, 0xffffffffffffff00ull), "negative id found");
    CHECK(q15t::PackageHas(global, 0x8000000000000001ull, 5), "negative type found");
    CHECK(q15t::PackageHas(global, q15t::kTypeArchive, 9), "archive found");
    CHECK(!q15t::PackageHas(global, q15t::kTypeScene, 4), "absent id");
    CHECK(!q15t::PackageHas(global, q15t::kTypeMeshList, 3), "absent type");
    uintptr_t null_global = 0;
    CHECK(!q15t::PackageHas(reinterpret_cast<uintptr_t>(&null_global), q15t::kTypeScene, 3), "no manager");
  }

  // ---------------------------------------------------------------- refusals
  {
    Reset();
    g_rows.clear();
    PublishTable();
    World w = MakeWorld();
    Touch("chassisfork.off");
    auto r = q15t::InstallChassisForkAt(w.base, w.pins, &w.word, 1);
    CHECK(std::strcmp(r.reason, "off_file") == 0 && SlotWord(w) == reinterpret_cast<uintptr_t>(&StockSwap), "off file");
    CHECK(Count("chassisfork off", NEVR_LOG_INFO) == 1, "off line");
    ClearDir();
    r = q15t::InstallChassisForkAt(0, w.pins, &w.word, 1);
    CHECK(std::strcmp(r.reason, "no_base") == 0, "no base");
    const q15t::WordPin bad = {kWord, 0xd10183fe};
    r = q15t::InstallChassisForkAt(w.base, w.pins, &bad, 1);
    CHECK(std::strcmp(r.reason, "word_pin") == 0 && SlotWord(w) == reinterpret_cast<uintptr_t>(&StockSwap), "word pin");
    Tick();
    CHECK(!q15t::ChassisForkSceneIsCombat(), "a refused install ticks");
  }

  // ---------------------------------------------------------------- the wrapper and the tick
  {
    Reset();
    g_rows.clear();
    AddFork(q15t::ForkIdCombat(kBodyFirst));
    AddFork(q15t::ForkIdSocial(kSocialFirst));
    AddRow(q15t::kTypeScene, q15t::ForkIdCombat(kBodyLast));  // only one of the three keys: not installed
    PublishTable();
    World w = MakeWorld();
    const auto r = q15t::InstallChassisForkAt(w.base, w.pins, &w.word, 1);
    CHECK(r.status == q15t::SwapStatus::kOk && SlotWord(w) == q15t::ChassisForkWrapperForTest(), "install %s", q15t::StatusName(r.status));
    CHECK(Count("q15t_chassisfork_install", NEVR_LOG_INFO) == 1 && Count("lobby=on") == 1 && Count("social=on") == 1, "install event");

    // no scene yet: stock
    Swap(w, kBodyFirst);
    Swap(w, kArena);
    CHECK(g_swaps.size() == 2 && g_swaps[0] == kBodyFirst && g_swaps[1] == kArena, "off scene rewrote");
    CHECK(q15t::ChassisForkGetCounters().passed == 1, "passed counter");

    // one combat look is not enough; the second one 0.5 s later publishes
    SetLevels({kGlobalMp, kCombatMap});
    Tick();
    CHECK(!q15t::ChassisForkSceneIsCombat(), "published after one look");
    g_clock += 0.1;
    Tick();
    CHECK(!q15t::ChassisForkSceneIsCombat(), "looked again before 0.5 s with nothing moved");
    g_clock += 0.5;
    Tick();
    CHECK(q15t::ChassisForkSceneIsCombat(), "not published after two looks");
    CHECK(Count("from=off to=combat") == 1, "scene line");

    g_swaps.clear();
    Swap(w, kBodyFirst);
    Swap(w, kSocialFirst);
    Swap(w, kBodyLast);
    Swap(w, kArena);
    CHECK(g_swaps.size() == 4, "swaps %zu", g_swaps.size());
    if (g_swaps.size() == 4) {
      CHECK(g_swaps[0] == q15t::ForkIdCombat(kBodyFirst), "combat body not forked");
      CHECK(g_swaps[1] == q15t::ForkIdSocial(kSocialFirst), "social model not forked");
      CHECK(g_swaps[2] == kBodyLast, "a fork missing from the pack was used");
      CHECK(g_swaps[3] == kArena, "unlisted id changed");
    }
    q15t::ChassisForkCounters c = q15t::ChassisForkGetCounters();
    CHECK(c.rewritten == 2 && c.closed == 1, "counters %llu %llu", static_cast<unsigned long long>(c.rewritten),
          static_cast<unsigned long long>(c.closed));
    CHECK(Count("combat body loads as its fork", NEVR_LOG_INFO) == 2, "rewrite lines");
    CHECK(Count("not in the installed content pack", NEVR_LOG_WARNING) == 1, "closed line");
    Swap(w, kBodyFirst);
    CHECK(Count("combat body loads as its fork") == 2, "rewrite line repeated on the same level");

    // an unlisted level joins: off at once
    SetLevels({kGlobalMp, kCombatMap, kArena});
    Tick();
    CHECK(!q15t::ChassisForkSceneIsCombat(), "not off at once");
    CHECK(Count("from=combat to=off") == 1, "off line");
    g_swaps.clear();
    Swap(w, kBodyFirst);
    CHECK(g_swaps.size() == 1 && g_swaps[0] == kBodyFirst, "rewrote outside the scene");

    // the lobby counts
    Settle({kGlobalMp, kLobby});
    CHECK(q15t::ChassisForkSceneIsCombat(), "lobby not a fork scene");
    // a table larger than the cap: off
    SetLevels({kGlobalMp, kCombatMap});
    const uint64_t big = 65;
    std::memcpy(g_game + q15t::kGameTables[0][1], &big, 8);
    Tick();
    CHECK(!q15t::ChassisForkSceneIsCombat(), "oversized table kept the scene");
  }

  // ---------------------------------------------------------------- chassisfork_lobby.off and chassisfork_soc.off
  {
    Reset();
    World w = MakeWorld();
    Touch("chassisfork_lobby.off");
    Touch("chassisfork_soc.off");
    q15t::InstallChassisForkAt(w.base, w.pins, &w.word, 1);
    CHECK(Count("lobby=off") == 1 && Count("social=off") == 1, "switch lines");
    Settle({kGlobalMp, kLobby});
    CHECK(!q15t::ChassisForkSceneIsCombat(), "lobby off still publishes the lobby");
    Settle({kGlobalMp, kCombatMap});
    g_swaps.clear();
    Swap(w, kSocialFirst);
    CHECK(g_swaps.size() == 1 && g_swaps[0] == kSocialFirst, "social off still forks");
  }

  std::printf("chassisfork_test: %s (%d failures)\n", g_fail == 0 ? "OK" : "FAILED", g_fail);
  return g_fail == 0 ? 0 : 1;
}
