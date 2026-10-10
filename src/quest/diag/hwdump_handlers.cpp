// Personality-free by construction: built with -fno-exceptions, no object with a destructor, no indirect call
// other than `original`. Every handler is live across its call into the game (callback_thunk.h rule 1), so
// everything it calls (the record table, the copies below) must be too.
#include "quest/diag/hwdump_handlers.h"

#include "quest/diag/hwdump_records.h"

#include <sys/utsname.h>
#include <time.h>

#include <atomic>

namespace nevr_quest::hwdump {
namespace {

std::atomic<void*> g_vm{nullptr};
std::atomic<void*> g_activity{nullptr};
std::atomic<bool> g_initialize_seen{false};
std::atomic<int> g_initialize_status{0};
std::atomic<std::uint64_t> g_initialize_seen_ms{0};

std::uint64_t MonotonicMs() noexcept {
  timespec ts{};
  ::clock_gettime(CLOCK_MONOTONIC, &ts);  // the sentinel's own import, not libr15's hooked slot
  return static_cast<std::uint64_t>(ts.tv_sec) * 1000U + static_cast<std::uint64_t>(ts.tv_nsec) / 1000000U;
}

// Keeps the first VM and activity seen. Both stay valid for the process (the activity until NativeActivity
// is destroyed; the dump thread checks it before use).
void Capture(const OvrJava* java) noexcept {
  if (java == nullptr) return;
  void* expected = nullptr;
  if (java->vm != nullptr) g_vm.compare_exchange_strong(expected, java->vm, std::memory_order_acq_rel);
  expected = nullptr;
  if (java->activity != nullptr) {
    g_activity.compare_exchange_strong(expected, java->activity, std::memory_order_acq_rel);
  }
}

// Appends `s` (at most `max` bytes, stopping at a NUL) to `dst` at `*pos`, bounded by `cap`.
void Append(char* dst, std::size_t cap, std::size_t* pos, const char* s, std::size_t max) noexcept {
  for (std::size_t i = 0; i < max && s[i] != '\0' && *pos + 1 < cap; ++i) dst[(*pos)++] = s[i];
  dst[*pos] = '\0';
}

void RecordTwoChars(QueryFn fn, const char* out) noexcept {
  // AConfiguration_getLanguage/getCountry write exactly two bytes and no terminator.
  char two[3] = {out[0], out[1], '\0'};
  Answer a;
  SetText(&a, two);
  Records().Record(fn, 0, nullptr, a);
}

}  // namespace

int HookSystemPropertyGet(SystemPropertyGetFn original, const char* name, char* value) noexcept {
  const int r = original(name, value);
  Answer a;
  AddInt(&a, r);
  if (value != nullptr) {
    char bounded[kTextBytes];
    std::size_t pos = 0;
    Append(bounded, sizeof bounded, &pos, value, 92);  // PROP_VALUE_MAX: the caller's buffer size
    SetText(&a, bounded);
  }
  Records().Record(QueryFn::kSystemPropertyGet, 0, name, a);
  return r;
}

long HookSysconf(SysconfFn original, int name) noexcept {
  const long r = original(name);
  Answer a;
  AddInt(&a, r);
  Records().Record(QueryFn::kSysconf, name, nullptr, a);
  return r;
}

int HookUname(UnameFn original, struct utsname* buf) noexcept {
  const int r = original(buf);
  Answer a;
  AddInt(&a, r);
  if (r == 0 && buf != nullptr) {
    char text[kTextBytes];
    std::size_t pos = 0;
    text[0] = '\0';
    const char* parts[] = {buf->sysname, buf->nodename, buf->release, buf->version, buf->machine};
    for (std::size_t i = 0; i < 5; ++i) {
      if (i != 0) Append(text, sizeof text, &pos, "|", 1);
      Append(text, sizeof text, &pos, parts[i], sizeof buf->sysname);
    }
    SetText(&a, text);
  }
  Records().Record(QueryFn::kUname, 0, nullptr, a);
  return r;
}

int HookGethostname(GethostnameFn original, char* name, std::size_t len) noexcept {
  const int r = original(name, len);
  Answer a;
  AddInt(&a, r);
  if (r == 0 && name != nullptr) {
    char text[kTextBytes];
    std::size_t pos = 0;
    text[0] = '\0';
    Append(text, sizeof text, &pos, name, len);  // never past the caller's buffer, terminated or not
    SetText(&a, text);
  }
  Records().Record(QueryFn::kGethostname, 0, nullptr, a);
  return r;
}

void HookConfigGetLanguage(ConfigGetTwoCharsFn original, void* config, char* out) noexcept {
  original(config, out);
  if (out != nullptr) RecordTwoChars(QueryFn::kConfigGetLanguage, out);
}

void HookConfigGetCountry(ConfigGetTwoCharsFn original, void* config, char* out) noexcept {
  original(config, out);
  if (out != nullptr) RecordTwoChars(QueryFn::kConfigGetCountry, out);
}

int HookVrapiInitialize(VrapiInitializeFn original, const OvrInitParms* parms) noexcept {
  Answer a;
  if (parms != nullptr) {
    for (std::int32_t f : parms->fields) AddInt(&a, f);
    Capture(&parms->java);
  }
  const int r = original(parms);
  AddInt(&a, r);
  g_initialize_status.store(r, std::memory_order_relaxed);
  g_initialize_seen_ms.store(MonotonicMs(), std::memory_order_relaxed);
  g_initialize_seen.store(true, std::memory_order_release);
  Records().Record(QueryFn::kVrapiInitialize, 0, nullptr, a);
  return r;
}

int HookVrapiGetSystemPropertyInt(VrapiGetIntFn original, const OvrJava* java, int id) noexcept {
  Capture(java);
  const int r = original(java, id);
  Answer a;
  AddInt(&a, r);
  Records().Record(QueryFn::kVrapiGetSystemPropertyInt, id, nullptr, a);
  return r;
}

float HookVrapiGetSystemPropertyFloat(VrapiGetFloatFn original, const OvrJava* java, int id) noexcept {
  Capture(java);
  const float r = original(java, id);
  Answer a;
  AddFloat(&a, r);
  Records().Record(QueryFn::kVrapiGetSystemPropertyFloat, id, nullptr, a);
  return r;
}

int HookVrapiGetSystemPropertyFloatArray(VrapiGetFloatArrayFn original, const OvrJava* java, int id, float* values,
                                         int capacity) noexcept {
  Capture(java);
  const int r = original(java, id, values, capacity);
  Answer a;
  AddInt(&a, r);
  AddInt(&a, capacity);
  const int filled = r < capacity ? r : capacity;
  for (int i = 0; values != nullptr && i < filled && i < static_cast<int>(kMaxFloats); ++i) AddFloat(&a, values[i]);
  Records().Record(QueryFn::kVrapiGetSystemPropertyFloatArray, id, nullptr, a);
  return r;
}

int HookVrapiGetSystemStatusInt(VrapiGetIntFn original, const OvrJava* java, int id) noexcept {
  Capture(java);
  const int r = original(java, id);
  Answer a;
  AddInt(&a, r);
  Records().Record(QueryFn::kVrapiGetSystemStatusInt, id, nullptr, a);
  return r;
}

int HookVrapiGetPropertyInt(VrapiGetPropertyIntFn original, const OvrJava* java, int id, int* out) noexcept {
  Capture(java);
  const int r = original(java, id, out);
  Answer a;
  AddInt(&a, r);
  if ((r & 1) != 0 && out != nullptr) AddInt(&a, *out);  // the out value only when the call says it is set
  Records().Record(QueryFn::kVrapiGetPropertyInt, id, nullptr, a);
  return r;
}

namespace {
void RecordExtensions(QueryFn fn, int r, const char* names, const std::uint32_t* size) noexcept {
  Answer a;
  AddInt(&a, r);
  if (size != nullptr) {
    AddInt(&a, *size);
    if (names != nullptr) {
      char text[kTextBytes];
      std::size_t pos = 0;
      text[0] = '\0';
      Append(text, sizeof text, &pos, names, *size);  // never past the size the callee reported
      SetText(&a, text);
      if (*size >= kTextBytes) a.text_truncated = true;
    }
  }
  Records().Record(fn, 0, nullptr, a);
}
}  // namespace

int HookVrapiGetInstanceExtensionsVulkan(VrapiGetExtensionsFn original, char* names, std::uint32_t* size) noexcept {
  const int r = original(names, size);
  RecordExtensions(QueryFn::kVrapiGetInstanceExtensionsVulkan, r, names, size);
  return r;
}

int HookVrapiGetDeviceExtensionsVulkan(VrapiGetExtensionsFn original, char* names, std::uint32_t* size) noexcept {
  const int r = original(names, size);
  RecordExtensions(QueryFn::kVrapiGetDeviceExtensionsVulkan, r, names, size);
  return r;
}

int HookVrapiSetPropertyInt(VrapiSetPropertyIntFn original, const OvrJava* java, int id, int value) noexcept {
  Capture(java);
  const int r = original(java, id, value);
  Answer a;
  AddInt(&a, value);
  Records().Record(QueryFn::kVrapiSetPropertyInt, id, nullptr, a);
  return r;
}

int HookVrapiSetDisplayRefreshRate(VrapiSetRefreshRateFn original, void* ovr, float rate) noexcept {
  const int r = original(ovr, rate);
  Answer a;
  AddFloat(&a, rate);
  AddInt(&a, r);
  Records().Record(QueryFn::kVrapiSetDisplayRefreshRate, 0, nullptr, a);
  return r;
}

int HookVrapiSetClockLevels(VrapiSetClockLevelsFn original, void* ovr, int cpu, int gpu) noexcept {
  const int r = original(ovr, cpu, gpu);
  Answer a;
  AddInt(&a, cpu);
  AddInt(&a, gpu);
  AddInt(&a, r);
  Records().Record(QueryFn::kVrapiSetClockLevels, 0, nullptr, a);
  return r;
}

int HookVrapiSetExtraLatencyMode(VrapiSetLatencyModeFn original, void* ovr, int mode) noexcept {
  const int r = original(ovr, mode);
  Answer a;
  AddInt(&a, mode);
  AddInt(&a, r);
  Records().Record(QueryFn::kVrapiSetExtraLatencyMode, 0, nullptr, a);
  return r;
}

GameCapture Captured() noexcept {
  GameCapture c;
  c.initialize_seen = g_initialize_seen.load(std::memory_order_acquire);
  c.initialize_status = g_initialize_status.load(std::memory_order_relaxed);
  c.initialize_seen_monotonic_ms = g_initialize_seen_ms.load(std::memory_order_relaxed);
  c.vm = g_vm.load(std::memory_order_acquire);
  c.activity = g_activity.load(std::memory_order_acquire);
  return c;
}

void ResetCaptureForTest() noexcept {
  g_vm.store(nullptr);
  g_activity.store(nullptr);
  g_initialize_seen.store(false);
  g_initialize_status.store(0);
  g_initialize_seen_ms.store(0);
}

}  // namespace nevr_quest::hwdump
