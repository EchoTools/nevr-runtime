// Source B for P-SUBLEVEL, and P-SKEW's `loaded` line: log-only detours on CGameLevelResource::Enable/Disable. They call the
// game's function, note the outcome and change nothing. Both functions run once per level enable or disable (a level load, not a
// frame), so the detour's mutex and one vector push are off any hot path (docs/adr/0038-hot-path-rule.md). The config alias
// (runtime/content/config_alias_hook.cpp) hooks the same two functions, and only while a map is staged, so a probe build with a
// staged map refuses here ("enable_not_stock") instead of stacking a second hook on them (one owner per hooked function).

#include <winsock2.h>  // before windows.h (tools/tests/test_header_include_order.py)
#include <windows.h>

#include <cstring>
#include <exception>
#include <mutex>
#include <vector>

#include "abi/echovr_functions.h"
#include "nevr/pc/nevr_common.h"
#include "runtime/hook/addresses.h"
#include "runtime/hook/patching.h"
#include "runtime/lifecycle/readable_memory.h"
#include "runtime/probe/probe_game.h"

namespace nevr_map_probe {

namespace {

namespace addr = nevr_patch_addresses;

using LevelResourceFn = std::int64_t (*)(void*);

// The two functions' first 16 bytes, the same pins config_alias_hook.cpp holds (echovr.exe sha256 b6d08277..., ReVault).
constexpr std::uint64_t kEnableVa = nevr::ECHOVR_DEFAULT_IMAGE_BASE + addr::CGAMELEVELRESOURCE_ENABLE;
constexpr std::uint8_t kEnablePrologue[] = {0x40, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41,
                                            0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8d, 0xac};
constexpr std::uint64_t kDisableVa = nevr::ECHOVR_DEFAULT_IMAGE_BASE + addr::CGAMELEVELRESOURCE_DISABLE;
constexpr std::uint8_t kDisablePrologue[] = {0x48, 0x89, 0x5c, 0x24, 0x18, 0x55, 0x56, 0x57,
                                             0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57};

LevelResourceFn g_enableOriginal = nullptr;
LevelResourceFn g_disableOriginal = nullptr;

// The detours may run off the game thread. The vector grows until the next frame drains it; a push that cannot allocate is
// counted in g_dropped.
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
    g_pending.push_back(event);
  } catch (const std::exception&) {
    ++g_dropped;  // a bad_alloc or a failed lock: the summary's `dropped` says so
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

}  // namespace nevr_map_probe
