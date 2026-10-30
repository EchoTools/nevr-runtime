// Host test of pfxshotpos (src/quest/plugins/q15t/src/pfxshotpos.{h,cpp}, pfxshotpos_core.h). Two layers:
//   1. the core: the id table's classes, the bins (edges, NaN), the distance, the line budget, the jump test and the summary cadence;
//   2. the install step: the kill file keeps the probe out with an INFO line, and with no kill file the probe logs that the
//      particle renderer's hooks are missing (WARNING, reason pfx_renderer_absent) and installs nothing.
// The switch directory is argv[1] (the mod script passes a directory under its build output).
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "module_switch.h"
#include "nevr/plugin_interface.h"
#include "pfxshotpos.h"
#include "pfxshotpos_core.h"
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

namespace {

struct Line {
  uint32_t level;
  std::string text;
};
std::vector<Line> g_lines;

void FakeLog(const char* id, uint32_t level, const char* msg, const NevrLogField* f, uint32_t n) {
  std::string t = std::string("plugin=") + id + " " + msg;
  for (uint32_t i = 0; i < n; ++i) t += std::string(" ") + f[i].key + "=" + f[i].value;
  g_lines.push_back({level, t});
}

int CountLines(const char* needle, int level) {
  int c = 0;
  for (const Line& l : g_lines)
    if (l.text.find(needle) != std::string::npos && static_cast<int>(l.level) == level) ++c;
  return c;
}

namespace sp = q15t::shotpos;

// ---- 1. the core -----------------------------------------------------------------------------------------------------------

void TestIdTable() {
  CHECK(sp::kShotIdCount == 20, "the probe follows 20 ids, got %d", sp::kShotIdCount);
  int per[sp::kClasses] = {0, 0, 0};
  for (const sp::ShotId& s : sp::kShotIds) {
    CHECK(s.cls >= 0 && s.cls < sp::kClasses, "a row has class %d", s.cls);
    per[s.cls]++;
  }
  CHECK(per[sp::kMuzzle] == 7 && per[sp::kTrail] == 5 && per[sp::kImpact] == 8, "rows per class muzzle/trail/impact %d/%d/%d",
        per[sp::kMuzzle], per[sp::kTrail], per[sp::kImpact]);
  CHECK(sp::ClassOf(0xa8cd797f5e8693feull) == sp::kMuzzle, "first muzzle id is a muzzle");
  CHECK(sp::ClassOf(0xf677d365ee22d7acull) == sp::kTrail, "first trail id is a trail");
  CHECK(sp::ClassOf(0x99e95eca20fdb52full) == sp::kImpact, "last impact id is an impact");
  CHECK(sp::ClassOf(0) == -1, "symbol 0 is not followed");
  CHECK(sp::ClassOf(0x1234ull) == -1, "an unlisted symbol is not followed");
}

void TestBins() {
  const float edges[] = {0.5f, 1.f, 2.f};
  CHECK(sp::Bin(0.f, edges, 3) == 0, "0 is below the first edge");
  CHECK(sp::Bin(0.5f, edges, 3) == 1, "a value on an edge goes up one bin");
  CHECK(sp::Bin(1.9f, edges, 3) == 2, "1.9 is in the bin below the last edge");
  CHECK(sp::Bin(2.f, edges, 3) == 3, "a value on the last edge is in the last bin");
  CHECK(sp::Bin(1e9f, edges, 3) == 3, "a value past the last edge is in the last bin");
  CHECK(sp::Bin(NAN, edges, 3) == 3, "NaN is counted in the last bin, not dropped");
  CHECK(sp::kEyeEdgeCount == 8 && sp::kStepEdgeCount == 8 && sp::kMuzzleHandEdgeCount == 8, "histogram edge counts");
}

void TestDist() {
  const float a[3] = {0.f, 0.f, 0.f};
  const float b[3] = {3.f, 4.f, 0.f};
  CHECK(std::fabs(sp::Dist(a, b) - 5.f) < 1e-6f, "dist to (3,4,0) is 5, got %f", sp::Dist(a, b));
  CHECK(std::fabs(sp::Dist(b, b)) < 1e-6f, "dist to itself is 0");
}

void TestTake() {
  CHECK(sp::Take(1) && sp::Take(48), "the first 48 lines are written");
  CHECK(!sp::Take(49) && !sp::Take(50) && !sp::Take(63), "lines 49, 50 and 63 are counted, not written");
  CHECK(sp::Take(64) && sp::Take(128) && sp::Take(1024), "the 2^k-th line is written");
}

void TestJump() {
  const float a[3] = {0.f, 0.f, 0.f};
  const float near[3] = {1.9f, 0.f, 0.f};
  const float far[3] = {2.5f, 0.f, 0.f};
  const float nan[3] = {NAN, 0.f, 0.f};
  CHECK(!sp::IsJump(a, near), "a 1.9 m step is not a jump");
  CHECK(sp::IsJump(a, far), "a 2.5 m step is a jump");
  CHECK(!sp::IsJump(a, nan), "a NaN position is not a jump");
}

void TestSumCadence() {
  CHECK(!sp::SumDue(9999, 0), "9.999 s is not yet a summary");
  CHECK(sp::SumDue(10000, 0), "10 s is a summary");
  CHECK(sp::SumDue(20000, 10000), "the next summary is due 10 s after the last");
}

// ---- 2. the install step ---------------------------------------------------------------------------------------------------

void TestInstall(const std::string& dir) {
  NvrGameContext ctx{};
  ctx.ctx_size = sizeof ctx;
  ctx.plugin_id = "q15t";
  ctx.log = &FakeLog;
  q15t::SetLogContext(&ctx);
  q15t::SetModuleSwitchDirForTest(dir.c_str());

  const std::string kill = dir + "/pfxshotpos.off";
  std::remove(kill.c_str());

  g_lines.clear();
  CHECK(!q15t::InstallPfxShotPos(), "without a kill file the probe reports not installed (no renderer hooks in this build)");
  CHECK(CountLines("pfxshotpos: not installed reason=pfx_renderer_absent", NEVR_LOG_WARNING) == 1,
        "one WARNING names the missing renderer hooks and the reason");
  CHECK(CountLines("off", NEVR_LOG_INFO) == 0, "no off line without the kill file");

  if (std::FILE* f = std::fopen(kill.c_str(), "w")) std::fclose(f);
  g_lines.clear();
  CHECK(!q15t::InstallPfxShotPos(), "with the kill file the probe stays out");
  CHECK(CountLines("pfxshotpos: off reason=kill_file", NEVR_LOG_INFO) == 1, "one INFO line says the kill file kept it off");
  CHECK(CountLines("not installed", NEVR_LOG_WARNING) == 0, "a kill file is a choice, not a warning");

  std::remove(kill.c_str());
  q15t::SetModuleSwitchDirForTest("");
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::printf("usage: pfxshotpos_test <switch directory>\n");
    return 2;
  }
  TestIdTable();
  TestBins();
  TestDist();
  TestTake();
  TestJump();
  TestSumCadence();
  TestInstall(argv[1]);
  if (g_fail != 0) {
    std::printf("pfxshotpos_test: %d FAILED\n", g_fail);
    return 1;
  }
  std::printf("pfxshotpos_test: OK\n");
  return 0;
}
