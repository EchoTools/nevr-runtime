// Host test of the level-decals module (src/quest/combat/decals.{h,cpp}, decals_core.h, #2183, #1619). Three layers:
//   1. the table file: the parser's refusals, the uv flips, the blend, the level gate, the index and vertex fill;
//   2. the per-system state machine (decals::Host) over stand-in operations: gating, retries, level changes, shutdown, the war room, and the scope (a game
//      space in a level that is not a combat map or the war room draws and creates nothing, and its level is read once a second, not on every update);
//   3. the real installer and handlers over a stand-in engine: the real GotHook into the GLOB_DAT word of UpdateMovement and the Shutdown vtable word of a libr15
//      image built in memory, a fake bullet system, allocator, node, mesh and vertex buffer laid out as the module reads them. Real SafeRead, real files.
// What moved with the module is every assertion of the plugin's test; what changed is the fixture (the manager's slots, the structured lines, the
// `combat.decals` keys in place of the switch files) and what is new: the scope and the chain above impactsnd's Shutdown handler.
//
// Built and run by `just test-quest-combat`.
#include <dlfcn.h>
#include <sys/mman.h>
#include <unistd.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "nevr/symbol_hash.h"
#include "quest/combat/combat_level.h"
#include "quest/combat/decals.h"
#include "quest/combat/decals_targets.h"
#include "quest/combat/legacy_switch.h"
#include "quest/combat/safe_read.h"
#include "quest/sentinel/hook_install.h"
#include "quest/sentinel/hook_log.h"
#include "quest/tests/combat_libr15_image.h"

static int g_fail = 0;
#define CHECK(cond, ...)                                \
  do {                                                  \
    if (!(cond)) {                                      \
      ++g_fail;                                         \
      std::printf("FAIL %s:%d: ", __FILE__, __LINE__);  \
      std::printf(__VA_ARGS__);                         \
      std::printf("\n");                                \
    }                                                   \
  } while (0)

struct Line {
  int level;  // 0 debug, 1 info, 2 warning, 3 error
  std::string text;
};
static std::vector<Line> g_lines;
static void Capture(sentinel::LogLevel level, const char* line) {
  const int l = level == sentinel::LogLevel::kInfo ? 1 : level == sentinel::LogLevel::kWarn ? 2 : level == sentinel::LogLevel::kError ? 3 : 0;
  g_lines.push_back({l, line});
}
// The plugin's `key=value` needles are matched against the record's fields; its message words against the events that replace them.
static std::string Needle(const char* needle) {
  static const struct {
    const char* from;
    const char* to;
  } kEvents[] = {
      {"decals: game space", "\"event\":\"combat_decals_game_space\""},
      {"level changed under a bullet system", "\"event\":\"combat_decals_level_changed\""},
      {"more bullet systems than the table keeps", "\"event\":\"combat_decals_systems_full\""},
      {"decals: window", "\"event\":\"combat_decals_window\""},
      {"decals on", "\"event\":\"combat_decals_table\""},
      {"decals shutdown wrapper", "\"event\":\"combat_decals_hook\",\"hook\":\"shutdown\""},
      {"decals update wrapper", "\"event\":\"combat_decals_hook\",\"hook\":\"update\""},
      {"decals: node made", "\"event\":\"combat_decals_node\",\"made\":1"},
      {"node not made", "\"event\":\"combat_decals_node\",\"made\":0"},
      {"bullet system shutdown, its nodes are destroyed", "\"event\":\"combat_decals_system_shutdown\""},
      {"decals off: no readable decals.bin", "\"reason\":\"no_table\""},
      {"decals refused: decals.bin is not a usable table", "\"reason\":\"bad_table\""},
  };
  std::string s = needle;
  for (const auto& e : kEvents)
    if (s.compare(0, std::strlen(e.from), e.from) == 0) return e.to + s.substr(std::strlen(e.from));
  const size_t eq = s.find('=');
  if (eq == std::string::npos) return s;
  const std::string key = s.substr(0, eq), val = s.substr(eq + 1);
  if (key == "level") return "\"level\":\"" + val;
  if (!val.empty() && val.find_first_not_of("0123456789") == std::string::npos) return "\"" + key + "\":" + val;
  return "\"" + key + "\":\"" + val;
}
static int CountLines(const char* needle, int level = -1) {
  const std::string n = Needle(needle);
  int c = 0;
  for (const auto& l : g_lines)
    if (l.text.find(n) != std::string::npos && (level < 0 || l.level == level)) ++c;
  return c;
}

using namespace nevr_quest::combat::decals;
namespace level = nevr_quest::combat::level;
namespace combat = nevr_quest::combat;
namespace dc = nevr_quest::combat::decals;

// ---- 1 and 2. the table and the state machine ----------------------------------------------------------------------------------

