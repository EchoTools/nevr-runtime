// PB1, P-RULES: the main level, the net game state and the three zone point values the game caches from
// mp_arena_rules, logged when any of them changes. It reads memory only and installs no hook.

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "abi/echovr_functions.h"
#include "core/logging.h"
#include "runtime/hook/addresses.h"
#include "runtime/lifecycle/readable_memory.h"
#include "runtime/maps/probe/probe_game.h"

namespace nevr_maps_probe {

namespace {

namespace addr = nevr_patch_addresses;

// The balance-settings update (NETGAME_BALANCE_SETTINGS_UPDATE, 0x1401B2A90) stores zone_0/1/2_point_value with
// `mov [rip+rel32], rsi / rdi / rcx` (48 89 35 / 3D / 0D) into qword_142023CE8/CF0/CF8. A global is read only when
// the store at its site still encodes its address. Read from the IDA export of echovr.exe 631547.
struct ZoneStore {
  std::uintptr_t site_rva;
  std::uint8_t opcode[3];
  std::uintptr_t global_rva;
};
constexpr ZoneStore kZoneStores[3] = {{0x1B2DDA, {0x48, 0x89, 0x35}, 0x2023CE8},
                                      {0x1B2DE1, {0x48, 0x89, 0x3D}, 0x2023CF0},
                                      {0x1B2DE8, {0x48, 0x89, 0x0D}, 0x2023CF8}};
constexpr std::size_t kStoreSize = 7;
constexpr std::uintptr_t kBalanceUpdateSize = 0x38A;
static_assert(kZoneStores[0].site_rva > addr::NETGAME_BALANCE_SETTINGS_UPDATE &&
                  kZoneStores[2].site_rva + kStoreSize <= addr::NETGAME_BALANCE_SETTINGS_UPDATE + kBalanceUpdateSize,
              "the zone stores are inside the balance-settings update");

bool g_pinned = false;
const unsigned char* g_globals[3] = {};

// Game thread only.
bool g_haveLevel = false;
std::uint64_t g_lastMain = 0;
std::int32_t g_lastState = 0;
bool g_haveValues = false;
std::int64_t g_lastValues[3] = {};

bool StoreEncodes(const unsigned char* base, const ZoneStore& store) {
  const unsigned char* site = base + store.site_rva;
  if (!nevr_crash_recovery::IsReadableMemory(site, kStoreSize)) return false;
  if (std::memcmp(site, store.opcode, sizeof(store.opcode)) != 0) return false;
  std::int32_t rel = 0;
  std::memcpy(&rel, site + sizeof(store.opcode), sizeof(rel));
  const std::int64_t target = static_cast<std::int64_t>(store.site_rva + kStoreSize) + rel;
  return target == static_cast<std::int64_t>(store.global_rva);
}

}  // namespace

void RulesStart() {
  const auto* base = reinterpret_cast<const unsigned char*>(EchoVR::g_GameBaseAddress);
  g_pinned = base != nullptr;
  for (std::size_t i = 0; g_pinned && i < 3; ++i) {
    g_pinned = StoreEncodes(base, kZoneStores[i]) &&
               nevr_crash_recovery::IsReadableMemory(base + kZoneStores[i].global_rva, sizeof(std::int64_t));
    g_globals[i] = base + kZoneStores[i].global_rva;
  }
  if (g_pinned) {
    Log(EchoVR::LogLevel::Info, "[NEVR.MAPS] probe_rules on");
  } else {
    Log(EchoVR::LogLevel::Warning,
        "[NEVR.MAPS] probe_rules off reason=not_pinned (the zone value stores are not the 631547 code; probe_level "
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
    Log(EchoVR::LogLevel::Info, "[NEVR.MAPS] probe_level main=0x%016llx state=%d frame=%llu",
        static_cast<unsigned long long>(main), state, static_cast<unsigned long long>(frame));
  }
  if (!g_pinned) return;
  std::int64_t values[3];
  for (std::size_t i = 0; i < 3; ++i) std::memcpy(&values[i], g_globals[i], sizeof(values[i]));
  if (g_haveValues && std::memcmp(values, g_lastValues, sizeof(values)) == 0) return;
  g_haveValues = true;
  std::memcpy(g_lastValues, values, sizeof(values));
  Log(EchoVR::LogLevel::Info, "[NEVR.MAPS] probe_rules z0=%lld z1=%lld z2=%lld main=0x%016llx frame=%llu",
      static_cast<long long>(values[0]), static_cast<long long>(values[1]), static_cast<long long>(values[2]),
      static_cast<unsigned long long>(main), static_cast<unsigned long long>(frame));
}

}  // namespace nevr_maps_probe
