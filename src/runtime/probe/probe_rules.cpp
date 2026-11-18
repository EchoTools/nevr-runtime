// P-RULES: the main level, the net game state and the three zone point values the game caches from mp_arena_rules, logged when
// any of them changes. It reads memory only and installs no hook, so the per-frame read is three qwords and a compare.

#include <winsock2.h>  // before windows.h (tools/tests/test_header_include_order.py)
#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "abi/echovr_functions.h"
#include "core/logging.h"
#include "runtime/hook/addresses.h"
#include "runtime/lifecycle/readable_memory.h"
#include "runtime/probe/probe_game.h"

namespace nevr_map_probe {

namespace {

namespace addr = nevr_patch_addresses;

// The balance-settings update (addresses.h NETGAME_BALANCE_SETTINGS_UPDATE, 0x1401B2A90) stores zone_0/1/2_point_value with
// `mov [rip+rel32], rsi / rdi / rcx` (48 89 35 / 3D / 0D) into 0x142023CE8 / CF0 / CF8 (addresses.h documents the three
// globals). A global is read only when the store at its site still encodes its address.
constexpr ZoneStore kZoneStores[kZoneCount] = {{0x1B2DDA, {0x48, 0x89, 0x35}, 0x2023CE8},
                                               {0x1B2DE1, {0x48, 0x89, 0x3D}, 0x2023CF0},
                                               {0x1B2DE8, {0x48, 0x89, 0x0D}, 0x2023CF8}};
// The update function's length (addresses.h: 906 bytes); the three stores lie inside it.
constexpr std::uintptr_t kBalanceUpdateSize = 906;
static_assert(kZoneStores[0].site_rva > addr::NETGAME_BALANCE_SETTINGS_UPDATE &&
                  kZoneStores[kZoneCount - 1].site_rva + kStoreSize <= addr::NETGAME_BALANCE_SETTINGS_UPDATE + kBalanceUpdateSize,
              "the zone stores are inside the balance-settings update");

bool g_pinned = false;
const unsigned char* g_globals[kZoneCount] = {};

// Game thread only.
bool g_haveLevel = false;
std::uint64_t g_lastMain = 0;
std::int32_t g_lastState = 0;
bool g_haveValues = false;
std::int64_t g_lastValues[kZoneCount] = {};

bool StoreInPlace(const unsigned char* base, const ZoneStore& store) {
  const unsigned char* site = base + store.site_rva;
  return nevr_crash_recovery::IsReadableMemory(site, kStoreSize) && StoreEncodes(site, store);
}

}  // namespace

void RulesStart() {
  const auto* base = reinterpret_cast<const unsigned char*>(EchoVR::g_GameBaseAddress);
  g_pinned = base != nullptr;
  for (std::size_t i = 0; g_pinned && i < kZoneCount; ++i) {
    g_globals[i] = base + kZoneStores[i].global_rva;
    g_pinned = StoreInPlace(base, kZoneStores[i]) && nevr_crash_recovery::IsReadableMemory(g_globals[i], sizeof(std::int64_t));
  }
  if (g_pinned) {
    Log(EchoVR::LogLevel::Info, "[NEVR.PROBE] probe_rules on zones=%u (the zone values and the main level are logged on change)",
        static_cast<unsigned>(kZoneCount));
  } else {
    Log(EchoVR::LogLevel::Warning,
        "[NEVR.PROBE] probe_rules off reason=not_pinned (the zone value stores are not the supported build's code; probe_level "
        "lines still come)");
  }
}

void RulesTick(std::uint64_t frame) {
  const std::uint64_t main = MainLevel();
  const std::int32_t state = NetState();
  if (!g_haveLevel || main != g_lastMain || state != g_lastState) {
    g_haveLevel = true;
    g_lastMain = main;
    g_lastState = state;
    Log(EchoVR::LogLevel::Info, "[NEVR.PROBE] probe_level main=0x%016llx state=%d frame=%llu",
        static_cast<unsigned long long>(main), state, static_cast<unsigned long long>(frame));
  }
  if (!g_pinned) return;
  std::int64_t values[kZoneCount];
  for (std::size_t i = 0; i < kZoneCount; ++i) std::memcpy(&values[i], g_globals[i], sizeof(values[i]));
  if (g_haveValues && std::memcmp(values, g_lastValues, sizeof(values)) == 0) return;
  g_haveValues = true;
  std::memcpy(g_lastValues, values, sizeof(values));
  Log(EchoVR::LogLevel::Info, "[NEVR.PROBE] probe_rules z0=%lld z1=%lld z2=%lld main=0x%016llx frame=%llu",
      static_cast<long long>(values[0]), static_cast<long long>(values[1]),
      static_cast<long long>(values[kZoneCount - 1]),
      static_cast<unsigned long long>(main), static_cast<unsigned long long>(frame));
}

}  // namespace nevr_map_probe
