#include "vk_ext_window.h"

#include <atomic>
#include <cstring>

#include "got_hook.h"
#include "hook_install.h"
#include "hook_log.h"
#include "vk_descriptor_guard.h"

namespace nevr_quest::vk_window {

// ---- the decision ------------------------------------------------------------------------------------------------------

namespace {

bool IsSpace(char c) noexcept { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

// The token starting at or after `p`: sets *begin and *len, returns the position after it; nullptr at the end.
const char* NextToken(const char* p, const char** begin, std::size_t* len) noexcept {
  while (*p != '\0' && IsSpace(*p)) ++p;
  if (*p == '\0') return nullptr;
  const char* start = p;
  while (*p != '\0' && !IsSpace(*p)) ++p;
  *begin = start;
  *len = static_cast<std::size_t>(p - start);
  return p;
}

const char* Record(const char* records, std::size_t i) noexcept { return records + i * kRecordStride; }

std::size_t RecordLen(const char* records, std::size_t i) noexcept {
  const char* r = Record(records, i);
  std::size_t n = 0;
  while (n < kRecordNameBytes && r[n] != '\0') ++n;
  return n;
}

bool Same(const char* name, std::size_t len, const char* want) noexcept {
  return std::strlen(want) == len && std::memcmp(name, want, len) == 0;
}

bool IsRequired(const char* name, std::size_t len, const char* vrapi) noexcept {
  for (const char* want : kGameRequired) {
    if (Same(name, len, want)) return true;
  }
  if (vrapi == nullptr) return false;
  const char* begin = nullptr;
  std::size_t tokenLen = 0;
  for (const char* p = vrapi; (p = NextToken(p, &begin, &tokenLen)) != nullptr;) {
    if (tokenLen == len && std::memcmp(begin, name, len) == 0) return true;
  }
  return false;
}

long RecordIndex(const char* records, std::size_t count, const char* name) noexcept {
  const std::size_t want = std::strlen(name);
  for (std::size_t i = 0; i < count; ++i) {
    if (RecordLen(records, i) == want && std::memcmp(Record(records, i), name, want) == 0) return static_cast<long>(i);
  }
  return -1;
}

}  // namespace

long FindToken(const char* list, const char* token) noexcept {
  if (list == nullptr || token == nullptr) return -1;
  const std::size_t want = std::strlen(token);
  long index = 0;
  const char* begin = nullptr;
  std::size_t len = 0;
  for (const char* p = list; (p = NextToken(p, &begin, &len)) != nullptr; ++index) {
    if (len == want && std::memcmp(begin, token, len) == 0) return index;
  }
  return -1;
}

std::size_t CountTokens(const char* list) noexcept {
  if (list == nullptr) return 0;
  std::size_t n = 0;
  const char* begin = nullptr;
  std::size_t len = 0;
  for (const char* p = list; (p = NextToken(p, &begin, &len)) != nullptr;) ++n;
  return n;
}

WindowPlan BuildWindow(const char* driver, std::size_t total, std::size_t capacity, const char* vrapi, char* out) noexcept {
  WindowPlan plan;
  if (driver == nullptr || out == nullptr || capacity == 0 || capacity > kMaxWindow || total <= capacity ||
      total > kFullCapacity) {
    return plan;
  }
  bool required[kFullCapacity];
  for (std::size_t i = 0; i < total; ++i) {
    required[i] = IsRequired(Record(driver, i), RecordLen(driver, i), vrapi);
    if (required[i]) {
      ++plan.required;
      if (i >= capacity) ++plan.outside;
    }
  }
  plan.swapchain_before = RecordIndex(driver, total, kSwapchain);
  if (plan.outside == 0) return plan;

  // The required names first, in the driver's order, then the others in the driver's order, `capacity` records in all.
  std::size_t written = 0;
  for (std::size_t i = 0; i < total && written < capacity; ++i) {
    if (required[i]) std::memcpy(out + written++ * kRecordStride, Record(driver, i), kRecordStride);
  }
  for (std::size_t i = 0; i < total && written < capacity; ++i) {
    if (!required[i]) std::memcpy(out + written++ * kRecordStride, Record(driver, i), kRecordStride);
  }
  plan.swapchain_after = RecordIndex(out, capacity, kSwapchain);
  plan.reorder = true;
  return plan;
}

// ---- the hooks -------------------------------------------------------------------------------------------------------

namespace {

using VoidFn = void (*)();
using GipaFn = VoidFn (*)(void* instance, const char* name);
using EnumFn = std::int32_t (*)(void* physical_device, const char* layer, std::uint32_t* count, void* properties);

constexpr const char* kGetInstanceProcAddr = "vkGetInstanceProcAddr";
constexpr const char* kEnumerateDeviceExtensions = "vkEnumerateDeviceExtensionProperties";
constexpr std::size_t kMovedChars = 300;

// The driver's functions as the game's FindMethod and vkGetInstanceProcAddr calls returned them.
std::atomic<void*> g_realGipa{nullptr};
std::atomic<void*> g_realEnumerate{nullptr};
std::atomic<unsigned> g_calls{0};
std::atomic_flag g_busy = ATOMIC_FLAG_INIT;
alignas(8) char g_driverList[kFullCapacity * kRecordStride];
alignas(8) char g_window[kMaxWindow * kRecordStride];

// What one property call came to; filled by Decide, logged by Report after the call has returned.
struct Outcome {
  const char* action = "passthrough";  // passthrough | reordered | unchanged
  const char* reason = "";
  std::uint32_t total = 0;      // the driver's total, when it was asked
  std::uint32_t examined = 0;   // records read into g_driverList
  std::int32_t vrapi_result = -1;
  std::size_t vrapi_tokens = 0;
  WindowPlan plan;
  char moved[kMovedChars + 1] = {};  // required names that were past the game's buffer
};

void NoteMoved(const char* driver, std::size_t examined, std::size_t capacity, const char* vrapi, char* moved) noexcept {
  std::size_t pos = 0;
  moved[0] = '\0';
  for (std::size_t i = capacity; i < examined; ++i) {
    const std::size_t len = RecordLen(driver, i);
    if (!IsRequired(Record(driver, i), len, vrapi)) continue;
    const std::size_t sep = pos > 0 ? 1 : 0;
    if (pos + sep + len + 1 > kMovedChars) break;
    if (sep != 0) moved[pos++] = ' ';
    std::memcpy(moved + pos, Record(driver, i), len);
    pos += len;
    moved[pos] = '\0';
  }
}

// The game's buffer call came back VK_INCOMPLETE: read the driver's whole list and the VrApi list, and rewrite the game's
// buffer when a required name is past it. Only called with g_busy held.
void Decide(EnumFn real, void* physical_device, std::uint32_t capacity, void* properties, Outcome* out) noexcept {
  std::uint32_t total = 0;
  const std::int32_t counted = real(physical_device, nullptr, &total, nullptr);
  out->total = total;
  if (counted != kVkSuccess) {
    out->reason = "total_query_failed";
    return;
  }
  std::uint32_t examined = total < kFullCapacity ? total : static_cast<std::uint32_t>(kFullCapacity);
  const std::int32_t listed = real(physical_device, nullptr, &examined, g_driverList);
  out->examined = examined;
  if (listed != kVkSuccess && listed != kVkIncomplete) {
    out->reason = "list_query_failed";
    return;
  }

  char vrapi[kVrapiChars + 1] = {};
  const DeviceExtensionsThunk::Fn vrapiFn = DeviceExtensionsThunk::Original();
  if (vrapiFn != nullptr) {
    std::uint32_t size = static_cast<std::uint32_t>(kVrapiChars);
    out->vrapi_result = vrapiFn(vrapi, &size);
    if (out->vrapi_result != kVkSuccess) vrapi[0] = '\0';
    vrapi[kVrapiChars] = '\0';
    out->vrapi_tokens = CountTokens(vrapi);
  }

  out->plan = BuildWindow(g_driverList, examined, capacity, vrapi, g_window);
  if (!out->plan.reorder) {
    out->action = "unchanged";
    out->reason = examined <= capacity ? "total_within_window" : "required_names_in_window";
    return;
  }
  NoteMoved(g_driverList, examined, capacity, vrapi, out->moved);
  std::memcpy(properties, g_window, static_cast<std::size_t>(capacity) * kRecordStride);
  out->action = "reordered";
  out->reason = "required_names_past_game_buffer";
}

// After the game's call has returned.
void Report(void* physical_device, std::uint32_t count_in, std::uint32_t count_out, std::int32_t result,
            const Outcome& o) noexcept {
  const unsigned call = g_calls.fetch_add(1, std::memory_order_relaxed) + 1;
  if (call > kMaxLoggedCalls) return;
  char hex[19];
  sentinel::LogFields(
      o.plan.reorder ? sentinel::LogLevel::kWarn : sentinel::LogLevel::kInfo, "vk_ext_window",
      {{"call", call},
       {"action", o.action},
       {"reason", o.reason},
       {"physical_device", sentinel::HexString(hex, reinterpret_cast<std::uintptr_t>(physical_device))},
       {"count_in", count_in},
       {"count_out", count_out},
       {"result", result},
       {"driver_total", o.total},
       {"examined", o.examined},
       {"required_present", o.plan.required},
       {"required_outside", o.plan.outside},
       {"swapchain_before", o.plan.swapchain_before},
       {"swapchain_after", o.plan.swapchain_after},
       {"vrapi_result", o.vrapi_result},
       {"vrapi_tokens", o.vrapi_tokens},
       {"moved", o.moved}});
}

// vkEnumerateDeviceExtensionProperties as the game calls it. The driver's answer is returned as it came; only the contents
// of the game's buffer can differ (Decide).
std::int32_t WrappedEnumerate(void* physical_device, const char* layer, std::uint32_t* count, void* properties) {
  const EnumFn real = reinterpret_cast<EnumFn>(g_realEnumerate.load(std::memory_order_acquire));
  if (real == nullptr) return -3;  // VK_ERROR_INITIALIZATION_FAILED; unreachable, the wrapper is handed out after the store
  const std::uint32_t count_in = count != nullptr ? *count : 0;
  const std::int32_t result = real(physical_device, layer, count, properties);
  if (layer != nullptr || properties == nullptr || count == nullptr) return result;  // not the game's buffer call

  Outcome o;
  const std::uint32_t count_out = *count;
  if (result != kVkIncomplete) {
    o.reason = result == kVkSuccess ? "complete" : "driver_error";
  } else if (count_out != count_in || count_in == 0 || count_in > kMaxWindow) {
    o.reason = "unexpected_count";
  } else if (g_busy.test_and_set(std::memory_order_acquire)) {
    o.reason = "busy";
  } else {
    Decide(real, physical_device, count_in, properties, &o);
    g_busy.clear(std::memory_order_release);
  }
  Report(physical_device, count_in, count_out, result, o);
  return result;
}

// vkGetInstanceProcAddr as the game calls it: vkEnumerateDeviceExtensionProperties is replaced here, vkGetDeviceProcAddr by
// the descriptor guard.
VoidFn WrappedGetInstanceProcAddr(void* instance, const char* name) {
  const GipaFn real = reinterpret_cast<GipaFn>(g_realGipa.load(std::memory_order_acquire));
  if (real == nullptr) return nullptr;
  const VoidFn found = real(instance, name);
  if (found != nullptr && name != nullptr && std::strcmp(name, kEnumerateDeviceExtensions) == 0) {
    g_realEnumerate.store(reinterpret_cast<void*>(found), std::memory_order_release);
    return reinterpret_cast<VoidFn>(&WrappedEnumerate);
  }
  // vkGetDeviceProcAddr goes to the descriptor guard (vk_descriptor_guard.h, #1737 G6A-030); every other name is the driver's.
  return vk_guard::OnInstanceProcAddr(real, instance, name, found);
}

// CSysModule::FindMethod: the game's way to dlsym. Only vkGetInstanceProcAddr is replaced.
void* FindMethodHandler(FindMethodThunk::Fn original, unsigned long module, const char* name) noexcept {
  void* const found = original(module, name);
  if (found == nullptr || name == nullptr || std::strcmp(name, kGetInstanceProcAddr) != 0) return found;
  g_realGipa.store(found, std::memory_order_release);
  return reinterpret_cast<void*>(&WrappedGetInstanceProcAddr);
}

// The game's own VrApi call, unchanged: this hook exists so the wrapper can reach the real function (Original()).
std::int32_t DeviceExtensionsHandler(DeviceExtensionsThunk::Fn original, char* names, std::uint32_t* size) noexcept {
  return original(names, size);
}

NEVR_HOOK_RECORD(kFindMethodHook, FindMethodThunk, &FindMethodHandler);
NEVR_HOOK_RECORD(kDeviceExtensionsHook, DeviceExtensionsThunk, &DeviceExtensionsHandler);

sentinel::GotHook g_findMethodHook;
sentinel::GotHook g_deviceHook;

void ArmAll() noexcept {
  FindMethodThunk::Arm(kFindMethodHook);
  DeviceExtensionsThunk::Arm(kDeviceExtensionsHook);
}

}  // namespace

bool Install() noexcept {
  ArmAll();
  const sentinel::GotStatus find =
      sentinel::InstallThunk<FindMethodThunk>(g_findMethodHook, sentinel::pinned::LibR15FindMethod());
  const sentinel::GotStatus device =
      sentinel::InstallThunk<DeviceExtensionsThunk>(g_deviceHook, sentinel::pinned::LibR15GetDeviceExtensionsVulkan());
  const bool ok = find == sentinel::GotStatus::kOk && device == sentinel::GotStatus::kOk;
  sentinel::LogFields(ok ? sentinel::LogLevel::kInfo : sentinel::LogLevel::kError, "vk_ext_window_install",
                      {{"result", ok ? "installed" : "partial_or_failed"},
                       {"find_method", sentinel::GotStatusName(find)},
                       {"device_extensions", sentinel::GotStatusName(device)}});
  return ok;
}

void ArmForTest() noexcept { ArmAll(); }

void ResetForTest() noexcept {
  g_realGipa.store(nullptr, std::memory_order_relaxed);
  g_realEnumerate.store(nullptr, std::memory_order_relaxed);
  g_calls.store(0, std::memory_order_relaxed);
  g_busy.clear(std::memory_order_relaxed);
}

}  // namespace nevr_quest::vk_window
