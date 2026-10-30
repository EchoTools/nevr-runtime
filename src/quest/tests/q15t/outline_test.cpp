// Host test of the outline module (src/quest/plugins/q15t/src/outline.cpp): a fake slot page (real mprotect, real /proc/self/maps)
// holds the CGScene::SetOutlineMode and CGDrawDataSet::SetOutlineMode slots, the render-switch word and one code-word pin; a fake
// scene laid out as the game's walk reads it (outline.h) is walked through the swapped slot, and a fake CGDrawDataSet::SetOutlineMode
// records every call. The CGVertexFormat functions are fakes over a 36-element, 0x128-byte format.
#include <sys/mman.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "module_switch.h"
#include "nevr/plugin_interface.h"
#include "outline.h"
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

// ---- the fake CGVertexFormat: 36 elements of 8 bytes, count at +0x120 ----
struct Fmt {
  uint8_t e[36][8];
  uint64_t count;
};
static_assert(sizeof(Fmt) == q15t::kFormatBytes, "format size");
static void FmtCtor(void* f) { std::memset(f, 0, sizeof(Fmt)); }
static uint64_t FmtCount(const void* f) { return static_cast<const Fmt*>(f)->count; }
static void FmtGetIndex(const void* f, uint64_t i, void* out) { std::memcpy(out, static_cast<const Fmt*>(f)->e[i], 8); }
static void FmtGetUsage(const void* f, uint32_t usage, void* out) {
  const Fmt* fm = static_cast<const Fmt*>(f);
  for (uint64_t i = 0; i < fm->count; ++i)
    if (fm->e[i][0] == usage) {
      std::memcpy(out, fm->e[i], 8);
      return;
    }
}
static void FmtAppend(void* f, const void* e) {
  Fmt* fm = static_cast<Fmt*>(f);
  std::memcpy(fm->e[fm->count++], e, 8);
}
static const q15t::OutlineFns kFns = {&FmtCtor, &FmtCount, &FmtGetIndex, &FmtGetUsage, &FmtAppend};
static void El(Fmt* f, uint8_t usage, uint8_t stream, uint8_t tag) {
  uint8_t* e = f->e[f->count++];
  std::memset(e, 0, 8);
  e[0] = usage;
  e[1] = tag;
  e[6] = stream;
}

// ---- the fake draw-data-set function and the stock scene function ----
struct DdsCall {
  uintptr_t dds;
  uint32_t mode;
  const void* fmt;
  uint32_t prim;
};
static std::vector<DdsCall> g_dds;
static int g_stock_scene = 0;
extern "C" __attribute__((noinline)) void FakeDds(uintptr_t dds, uint32_t mode, const void* fmt, uint32_t prim) {
  g_dds.push_back({dds, mode, fmt, prim});
}
extern "C" __attribute__((noinline)) void StockScene(uintptr_t, uint32_t) { ++g_stock_scene; }

// ---- the fake scene (outline.h layout) ----
static uint8_t g_scene[0x80], g_res[0x100];
static uint8_t g_meshes[3][0x268];
static uint8_t g_rows[3][0x98];
static uint8_t g_prims[8][0x70];
static uint8_t g_formats[2][0x150];
static uint8_t g_dds_mem[8][0x778];
template <typename T>
static void Put(void* base, uintptr_t off, T v) {
  std::memcpy(static_cast<uint8_t*>(base) + off, &v, sizeof v);
}
static Fmt* SceneFmt(int i) { return reinterpret_cast<Fmt*>(g_formats[i]); }
static Fmt* ComputeFmt(int mesh) { return reinterpret_cast<Fmt*>(g_meshes[mesh] + q15t::kMeshComputeFmt); }

