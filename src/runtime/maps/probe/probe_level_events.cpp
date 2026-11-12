// Source B for P-SUBLEVEL, and P-SKEW's `loaded` line: log-only detours on CGameLevelResource::Enable/Disable. They
// call the game's function, note the outcome and change nothing. EchoMapLoader does not hook these two (it detours
// 0x1404FE050 and 0x140515E40); PC8b's config alias does, and only while a map is staged, so a probe build with a
// staged map refuses here instead of stacking a second hook.

#include <winsock2.h>
#include <windows.h>

#include <cstring>
#include <mutex>
#include <vector>

#include "abi/echovr_functions.h"
#include "nevr/pc/nevr_common.h"
#include "runtime/hook/addresses.h"
#include "runtime/hook/patching.h"
#include "runtime/lifecycle/readable_memory.h"
#include "runtime/maps/probe/probe_game.h"

namespace nevr_maps_probe {

namespace {

namespace addr = nevr_patch_addresses;

using LevelResourceFn = std::int64_t (*)(void*);

// Prologues read from the IDA export of echovr.exe 631547 (D:\ida-mcp-db\projects\echovr.db), the same 16 bytes
// config_alias_hook.cpp pins.
constexpr std::uint64_t kEnableVa = 0x1404ed4b0;
static_assert(kEnableVa - ECHOVR_DEFAULT_IMAGE_BASE == addr::CGAMELEVELRESOURCE_ENABLE, "one pin");
constexpr std::uint8_t kEnablePrologue[] = {0x40, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41,
                                            0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8d, 0xac};
constexpr std::uint64_t kDisableVa = 0x1404ec900;
static_assert(kDisableVa - ECHOVR_DEFAULT_IMAGE_BASE == addr::CGAMELEVELRESOURCE_DISABLE, "one pin");
constexpr std::uint8_t kDisablePrologue[] = {0x48, 0x89, 0x5c, 0x24, 0x18, 0x55, 0x56, 0x57,
                                             0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57};

constexpr std::size_t kMaxPendingEvents = 4096;

LevelResourceFn g_enableOriginal = nullptr;
LevelResourceFn g_disableOriginal = nullptr;

// The detours may run off the game thread.
std::mutex g_mutex;
std::vector<LevelEvent> g_pending;
std::uint32_t g_dropped = 0;

std::uint64_t LevelOf(const void* resource) {
  const auto* at = static_cast<const unsigned char*>(resource) + addr::LEVEL_RESOURCE_LEVEL_OFFSET;
  std::uint64_t level = 0;
  if (nevr_crash_recovery::IsReadableMemory(at, sizeof(level))) std::memcpy(&level, at, sizeof(level));
  return level;
}

// False when the flags cannot be read.
bool ReadsDisabled(const void* resource, bool* disabled) {
  const auto* at = static_cast<const unsigned char*>(resource) + addr::LEVEL_RESOURCE_FLAGS_OFFSET;
  std::uint32_t flags = 0;
  if (!nevr_crash_recovery::IsReadableMemory(at, sizeof(flags))) return false;
  std::memcpy(&flags, at, sizeof(flags));
  *disabled = (flags & addr::LEVEL_RESOURCE_DISABLED_FLAG) != 0;
  return true;
}

void Note(bool enable, std::uint64_t level, bool noop, std::int64_t result) noexcept {
  LevelEvent event;
  event.enable = enable;
  // The callers test the low 32 bits (config_alias_hook.cpp EnableDetour).
  event.ok = static_cast<std::uint32_t>(result) == 0;
  event.noop = noop;
  event.level = level;
  event.frame = ProbeFrame();
  try {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_pending.size() >= kMaxPendingEvents) {
      ++g_dropped;
      return;
    }
    g_pending.push_back(event);
  } catch (...) {
    ++g_dropped;
  }
}

std::int64_t EnableDetour(void* resource) {
  const std::uint64_t level = resource != nullptr ? LevelOf(resource) : 0;
  bool disabled = true;
  const bool noop = resource != nullptr && ReadsDisabled(resource, &disabled) && !disabled;
  const std::int64_t result = g_enableOriginal(resource);
  if (resource != nullptr) Note(true, level, noop, result);
  return result;
}

std::int64_t DisableDetour(void* resource) {
  const std::uint64_t level = resource != nullptr ? LevelOf(resource) : 0;
  bool disabled = false;
  const bool noop = resource != nullptr && ReadsDisabled(resource, &disabled) && disabled;
  const std::int64_t result = g_disableOriginal(resource);
  if (resource != nullptr) Note(false, level, noop, result);
  return result;
}

void* StockTarget(std::uint64_t va, const std::uint8_t* prologue, std::size_t size) {
  void* target = nevr::ResolveVA_Checked(reinterpret_cast<uintptr_t>(EchoVR::g_GameBaseAddress), va);
  return target != nullptr && nevr::ValidatePrologue(target, prologue, size) ? target : nullptr;
}

const char* Install() {
  void* enable = StockTarget(kEnableVa, kEnablePrologue, sizeof(kEnablePrologue));
  if (enable == nullptr) return "enable_not_stock";
  void* disable = StockTarget(kDisableVa, kDisablePrologue, sizeof(kDisablePrologue));
  if (disable == nullptr) return "disable_not_stock";
  g_enableOriginal = reinterpret_cast<LevelResourceFn>(enable);
  if (!PatchDetour(&g_enableOriginal, reinterpret_cast<PVOID>(&EnableDetour), "level resource enable (map probe)")) {
    return "enable_patch_failed";
  }
  g_disableOriginal = reinterpret_cast<LevelResourceFn>(disable);
  if (!PatchDetour(&g_disableOriginal, reinterpret_cast<PVOID>(&DisableDetour), "level resource disable (map probe)")) {
    const MH_STATUS retired = PatchRetire(enable);
    return retired == MH_OK || retired == MH_ERROR_NOT_CREATED ? "disable_patch_failed" : "disable_patch_failed_enable_kept";
  }
  return nullptr;
}

}  // namespace

const char* InstallLevelEvents() {
  static const char* const refusal = Install();
  return refusal;
}

void DrainLevelEvents(std::vector<LevelEvent>* out) {
  out->clear();
  std::lock_guard<std::mutex> lock(g_mutex);
  out->swap(g_pending);
}

std::uint32_t DroppedLevelEvents() {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_dropped;
}

}  // namespace nevr_maps_probe
