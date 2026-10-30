#include "pfx.h"

#include <atomic>

#include "module_switch.h"
#include "plugin_log.h"

namespace q15t {
namespace {

// Stored once by InitPfxGate. Atomics with constant initialisation: no constructor runs at load time.
std::atomic<bool> g_decided{false};
std::atomic<bool> g_on{false};

}  // namespace

PfxGateDecision DecidePfxGate(bool off_file_present) {
  if (off_file_present) return {false, PfxGateReason::kOffFile};
  return {true, PfxGateReason::kDefault};
}

PfxGateDecision InitPfxGate() {
  const PfxGateDecision d = DecidePfxGate(ModuleSwitchPresent("pfx.off"));
  g_on.store(d.on, std::memory_order_release);
  g_decided.store(true, std::memory_order_release);
  const char* reason = d.reason == PfxGateReason::kOffFile ? "off_file" : "default";
  const NevrLogField f[] = {{"module", "pfx"},
                            {"status", d.on ? "on" : "off"},
                            {"reason", reason},
                            {"switch", "pfx.off"}};
  Log(NEVR_LOG_INFO, "pfx gate", f, 4);
  return d;
}

bool PfxGateOn() { return g_decided.load(std::memory_order_acquire) && g_on.load(std::memory_order_acquire); }

void ResetPfxGateForTest() {
  g_decided.store(false, std::memory_order_release);
  g_on.store(false, std::memory_order_release);
}

}  // namespace q15t
