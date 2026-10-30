// Host test of shieldgamma (src/quest/plugins/q15t/src/shieldgamma.cpp, row R58): the real install path against a fake game-library image whose
// JUMP_SLOT 0x36d2020 holds base + 0x272d55c. The fake CSetMaterialPropertyNode::Update records every call.
#include <sys/mman.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "nevr/plugin_interface.h"
#include "pbhud_image.h"
#include "plugin_log.h"
#include "shieldgamma.h"

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
  if (std::getenv("PBHUD_LOG")) std::printf("LOG level=%u %s\n", level, t.c_str());  // PBHUD_LOG=1: every record as it is emitted
  g_lines.push_back({level, t});
}
static int Count(const char* needle, int level = -1) {
  int n = 0;
  for (const auto& l : g_lines)
    if (l.text.find(needle) != std::string::npos && (level < 0 || (int)l.level == level)) ++n;
  return n;
}

static const uintptr_t kSlot = 0x36d2020, kFn = 0x272d55c;
static const uint64_t kMat = 0xdbde1b05fa8f6200ull, kTube = 0xb28e940eb21a5798ull, kRing = 0xdd35e419672ee0baull;
static const uint64_t kOtherProp = 0xd1b26f18aaaaaaaaull;

struct Call {
  const void* node;
  const void* data;
  uint8_t bytes[0x58];
};
static std::vector<Call> g_calls;
static bool g_reads = true;
static uint64_t FakeUpdate(void* node, const void* data) {
  Call c{node, data, {}};
  if (g_reads) std::memcpy(c.bytes, data, sizeof c.bytes);
  g_calls.push_back(c);
  return 0x70 + g_calls.size();
}
using UpdateFn = uint64_t (*)(void*, const void*);

struct alignas(16) Data {
  uint8_t b[0x60];
};
static Data MakeData(uint64_t mat, uint64_t prop, int32_t type, float r, float g, float b, float a, int32_t space) {
  Data d;
  std::memset(d.b, 0x5c, sizeof d.b);
  std::memcpy(d.b + 0x20, &mat, 8);
  std::memcpy(d.b + 0x28, &prop, 8);
  std::memcpy(d.b + 0x30, &type, 4);
  const float v[4] = {r, g, b, a};
  std::memcpy(d.b + 0x38, v, 16);
  std::memcpy(d.b + 0x4c, &space, 4);
  return d;
}
static void Rgba(const uint8_t* b, float* out) { std::memcpy(out, b + 0x38, 16); }
static uint64_t Mat(const uint8_t* b) {
  uint64_t m;
  std::memcpy(&m, b + 0x20, 8);
  return m;
}
static float Dec(float x) { return std::pow(x < 0 ? 0 : (x > 1 ? 1 : x), 2.2f); }

static void Fresh(pbhud::Image& img) {
  g_lines.clear();
  g_calls.clear();
  g_reads = true;
  q15t::ResetShieldGammaForTest();
  img.Real(kFn, reinterpret_cast<void*>(&FakeUpdate));
  img.Slot(kSlot, kFn);
}

