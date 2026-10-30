// Host test of the pfx gate (src/quest/plugins/q15t/src/pfx.cpp, issue #1631). DecidePfxGate is the decision: on by default,
// off only with the kill file. InitPfxGate stores that decision for PfxGateOn(), which the pfx* modules read; before it runs
// the gate reads off, so no pfx* module acts before the plugin has decided.
#include <cstdio>

#include "module_switch.h"
#include "pfx.h"

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

int main() {
  // 1. No kill file: on, for the default reason.
  {
    const q15t::PfxGateDecision d = q15t::DecidePfxGate(false);
    CHECK(d.on, "no pfx.off must give on");
    CHECK(d.reason == q15t::PfxGateReason::kDefault, "no pfx.off must give reason default, got %d", static_cast<int>(d.reason));
  }
  // 2. The kill file present: off, for the off_file reason.
  {
    const q15t::PfxGateDecision d = q15t::DecidePfxGate(true);
    CHECK(!d.on, "pfx.off must give off");
    CHECK(d.reason == q15t::PfxGateReason::kOffFile, "pfx.off must give reason off_file, got %d", static_cast<int>(d.reason));
  }
  // 3. Before InitPfxGate the gate reads off, whatever the switch says.
  q15t::ResetPfxGateForTest();
  CHECK(!q15t::PfxGateOn(), "PfxGateOn must read off before InitPfxGate");
  // 4. InitPfxGate reads the switch directory. A directory with no pfx.off: on, stored, and PfxGateOn reads it.
  q15t::SetModuleSwitchDirForTest("/nonexistent-q15t-pfx-test-dir");
  {
    const q15t::PfxGateDecision d = q15t::InitPfxGate();
    CHECK(d.on, "InitPfxGate with no pfx.off must decide on");
    CHECK(q15t::PfxGateOn(), "PfxGateOn must read on after InitPfxGate decided on");
  }
  // 5. The reset forgets the stored decision again.
  q15t::ResetPfxGateForTest();
  CHECK(!q15t::PfxGateOn(), "PfxGateOn must read off after ResetPfxGateForTest");
  q15t::SetModuleSwitchDirForTest("");
  if (g_fail != 0) {
    std::printf("pfx_test: %d failure(s)\n", g_fail);
    return 1;
  }
  std::printf("pfx_test: all checks passed\n");
  return 0;
}
