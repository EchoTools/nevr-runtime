// Host test of decals (src/quest/plugins/q15t/src/decals.{h,cpp}, decals_core.h; issue #1619). Three layers:
//   1. the table file: the parser's refusals, the uv flips, the blend, the level gate, the index and vertex fill;
//   2. the per-system state machine (decals::Host) over stand-in operations: gating, retries, level changes, shutdown, the war room;
//   3. the real installer and wrappers over a stand-in engine: slot words on a fake data page, a fake bullet system, allocator, node,
//      mesh and vertex buffer laid out as the module reads them. Real SafeRead, real mprotect, real files.
#include <dlfcn.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "decals.h"
#include "module_switch.h"
#include "nevr/plugin_interface.h"
#include "nevr/symbol_hash.h"
#include "plugin_log.h"

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
  uint32_t level;
  std::string text;
};
static std::vector<Line> g_lines;
static void FakeLog(const char* id, uint32_t level, const char* msg, const NevrLogField* f, uint32_t n) {
  std::string t = std::string("plugin=") + id + " " + msg;
  for (uint32_t i = 0; i < n; ++i) t += std::string(" ") + f[i].key + "=" + f[i].value;
  g_lines.push_back({level, t});
}
static int CountLines(const char* needle, int level = -1) {
  int c = 0;
  for (const auto& l : g_lines)
    if (l.text.find(needle) != std::string::npos && (level < 0 || static_cast<int>(l.level) == level)) ++c;
  return c;
}

using namespace q15t::decals;

// ---- 1. the table ----------------------------------------------------------------------------------------------------

namespace {

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
  CHECK(kCombatLevels[0] == EchoVR::CSymbol64Hash("mpl_combat_dyson") && kCombatLevels[1] == EchoVR::CSymbol64Hash("mpl_combat_combustion") &&
            kCombatLevels[2] == EchoVR::CSymbol64Hash("mpl_combat_fission") && kCombatLevels[3] == EchoVR::CSymbol64Hash("mpl_combat_gauss"),
        "a combat level symbol is not the SDK's hash of its name");

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
};

int SysIndex(uintptr_t sys) { return static_cast<int>(sys / 0x1000) - 1; }
bool FLevelOf(void* u, uintptr_t sys, uint64_t* level) {
  Fake* f = static_cast<Fake*>(u);
  const int i = SysIndex(sys);
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
  FreeTable(&t);
}