namespace {

// What a path costs, counted: every SafeRead and every level scan goes through a counter behind its test seam (safe_read.h, combat_level.h),
// installed for the whole run at the top of main().
std::atomic<std::uint64_t> g_reads{0}, g_scans{0};
void CountScan() { g_scans.fetch_add(1, std::memory_order_relaxed); }
std::uint64_t ReadCount() { return g_reads.load(std::memory_order_relaxed); }
std::uint64_t ScanCount() { return g_scans.load(std::memory_order_relaxed); }
void StartCounting() {
  nevr_quest::combat::SetReadCounterForTest(&g_reads);
  nevr_quest::combat::level::SetScanSeamForTest(&CountScan);
}


constexpr uint64_t kDyson = 0x43e2da7914642604ull, kCombustion = 0x42670f2bed45703cull;

struct QuadIn {
  float c[4][5];  // 4 corners x {x, y, z, u, v}
};

QuadIn MakeQuad(float seed) {
  QuadIn q;
  for (int i = 0; i < 4; ++i)
    for (int k = 0; k < 5; ++k) q.c[i][k] = seed + static_cast<float>(10 * i + k);
  return q;
}

struct GroupIn {
  uint64_t level, mat;
  uint32_t flags;
  std::vector<QuadIn> quads;
  uint32_t claimed = 0xffffffffu;  // the quad count written in the header (all ones: the real count)
};

std::vector<uint8_t> Bytes(const std::vector<GroupIn>& groups, uint32_t claimed_groups = 0xffffffffu) {
  std::vector<uint8_t> b = {'A', 'D', 'C', '1'};
  auto put32 = [&b](uint32_t v) { for (int i = 0; i < 4; ++i) b.push_back(static_cast<uint8_t>(v >> (8 * i))); };
  auto put64 = [&b](uint64_t v) { for (int i = 0; i < 8; ++i) b.push_back(static_cast<uint8_t>(v >> (8 * i))); };
  put32(claimed_groups == 0xffffffffu ? static_cast<uint32_t>(groups.size()) : claimed_groups);
  for (const auto& g : groups) {
    put64(g.level);
    put64(g.mat);
    put32(g.claimed == 0xffffffffu ? static_cast<uint32_t>(g.quads.size()) : g.claimed);
    put32(g.flags);
    for (const auto& q : g.quads) {
      const uint8_t* p = reinterpret_cast<const uint8_t*>(&q);
      b.insert(b.end(), p, p + sizeof q);
    }
  }
  return b;
}

LoadResult Parse(const std::vector<uint8_t>& b, Table* t, bool fu = false, bool fv = false) {
  return ParseTable(b.data(), b.size(), fu, fv, t);
}

void TestTable() {
  // the level symbols are the SDK's hash of the level names
  CHECK(kLevelWarRoom == EchoVR::CSymbol64Hash("mpl_combat_war_room"), "war room symbol %016llx",
        static_cast<unsigned long long>(EchoVR::CSymbol64Hash("mpl_combat_war_room")));
  // the combat maps are combat_level.h's, the one answer to "is this combat?"; this module keeps no list of its own
  for (const char* name : {"mpl_combat_dyson", "mpl_combat_combustion", "mpl_combat_fission", "mpl_combat_gauss"})
    CHECK(nevr_quest::combat::level::IsCombatMap(EchoVR::CSymbol64Hash(name)), "%s is not a combat map", name);
  for (const char* name : {"mpl_arena_a", "mpl_arena_a_lowspec", "mpl_arena_b", "mpl_tutorial_a", "mpl_lobby_b", "mpl_lobby_b_combat", "mpl_combat_war_room"})
    CHECK(!nevr_quest::combat::level::IsCombatMap(EchoVR::CSymbol64Hash(name)), "%s is a combat map", name);
  CHECK(ActsIn(kDyson) && ActsIn(kLevelWarRoom) && !ActsIn(EchoVR::CSymbol64Hash("mpl_arena_a")) && !ActsIn(EchoVR::CSymbol64Hash("mpl_lobby_b_combat")) &&
            !ActsIn(0) && !ActsIn(~0ull),
        "the levels the module acts in: combat maps and the war room");

  // a good table: two groups, the second longer than a node holds
  std::vector<GroupIn> in(2);
  in[0] = {kDyson, 0x1111, 0, {MakeQuad(1), MakeQuad(100), MakeQuad(200)}};
  in[1] = {kCombustion, 0x2222, 11, {}};
  for (int i = 0; i < 200; ++i) in[1].quads.push_back(MakeQuad(static_cast<float>(i)));
  std::vector<uint8_t> b = Bytes(in);
  b.push_back(0x5a);  // a trailing byte: kept out of the table, counted
  Table t;
  CHECK(Parse(b, &t) == LoadResult::kOk, "a good table must load");
  CHECK(t.count == 2 && t.total_quads == 203 && t.truncated_groups == 1 && t.trailing_bytes == 1, "counts: %u groups %u quads %u cut %zu trailing",
        t.count, t.total_quads, t.truncated_groups, t.trailing_bytes);
  if (t.count == 2) {
    CHECK(t.groups[0].level == kDyson && t.groups[0].mat == 0x1111 && t.groups[0].quads == 3 && t.groups[0].n == 3 && t.groups[0].blend == 1,
          "group 0 fields");
    CHECK(t.groups[1].level == kCombustion && t.groups[1].mat == 0x2222 && t.groups[1].quads == 200 && t.groups[1].n == kMaxNodeQuads &&
              t.groups[1].blend == 11,
          "group 1 fields: n %u blend %u", t.groups[1].n, t.groups[1].blend);
    CHECK(t.groups[0].v[0] == 1.0f && t.groups[0].v[20] == 100.0f && t.groups[1].v[0] == 0.0f && t.groups[1].v[20] == 1.0f,
          "quads are kept in order");
    // the vertex fill
    std::vector<Vtx> v(4 * 3);
    FillVertices(t.groups[0], v.data());
    CHECK(v[0].p[0] == 1.0f && v[0].p[1] == 2.0f && v[0].p[2] == 3.0f && v[0].uv[0] == 4.0f && v[0].uv[1] == 5.0f, "vertex 0 position and uv");
    CHECK(v[5].p[0] == 100.0f + 10.0f && v[5].uv[1] == 100.0f + 14.0f, "vertex 5 is corner 1 of quad 1");
    CHECK(v[11].c0 == 0xffffffffu && v[11].c1 == 0xffffffffu && v[11].n[0] == 0 && v[11].t[3] == 0, "colours white, normal and tangent zero");
  }
  FreeTable(&t);
  CHECK(t.groups == nullptr && t.count == 0, "FreeTable empties the table");

  // an empty table is a table
  std::vector<GroupIn> none;
  CHECK(Parse(Bytes(none), &t) == LoadResult::kOk && t.count == 0, "zero groups load");
  FreeTable(&t);

  // every refusal leaves the table empty
  const std::vector<uint8_t> good = Bytes(in);
  std::vector<uint8_t> short_file(good.begin(), good.begin() + 7);
  CHECK(Parse(short_file, &t) == LoadResult::kTooShort && t.count == 0, "7 bytes");
  std::vector<uint8_t> bad_magic = good;
  bad_magic[3] = '2';
  CHECK(Parse(bad_magic, &t) == LoadResult::kBadMagic, "magic ADC2");
  CHECK(Parse(Bytes(none, kMaxGroups + 1), &t) == LoadResult::kTooManyGroups, "257 groups");
  std::vector<uint8_t> cut_header(good.begin(), good.begin() + 8 + 23);
  CHECK(Parse(cut_header, &t) == LoadResult::kGroupHeaderCut, "group header cut");
  std::vector<GroupIn> many(1);
  many[0] = {kDyson, 1, 0, {}};
  many[0].claimed = kMaxFileQuads + 1;
  CHECK(Parse(Bytes(many), &t) == LoadResult::kTooManyQuads, "4097 quads");
  many[0].claimed = 2;  // claims two quads, carries none
  CHECK(Parse(Bytes(many), &t) == LoadResult::kGroupDataCut, "quads past the end");
  many[0].quads = {MakeQuad(1)};
  many[0].claimed = 1;
  many[0].quads[0].c[2][3] = std::nanf("");
  CHECK(Parse(Bytes(many), &t) == LoadResult::kNotFinite, "a NaN uv");
  many[0].quads[0].c[2][3] = 0.0f;
  many[0].quads[0].c[0][0] = INFINITY;
  CHECK(Parse(Bytes(many), &t) == LoadResult::kNotFinite, "an infinite x");
  many[0].quads[0].c[0][0] = 0.0f;
  CHECK(Parse(Bytes(many), &t) == LoadResult::kOk, "the same quad with finite numbers loads");
  FreeTable(&t);
  // a second group whose data is cut: the first group is not kept
  std::vector<GroupIn> two(2);
  two[0] = {kDyson, 1, 0, {MakeQuad(1)}};
  two[1] = {kDyson, 2, 0, {}};
  two[1].claimed = 5;
  CHECK(Parse(Bytes(two), &t) == LoadResult::kGroupDataCut && t.groups == nullptr, "a cut second group refuses the table whole");

  // uv flips: corners (-,-) (+,-) (+,+) (-,+); u swaps 0<->1 and 3<->2, v swaps 0<->3 and 1<->2
  std::vector<GroupIn> one(1);
  one[0] = {kDyson, 1, 0, {MakeQuad(0)}};
  const QuadIn& q0 = one[0].quads[0];
  Table fu, fv, fb;
  CHECK(Parse(Bytes(one), &fu, true, false) == LoadResult::kOk && Parse(Bytes(one), &fv, false, true) == LoadResult::kOk &&
            Parse(Bytes(one), &fb, true, true) == LoadResult::kOk,
        "flip tables load");
  auto uvof = [](const Table& t2, int corner) { return std::make_pair(t2.groups[0].v[5 * corner + 3], t2.groups[0].v[5 * corner + 4]); };
  CHECK(uvof(fu, 0) == std::make_pair(q0.c[1][3], q0.c[1][4]) && uvof(fu, 1) == std::make_pair(q0.c[0][3], q0.c[0][4]) &&
            uvof(fu, 2) == std::make_pair(q0.c[3][3], q0.c[3][4]) && uvof(fu, 3) == std::make_pair(q0.c[2][3], q0.c[2][4]),
        "flip u");
  CHECK(uvof(fv, 0) == std::make_pair(q0.c[3][3], q0.c[3][4]) && uvof(fv, 3) == std::make_pair(q0.c[0][3], q0.c[0][4]) &&
            uvof(fv, 1) == std::make_pair(q0.c[2][3], q0.c[2][4]) && uvof(fv, 2) == std::make_pair(q0.c[1][3], q0.c[1][4]),
        "flip v");
  CHECK(uvof(fb, 0) == std::make_pair(q0.c[2][3], q0.c[2][4]) && uvof(fb, 2) == std::make_pair(q0.c[0][3], q0.c[0][4]), "flip u and v");
  CHECK(fu.groups[0].v[0] == q0.c[0][0] && fu.groups[0].v[1] == q0.c[0][1] && fu.groups[0].v[2] == q0.c[0][2], "a flip leaves positions alone");
  FreeTable(&fu);
  FreeTable(&fv);
  FreeTable(&fb);

  // the blend: only the render states the decals may use
  CHECK(BlendOf(0) == 1 && BlendOf(1) == 1 && BlendOf(6) == 6 && BlendOf(7) == 7 && BlendOf(8) == 8 && BlendOf(11) == 11, "blend table");
  CHECK(BlendOf(2) == 1 && BlendOf(9) == 1 && BlendOf(0x10b) == 11 && BlendOf(0xff) == 1, "blend: other values draw as 1, only the low byte counts");

  // the level gate
  CHECK(Applies(kDyson, kDyson) && !Applies(kDyson, kCombustion), "a group draws in its own level only");
  CHECK(Applies(kLevelWarRoom, kDyson) && Applies(kLevelWarRoom, kCombustion) && Applies(kLevelWarRoom, kLevelWarRoom) &&
            !Applies(kLevelWarRoom, 0x1234),
        "the war room's groups draw in every combat arena");
  CHECK(!Applies(kDyson, kLevelWarRoom), "an arena's group does not draw in the war room");

  // both windings, 12 indices a quad
  std::vector<uint16_t> idx(12 * kMaxNodeQuads);
  FillIndices(idx.data());
  const uint16_t want0[12] = {0, 1, 2, 0, 2, 3, 0, 2, 1, 0, 3, 2};
  CHECK(std::memcmp(idx.data(), want0, sizeof want0) == 0, "quad 0 indices");
  CHECK(idx[12 * 159 + 0] == 636 && idx[12 * 159 + 10] == 639 && idx[12 * 159 + 11] == 638, "quad 159 indices: %u .. %u", idx[12 * 159],
        idx[12 * 159 + 11]);
}

// ---- 2. the state machine over stand-in operations -------------------------------------------------------------------------

struct Fake {
  uint64_t levels[16] = {};  // by system index: sys = 0x1000 * (index + 1)
  bool has_level[16] = {};
  int fail_next_creates = 0;
  uintptr_t next_node = 0x9000;
  std::vector<std::pair<uintptr_t, int>> made;   // sys, group mat low bits
  std::vector<uintptr_t> destroyed, hidden, drawn;
  bool draw_ok = true;
  int level_reads = 0;   // how often the module asked for a game space's level
};

int SysIndex(uintptr_t sys) { return static_cast<int>(sys / 0x1000) - 1; }
bool FLevelOf(void* u, uintptr_t sys, uint64_t* level) {
  Fake* f = static_cast<Fake*>(u);
  const int i = SysIndex(sys);
  ++f->level_reads;
  if (!f->has_level[i]) return false;
  *level = f->levels[i];
  return true;
}
uintptr_t FCreate(void* u, uintptr_t sys, const Group& g, int* rc, uintptr_t* tried, bool* blended) {
  Fake* f = static_cast<Fake*>(u);
  *tried = 0;
  *blended = false;
  if (f->fail_next_creates > 0) {
    --f->fail_next_creates;
    *rc = 3;
    return 0;
  }
  *rc = 0;
  *blended = true;
  const uintptr_t n = f->next_node;
  f->next_node += 0x100;
  *tried = n;
  f->made.push_back({sys, static_cast<int>(g.mat & 0xff)});
  return n;
}
void FDestroy(void* u, uintptr_t, uintptr_t node) { static_cast<Fake*>(u)->destroyed.push_back(node); }
bool FDraw(void* u, uintptr_t node, const Group&) {
  Fake* f = static_cast<Fake*>(u);
  if (f->draw_ok) f->drawn.push_back(node);
  return f->draw_ok;
}
void FHide(void* u, uintptr_t node) { static_cast<Fake*>(u)->hidden.push_back(node); }

Host* NewHost(Fake* f, const Table* t) {
  Host* h = new Host();
  h->table = t;
  h->ops.user = f;
  h->ops.level_of = &FLevelOf;
  h->ops.create = &FCreate;
  h->ops.destroy = &FDestroy;
  h->ops.draw = &FDraw;
  h->ops.hide = &FHide;
  return h;
}

void TestHost() {
  // groups: 0x11 dyson, 0x22 combustion, 0x33 war room
  std::vector<GroupIn> in(3);
  in[0] = {kDyson, 0x11, 0, {MakeQuad(1), MakeQuad(2)}};
  in[1] = {kCombustion, 0x22, 0, {MakeQuad(3)}};
  in[2] = {kLevelWarRoom, 0x33, 0, {MakeQuad(4)}};
  Table t;
  CHECK(Parse(Bytes(in), &t) == LoadResult::kOk, "host table");
  const uintptr_t A = 0x1000, B = 0x2000;

  {  // one dyson system: its own group and the war room's, nothing of combustion's
    Fake f;
    f.levels[0] = kDyson;
    f.has_level[0] = true;
    Host* h = NewHost(&f, &t);
    h->Tick(A, 100);
    CHECK(f.made.size() == 2 && f.made[0].second == 0x11 && f.made[1].second == 0x33, "dyson makes groups 0x11 and 0x33, made %zu", f.made.size());
    CHECK(f.drawn.size() == 2 && h->node_n == 2 && h->counters.nodes_made == 2, "both nodes are drawn the tick they are made");
    CHECK(CountLines("decals: game space") == 1 && CountLines("level=0x43e2da7914642604") >= 1, "one game-space line naming the level");
    h->Tick(A, 200);
    CHECK(f.made.size() == 2 && f.drawn.size() == 4, "a second tick makes nothing and draws again");
    CHECK(CountLines("decals: game space") == 1, "the game-space line is written once");
    // the war room's own game space shows up: the arena's copy of the war room group is hidden
    f.levels[1] = kLevelWarRoom;
    f.has_level[1] = true;
    h->Tick(B, 300);
    CHECK(f.made.size() == 3 && f.made[2].first == B && f.made[2].second == 0x33, "the war room space makes its own group");
    CHECK(f.hidden.empty(), "the war room space does not hide its own ships");
    f.hidden.clear();
    f.drawn.clear();
    h->Tick(A, 400);
    CHECK(f.hidden.size() == 1 && f.drawn.size() == 1, "the arena hides the war room group while a war room space exists: hid %zu drew %zu",
          f.hidden.size(), f.drawn.size());
    // the war room space leaves: the arena draws the group again
    h->Shutdown(B);
    f.hidden.clear();
    f.drawn.clear();
    h->Tick(A, 500);
    CHECK(f.hidden.empty() && f.drawn.size() == 2, "after the war room space shut down the arena draws it again");
    CHECK(f.destroyed.size() == 1, "Shutdown destroyed the war room space's node only");
    // two war room spaces: one leaving does not make the arena draw the ships under the other
    const uintptr_t C = 0x3000, D = 0x4000;
    f.levels[2] = f.levels[3] = kLevelWarRoom;
    f.has_level[2] = f.has_level[3] = true;
    h->Tick(C, 600);
    h->Tick(D, 700);
    h->Shutdown(C);
    f.hidden.clear();
    f.drawn.clear();
    h->Tick(A, 800);
    CHECK(f.hidden.size() == 1 && f.drawn.size() == 1, "one war room space left, the other stays: the arena still hides its copy (hid %zu)", f.hidden.size());
    h->Shutdown(D);
    f.hidden.clear();
    h->Tick(A, 900);
    CHECK(f.hidden.empty(), "no war room space left: the arena draws it");
    delete h;
  }
  {  // a level change under a live system: nodes go, the system is gated again
    Fake f;
    f.levels[0] = kDyson;
    f.has_level[0] = true;
    Host* h = NewHost(&f, &t);
    h->Tick(A, 100);
    f.levels[0] = kCombustion;
    h->Tick(A, 200);
    CHECK(f.destroyed.size() == 2 && h->counters.level_changes == 1, "the dyson nodes are destroyed on the change: %zu", f.destroyed.size());
    CHECK(f.made.size() == 4 && f.made[2].second == 0x22 && f.made[3].second == 0x33, "combustion makes 0x22 and 0x33");
    CHECK(CountLines("level changed under a bullet system") == 1, "the change is logged");
    h->Shutdown(A);
    CHECK(f.destroyed.size() == 4 && h->node_n == 0 && h->sys_n == 0, "Shutdown takes the rest");
    h->Shutdown(A);
    CHECK(f.destroyed.size() == 4, "a second Shutdown of the same system destroys nothing");
    delete h;
  }
  {  // a group that fails is retried every 5 s, 3 tries in all, and the nodes made meanwhile are still drawn
    Fake f;
    f.levels[0] = kDyson;
    f.has_level[0] = true;
    f.fail_next_creates = 1;  // the first create (0x11) fails, 0x33 is made
    Host* h = NewHost(&f, &t);
    h->Tick(A, 1000);
    CHECK(f.made.size() == 1 && f.made[0].second == 0x33 && h->counters.nodes_failed == 1, "try 1: one made, one failed");
    CHECK(f.drawn.size() == 1, "the node made is drawn while the other waits for its retry");
    h->Tick(A, 3000);
    CHECK(f.made.size() == 1, "no retry before 5 s");
    f.fail_next_creates = 1;
    h->Tick(A, 6000);
    CHECK(f.made.size() == 1 && h->counters.nodes_failed == 2, "try 2 fails again");
    h->Tick(A, 9000);
    CHECK(f.made.size() == 1, "no retry before the next 5 s");
    h->Tick(A, 11000);
    CHECK(f.made.size() == 2 && f.made[1].second == 0x11, "try 3 succeeds");
    f.fail_next_creates = 0;
    h->Tick(A, 20000);
    CHECK(f.made.size() == 2, "all made: no further tries");
    delete h;
  }
  {  // three failures end the tries for the system's life
    Fake f;
    f.levels[0] = kDyson;
    f.has_level[0] = true;
    f.fail_next_creates = 100;
    Host* h = NewHost(&f, &t);
    h->Tick(A, 1000);
    h->Tick(A, 7000);
    h->Tick(A, 13000);
    const int after3 = 100 - f.fail_next_creates;
    h->Tick(A, 30000);
    h->Tick(A, 60000);
    CHECK(after3 == 6 && f.fail_next_creates == 94 && f.made.empty(), "3 tries x 2 groups, then no more: %d attempts", after3);
    delete h;
  }
  {  // a system with no game space or level yet is left alone; a table with no groups does nothing
    Fake f;
    Host* h = NewHost(&f, &t);
    h->Tick(A, 100);
    CHECK(f.made.empty() && h->sys_n == 0, "no level: nothing");
    delete h;
    Table empty;
    std::vector<GroupIn> none;
    CHECK(Parse(Bytes(none), &empty) == LoadResult::kOk, "empty table");
    f.levels[0] = kDyson;
    f.has_level[0] = true;
    h = NewHost(&f, &empty);
    h->Tick(A, 100);
    CHECK(f.made.empty() && h->sys_n == 0, "an empty table: nothing");
    delete h;
    FreeTable(&empty);
  }
  {  // the table of systems is bounded and says so once; the node table is bounded too
    Fake f;
    for (int i = 0; i < 12; ++i) {
      f.levels[i] = kDyson;
      f.has_level[i] = true;
    }
    Host* h = NewHost(&f, &t);
    for (int i = 0; i < 12; ++i) h->Tick(0x1000 * (i + 1), 100);
    CHECK(h->sys_n == kMaxSystems && CountLines("more bullet systems than the table keeps", 2) == 1,
          "8 systems kept, the ninth onward logged once: %d", h->sys_n);
    CHECK(h->node_n == 16, "8 systems x 2 groups = 16 nodes, got %d", h->node_n);
    delete h;
  }
  {  // 8 systems of a 4-group level want 32 nodes: the node table caps at 24
    Fake f;
    std::vector<GroupIn> g4(4);
    for (int i = 0; i < 4; ++i) g4[i] = {kDyson, static_cast<uint64_t>(0x11 + i), 0, {MakeQuad(1)}};
    Table t4;
    CHECK(Parse(Bytes(g4), &t4) == LoadResult::kOk, "4 group table");
    for (int i = 0; i < 8; ++i) {
      f.levels[i] = kDyson;
      f.has_level[i] = true;
    }
    Host* h = NewHost(&f, &t4);
    for (int i = 0; i < 8; ++i) h->Tick(0x1000 * (i + 1), 100);
    CHECK(h->node_n == kMaxNodes && static_cast<int>(f.made.size()) == kMaxNodes, "the node table caps at %d, got %d nodes, %zu made", kMaxNodes,
          h->node_n, f.made.size());
    delete h;
    FreeTable(&t4);
  }
  {  // the window line: first tick starts it, one line after 10 s of ticks
    Fake f;
    f.levels[0] = kDyson;
    f.has_level[0] = true;
    Host* h = NewHost(&f, &t);
    g_lines.clear();
    h->Tick(A, 1000000);  // the first tick opens the window (its frame is not counted)
    h->Tick(A, 1005000);
    CHECK(CountLines("decals: window") == 0, "no window line before 10 s");
    h->Tick(A, 1010200);
    CHECK(CountLines("decals: window") == 1 && CountLines("frames=2") == 1, "one window line with the frames counted");
    delete h;
  }

  {  // the scope (arena shares the APK): a game space in a level that is not a combat map or the war room creates and draws nothing, even when the
     // table names that level, and its level is read once a second, not on every update
    const uint64_t kArena = EchoVR::CSymbol64Hash("mpl_arena_a"), kLobby = EchoVR::CSymbol64Hash("mpl_lobby_b_combat");
    std::vector<GroupIn> ia(3);
    ia[0] = {kArena, 0x44, 0, {MakeQuad(5)}};
    ia[1] = {kLobby, 0x45, 0, {MakeQuad(6)}};
    ia[2] = {kDyson, 0x55, 0, {MakeQuad(7)}};
    Table ta;
    CHECK(Parse(Bytes(ia), &ta) == LoadResult::kOk, "scope table");
    for (uint64_t lvl : {kArena, kLobby, EchoVR::CSymbol64Hash("mpl_lobby_b"), uint64_t{0x1234}}) {
      Fake f;
      f.levels[0] = lvl;
      f.has_level[0] = true;
      Host* h = NewHost(&f, &ta);
      g_lines.clear();
      h->Tick(0x1000, 100);
      CHECK(f.made.empty() && f.drawn.empty() && h->node_n == 0 && h->counters.nodes_made == 0, "level %llx: a node was made or drawn", static_cast<unsigned long long>(lvl));
      CHECK(CountLines("decals: game space") == 1 && CountLines("acts=0") == 1 && h->sys_n == 1, "level %llx: one game-space line that says it does not act", static_cast<unsigned long long>(lvl));
      CHECK(f.level_reads == 1, "level %llx: first look reads the level once (%d)", static_cast<unsigned long long>(lvl), f.level_reads);
      for (int64_t t2 = 110; t2 < 100 + kInertRecheckMs; t2 += 16) h->Tick(0x1000, t2);  // sixty frames inside the second
      CHECK(f.level_reads == 1 && f.made.empty(), "level %llx: the level was read again inside the second (%d reads)", static_cast<unsigned long long>(lvl), f.level_reads);
      h->Tick(0x1000, 100 + kInertRecheckMs);
      CHECK(f.level_reads == 2 && f.made.empty(), "level %llx: not read again after the second (%d reads)", static_cast<unsigned long long>(lvl), f.level_reads);
      // the space becomes a combat map (the level changes under it): its nodes are made on the next look
      f.levels[0] = kDyson;
      h->Tick(0x1000, 100 + 2 * kInertRecheckMs);
      CHECK(f.made.size() == 1 && f.made[0].second == 0x55 && CountLines("level changed under a bullet system") == 1, "level %llx: a space that became a combat map made no node", static_cast<unsigned long long>(lvl));
      delete h;
    }
    // a combat space is read on every update (a level change is seen at once)
    Fake f2;
    f2.levels[0] = kDyson;
    f2.has_level[0] = true;
    Host* h2 = NewHost(&f2, &ta);
    for (int i = 0; i < 10; ++i) h2->Tick(0x1000, 100 + i);
    CHECK(f2.level_reads == 10, "a combat space reads its level on every update (%d)", f2.level_reads);
    delete h2;
    FreeTable(&ta);
  }
  FreeTable(&t);
}

// ---- 3. the installer and the handlers over a stand-in engine ---------------------------------------------------------------------

// The stand-in engine. Every object is laid out as the module reads it.
constexpr size_t kNodeSize = 0x2048;
constexpr size_t kBlendOff = 0xa8 + 0x320 + 0x700 + 8;
struct Engine {
  std::vector<std::string> calls;                 // the order of engine calls
  alignas(16) uint8_t nodes[4][kNodeSize] = {};
  int nodes_used = 0;
  void* node_vtable[16] = {};
  void* alloc_vtable[16] = {};
  void* alloc_obj[2] = {};                        // the allocator: its vtable pointer
  void* allocator1[2] = {};                       // a second allocator for the memory context
  void* ctx_array[2] = {};                        // the memory context: two allocators
  void** ctx_ptr = nullptr;
  uint32_t gs_initialized = 1;
  int init_rc = 0;
  uint32_t preset_blend = 0;                      // written into a node's blend word before Initialize returns
  void* current = reinterpret_cast<void*>(0xabc0);
  std::vector<void*> mem_sets;
  // what Initialize saw
  void* init_mesh = nullptr;
  uint64_t init_material = 0, init_max_vtx = 0, init_max_idx = 0;
  uint32_t init_index_type = 0;
  void *init_a0 = nullptr, *init_a1 = nullptr;
  uint16_t init_idx_head[12] = {};
  Vtx vbuf[4 * kMaxNodeQuads] = {};
  bool map_fails = false;
  uint32_t visible[4] = {9, 9, 9, 9};
  std::vector<void*> freed;
  // update / shutdown stand-ins' counters
  int real_updates = 0, real_shutdowns = 0, earlier_shutdowns = 0;
};
Engine E;

int NodeIndex(void* node) {
  for (int i = 0; i < 4; ++i)
    if (node == E.nodes[i]) return i;
  return -1;
}

// the allocator's slots: vtable +0x10 Alloc(this, size), +0x38 Free(this, ptr)
void* FAlloc(void*, size_t size) {
  E.calls.push_back("alloc");
  if (size != kNodeSize || E.nodes_used >= 4) return nullptr;
  void* n = E.nodes[E.nodes_used++];
  std::memset(n, 0, kNodeSize);
  return n;
}
void FFree(void*, void* p) {
  E.calls.push_back("free");
  E.freed.push_back(p);
}
// the node's slots: +0x0 destructor, +0x58 link, +0x60 visible
void FCtor(void* node) {
  E.calls.push_back("ctor");
  *static_cast<void***>(node) = E.node_vtable;
}
void FDtor(void*) { E.calls.push_back("dtor"); }
void FLink(void*, void*) { E.calls.push_back("link"); }
void FVisible(void* node, uint32_t on) {
  E.calls.push_back(on ? "visible1" : "visible0");
  const int i = NodeIndex(node);
  if (i >= 0) E.visible[i] = on;
}
nevr_quest::combat::decals::VertexFormat FFormat() {
  E.calls.push_back("format");
  nevr_quest::combat::decals::VertexFormat f;
  std::memset(f.raw, 0x7e, sizeof f.raw);
  return f;
}
int FInit(void* mesh, const nevr_quest::combat::decals::VertexFormat* fmt, const nevr_quest::combat::decals::MeshInit* in) {
  E.calls.push_back("init");
  E.init_mesh = mesh;
  E.init_material = in->material;
  E.init_max_vtx = in->max_vertices;
  E.init_max_idx = in->max_indices;
  E.init_index_type = in->index_type;
  E.init_a0 = in->allocator0;
  E.init_a1 = in->allocator1;
  std::memcpy(E.init_idx_head, in->indices, sizeof E.init_idx_head);
  if (fmt->raw[0] != 0x7e || fmt->raw[0x127] != 0x7e) E.calls.push_back("BAD_FORMAT");
  // the node this mesh belongs to: mesh - 0xa8
  void* node = static_cast<uint8_t*>(mesh) - 0xa8;
  if (NodeIndex(node) >= 0) std::memcpy(static_cast<uint8_t*>(node) + kBlendOff, &E.preset_blend, 4);
  return E.init_rc;
}
void* FMap(void*) {
  E.calls.push_back("map");
  return E.map_fails ? nullptr : E.vbuf;
}
void FUnmap(void*) { E.calls.push_back("unmap"); }
void FShutdownNode(void*) { E.calls.push_back("node_shutdown"); }
void* FMemCurrent() { return E.current; }
void FMemSet(void* a) { E.mem_sets.push_back(a); }


// The game's functions the slots hold
__attribute__((noinline)) void FakeUpdateMovement(uintptr_t) {
  ++E.real_updates;
  E.calls.push_back("real_update");
}
__attribute__((noinline)) uint64_t FakeShutdown(uint64_t self) {
  ++E.real_shutdowns;
  E.calls.push_back("real_shutdown");
  return self + 1;
}
dc::Api MakeApi() {
  dc::Api a;
  a.ctor = &FCtor;
  a.vertex_format = &FFormat;
  a.init = &FInit;
  a.map = &FMap;
  a.unmap = &FUnmap;
  a.shutdown = &FShutdownNode;
  a.mem_current = &FMemCurrent;
  a.mem_set = &FMemSet;
  a.mem_context = &E.ctx_ptr;
  a.gs_initialized = &E.gs_initialized;
  return a;
}

void ResetEngine() {
  E = Engine();
  E.node_vtable[0] = reinterpret_cast<void*>(&FDtor);
  E.node_vtable[0x58 / 8] = reinterpret_cast<void*>(&FLink);
  E.node_vtable[0x60 / 8] = reinterpret_cast<void*>(&FVisible);
  E.alloc_vtable[0x10 / 8] = reinterpret_cast<void*>(&FAlloc);
  E.alloc_vtable[0x38 / 8] = reinterpret_cast<void*>(&FFree);
  E.alloc_obj[0] = E.alloc_vtable;
  E.allocator1[0] = E.alloc_vtable;
  E.ctx_array[0] = reinterpret_cast<void*>(0xa110c0);
  E.ctx_array[1] = reinterpret_cast<void*>(0xa110c1);
  E.ctx_ptr = E.ctx_array;
}

// ---- the image: the pinned update (GLOB_DAT) and Shutdown (ABS64) words, and a trampoline at each function's address ----
combat_test::Image g_image;
uintptr_t g_base = 0;
bool Lookup(const char* module, sentinel::ElfImage* out) {
  if (!g_image.Present() || std::strcmp(module, sentinel::pinned::kLibR15) != 0) return false;
  *out = g_image.Elf();
  return true;
}
void Tramp(uintptr_t va, void* target) {  // movabs rax, <target>; jmp rax, on a page made executable and never written afterwards
  uint8_t code[12] = {0x48, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xe0};
  const uint64_t t = reinterpret_cast<uint64_t>(target);
  std::memcpy(code + 2, &t, 8);
  std::memcpy(reinterpret_cast<void*>(g_base + va), code, sizeof code);
  mprotect(reinterpret_cast<void*>((g_base + va) & ~0xfffull), 0x1000, PROT_READ | PROT_EXEC);
}
void W64(uintptr_t va, uint64_t v) { *reinterpret_cast<uint64_t*>(g_base + va) = v; }
uint64_t R64(uintptr_t va) { return *reinterpret_cast<uint64_t*>(g_base + va); }
void BuildImage() {
  g_image.Build({{dc::kUpdateSymbol, dc::kUpdateSlotVa, true, false, dc::kUpdateRealVa},
                 {dc::kShutdownSymbol, dc::kShutdownSlotVa, false, false, dc::kShutdownRealVa, true}});
  g_base = g_image.Base();
  Tramp(dc::kUpdateRealVa, reinterpret_cast<void*>(&FakeUpdateMovement));
  Tramp(dc::kShutdownRealVa, reinterpret_cast<void*>(&FakeShutdown));
  W64(dc::kUpdateSlotVa, g_base + dc::kUpdateRealVa);
  W64(dc::kShutdownSlotVa, g_base + dc::kShutdownRealVa);
}
void Fresh() {
  dc::ResetForTest();
  ResetEngine();
  g_lines.clear();
  BuildImage();
}

// What impactsnd's Shutdown handler looks like to the module: a handler on the same word that counts and calls on.
struct EarlierTag {};
using EarlierThunk = sentinel::CallbackThunk<EarlierTag, uint64_t(uint64_t)>;
uint64_t OnEarlier(EarlierThunk::Fn original, uint64_t self) noexcept {
  ++E.earlier_shutdowns;
  return original(self);
}
NEVR_HOOK_RECORD(kEarlierRecord, EarlierThunk, &OnEarlier);
sentinel::GotHook g_earlierHook;
void InstallEarlier() {
  EarlierThunk::Arm(kEarlierRecord);
  sentinel::GotTarget t(sentinel::pinned::kLibR15, dc::kShutdownSymbol, sentinel::RelocKind::kAbs64, sentinel::pinned::LibR15PatchedBuildId(), dc::kShutdownSlotVa);
  t.expectedOriginal = reinterpret_cast<const void*>(g_base + dc::kShutdownRealVa);
  CHECK(sentinel::InstallThunk<EarlierThunk>(g_earlierHook, t.Owned("test_earlier"), &Lookup) == sentinel::GotStatus::kOk, "the earlier handler did not install");
}
void ForgetEarlier() {
  g_earlierHook.Forget();
  EarlierThunk::Reset();
}

struct Sys {
  alignas(16) uintptr_t w[0x20] = {};
  alignas(16) uintptr_t gs[0x20] = {};
  uintptr_t addr() { return reinterpret_cast<uintptr_t>(w); }
};
void MakeSys(Sys* s, uint64_t lvl) {
  std::memset(s->w, 0, sizeof s->w);
  std::memset(s->gs, 0, sizeof s->gs);
  s->w[0x30 / 8] = reinterpret_cast<uintptr_t>(E.alloc_obj);
  s->w[0x80 / 8] = reinterpret_cast<uintptr_t>(s->gs);
  s->gs[0x58 / 8] = lvl;
}

std::string WriteFile(const std::string& dir, const char* name, const std::vector<uint8_t>& b) {
  const std::string path = dir + "/" + name;
  FILE* f = std::fopen(path.c_str(), "wb");
  if (f == nullptr) std::abort();
  std::fwrite(b.data(), 1, b.size(), f);
  std::fclose(f);
  return path;
}

int Index(const char* name, int from = 0) {
  for (size_t i = static_cast<size_t>(from); i < E.calls.size(); ++i)
    if (E.calls[i] == name) return static_cast<int>(i);
  return -1;
}

using UpdateFn = void (*)(uintptr_t);
using ShutFn = uint64_t (*)(uint64_t);
UpdateFn UpdateSlot() { return reinterpret_cast<UpdateFn>(R64(dc::kUpdateSlotVa)); }
ShutFn ShutSlot() { return reinterpret_cast<ShutFn>(R64(dc::kShutdownSlotVa)); }
combat::ModuleResult Install(const char* table, const dc::Api* api, bool on = true, bool fu = false, bool fv = false) {
  dc::Switches sw;
  sw.on = on;
  sw.flipU = fu;
  sw.flipV = fv;
  return dc::InstallAt(sw, &Lookup, table, api);
}

void TestInstall(const std::string& dir) {
  const dc::Api api = MakeApi();
  std::vector<GroupIn> in(2);
  in[0] = {kDyson, 0x1111, 7, {MakeQuad(1), MakeQuad(10), MakeQuad(20)}};
  in[1] = {kLevelWarRoom, 0x2222, 0, {MakeQuad(50)}};
  const std::string table = WriteFile(dir, "decals.bin", Bytes(in));
  combat::legacy::SetDirectoryForTest(dir.c_str());
  g_lines.clear();

  // ---- the slots
  Fresh();
  combat::ModuleResult r = Install(table.c_str(), &api);
  CHECK(r.installed && !r.off && std::strcmp(r.status, "installed") == 0, "install: %s", r.status);
  CHECK(R64(dc::kUpdateSlotVa) != g_base + dc::kUpdateRealVa && R64(dc::kShutdownSlotVa) != g_base + dc::kShutdownRealVa, "both slot words are replaced");
  CHECK(CountLines("decals on", 1) == 1 && CountLines("groups=2") >= 1 && CountLines("quads=4") >= 1, "the table line carries the table's counts");
  CHECK(CountLines("decals shutdown wrapper", 1) == 1 && CountLines("decals update wrapper", 1) == 1, "one INFO line a hook");
  const UpdateFn update = UpdateSlot();
  const ShutFn shutdown = ShutSlot();

  // ---- the first update: the game's own update runs, then the nodes are made and drawn
  Sys s1;
  MakeSys(&s1, kDyson);
  update(s1.addr());
  CHECK(E.real_updates == 1, "the real update ran");
  CHECK(Index("real_update") == 0 && Index("alloc") > Index("real_update"), "the game's update runs first, the nodes are made after it");
  CHECK(kInertRecheckMs == 1000, "a game space that does not act has its level read once a second");
  CHECK(E.nodes_used == 2, "one node for the dyson group, one for the war room group: %d", E.nodes_used);
  CHECK(E.init_max_vtx == 4 && E.init_max_idx == 12 && E.init_index_type == 2 && E.init_material == 0x2222,
        "Initialize of the last node: material %llx vtx %llu idx %llu type %u (the 1-quad war room group)",
        static_cast<unsigned long long>(E.init_material), static_cast<unsigned long long>(E.init_max_vtx),
        static_cast<unsigned long long>(E.init_max_idx), E.init_index_type);
  CHECK(E.init_a0 == E.ctx_array[0] && E.init_a1 == E.ctx_array[1], "Initialize got the memory context's two allocators");
  const uint16_t want_idx[12] = {0, 1, 2, 0, 2, 3, 0, 2, 1, 0, 3, 2};
  CHECK(std::memcmp(E.init_idx_head, want_idx, sizeof want_idx) == 0, "Initialize got the index table");
  CHECK(E.init_mesh == static_cast<void*>(E.nodes[1] + 0xa8), "the mesh is the node + 0xa8");
  const int c_alloc = Index("alloc"), c_ctor = Index("ctor"), c_link = Index("link"), c_fmt = Index("format"), c_init = Index("init");
  CHECK(c_alloc >= 0 && c_alloc < c_ctor && c_ctor < c_link && c_link < c_fmt && c_fmt < c_init, "alloc, ctor, link, format, init in order");
  CHECK(E.mem_sets.size() >= 2 && E.mem_sets[0] == static_cast<void*>(E.alloc_obj) && E.mem_sets[1] == E.current,
        "the memory context is the system's allocator while the node is made, then put back");
  uint64_t vc, ic;
  std::memcpy(&vc, E.nodes[0] + 0x98, 8);
  std::memcpy(&ic, E.nodes[0] + 0xa0, 8);
  CHECK(vc == 12 && ic == 36, "node 0 draws 3 quads: vertex count %llu (4 x 3), index count %llu (12 x 3)", static_cast<unsigned long long>(vc),
        static_cast<unsigned long long>(ic));
  uint32_t blend0, blend1;
  std::memcpy(&blend0, E.nodes[0] + kBlendOff, 4);
  std::memcpy(&blend1, E.nodes[1] + kBlendOff, 4);
  CHECK(blend0 == 7 && blend1 == 1, "the group's blend is written over the pool's 0: %u and %u", blend0, blend1);
  CHECK(E.visible[0] == 1 && E.visible[1] == 1, "both nodes are shown");
  CHECK(CountLines("decals: node made") == 2, "one line a node");
  CHECK(Index("map") > Index("init") && Index("unmap") > Index("map"), "the vertices are mapped after the node exists");
  CHECK(Index("BAD_FORMAT") < 0, "Initialize got the format VertexFormat returned");
  CHECK(E.vbuf[0].p[0] == 50.0f && E.vbuf[3].uv[1] == 50.0f + 34.0f && E.vbuf[0].c0 == 0xffffffffu, "the vertex buffer holds the group's quad");

  // ---- the second update does not make nodes again
  const int nodes_before = E.nodes_used;
  update(s1.addr());
  CHECK(E.nodes_used == nodes_before && E.real_updates == 2, "a second update makes no node");

  // ---- Shutdown: the nodes are destroyed first, in the engine's order, then the game's function runs
  E.calls.clear();
  E.mem_sets.clear();
  const uint64_t ret = shutdown(s1.addr());
  CHECK(ret == s1.addr() + 1 && E.real_shutdowns == 1, "the real Shutdown ran and its value came back");
  const int d_vis = Index("visible0"), d_sh = Index("node_shutdown"), d_dtor = Index("dtor"), d_free = Index("free"), d_real = Index("real_shutdown");
  CHECK(d_vis >= 0 && d_vis < d_sh && d_sh < d_dtor && d_dtor < d_free && d_free < d_real,
        "per node: hide, node Shutdown, destructor, free; all before the real Shutdown (%d %d %d %d %d)", d_vis, d_sh, d_dtor, d_free, d_real);
  CHECK(E.freed.size() == 2 && E.freed[0] == static_cast<void*>(E.nodes[0]) && E.freed[1] == static_cast<void*>(E.nodes[1]), "both nodes freed");
  CHECK(E.mem_sets.size() == 4 && E.mem_sets[0] == static_cast<void*>(E.alloc_obj) && E.mem_sets[1] == E.current &&
            E.mem_sets[2] == static_cast<void*>(E.alloc_obj) && E.mem_sets[3] == E.current,
        "per node the allocator is current while it is destroyed and the context is put back: %zu sets", E.mem_sets.size());
  // a second install over our own handlers: refused, not chained on itself
  {
    const combat::ModuleResult again = Install(table.c_str(), &api);
    CHECK(!again.installed && std::strcmp(again.status, "already_installed") == 0, "a second install over our own handlers: %s", again.status);
  }
  CHECK(CountLines("bullet system shutdown, its nodes are destroyed") == 1, "the teardown is logged");
  CHECK(dc::CountersForTest().nodes_destroyed == 2, "counter");

  // ---- a node whose render state is not the pool's 0 is left alone
  ResetEngine();
  E.preset_blend = 5;
  Sys s2;
  MakeSys(&s2, kDyson);
  update(s2.addr());
  std::memcpy(&blend0, E.nodes[0] + kBlendOff, 4);
  CHECK(blend0 == 5, "a blend word that is not 0 stays: %u", blend0);
  shutdown(s2.addr());

  // ---- a game space that has not loaded its level yet (the constructor leaves -1 in it): nothing is made
  Fresh();
  CHECK(Install(table.c_str(), &api).installed, "install A");
  Sys sa;
  MakeSys(&sa, ~0ull);
  UpdateSlot()(sa.addr());
  CHECK(Index("alloc") < 0 && E.real_updates == 1, "level -1: the real update runs, no node is made");

  // ---- Initialize fails: no node is kept, the node is destroyed in the engine's order, and the next tries are made
  Fresh();
  CHECK(Install(table.c_str(), &api).installed, "reinstall");
  E.init_rc = 3;
  Sys s3;
  MakeSys(&s3, kDyson);
  UpdateSlot()(s3.addr());
  CHECK(E.freed.size() == 2 && Index("node_shutdown") >= 0 && Index("dtor") >= 0, "a node whose Initialize fails is shut down, destructed and freed");
  CHECK(CountLines("node not made", 2) == 2 && CountLines("rc=3") == 2, "each failure is a Warning with the engine's answer");
  CHECK(Index("map") < 0, "nothing is drawn");

  // ---- graphics not up: no node, no allocation
  Fresh();
  E.gs_initialized = 0;
  CHECK(Install(table.c_str(), &api).installed, "install 3");
  Sys s4;
  MakeSys(&s4, kDyson);
  UpdateSlot()(s4.addr());
  CHECK(Index("alloc") < 0 && E.nodes_used == 0, "CGS not initialized: nothing allocated");

  // ---- a vertex buffer that cannot be mapped: not drawn, not shown again
  Fresh();
  E.map_fails = true;
  CHECK(Install(table.c_str(), &api).installed, "install 4");
  Sys s5;
  MakeSys(&s5, kDyson);
  UpdateSlot()(s5.addr());
  CHECK(Index("map") >= 0 && Index("unmap") < 0 && Index("visible1") < 0, "map failed: no unmap, nothing shown");

  // ---- Shutdown chains on an earlier handler (impactsnd's) and still calls the game's function through it
  Fresh();
  InstallEarlier();
  r = Install(table.c_str(), &api);
  CHECK(r.installed, "install over an earlier handler: %s", r.status);
  Sys s6;
  MakeSys(&s6, kDyson);
  UpdateSlot()(s6.addr());
  const uint64_t ret2 = ShutSlot()(s6.addr());
  CHECK(E.earlier_shutdowns == 1 && E.real_shutdowns == 1 && ret2 == s6.addr() + 1, "our handler calls the earlier one, which calls the game's");
  CHECK(Index("free") >= 0 && Index("free") < Index("real_shutdown"), "our nodes are freed before the earlier handler runs");
  ForgetEarlier();

  // ---- Shutdown slot holds something else: refused, the update slot is not touched, nothing runs
  Fresh();
  W64(dc::kShutdownSlotVa, reinterpret_cast<uintptr_t>(dlsym(RTLD_DEFAULT, "getpid")));
  r = Install(table.c_str(), &api);
  CHECK(!r.installed && std::strcmp(r.status, "original_mismatch") == 0, "a word in another library: %s", r.status);
  CHECK(R64(dc::kUpdateSlotVa) == g_base + dc::kUpdateRealVa, "the update slot was not touched");
  CHECK(CountLines("decals shutdown wrapper", 3) == 1, "one ERROR line");

  // ---- the update slot holds something else: refused; the Shutdown handler that did go in passes through and makes no node
  Fresh();
  W64(dc::kUpdateSlotVa, g_base + dc::kUpdateRealVa + 8);
  r = Install(table.c_str(), &api);
  CHECK(!r.installed && std::strcmp(r.status, "original_mismatch") == 0, "update slot not the game's function: %s", r.status);
  CHECK(R64(dc::kUpdateSlotVa) == g_base + dc::kUpdateRealVa + 8, "the update slot is untouched");
  Sys s7;
  MakeSys(&s7, kDyson);
  CHECK(ShutSlot()(s7.addr()) == s7.addr() + 1 && Index("free") < 0, "with the module out, the Shutdown handler only passes through");

  // ---- the table: absent, refused, switched off
  Fresh();
  r = Install((dir + "/nope.bin").c_str(), &api);
  CHECK(!r.installed && r.off && std::strcmp(r.status, "no_table") == 0 && R64(dc::kUpdateSlotVa) == g_base + dc::kUpdateRealVa, "no table: off, slots untouched");
  CHECK(CountLines("decals off: no readable decals.bin", 1) == 1, "one INFO line says no table was found");
  std::vector<uint8_t> broken = Bytes(in);
  broken[0] = 'X';
  const std::string bad = WriteFile(dir, "bad.bin", broken);
  r = Install(bad.c_str(), &api);
  CHECK(!r.installed && !r.off && std::strcmp(r.status, "bad_table") == 0 && CountLines("status=bad_table", 3) == 1 && CountLines("reason=bad_magic", 3) == 1,
        "a bad table: refused with an ERROR line (%s)", r.status);
  std::vector<GroupIn> none;
  const std::string empty = WriteFile(dir, "empty.bin", Bytes(none));
  r = Install(empty.c_str(), &api);
  CHECK(std::strcmp(r.status, "empty_table") == 0, "a table with no groups: %s", r.status);
  // the search without a path: the plugin data directory's decals.bin
  r = Install(nullptr, &api);
  CHECK(r.installed, "found in the data directory: %s", r.status);
  // combat.decals false
  Fresh();
  r = Install(table.c_str(), &api, /*on=*/false);
  CHECK(!r.installed && r.off && std::strcmp(r.status, "off_config") == 0 && R64(dc::kUpdateSlotVa) == g_base + dc::kUpdateRealVa, "combat.decals false keeps the module out");
  // an engine that lacks an entry: refused
  dc::Api missing = api;
  missing.init = nullptr;
  r = Install(table.c_str(), &missing);
  CHECK(std::strcmp(r.status, "engine_export_missing") == 0 && R64(dc::kUpdateSlotVa) == g_base + dc::kUpdateRealVa, "a missing engine entry refuses the module");
  // no libr15
  r = dc::InstallAt(dc::Switches{true, false, false}, nullptr, table.c_str(), &api);
  CHECK(std::strcmp(r.status, "module_not_loaded") == 0, "no libr15: %s", r.status);
  // the plugin's switch files are warned about and changed nothing
  Fresh();
  WriteFile(dir, "decals.off", {});
  WriteFile(dir, "decalflipu.on", {});
  WriteFile(dir, "decalflipv.on", {});
  r = Install(table.c_str(), &api);
  CHECK(r.installed, "the legacy kill file still keeps the module out: %s", r.status);
  CHECK(CountLines("\"event\":\"combat_decals_legacy_switch\"", 2) == 3 && CountLines("file=decals.off") == 1 && CountLines("file=decalflipu.on") == 1 &&
            CountLines("file=decalflipv.on") == 1,
        "one Warning a legacy file");
  CHECK(CountLines("flip_u=0") == 1 && CountLines("flip_v=0") == 1, "the flip files were read");
  std::remove((dir + "/decals.off").c_str());
  std::remove((dir + "/decalflipu.on").c_str());
  std::remove((dir + "/decalflipv.on").c_str());
  // the keys flip the uv: the last group drawn is the war room's one quad (MakeQuad(50): corner i has u = 53 + 10 i, v = 54 + 10 i)
  Fresh();
  Install(table.c_str(), &api, true, true, true);
  CHECK(CountLines("flip_u=1") == 1 && CountLines("flip_v=1") == 1, "the flip keys are in the table line");
  Sys sf;
  MakeSys(&sf, kDyson);
  UpdateSlot()(sf.addr());
  CHECK(E.vbuf[0].uv[0] == 73.0f && E.vbuf[0].uv[1] == 74.0f, "u and v flipped: corner 0 holds corner 2's uv, got %f %f", E.vbuf[0].uv[0], E.vbuf[0].uv[1]);
  Fresh();
  Install(table.c_str(), &api, true, true, false);
  MakeSys(&sf, kDyson);
  UpdateSlot()(sf.addr());
  CHECK(E.vbuf[0].uv[0] == 63.0f && E.vbuf[0].uv[1] == 64.0f, "u flipped: corner 0 holds corner 1's uv, got %f %f", E.vbuf[0].uv[0], E.vbuf[0].uv[1]);
  Fresh();
  Install(table.c_str(), &api, true, false, true);
  MakeSys(&sf, kDyson);
  UpdateSlot()(sf.addr());
  CHECK(E.vbuf[0].uv[0] == 83.0f && E.vbuf[0].uv[1] == 84.0f, "v flipped: corner 0 holds corner 3's uv, got %f %f", E.vbuf[0].uv[0], E.vbuf[0].uv[1]);
  dc::ResetForTest();
}

// A game space whose level is not stored yet (the constructor leaves 0 or -1) has no record; its updates are throttled the same way: no read on each frame.
static void TestNoLevelYetIsThrottled(const std::string& dir) {
  const dc::Api api = MakeApi();
  std::vector<GroupIn> in(1);
  in[0] = {kDyson, 0x1111, 0, {MakeQuad(1)}};
  const std::string table = WriteFile(dir, "nolevel.bin", Bytes(in));
  Fresh();
  CHECK(Install(table.c_str(), &api).installed, "install");
  Sys s;
  MakeSys(&s, 0);
  UpdateSlot()(s.addr());
  const uint64_t reads0 = ReadCount();
  for (int i = 0; i < 49; ++i) UpdateSlot()(s.addr());
  CHECK(ReadCount() == reads0 && dc::CountersForTest().tick_calls == 1, "no level yet: %llu reads and %llu calls in 49 updates",
        (unsigned long long)(ReadCount() - reads0), (unsigned long long)dc::CountersForTest().tick_calls);
  // after the skip the space is looked at again, and a space that has become a combat map is found
  s.gs[0x58 / 8] = kDyson;
  for (int i = 0; i < 70; ++i) UpdateSlot()(s.addr());
  CHECK(dc::CountersForTest().tick_calls >= 2, "the throttled space was never looked at again");
  // Shutdown forgets the entry: a new bullet system at the same address, in a combat level, is looked at on its first update
  Fresh();
  CHECK(Install(table.c_str(), &api).installed, "install");
  Sys re;
  MakeSys(&re, EchoVR::CSymbol64Hash("mpl_arena_a"));
  UpdateSlot()(re.addr());  // looked at, found quiet, skipped from now on
  ShutSlot()(re.addr());
  MakeSys(&re, kDyson);
  const uint64_t before = dc::CountersForTest().tick_calls;
  UpdateSlot()(re.addr());
  CHECK(dc::CountersForTest().tick_calls == before + 1, "a new system at a freed address was skipped");
  // a combat space is looked at on every update
  Fresh();
  CHECK(Install(table.c_str(), &api).installed, "install");
  Sys c;
  MakeSys(&c, kDyson);
  for (int i = 0; i < 10; ++i) UpdateSlot()(c.addr());
  CHECK(dc::CountersForTest().tick_calls == 10, "a combat space ran the state machine %llu times in 10 updates", (unsigned long long)dc::CountersForTest().tick_calls);
}

// ---- the scope: arena shares the APK, so a game space in an arena level creates and draws nothing ----

void TestArenaScope(const std::string& dir) {
  const dc::Api api = MakeApi();
  std::vector<GroupIn> in(2);
  in[0] = {kDyson, 0x1111, 0, {MakeQuad(1)}};
  in[1] = {EchoVR::CSymbol64Hash("mpl_arena_a"), 0x2222, 0, {MakeQuad(2)}};  // a table that names an arena level
  const std::string table = WriteFile(dir, "scope.bin", Bytes(in));
  for (const char* name : {"mpl_arena_a", "mpl_arena_a_lowspec", "mpl_arena_b", "mpl_tutorial_a", "mpl_lobby_b", "mpl_lobby_b2", "mpl_lobby_b_combat"}) {
    Fresh();
    CHECK(Install(table.c_str(), &api).installed, "install (%s)", name);
    Sys s;
    MakeSys(&s, EchoVR::CSymbol64Hash(name));
    UpdateSlot()(s.addr());  // the first look reads the space's level
    const uint64_t reads0 = ReadCount(), scans0 = ScanCount();
    for (int i = 0; i < 49; ++i) UpdateSlot()(s.addr());
    CHECK(E.real_updates == 50, "%s: the game's update ran %d times", name, E.real_updates);
    CHECK(ReadCount() == reads0 && ScanCount() == scans0,
          "%s: 49 quiet arena updates made %llu reads and %llu scans", name, (unsigned long long)(ReadCount() - reads0),
          (unsigned long long)(ScanCount() - scans0));
    CHECK(Index("alloc") < 0 && E.nodes_used == 0 && Index("map") < 0 && Index("init") < 0, "%s: an arena or lobby level made or drew a node", name);
    CHECK(CountLines("decals: game space") == 1 && CountLines("acts=0") == 1, "%s: one game-space line that says it does not act", name);
    // the module's lock, its clock and its state machine are reached by the first update and not by the 49 quiet ones (a few relaxed loads each)
    CHECK(dc::CountersForTest().tick_calls == 1, "%s: the state machine ran %llu times in 50 arena updates", name,
          (unsigned long long)dc::CountersForTest().tick_calls);
    CHECK(dc::CountersForTest().nodes_made == 0 && dc::CountersForTest().drawn_quads == 0, "%s: counters", name);
    // Shutdown still passes through, and finds no node
    CHECK(ShutSlot()(s.addr()) == s.addr() + 1 && Index("free") < 0, "%s: Shutdown", name);
  }
}

}  // namespace

int main() {
  StartCounting();
  sentinel::SetLogSink(&Capture);
  char tmpl[] = "/tmp/combat-decals-XXXXXX";
  if (mkdtemp(tmpl) == nullptr) return 2;
  const std::string dir = tmpl;

  CHECK(dc::kUpdateSlotVa == 0x370dbc8 && dc::kUpdateRealVa == 0x2124290 && dc::kShutdownSlotVa == 0x36933e8 && dc::kShutdownRealVa == 0x21226f0,
        "pinned addresses changed");

  TestTable();
  TestHost();
  TestInstall(dir);
  TestArenaScope(dir);
  TestNoLevelYetIsThrottled(dir);

  std::string cmd = "rm -rf " + dir;
  if (std::system(cmd.c_str()) != 0) std::printf("note: could not remove %s\n", dir.c_str());
  if (g_fail != 0) {
    std::printf("decals_test: %d failure(s)\n", g_fail);
    return 1;
  }
  std::printf("decals_test: all checks passed\n");
  return 0;
}
