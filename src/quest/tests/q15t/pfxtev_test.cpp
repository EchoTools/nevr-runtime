// Host test of pfxtev (src/quest/plugins/q15t/src/pfxtev.cpp, issue #1642): the kill file, the six event ids, the non-event pass-through,
// and one INFO line per event-id decision. Log lines are captured through a fake host log.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "module_switch.h"
#include "nevr/plugin_interface.h"
#include "pfxtev.h"
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

int main() {
  NvrGameContext ctx{};
  ctx.ctx_size = sizeof ctx;
  ctx.plugin_id = "q15t";
  ctx.log = &FakeLog;
  q15t::SetLogContext(&ctx);
  char tmpl[] = "/tmp/q15t-pfxtev-XXXXXX";
  if (mkdtemp(tmpl) == nullptr) return 2;
  const std::string dir = tmpl;
  q15t::SetModuleSwitchDirForTest(dir.c_str());

  CHECK(q15t::kPfxTevIdCount == 6, "the event tier has six ids, got %zu", q15t::kPfxTevIdCount);
  const std::uint64_t kNonEvent = 0x66497bb6ull;   // not an event id: never gated by this switch

  // default: on before InitPfxTev (the decision is taken, then the counters and lines are cleared)
  q15t::ResetPfxTevForTest();
  CHECK(q15t::PfxTevAllows(q15t::kPfxTevIds[0]), "event id refused before InitPfxTev (default on)");
  q15t::ResetPfxTevForTest();
  g_lines.clear();
  q15t::InitPfxTev();
  CHECK(CountLines("pfxtev: event-effect ids allowed", NEVR_LOG_INFO) == 1, "one INFO line for the on state at init");
  for (std::size_t i = 0; i < q15t::kPfxTevIdCount; ++i)
    CHECK(q15t::PfxTevAllows(q15t::kPfxTevIds[i]), "event id %zu refused with the switch on", i);
  CHECK(q15t::PfxTevAllows(kNonEvent), "a non-event id is not gated");
  CHECK(CountLines("pfxtev: event-effect create decision", NEVR_LOG_INFO) == 6, "one INFO line per event-id decision (6), got %d",
        CountLines("pfxtev: event-effect create decision", NEVR_LOG_INFO));
  CHECK(CountLines("result=allowed") == 6, "the six decisions read result=allowed");
  CHECK(q15t::GetPfxTevStats().allowed == 6 && q15t::GetPfxTevStats().refused == 0, "stats after the on pass");

  // kill file present: every event id refused, the non-event id still passes
  q15t::ResetPfxTevForTest();
  g_lines.clear();
  const std::string kill = dir + "/pfxtev.off";
  if (std::FILE* f = std::fopen(kill.c_str(), "w")) std::fclose(f);
  q15t::InitPfxTev();
  CHECK(CountLines("pfxtev: event-effect ids refused (kill file present)", NEVR_LOG_INFO) == 1, "one INFO line for the off state");
  for (std::size_t i = 0; i < q15t::kPfxTevIdCount; ++i)
    CHECK(!q15t::PfxTevAllows(q15t::kPfxTevIds[i]), "event id %zu allowed with pfxtev.off", i);
  CHECK(q15t::PfxTevAllows(kNonEvent), "a non-event id is not gated with pfxtev.off");
  CHECK(CountLines("result=refused") == 6, "the six decisions read result=refused");
  CHECK(q15t::GetPfxTevStats().refused == 6 && q15t::GetPfxTevStats().allowed == 0, "stats after the off pass");
  std::remove(kill.c_str());

  std::printf(g_fail == 0 ? "pfxtev_test: OK\n" : "pfxtev_test: %d FAILED\n", g_fail);
  return g_fail == 0 ? 0 : 1;
}
