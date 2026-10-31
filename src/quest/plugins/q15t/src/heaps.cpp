#include "heaps.h"

#include <cstdio>
#include <cstring>

#if defined(__ANDROID__)
#include <sys/system_properties.h>
#endif

#include "got_swap.h"
#include "module_switch.h"
#include "plugin_log.h"
#include "safe_read.h"

namespace q15t {
namespace {

constexpr std::uint64_t kMiB = 1ull << 20;
constexpr std::uint32_t kLdrX1FromX8 = 0xf9400101;  // ldr x1,[x8]
constexpr std::uint32_t kLdrX0FromX8 = 0xf9400100;  // ldr x0,[x8]
constexpr int kRows = 4;
constexpr int kSystemRow = 1;

// ELF vaddrs. GLOB_DAT slot -> symbol value: a source-build readelf note (not in this tree), lines 89726 (R06), 98594 (R07), 92082 (R08), 94611 (R09).
// Reader: the disassembly VA of the load is ELF + 0x100000 (android_main 0x1078590 / 0x1078524 / 0x1078614, GameSpaceAllocatorSize
// 0x12ffdec).
constexpr HeapRowPin kPins[kRows] = {
    {"R06", "_ZN10NRadEngine22kResourceMemoryAndroidE", 0x370b720, 0x2b9caa0, 0xf78590, kLdrX1FromX8, 206 * kMiB, 320 * kMiB},
    {"R07", "_ZN10NRadEngine20kSystemMemoryAndroidE", 0x371d020, 0x2b9ca90, 0xf78524, kLdrX1FromX8, 400 * kMiB, 640 * kMiB},
    {"R08", "_ZN10NRadEngine17kGpuMemoryAndroidE", 0x3710188, 0x2b9ca98, 0xf78614, kLdrX1FromX8, 160 * kMiB, 512 * kMiB},
    {"R09", "_ZN10NRadEngine24kR15GameSpaceMemoryQuestE", 0x3715198, 0x2b9e1c0, 0x11ffdec, kLdrX0FromX8, 40 * kMiB, 128 * kMiB},
};
constexpr std::uint64_t kSystemQuest2 = 512 * kMiB;  // the source build's memsys:253 (ro.product.device "hollywood")

// The values the game reads. A game reader holds the slot's pointer for the process's lifetime, so these stay valid for it:
// plain static storage of a library that is never unloaded.
alignas(8) std::uint64_t g_value[kRows];

int g_swapped = 0, g_refused = 0;
char g_device_override[96] = "";
bool g_have_device_override = false;

struct Outcome {
  SwapStatus status = SwapStatus::kOk;
  const char* reason = "";
};

void ReadDevice(char* out, std::size_t n) {
  out[0] = 0;
  if (g_have_device_override) {
    std::snprintf(out, n, "%s", g_device_override);
    return;
  }
#if defined(__ANDROID__)
  char v[PROP_VALUE_MAX] = "";
  __system_property_get("ro.product.device", v);
  std::snprintf(out, n, "%s", v);
#endif
}

std::uint64_t ValueFor(int row, const char* device) {
  if (row == kSystemRow) return std::strcmp(device, "hollywood") == 0 ? kSystemQuest2 : kPins[row].target;
  return kPins[row].target;
}

void LogRow(std::uint32_t level, const char* message, int row, const char* why, std::uint64_t observed, std::uint64_t value,
            const char* device) {
  char slot[24], seen[24], mib[24];
  std::snprintf(slot, sizeof slot, "0x%llx", static_cast<unsigned long long>(kPins[row].got));
  std::snprintf(seen, sizeof seen, "0x%llx", static_cast<unsigned long long>(observed));
  std::snprintf(mib, sizeof mib, "%llu", static_cast<unsigned long long>(value / kMiB));
  const NevrLogField f[] = {{"row", kPins[row].id}, {"slot_vaddr", slot}, {"why", why},
                            {"observed", seen},     {"value_mib", mib},   {"device", device}};
  Log(level, message, f, 6);
}

Outcome InstallRow(std::uintptr_t base, int row, const char* device) {
  const HeapRowPin& pin = kPins[row];
  Outcome out;
  const std::uint64_t value = ValueFor(row, device);

  bool ok = true;
  const auto reader = Rd<std::uint32_t>(base + pin.reader, &ok);
  if (!ok || reader != pin.reader_word) {
    LogRow(NEVR_LOG_WARNING, "heaps row refused", row, "reader word is not the stock load", reader, value, device);
    out.status = SwapStatus::kUnexpectedOriginal;
    out.reason = "reader_word_mismatch";
    return out;
  }
  const auto rodata = Rd<std::uint64_t>(base + pin.ro, &ok);
  if (!ok || rodata != pin.stock) {
    LogRow(NEVR_LOG_WARNING, "heaps row refused", row, "rodata word is not the stock size", rodata, value, device);
    out.status = SwapStatus::kUnexpectedOriginal;
    out.reason = "rodata_not_stock";
    return out;
  }

  g_value[row] = value;  // published before the slot points at it
  const SwapResult r = SwapWord(base + pin.got, base + pin.ro, reinterpret_cast<std::uintptr_t>(&g_value[row]));
  if (r.status != SwapStatus::kOk) {
    LogRow(NEVR_LOG_ERROR, "heaps row refused", row, StatusName(r.status), r.original, value, device);
    out.status = r.status;
    out.reason = "swap_refused";
    return out;
  }

  // What the game will read now: slot -> pointer -> u64 (0 when unreadable). The line carries it as `observed`.
  ok = true;
  const auto word = Rd<std::uintptr_t>(base + pin.got, &ok);
  const std::uint64_t seen = ok ? Rd<std::uint64_t>(word, &ok) : 0;
  LogRow(NEVR_LOG_INFO, "heaps row swapped", row, "slot now points at the plugin's u64", ok ? seen : 0, value, device);
  return out;
}

}  // namespace

const HeapRowPin* HeapRowPins(int* count) {
  if (count != nullptr) *count = kRows;
  return kPins;
}

InstallResult InstallHeaps(std::uintptr_t base) {
  InstallResult result;
  if (base == 0) {
    result.reason = "no_base";
    Log(NEVR_LOG_ERROR, "heaps not installed: no game base");
    return result;
  }
  if (ModuleSwitchPresent("heaps.off")) {
    result.status = SwapStatus::kOk;
    result.reason = "heaps_off";
    Log(NEVR_LOG_INFO, "heaps off: heaps.off present, nothing changed");
    return result;
  }
  const bool memsys_off = ModuleSwitchPresent("memsys.off");
  char device[96];
  ReadDevice(device, sizeof device);
  result.status = SwapStatus::kOk;
  for (int row = 0; row < kRows; ++row) {
    if (row == kSystemRow && memsys_off) {
      LogRow(NEVR_LOG_INFO, "heaps row off", row, "memsys.off present", 0, kPins[row].stock, device);
      continue;
    }
    const Outcome o = InstallRow(base, row, device);
    if (o.status == SwapStatus::kOk) {
      ++g_swapped;
    } else {
      ++g_refused;
      if (result.status == SwapStatus::kOk) {
        result.status = o.status;
        result.reason = kPins[row].id;
      }
    }
  }
  return result;
}

HeapsStats GetHeapsStats() { return {g_swapped + g_refused, g_swapped, g_refused}; }

void SetHeapsDeviceForTest(const char* device) {
  g_have_device_override = device != nullptr;
  std::snprintf(g_device_override, sizeof g_device_override, "%s", device != nullptr ? device : "");
}

void ResetHeapsForTest() {
  g_swapped = g_refused = 0;
  std::memset(g_value, 0, sizeof g_value);
}

}  // namespace q15t
