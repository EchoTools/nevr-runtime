#include "pfxtev.h"

#include <atomic>
#include <cstdio>

#include "module_switch.h"
#include "plugin_log.h"

namespace q15t {
namespace {

constexpr std::uint32_t kLogCap = 64;  // event-id decision lines per process; the first line over it is one WARNING, then silence

// Plain atomics with constant initialisation: no constructor runs at load (the repack refuses one).
std::atomic<bool> g_on{true};
std::atomic<std::uint32_t> g_lines{0}, g_allowed{0}, g_refused{0};

bool IsEventId(std::uint64_t id) {
  for (std::size_t i = 0; i < kPfxTevIdCount; ++i)
    if (kPfxTevIds[i] == id) return true;
  return false;
}

}  // namespace

void InitPfxTev() {
  const bool off = ModuleSwitchPresent("pfxtev.off");
  g_on.store(!off, std::memory_order_relaxed);
  const NevrLogField f[] = {{"switch", off ? "off" : "on"}, {"kill_file", "pfxtev.off"}};
  Log(NEVR_LOG_INFO, off ? "pfxtev: event-effect ids refused (kill file present)" : "pfxtev: event-effect ids allowed", f, 2);
}

bool PfxTevAllows(std::uint64_t id) {
  if (!IsEventId(id)) return true;
  const bool on = g_on.load(std::memory_order_relaxed);
  (on ? g_allowed : g_refused).fetch_add(1, std::memory_order_relaxed);
  const std::uint32_t n = g_lines.fetch_add(1, std::memory_order_relaxed) + 1;
  if (n == kLogCap + 1) Log(NEVR_LOG_WARNING, "pfxtev log cap reached: further event-id decisions are not written");
  if (n > kLogCap) return on;
  char hex[24];
  std::snprintf(hex, sizeof hex, "%016llx", static_cast<unsigned long long>(id));
  const NevrLogField f[] = {{"id", hex}, {"switch", on ? "on" : "off"}, {"result", on ? "allowed" : "refused"}};
  Log(NEVR_LOG_INFO, "pfxtev: event-effect create decision", f, 3);
  return on;
}

PfxTevStats GetPfxTevStats() {
  return PfxTevStats{g_allowed.load(std::memory_order_relaxed), g_refused.load(std::memory_order_relaxed)};
}

void ResetPfxTevForTest() {
  g_on.store(true, std::memory_order_relaxed);
  g_lines.store(0, std::memory_order_relaxed);
  g_allowed.store(0, std::memory_order_relaxed);
  g_refused.store(0, std::memory_order_relaxed);
}

}  // namespace q15t