// mesh 0: 2 draw sets, not skinned, format 0, prims 0..1; mesh 1: 3 draw sets, skinned, format 1, prims 2..4; mesh 2: no draw sets
static void BuildScene(uint64_t scene_elements) {
  std::memset(g_scene, 0, sizeof g_scene);
  std::memset(g_res, 0, sizeof g_res);
  std::memset(g_meshes, 0, sizeof g_meshes);
  std::memset(g_formats, 0, sizeof g_formats);
  Put(g_scene, q15t::kSceneRes, reinterpret_cast<uintptr_t>(g_res));
  Put(g_scene, q15t::kSceneMeshes, reinterpret_cast<uintptr_t>(g_meshes));
  Put<uint64_t>(g_scene, q15t::kSceneMeshCount, 3);
  Put(g_res, q15t::kResRows, reinterpret_cast<uintptr_t>(g_rows));
  Put(g_res, q15t::kResPrims, reinterpret_cast<uintptr_t>(g_prims));
  Put(g_res, q15t::kResFormats, reinterpret_cast<uintptr_t>(g_formats));
  const uint32_t fmt_idx[3] = {0, 1, 0}, prim_idx[3] = {0, 2, 5};
  const uint64_t sets[3] = {2, 3, 0};
  for (int i = 0; i < 3; ++i) {
    Put(g_rows[i], q15t::kRowFormat, fmt_idx[i]);
    Put(g_rows[i], q15t::kRowPrim, prim_idx[i]);
    Put(g_meshes[i], q15t::kMeshDds, reinterpret_cast<uintptr_t>(g_dds_mem[i * 3]));
    Put(g_meshes[i], q15t::kMeshDdsCount, sets[i]);
  }
  for (int p = 0; p < 8; ++p) Put<uint32_t>(g_prims[p], q15t::kPrimType, 100 + p);
  Put<uint32_t>(g_meshes[1], q15t::kMeshSkinned, 1);
  El(SceneFmt(0), 0, 0, 0x10);
  // the skinned mesh's scene format: position in stream 0, position / colour (5) / normal in stream 1
  El(SceneFmt(1), 0, 0, 0x20);
  El(SceneFmt(1), 0, 1, 0xAA);
  El(SceneFmt(1), 5, 1, 0xAB);
  El(SceneFmt(1), 2, 1, 0xAC);
  while (SceneFmt(1)->count < scene_elements && SceneFmt(1)->count < 36) El(SceneFmt(1), 9, 0, 0x30);
  if (scene_elements > 36) SceneFmt(1)->count = scene_elements;  // a format claiming more elements than it holds
  El(ComputeFmt(1), 0, 1, 0xBB);
  El(ComputeFmt(1), 2, 1, 0xCC);
}

