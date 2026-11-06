#include "quest/combat/heaps.h"

#include <atomic>
#include <cstdio>
#include <cstring>

#if defined(__ANDROID__)
#include <sys/system_properties.h>
#endif

#include "quest/sentinel/hook_log.h"
#include "quest/sentinel/mapping_perms.h"

namespace nevr_quest::combat::heaps {

namespace {

namespace pinned = sentinel::pinned;

// The values the game reads. A reader holds the slot's pointer for the process's lifetime, so these stay valid for it: plain static
// storage of the sentinel, which is never unloaded. A GotDataSlot refuses a value in an executable segment; .bss is not one.
alignas(8) std::uint64_t g_value[kRowCount];
sentinel::GotDataSlot g_slots[kRowCount];

std::atomic<bool> g_ran{false};
std::atomic<const char*> g_deviceOverride{nullptr};
std::atomic<sentinel::ImageLookup> g_lookup{nullptr};

// A read of the game's memory (mapping_perms.h): the range must lie in readable mappings of /proc/self/maps, then it is copied.
template <typename T>
bool ReadGame(std::uintptr_t address, T* out) noexcept {
  if (!sentinel::MappingsAllow(address, sizeof(T), /*write=*/false)) return false;
  std::memcpy(out, reinterpret_cast<const void*>(address), sizeof(T));
  return true;
}

void ReadDevice(char* out, std::size_t size) noexcept {
  out[0] = '\0';
  if (const char* const device = g_deviceOverride.load(std::memory_order_acquire)) {
    std::snprintf(out, size, "%s", device);
    return;
  }
#if defined(__ANDROID__)
  char value[PROP_VALUE_MAX] = "";
  __system_property_get("ro.product.device", value);
  std::snprintf(out, size, "%s", value);
#endif
}

// What the game reads through row `row`'s slot now: slot -> pointer -> u64, in MiB; 0 when either read fails.
std::uint64_t ObservedMiB(std::uintptr_t base, std::size_t row) noexcept {
  std::uintptr_t pointer = 0;
  std::uint64_t bytes = 0;
  if (!ReadGame(base + kRows[row].slot, &pointer) || !ReadGame(pointer, &bytes)) return 0;
  return bytes / kMiB;
}

void LogRow(sentinel::LogLevel level, const RowPin& row, const char* status, const char* why, std::uint64_t valueBytes,
            std::uint64_t observedMiB, const char* device) noexcept {
  char slot[19];
  sentinel::LogFields(level, "combat_heaps_row",
                      {{"row", row.id}, {"status", status}, {"why", why}, {"slot_vaddr", sentinel::HexString(slot, row.slot)},
                       {"value_mib", valueBytes / kMiB}, {"observed_mib", observedMiB}, {"device", device}});
}

// A refusal because a pinned word is not the stock one (logging.md Rule 6): the word read and the word wanted, `have` "unreadable" when the
// read failed.
void LogWordRefusal(const RowPin& row, const char* why, bool readable, std::uint64_t have, std::uint64_t want, std::uint64_t valueBytes,
                    const char* device) noexcept {
  char slot[19];
  char haveText[19];
  char wantText[19];
  sentinel::LogFields(sentinel::LogLevel::kWarn, "combat_heaps_row",
                      {{"row", row.id}, {"status", "refused"}, {"why", why}, {"slot_vaddr", sentinel::HexString(slot, row.slot)},
                       {"have", readable ? sentinel::HexString(haveText, have) : "unreadable"},
                       {"want", sentinel::HexString(wantText, want)}, {"value_mib", valueBytes / kMiB}, {"device", device}});
}

struct Counts {
  std::size_t swapped = 0;
  std::size_t refused = 0;
  std::size_t left = 0;
};

// One row: the reader word and the .rodata size are the stock ones, then the slot is pointed at our u64. A row that fails any check is
// refused alone and writes nothing.
bool SwapRow(const sentinel::ElfImage& image, sentinel::ImageLookup lookup, std::size_t row, std::uint64_t valueBytes,
             const char* device) noexcept {
  const RowPin& pin = kRows[row];
  std::uint32_t readerWord = 0;
  const bool readerRead = ReadGame(image.base + pin.reader, &readerWord);
  if (!readerRead || readerWord != pin.readerWord) {
    LogWordRefusal(pin, "reader_word_not_stock", readerRead, readerWord, pin.readerWord, valueBytes, device);
    return false;
  }
  std::uint64_t stockBytes = 0;
  const bool stockRead = ReadGame(image.base + pin.ro, &stockBytes);
  if (!stockRead || stockBytes != pin.stock) {
    LogWordRefusal(pin, "rodata_not_stock", stockRead, stockBytes, pin.stock, valueBytes, device);
    return false;
  }
  g_value[row] = valueBytes;  // published before the slot points at it
  const sentinel::GotStatus status = g_slots[row].Write(RowTarget(pin), reinterpret_cast<const void*>(image.base + pin.ro),
                                                        &g_value[row], lookup);
  if (status != sentinel::GotStatus::kOk) {
    LogRow(sentinel::LogLevel::kError, pin, "refused", sentinel::GotStatusName(status), valueBytes, 0, device);
    return false;
  }
  LogRow(sentinel::LogLevel::kDebug, pin, "swapped", "slot_points_at_our_value", valueBytes, ObservedMiB(image.base, row), device);
  return true;
}

void LogInstall(const char* result, const char* reason, const Counts& counts, std::uintptr_t base, const char* device, const char* consequence) noexcept {
  const bool ok = counts.refused == 0 && std::strcmp(result, "refused") != 0;
  sentinel::LogFields(ok ? sentinel::LogLevel::kInfo : sentinel::LogLevel::kError, "combat_heaps_install",
                      {{"result", result}, {"reason", reason}, {"swapped", counts.swapped}, {"refused", counts.refused}, {"left", counts.left},
                       {"resource_mib", base != 0 ? ObservedMiB(base, 0) : 0}, {"system_mib", base != 0 ? ObservedMiB(base, kSystemRow) : 0},
                       {"gpu_mib", base != 0 ? ObservedMiB(base, 2) : 0}, {"game_space_mib", base != 0 ? ObservedMiB(base, 3) : 0},
                       {"device", device}, {"consequence", consequence}});
}

constexpr const char* kStockConsequence = "the game keeps its stock heap sizes (206, 400, 160 and 40 MiB)";

ModuleResult Refused(const char* token, const char* device) noexcept {
  const Counts none;
  LogInstall("refused", token, none, 0, device, kStockConsequence);
  return {false, token};
}

}  // namespace

std::uint64_t SystemBytesFor(const char* device) noexcept {
  return device != nullptr && std::strcmp(device, "hollywood") == 0 ? kQuest2SystemBytes : kRows[kSystemRow].target;
}

ModuleResult InstallAt(sentinel::ImageLookup lookup, const char* device, bool heapsOn, bool systemOn) noexcept {
  if (device == nullptr) device = "";
  if (!heapsOn) {
    const Counts none;
    LogInstall("off", "combat.heaps", none, 0, device, "combat.heaps is false: the game keeps its stock heap sizes (206, 400, 160 and 40 MiB)");
    return {false, "off"};
  }
  if (g_ran.exchange(true, std::memory_order_acq_rel)) {
    sentinel::LogFields(sentinel::LogLevel::kWarn, "combat_heaps_refused",
                        {{"why", "already_ran"}, {"consequence", "a second step would find its slots already ours; nothing was written"}});
    return {false, "already_ran"};
  }
  sentinel::ElfImage image;
  if (lookup == nullptr || !lookup(pinned::kLibR15, &image)) return Refused("module_not_loaded", device);
  char id[64] = {};
  if (!sentinel::ReadBuildId(image, id, sizeof(id))) return Refused("no_build_id", device);
  if (std::strcmp(id, pinned::LibR15PatchedBuildId()) != 0) return Refused("build_id_mismatch", device);

  Counts counts;
  for (std::size_t row = 0; row < kRowCount; ++row) {
    if (row == kSystemRow && !systemOn) {
      ++counts.left;
      LogRow(sentinel::LogLevel::kInfo, kRows[row], "left", "combat.memsys_is_false", kRows[row].stock, ObservedMiB(image.base, row), device);
      continue;
    }
    const std::uint64_t value = row == kSystemRow ? SystemBytesFor(device) : kRows[row].target;
    if (SwapRow(image, lookup, row, value, device)) {
      ++counts.swapped;
    } else {
      ++counts.refused;
    }
  }
  const char* const result = counts.refused == 0 ? "installed" : (counts.swapped > 0 ? "partial" : "refused");
  LogInstall(result, counts.refused == 0 ? "ok" : "row_refused", counts, image.base, device,
             counts.refused == 0 ? "the game's heaps are the raised sizes when android_main reads them"
                                 : (counts.swapped > 0 ? "a refused row keeps the game's stock size; the others are raised" : kStockConsequence));
  return {counts.refused == 0, result};
}

ModuleResult Install(bool heapsOn, bool systemOn) noexcept {
  char device[96];
  ReadDevice(device, sizeof(device));
  const sentinel::ImageLookup lookup = g_lookup.load(std::memory_order_acquire);
  return InstallAt(lookup != nullptr ? lookup : &sentinel::FindLoadedImage, device, heapsOn, systemOn);
}

void SetDeviceForTest(const char* device) noexcept { g_deviceOverride.store(device, std::memory_order_release); }
void SetImageLookupForTest(sentinel::ImageLookup lookup) noexcept { g_lookup.store(lookup, std::memory_order_release); }

void ResetForTest() noexcept {
  for (sentinel::GotDataSlot& slot : g_slots) {
    if (slot.written() && slot.Restore() != sentinel::GotStatus::kOk) slot.Forget();
  }
  std::memset(g_value, 0, sizeof(g_value));
  g_ran.store(false, std::memory_order_release);
}

}  // namespace nevr_quest::combat::heaps