// ---- 3. the installer and the wrappers over a stand-in engine --------------------------------------------------------------------

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
q15t::DecalsVertexFormat FFormat() {
  E.calls.push_back("format");
  q15t::DecalsVertexFormat f;
  std::memset(f.raw, 0x7e, sizeof f.raw);
  return f;
}
int FInit(void* mesh, const q15t::DecalsVertexFormat* fmt, const q15t::DecalsMeshInit* in) {
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
__attribute__((noinline)) void FakeUpdateMovement(uintptr_t) { ++E.real_updates; }
__attribute__((noinline)) uint64_t FakeShutdown(uint64_t self) {
  ++E.real_shutdowns;
  E.calls.push_back("real_shutdown");
  return self + 1;
}
// what impactsnd's wrapper looks like to the module: a function of this same library that calls on
static uintptr_t g_earlier_real = 0;
__attribute__((noinline)) uint64_t EarlierWrapper(uint64_t self) {
  ++E.earlier_shutdowns;
  return reinterpret_cast<uint64_t (*)(uint64_t)>(g_earlier_real)(self);
}

q15t::DecalsApi MakeApi() {
  q15t::DecalsApi a;
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

// a fake bullet system: +0x30 allocator, +0x80 game space; a game space: +0x58 level
struct Sys {
  alignas(16) uintptr_t w[0x20] = {};
  alignas(16) uintptr_t gs[0x20] = {};
  uintptr_t addr() { return reinterpret_cast<uintptr_t>(w); }
};
void MakeSys(Sys* s, uint64_t level) {
  std::memset(s->w, 0, sizeof s->w);
  std::memset(s->gs, 0, sizeof s->gs);
  s->w[0x30 / 8] = reinterpret_cast<uintptr_t>(E.alloc_obj);
  s->w[0x80 / 8] = reinterpret_cast<uintptr_t>(s->gs);
  s->gs[0x58 / 8] = level;
}

struct Page {
  uintptr_t* w;
  uintptr_t base;
};
Page MakePage(uintptr_t update_word, uintptr_t shutdown_word) {
  void* p = mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (p == MAP_FAILED) std::abort();
  auto* w = static_cast<uintptr_t*>(p);
  w[4] = update_word;    // slot at 0x20
  w[5] = shutdown_word;  // slot at 0x28
  mprotect(p, 4096, PROT_READ);
  return {w, reinterpret_cast<uintptr_t>(p)};
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

void TestInstall(const std::string& dir) {
  using UpdateFn = void (*)(uintptr_t);
  using ShutFn = uint64_t (*)(uint64_t);
  const q15t::DecalsApi api = MakeApi();
  std::vector<GroupIn> in(2);
  in[0] = {kDyson, 0x1111, 7, {MakeQuad(1), MakeQuad(10), MakeQuad(20)}};
  in[1] = {kLevelWarRoom, 0x2222, 0, {MakeQuad(50)}};
  const std::string table = WriteFile(dir, "decals.bin", Bytes(in));
  q15t::SetModuleSwitchDirForTest(dir.c_str());
  g_lines.clear();
  const uintptr_t kUp = reinterpret_cast<uintptr_t>(&FakeUpdateMovement), kSh = reinterpret_cast<uintptr_t>(&FakeShutdown);

  // ---- the slots
  q15t::ResetDecalsForTest();
  ResetEngine();
  Page pg = MakePage(kUp, kSh);
  q15t::InstallResult r = q15t::InstallDecalsAt(pg.base, 0x20, kUp - pg.base, 0x28, kSh - pg.base, table.c_str(), &api);
  CHECK(r.status == q15t::SwapStatus::kOk, "install: %s %s", q15t::StatusName(r.status), r.reason);
  CHECK(pg.w[4] != kUp && pg.w[5] != kSh, "both slot words are replaced");
  CHECK(CountLines("decals on", 1) == 1 && CountLines("groups=2") >= 1 && CountLines("quads=4") >= 1, "the install line carries the table's counts");
  CHECK(CountLines("decals shutdown wrapper", 1) == 1 && CountLines("decals update wrapper", 1) == 1, "one INFO line a wrapper");
  const UpdateFn update = reinterpret_cast<UpdateFn>(pg.w[4]);
  const ShutFn shutdown = reinterpret_cast<ShutFn>(pg.w[5]);

  // ---- the first update: the game's own update runs, then the nodes are made and drawn
  Sys s1;
  MakeSys(&s1, kDyson);
  update(s1.addr());
  CHECK(E.real_updates == 1, "the real update ran");
  CHECK(E.nodes_used == 2, "one node for the dyson group, one for the war room group: %d", E.nodes_used);
  CHECK(E.init_max_vtx == 4 && E.init_max_idx == 12 && E.init_index_type == 2 && E.init_material == 0x2222,
        "Initialize of the last node: material %llx vtx %llu idx %llu type %u (the 1-quad war room group)",
        static_cast<unsigned long long>(E.init_material), static_cast<unsigned long long>(E.init_max_vtx),
        static_cast<unsigned long long>(E.init_max_idx), E.init_index_type);
  CHECK(E.init_a0 == E.ctx_array[0] && E.init_a1 == E.ctx_array[1], "Initialize got the memory context's two allocators");
  const uint16_t want_idx[12] = {0, 1, 2, 0, 2, 3, 0, 2, 1, 0, 3, 2};
  CHECK(std::memcmp(E.init_idx_head, want_idx, sizeof want_idx) == 0, "Initialize got the index table");
  CHECK(E.init_mesh == static_cast<void*>(E.nodes[1] + 0xa8), "the mesh is the node + 0xa8");
  // the order of calls of the first node
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
  // the last drawn group is group 1 (1 quad): vbuf holds its 4 vertices
  CHECK(E.vbuf[0].p[0] == 50.0f && E.vbuf[3].uv[1] == 50.0f + 34.0f && E.vbuf[0].c0 == 0xffffffffu, "the vertex buffer holds the group's quad");

  // ---- the second update does not make nodes again
  const int nodes_before = E.nodes_used;
  update(s1.addr());
  CHECK(E.nodes_used == nodes_before && E.real_updates == 2, "a second update makes no node");

  // ---- Shutdown: the nodes are destroyed first, in the engine's order, then the earlier function runs
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
  // the slot already holds our wrapper (a second install): refused, not chained on itself
  {
    const q15t::InstallResult again = q15t::InstallDecalsAt(pg.base, 0x20, kUp - pg.base, 0x28, kSh - pg.base, table.c_str(), &api);
    CHECK(again.status == q15t::SwapStatus::kUnexpectedOriginal && std::strcmp(again.reason, "already_installed") == 0,
          "a second install over our own wrapper: %s %s", q15t::StatusName(again.status), again.reason);
  }
  CHECK(CountLines("bullet system shutdown, its nodes are destroyed") == 1, "the teardown is logged");
  CHECK(q15t::DecalsCountersForTest().nodes_destroyed == 2, "counter");

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
  q15t::ResetDecalsForTest();
  ResetEngine();
  Page pgA = MakePage(kUp, kSh);
  CHECK(q15t::InstallDecalsAt(pgA.base, 0x20, kUp - pgA.base, 0x28, kSh - pgA.base, table.c_str(), &api).status == q15t::SwapStatus::kOk, "install A");
  Sys sa;
  MakeSys(&sa, ~0ull);
  reinterpret_cast<UpdateFn>(pgA.w[4])(sa.addr());
  CHECK(Index("alloc") < 0 && E.real_updates == 1, "level -1: the real update runs, no node is made");

  // ---- Initialize fails: no node is kept, the node is destroyed in the engine's order, and the next tries are made
  q15t::ResetDecalsForTest();
  ResetEngine();
  g_lines.clear();
  Page pg2 = MakePage(kUp, kSh);
  CHECK(q15t::InstallDecalsAt(pg2.base, 0x20, kUp - pg2.base, 0x28, kSh - pg2.base, table.c_str(), &api).status == q15t::SwapStatus::kOk, "reinstall");
  const UpdateFn update2 = reinterpret_cast<UpdateFn>(pg2.w[4]);
  E.init_rc = 3;
  Sys s3;
  MakeSys(&s3, kDyson);
  update2(s3.addr());
  CHECK(E.freed.size() == 2 && Index("node_shutdown") >= 0 && Index("dtor") >= 0, "a node whose Initialize fails is shut down, destructed and freed");
  CHECK(CountLines("node not made", 2) == 2 && CountLines("rc=3") == 2, "each failure is a Warning with the engine's answer");
  CHECK(Index("map") < 0, "nothing is drawn");

  // ---- graphics not up: no node, no allocation
  q15t::ResetDecalsForTest();
  ResetEngine();
  E.gs_initialized = 0;
  Page pg3 = MakePage(kUp, kSh);
  CHECK(q15t::InstallDecalsAt(pg3.base, 0x20, kUp - pg3.base, 0x28, kSh - pg3.base, table.c_str(), &api).status == q15t::SwapStatus::kOk, "install 3");
  Sys s4;
  MakeSys(&s4, kDyson);
  reinterpret_cast<UpdateFn>(pg3.w[4])(s4.addr());
  CHECK(Index("alloc") < 0 && E.nodes_used == 0, "CGS not initialized: nothing allocated");

  // ---- a vertex buffer that cannot be mapped: not drawn, not shown again
  q15t::ResetDecalsForTest();
  ResetEngine();
  E.map_fails = true;
  Page pg4 = MakePage(kUp, kSh);
  CHECK(q15t::InstallDecalsAt(pg4.base, 0x20, kUp - pg4.base, 0x28, kSh - pg4.base, table.c_str(), &api).status == q15t::SwapStatus::kOk, "install 4");
  Sys s5;
  MakeSys(&s5, kDyson);
  reinterpret_cast<UpdateFn>(pg4.w[4])(s5.addr());
  CHECK(Index("map") >= 0 && Index("unmap") < 0 && Index("visible1") < 0, "map failed: no unmap, nothing shown");

  // ---- Shutdown chains on an earlier wrapper of this library (impactsnd's) and still calls the game's function through it
  q15t::ResetDecalsForTest();
  ResetEngine();
  g_earlier_real = kSh;
  Page pg5 = MakePage(kUp, reinterpret_cast<uintptr_t>(&EarlierWrapper));
  r = q15t::InstallDecalsAt(pg5.base, 0x20, kUp - pg5.base, 0x28, kSh - pg5.base, table.c_str(), &api);
  CHECK(r.status == q15t::SwapStatus::kOk, "install over an earlier wrapper: %s %s", q15t::StatusName(r.status), r.reason);
  Sys s6;
  MakeSys(&s6, kDyson);
  reinterpret_cast<UpdateFn>(pg5.w[4])(s6.addr());
  const uint64_t ret2 = reinterpret_cast<ShutFn>(pg5.w[5])(s6.addr());
  CHECK(E.earlier_shutdowns == 1 && E.real_shutdowns == 1 && ret2 == s6.addr() + 1, "our wrapper calls the earlier one, which calls the game's");
  CHECK(Index("free") >= 0 && Index("free") < Index("real_shutdown"), "our nodes are freed before the earlier wrapper runs");

  // ---- Shutdown slot holds something else: refused, the update slot is not touched, nothing runs
  q15t::ResetDecalsForTest();
  ResetEngine();
  g_lines.clear();
  void* const libc_fn = dlsym(RTLD_DEFAULT, "getpid");
  Page pg6 = MakePage(kUp, reinterpret_cast<uintptr_t>(libc_fn));
  r = q15t::InstallDecalsAt(pg6.base, 0x20, kUp - pg6.base, 0x28, kSh - pg6.base, table.c_str(), &api);
  CHECK(r.status == q15t::SwapStatus::kUnexpectedOriginal && std::strcmp(r.reason, "slot_not_stock_or_own") == 0,
        "a word in another library: %s %s", q15t::StatusName(r.status), r.reason);
  CHECK(pg6.w[4] == kUp && pg6.w[5] == reinterpret_cast<uintptr_t>(libc_fn), "neither slot was touched");
  CHECK(CountLines("decals shutdown wrapper", 3) == 1, "one ERROR line");
  // a word in a data mapping of this very library is not code: refused
  q15t::ResetDecalsForTest();
  ResetEngine();
  Page pg6b = MakePage(kUp, reinterpret_cast<uintptr_t>(&E.gs_initialized));
  r = q15t::InstallDecalsAt(pg6b.base, 0x20, kUp - pg6b.base, 0x28, kSh - pg6b.base, table.c_str(), &api);
  CHECK(r.status == q15t::SwapStatus::kUnexpectedOriginal && std::strcmp(r.reason, "slot_not_stock_or_own") == 0,
        "a word in this library's data: %s %s", q15t::StatusName(r.status), r.reason);

  // ---- the update slot holds something else: refused
  q15t::ResetDecalsForTest();
  ResetEngine();
  Page pg7 = MakePage(kUp + 8, kSh);
  r = q15t::InstallDecalsAt(pg7.base, 0x20, kUp - pg7.base, 0x28, kSh - pg7.base, table.c_str(), &api);
  CHECK(r.status == q15t::SwapStatus::kUnexpectedOriginal, "update slot not the game's function: %s", q15t::StatusName(r.status));
  CHECK(pg7.w[4] == kUp + 8, "the update slot is untouched");
  // the Shutdown wrapper that did go in passes through and makes no node
  const ShutFn sh7 = reinterpret_cast<ShutFn>(pg7.w[5]);
  Sys s7;
  MakeSys(&s7, kDyson);
  CHECK(sh7(s7.addr()) == s7.addr() + 1 && Index("free") < 0, "with the module out, the Shutdown wrapper only passes through");

  // ---- the table: absent, refused, switched off
  q15t::ResetDecalsForTest();
  ResetEngine();
  g_lines.clear();
  Page pg8 = MakePage(kUp, kSh);
  r = q15t::InstallDecalsAt(pg8.base, 0x20, kUp - pg8.base, 0x28, kSh - pg8.base, (dir + "/nope.bin").c_str(), &api);
  CHECK(r.status != q15t::SwapStatus::kOk && std::strcmp(r.reason, "off") == 0 && pg8.w[4] == kUp && pg8.w[5] == kSh, "no table: off, slots untouched");
  CHECK(CountLines("decals off: no readable decals.bin", 1) == 1, "one INFO line says what was searched");
  std::vector<uint8_t> broken = Bytes(in);
  broken[0] = 'X';
  const std::string bad = WriteFile(dir, "bad.bin", broken);
  r = q15t::InstallDecalsAt(pg8.base, 0x20, kUp - pg8.base, 0x28, kSh - pg8.base, bad.c_str(), &api);
  CHECK(std::strcmp(r.reason, "bad_table") == 0 && pg8.w[4] == kUp && CountLines("decals refused: decals.bin is not a usable table", 3) == 1,
        "a bad table: refused with an ERROR line, slots untouched (%s)", r.reason);
  std::vector<GroupIn> none;
  const std::string empty = WriteFile(dir, "empty.bin", Bytes(none));
  r = q15t::InstallDecalsAt(pg8.base, 0x20, kUp - pg8.base, 0x28, kSh - pg8.base, empty.c_str(), &api);
  CHECK(std::strcmp(r.reason, "empty_table") == 0, "a table with no groups: %s", r.reason);
  // the search without a path: the plugin data directory's decals.bin
  Page pg9 = MakePage(kUp, kSh);
  r = q15t::InstallDecalsAt(pg9.base, 0x20, kUp - pg9.base, 0x28, kSh - pg9.base, nullptr, &api);
  CHECK(r.status == q15t::SwapStatus::kOk, "found in the data directory: %s %s", q15t::StatusName(r.status), r.reason);
  // decals.off
  q15t::ResetDecalsForTest();
  const std::string off = WriteFile(dir, "decals.off", {});
  Page pg10 = MakePage(kUp, kSh);
  r = q15t::InstallDecalsAt(pg10.base, 0x20, kUp - pg10.base, 0x28, kSh - pg10.base, table.c_str(), &api);
  CHECK(std::strcmp(r.reason, "off_file") == 0 && pg10.w[4] == kUp, "decals.off keeps the module out");
  std::remove(off.c_str());
  // an engine that lacks an entry: refused
  q15t::DecalsApi missing = api;
  missing.init = nullptr;
  r = q15t::InstallDecalsAt(pg10.base, 0x20, kUp - pg10.base, 0x28, kSh - pg10.base, table.c_str(), &missing);
  CHECK(std::strcmp(r.reason, "engine_export_missing") == 0 && pg10.w[4] == kUp, "a missing engine entry refuses the module");
  q15t::ResetDecalsForTest();
}

}  // namespace

int main() {
  NvrGameContext ctx{};
  ctx.ctx_size = sizeof ctx;
  ctx.plugin_id = "q15t";
  ctx.log = &FakeLog;
  q15t::SetLogContext(&ctx);
  char tmpl[] = "/tmp/q15t-decals-XXXXXX";
  if (mkdtemp(tmpl) == nullptr) return 2;
  const std::string dir = tmpl;

  CHECK(q15t::kDecalsUpdateSlot == 0x370dbc8 && q15t::kDecalsUpdateReal == 0x2124290 && q15t::kDecalsShutdownSlot == 0x36933e8 &&
            q15t::kDecalsShutdownReal == 0x21226f0,
        "pinned addresses changed");

  TestTable();
  TestHost();
  TestInstall(dir);

  std::string cmd = "rm -rf " + dir;
  if (std::system(cmd.c_str()) != 0) std::printf("note: could not remove %s\n", dir.c_str());
  if (g_fail != 0) {
    std::printf("decals_test: %d failure(s)\n", g_fail);
    return 1;
  }
  std::printf("decals_test: all checks passed\n");
  return 0;
}