// ---- the slot page ----
enum : uintptr_t { kSceneSlot = 0x10, kDdsSlot = 0x18, kSwitch = 0x20, kWord = 0x28 };
struct World {
  uint8_t* mem = nullptr;
  uintptr_t base = 0;
  q15t::OutlinePins pins{};
  q15t::WordPin word{};
};
static World MakeWorld(bool dds_slot_ok = true) {
  World w;
  w.mem = static_cast<uint8_t*>(mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
  if (w.mem == MAP_FAILED) std::abort();
  w.base = reinterpret_cast<uintptr_t>(w.mem);
  w.pins.scene_slot = kSceneSlot;
  w.pins.scene_fn = reinterpret_cast<uintptr_t>(&StockScene) - w.base;
  w.pins.dds_slot = kDdsSlot;
  w.pins.dds_fn = reinterpret_cast<uintptr_t>(&FakeDds) - w.base;
  w.pins.render_switch = kSwitch;
  Put(w.mem, kSceneSlot, reinterpret_cast<uintptr_t>(&StockScene));
  Put(w.mem, kDdsSlot, dds_slot_ok ? reinterpret_cast<uintptr_t>(&FakeDds) : reinterpret_cast<uintptr_t>(&StockScene));
  Put<uint32_t>(w.mem, kSwitch, 1);
  Put<uint32_t>(w.mem, kWord, 0xf9403016);
  w.word = {kWord, 0xf9403016};
  mprotect(w.mem, 4096, PROT_READ);
  return w;
}
static uintptr_t SlotWord(const World& w) { return *reinterpret_cast<const uintptr_t*>(w.mem + kSceneSlot); }
static void CallScene(const World& w, uint32_t mode) {
  reinterpret_cast<void (*)(uintptr_t, uint32_t)>(SlotWord(w))(reinterpret_cast<uintptr_t>(g_scene), mode);
}

static std::string g_dir;
static void Touch(const char* name) {
  FILE* f = std::fopen((g_dir + "/" + name).c_str(), "w");
  if (f) std::fclose(f);
}
static void ClearDir() {
  unlink((g_dir + "/outline.off").c_str());
  unlink((g_dir + "/outline_noteam.off").c_str());
}
static void Reset() {
  g_lines.clear();
  g_dds.clear();
  g_stock_scene = 0;
  q15t::ResetOutlineForTest();
  ClearDir();
}

int main() {
  char tmpl[] = "/tmp/q15t-ol-XXXXXX";
  if (mkdtemp(tmpl) == nullptr) return 2;
  g_dir = tmpl;
  NvrGameContext ctx{};
  ctx.ctx_size = sizeof ctx;
  ctx.plugin_id = "q15t";
  ctx.log = &FakeLog;
  q15t::SetLogContext(&ctx);
  q15t::SetModuleSwitchDirForTest(g_dir.c_str());

  // ---------------------------------------------------------------- pinned constants (ReVault libr15 - 0x100000; docs section 1)
  {
    const q15t::OutlinePins& p = q15t::kOutlinePinned;
    CHECK(p.scene_slot == 0x36fb718 && p.scene_fn == 0x18c5140, "scene pin");
    CHECK(p.dds_slot == 0x36e3bc0 && p.dds_fn == 0x183ab9c, "draw data set pin");
    CHECK(p.render_switch == 0x3753260, "render switch pin");
    CHECK(q15t::kMeshStride == 0x268 && q15t::kDdsStride == 0x778 && q15t::kRowStride == 0x98 && q15t::kFormatStride == 0x150 &&
              q15t::kPrimStride == 0x70 && q15t::kPrimType == 0x40,
          "walk strides");
    CHECK(sizeof q15t::kOutlineWalkWords / sizeof q15t::kOutlineWalkWords[0] == 13, "walk word pins");
    CHECK(q15t::kModeTeam == 7 && q15t::kModeNone == 0, "modes");
  }

  // ---------------------------------------------------------------- refusals: nothing swapped
  {
    Reset();
    World w = MakeWorld();
    Touch("outline.off");
    auto r = q15t::InstallOutlineAt(w.base, w.pins, &w.word, 1, kFns);
    CHECK(std::strcmp(r.reason, "off_file") == 0, "off file reason %s", r.reason);
    CHECK(SlotWord(w) == reinterpret_cast<uintptr_t>(&StockScene), "off file swapped the slot");
    CHECK(Count("outline off", NEVR_LOG_INFO) == 1, "off line");
    ClearDir();
    r = q15t::InstallOutlineAt(0, w.pins, &w.word, 1, kFns);
    CHECK(std::strcmp(r.reason, "no_base") == 0, "no base");
    q15t::OutlineFns missing = kFns;
    missing.get_usage = nullptr;
    r = q15t::InstallOutlineAt(w.base, w.pins, &w.word, 1, missing);
    CHECK(std::strcmp(r.reason, "vertex_format_fns") == 0, "missing fn %s", r.reason);
    const q15t::WordPin bad = {kWord, 0xf9403017};
    r = q15t::InstallOutlineAt(w.base, w.pins, &bad, 1, kFns);
    CHECK(std::strcmp(r.reason, "word_pin") == 0, "word pin %s", r.reason);
    CHECK(Count("outline refused: code word pin mismatch", NEVR_LOG_ERROR) == 1, "word pin line");
    CHECK(SlotWord(w) == reinterpret_cast<uintptr_t>(&StockScene), "a refusal swapped the slot");
    World w2 = MakeWorld(false);
    r = q15t::InstallOutlineAt(w2.base, w2.pins, &w2.word, 1, kFns);
    CHECK(std::strcmp(r.reason, "dds_slot") == 0, "dds slot %s", r.reason);
    CHECK(SlotWord(w2) == reinterpret_cast<uintptr_t>(&StockScene), "dds refusal swapped the slot");
    CHECK(Count("outline refused: nothing changed", NEVR_LOG_ERROR) >= 3, "refusal lines are ERROR");
  }

  // ---------------------------------------------------------------- the walk: merged format for skinned meshes, team mode dropped
  {
    Reset();
    World w = MakeWorld();
    BuildScene(4);
    const auto r = q15t::InstallOutlineAt(w.base, w.pins, &w.word, 1, kFns);
    CHECK(r.status == q15t::SwapStatus::kOk, "install %s %s", q15t::StatusName(r.status), r.reason);
    CHECK(SlotWord(w) == q15t::OutlineWrapperForTest(), "slot not swapped");
    CHECK(Count("q15t_outline_install", NEVR_LOG_INFO) == 1, "install event");
    CallScene(w, 7);
    CHECK(g_stock_scene == 0, "the stock scene function ran");
    CHECK(g_dds.size() == 5, "draw set calls %zu", g_dds.size());
    if (g_dds.size() == 5) {
      for (int k = 0; k < 2; ++k) {
        CHECK(g_dds[k].dds == reinterpret_cast<uintptr_t>(g_dds_mem[0]) + k * 0x778, "mesh 0 dds %d", k);
        CHECK(g_dds[k].mode == 7, "unskinned mesh keeps the team mode");
        CHECK(g_dds[k].fmt == SceneFmt(0), "unskinned mesh keeps the scene format");
        CHECK(g_dds[k].prim == 100u + k, "mesh 0 prim %u", g_dds[k].prim);
      }
      const Fmt* m = static_cast<const Fmt*>(g_dds[2].fmt);
      CHECK(m != SceneFmt(1) && m != ComputeFmt(1), "skinned mesh got a merged format");
      CHECK(m->count == 4, "merged count %llu", static_cast<unsigned long long>(m->count));
      CHECK(m->e[0][1] == 0x20 && m->e[1][1] == 0xBB && m->e[2][1] == 0xAB && m->e[3][1] == 0xCC, "merge rule %02x %02x %02x %02x",
            m->e[0][1], m->e[1][1], m->e[2][1], m->e[3][1]);
      for (int k = 2; k < 5; ++k) {
        CHECK(g_dds[k].mode == 0, "skinned mesh's team outline not dropped");
        CHECK(g_dds[k].fmt == m, "all draw sets share the merged format");
        CHECK(g_dds[k].prim == 100u + k, "mesh 1 prim %u", g_dds[k].prim);
        CHECK(g_dds[k].dds == reinterpret_cast<uintptr_t>(g_dds_mem[3]) + (k - 2) * 0x778, "mesh 1 dds %d", k);
      }
    }
    q15t::OutlineCounters c = q15t::OutlineGetCounters();
    CHECK(c.scene_calls == 1 && c.skinned_meshes == 1 && c.merged == 1 && c.team_dropped == 3, "counters");
    CHECK(Count("renderer outline switch", NEVR_LOG_INFO) == 1 && Count("render_switch=1") == 1, "render switch witness");
    CHECK(Count("skinned meshes got the main pass's vertex format", NEVR_LOG_INFO) == 1, "summary at call 1");

    // the scanner's mode 12 is passed unchanged; the merged format is interned (same pointer)
    const void* first = g_dds.size() == 5 ? g_dds[2].fmt : nullptr;
    g_dds.clear();
    CallScene(w, 12);
    CHECK(g_dds.size() == 5 && g_dds[2].mode == 12 && g_dds[0].mode == 12, "mode 12 changed");
    CHECK(g_dds.size() == 5 && g_dds[2].fmt == first, "merged format not interned");
    CHECK(Count("renderer outline switch") == 1, "switch witness repeated");
    CHECK(Count("skinned meshes got the main pass's vertex format") == 2, "summary at call 2");
    g_dds.clear();
    CallScene(w, 12);
    CHECK(Count("skinned meshes got the main pass's vertex format") == 2, "summary at call 3 (not a power of two)");

    // a compute buffer with no elements: not skinned
    ComputeFmt(1)->count = 0;
    g_dds.clear();
    CallScene(w, 7);
    CHECK(g_dds.size() == 5 && g_dds[2].mode == 7 && g_dds[2].fmt == SceneFmt(1), "empty compute format treated as skinned");
    // the skinned word cleared: not skinned
    ComputeFmt(1)->count = 2;
    Put<uint32_t>(g_meshes[1], q15t::kMeshSkinned, 0);
    g_dds.clear();
    CallScene(w, 7);
    CHECK(g_dds.size() == 5 && g_dds[2].mode == 7 && g_dds[2].fmt == SceneFmt(1), "skinned word ignored");
  }

  // ---------------------------------------------------------------- outline_noteam.off keeps mode 7
  {
    Reset();
    World w = MakeWorld();
    BuildScene(4);
    Touch("outline_noteam.off");
    q15t::InstallOutlineAt(w.base, w.pins, &w.word, 1, kFns);
    CHECK(Count("team_outline=kept") == 1, "noteam off not logged");
    CallScene(w, 7);
    CHECK(g_dds.size() == 5 && g_dds[2].mode == 7, "noteam off still drops mode 7");
    CHECK(g_dds.size() == 5 && g_dds[2].fmt != SceneFmt(1), "noteam off dropped the merge");
    CHECK(q15t::OutlineGetCounters().team_dropped == 0, "team counter");
  }

  // ---------------------------------------------------------------- a scene format with more than 36 elements: scene format, one warning
  {
    Reset();
    World w = MakeWorld();
    BuildScene(37);
    q15t::InstallOutlineAt(w.base, w.pins, &w.word, 1, kFns);
    CallScene(w, 12);
    CallScene(w, 12);
    CHECK(g_dds.size() == 10 && g_dds[2].fmt == SceneFmt(1), "an oversized format was merged");
    CHECK(Count("more elements than a format holds", NEVR_LOG_WARNING) == 1, "oversize warning once");
    CHECK(q15t::OutlineGetCounters().merge_refused == 2, "refused counter");
  }

  std::printf("outline_test: %s (%d failures)\n", g_fail == 0 ? "OK" : "FAILED", g_fail);
  return g_fail == 0 ? 0 : 1;
}