int main() {
  NvrGameContext ctx{};
  ctx.ctx_size = sizeof ctx;
  ctx.plugin_id = "q15t";
  ctx.log = &FakeLog;
  q15t::SetLogContext(&ctx);
  pbhud::Image img;
  void* node = reinterpret_cast<void*>(0x6000);

  // 1. gamma only (the source's shieldgamma.on, tube mirror off)
  Fresh(img);
  q15t::InstallResult ir = q15t::InstallShieldGamma(img.base, true, false);
  CHECK(ir.status == q15t::SwapStatus::kOk && img.Get(kSlot) != img.base + kFn, "install: %s", q15t::StatusName(ir.status));
  CHECK(Count("shieldgamma: on", 1) == 1 && Count("gamma=1") == 1 && Count("tube_mirror=0") == 1, "no INFO install line with the switches");
  UpdateFn update = reinterpret_cast<UpdateFn>(img.Get(kSlot));
  Data d = MakeData(kMat, kOtherProp, 1, 0.941f, 0.466f, 0.054f, 0.25f, 0);
  uint64_t r = update(node, &d);
  CHECK(g_calls.size() == 1 && g_calls[0].node == node && g_calls[0].data == &d, "gamma: one call with the same node and data pointer");
  float seen[4];
  Rgba(g_calls[0].bytes, seen);
  CHECK(seen[0] == Dec(0.941f) && seen[1] == Dec(0.466f) && seen[2] == Dec(0.054f), "rgb not pow(sat(x), 2.2): %g %g %g", (double)seen[0], (double)seen[1], (double)seen[2]);
  CHECK(seen[3] == 0.25f, "alpha changed: %g", (double)seen[3]);
  float after[4];
  Rgba(d.b, after);
  CHECK(after[0] == 0.941f && after[1] == 0.466f && after[2] == 0.054f && after[3] == 0.25f, "node's value not put back");
  CHECK(r == 0x71, "result not passed: %llx", (unsigned long long)r);
  CHECK(Count("shieldgamma: decode") == 1 && Count("n=1") == 1, "no decode #1 line");
  CHECK(std::fabs(seen[0] - 0.8738f) < 0.01f, "sanity of the decode of the orange team colour: %g", (double)seen[0]);

  // 2. the clamp: sat() before pow
  g_calls.clear();
  d = MakeData(kMat, kOtherProp, 1, -0.5f, 0.5f, 2.0f, 1.0f, 0);
  update(node, &d);
  Rgba(g_calls[0].bytes, seen);
  CHECK(seen[0] == 0.0f && seen[1] == Dec(0.5f) && seen[2] == 1.0f, "clamp: %g %g %g", (double)seen[0], (double)seen[1], (double)seen[2]);

  // 3. what is not a linear vec4 write to the shield material passes with the original pointer and bytes
  struct Case {
    const char* what;
    Data d;
  } cases[] = {
      {"other material", MakeData(kMat + 1, kOtherProp, 1, 0.5f, 0.5f, 0.5f, 0.5f, 0)},
      {"float type", MakeData(kMat, kOtherProp, 0, 0.5f, 0.5f, 0.5f, 0.5f, 0)},
      {"sRGB colourspace", MakeData(kMat, kOtherProp, 1, 0.5f, 0.5f, 0.5f, 0.5f, 1)},
  };
  for (auto& c : cases) {
    g_calls.clear();
    update(node, &c.d);
    CHECK(g_calls.size() == 1 && g_calls[0].data == &c.d, "%s: not one pass-through call", c.what);
    float v[4];
    Rgba(g_calls[0].bytes, v);
    CHECK(v[0] == 0.5f && v[1] == 0.5f && v[2] == 0.5f && v[3] == 0.5f, "%s: value changed", c.what);
  }
  // the ring colour to the shield material with the mirror off: no second call
  g_calls.clear();
  d = MakeData(kMat, kRing, 1, 0.5f, 0.5f, 0.5f, 0.5f, 0);
  update(node, &d);
  CHECK(g_calls.size() == 1, "mirror off: %zu calls", g_calls.size());

  // 4. thinning: decode lines on the 1st, 2nd, 4th, 8th ...
  g_lines.clear();
  for (int i = 0; i < 6; ++i) update(node, &d);  // decodes so far: 3 (cases 1, 2 and the ring write); these are n = 4..9
  CHECK(Count("shieldgamma: decode") == 2, "decode line thinning: %d (want n=4 and n=8)", Count("shieldgamma: decode"));

  // 5. no per-call logging for calls that are not decoded
  g_lines.clear();
  Data other = MakeData(kMat + 5, kOtherProp, 1, 0.1f, 0.2f, 0.3f, 0.4f, 0);
  for (int i = 0; i < 2000; ++i) update(node, &other);
  CHECK(g_lines.empty(), "per-call lines for foreign materials: %zu", g_lines.size());

  // 6. unreadable data: the real function, same arguments, nothing written
  g_calls.clear();
  g_reads = false;
  const unsigned un = q15t::GetShieldGammaStats().unreadable;
  r = update(node, nullptr);
  CHECK(g_calls.size() == 1 && g_calls[0].data == nullptr && g_calls[0].node == node && r == 0x71, "null data");
  CHECK(q15t::GetShieldGammaStats().unreadable == un + 1, "unreadable not counted");
  g_reads = true;

  // 7. mirror only (the source's default: tube mirror ON, gamma off)
  Fresh(img);
  ir = q15t::InstallShieldGamma(img.base, false, true);
  CHECK(ir.status == q15t::SwapStatus::kOk && Count("gamma=0") == 1 && Count("tube_mirror=1") == 1, "mirror-only install");
  update = reinterpret_cast<UpdateFn>(img.Get(kSlot));
  d = MakeData(kMat, kRing, 1, 0.941f, 0.466f, 0.054f, 0.25f, 0);
  r = update(node, &d);
  CHECK(g_calls.size() == 2, "mirror: %zu calls (want the copy, then the original)", g_calls.size());
  if (g_calls.size() == 2) {
    CHECK(g_calls[0].data != &d && g_calls[0].node == node, "mirror call must be on a copy");
    CHECK(Mat(g_calls[0].bytes) == kTube, "mirror material %016llx", (unsigned long long)Mat(g_calls[0].bytes));
    Rgba(g_calls[0].bytes, seen);
    CHECK(seen[0] == Dec(0.941f) && seen[1] == Dec(0.466f) && seen[2] == Dec(0.054f) && seen[3] == 0.25f, "mirror rgb / alpha");
    // every other byte of the copy is the original's
    Data o = MakeData(kMat, kRing, 1, 0.941f, 0.466f, 0.054f, 0.25f, 0);
    uint8_t cmp[0x58];
    std::memcpy(cmp, o.b, sizeof cmp);
    std::memcpy(cmp + 0x20, &kTube, 8);
    float dv[4] = {Dec(0.941f), Dec(0.466f), Dec(0.054f), 0.25f};
    std::memcpy(cmp + 0x38, dv, 16);
    CHECK(std::memcmp(cmp, g_calls[0].bytes, sizeof cmp) == 0, "mirror copy differs from the original in more than material and rgb");
    CHECK(g_calls[1].data == &d && Mat(g_calls[1].bytes) == kMat, "second call must be the original, material unchanged");
    Rgba(g_calls[1].bytes, seen);
    CHECK(seen[0] == 0.941f && seen[1] == 0.466f && seen[2] == 0.054f, "gamma off: the original call must carry the raw rgb");
  }
  CHECK(r == 0x72, "the original call's result is the one returned: %llx", (unsigned long long)r);
  CHECK(Count("shieldgamma: tube mirror write") == 1, "no mirror line");
  float orig[4];
  Rgba(d.b, orig);
  CHECK(orig[0] == 0.941f && Mat(d.b) == kMat, "original data modified by the mirror");
  // a non-ring property, or a material other than the shield's: no mirror
  g_calls.clear();
  d = MakeData(kMat, kOtherProp, 1, 0.5f, 0.5f, 0.5f, 0.5f, 0);
  update(node, &d);
  CHECK(g_calls.size() == 1, "mirror on another property: %zu calls", g_calls.size());
  g_calls.clear();
  d = MakeData(kMat + 1, kRing, 1, 0.5f, 0.5f, 0.5f, 0.5f, 0);
  update(node, &d);
  CHECK(g_calls.size() == 1, "mirror for another material: %zu calls", g_calls.size());
  g_calls.clear();
  d = MakeData(kMat, kRing, 1, 0.5f, 0.5f, 0.5f, 0.5f, 1);
  update(node, &d);
  CHECK(g_calls.size() == 1, "mirror for a non-linear write: %zu calls", g_calls.size());

  // 8. both: the copy is decoded and the original call is decoded in place and put back
  Fresh(img);
  q15t::InstallShieldGamma(img.base, true, true);
  update = reinterpret_cast<UpdateFn>(img.Get(kSlot));
  d = MakeData(kMat, kRing, 1, 0.2f, 0.4f, 0.6f, 0.8f, 0);
  r = update(node, &d);
  CHECK(g_calls.size() == 2 && Mat(g_calls[0].bytes) == kTube && g_calls[1].data == &d, "gamma + mirror: calls");
  if (g_calls.size() == 2) {
    Rgba(g_calls[0].bytes, seen);
    CHECK(seen[0] == Dec(0.2f) && seen[3] == 0.8f, "copy rgb decoded once");
    Rgba(g_calls[1].bytes, seen);
    CHECK(seen[0] == Dec(0.2f) && seen[1] == Dec(0.4f) && seen[2] == Dec(0.6f) && seen[3] == 0.8f, "original call decoded once (not twice)");
  }
  Rgba(d.b, orig);
  CHECK(orig[0] == 0.2f && orig[1] == 0.4f && orig[2] == 0.6f, "node's value not put back");
  CHECK(r == 0x72, "result");

  // 9. both off: nothing installed, one INFO line
  Fresh(img);
  ir = q15t::InstallShieldGamma(img.base, false, false);
  CHECK(img.Get(kSlot) == img.base + kFn && std::strcmp(ir.reason, "off") == 0 && Count("shieldgamma: off", 1) == 1, "both off installed something");

  // 10. a slot that does not hold the pinned function: refused, untouched, ERROR
  Fresh(img);
  img.SetSlot(kSlot, 0x4242420);
  ir = q15t::InstallShieldGamma(img.base, true, true);
  CHECK(ir.status == q15t::SwapStatus::kUnexpectedOriginal && img.Get(kSlot) == 0x4242420, "foreign slot: %s", q15t::StatusName(ir.status));
  CHECK(Count("shieldgamma: OFF", 3) == 1, "no ERROR line");

  if (g_fail == 0) std::printf("shieldgamma_test: all checks passed\n");
  return g_fail == 0 ? 0 : 1;
}
