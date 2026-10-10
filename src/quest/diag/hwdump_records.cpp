#include "quest/diag/hwdump_records.h"

#include <cstring>

namespace nevr_quest::hwdump {
namespace {

// Copies at most `cap - 1` bytes of `s` and terminates; reports whether it had to cut.
bool CopyBounded(char* dst, std::size_t cap, const char* s) noexcept {
  if (s == nullptr) {
    dst[0] = '\0';
    return false;
  }
  std::size_t n = 0;
  while (n + 1 < cap && s[n] != '\0') {
    dst[n] = s[n];
    ++n;
  }
  dst[n] = '\0';
  return s[n] != '\0';
}

bool SameName(const char* stored, bool stored_truncated, const char* name) noexcept {
  if (name == nullptr) return stored[0] == '\0' && !stored_truncated;
  // A cut name matches any name with the same first kNameBytes - 1 bytes; good enough for a dump.
  return std::strncmp(stored, name, kNameBytes - 1) == 0 &&
         (stored_truncated || std::strlen(name) < kNameBytes);
}

// Spins this many loads waiting for a concurrent claim of the same slot to publish before moving on.
constexpr int kClaimSpin = 1000;

}  // namespace

const char* QueryFnName(QueryFn fn) noexcept {
  switch (fn) {
    case QueryFn::kSystemPropertyGet: return "__system_property_get";
    case QueryFn::kSysconf: return "sysconf";
    case QueryFn::kUname: return "uname";
    case QueryFn::kGethostname: return "gethostname";
    case QueryFn::kConfigGetLanguage: return "AConfiguration_getLanguage";
    case QueryFn::kConfigGetCountry: return "AConfiguration_getCountry";
    case QueryFn::kVrapiInitialize: return "vrapi_Initialize";
    case QueryFn::kVrapiGetSystemPropertyInt: return "vrapi_GetSystemPropertyInt";
    case QueryFn::kVrapiGetSystemPropertyFloat: return "vrapi_GetSystemPropertyFloat";
    case QueryFn::kVrapiGetSystemPropertyFloatArray: return "vrapi_GetSystemPropertyFloatArray";
    case QueryFn::kVrapiGetSystemStatusInt: return "vrapi_GetSystemStatusInt";
    case QueryFn::kVrapiGetPropertyInt: return "vrapi_GetPropertyInt";
    case QueryFn::kVrapiSetPropertyInt: return "vrapi_SetPropertyInt";
    case QueryFn::kVrapiSetDisplayRefreshRate: return "vrapi_SetDisplayRefreshRate";
    case QueryFn::kVrapiSetClockLevels: return "vrapi_SetClockLevels";
    case QueryFn::kVrapiSetExtraLatencyMode: return "vrapi_SetExtraLatencyMode";
    case QueryFn::kVrapiGetInstanceExtensionsVulkan: return "vrapi_GetInstanceExtensionsVulkan";
    case QueryFn::kVrapiGetDeviceExtensionsVulkan: return "vrapi_GetDeviceExtensionsVulkan";
    case QueryFn::kCount: break;
  }
  return "unknown";
}

void SetText(Answer* out, const char* s) noexcept {
  out->text_truncated = CopyBounded(out->text, kTextBytes, s);
  out->has_text = true;
}

void AddInt(Answer* out, std::int64_t v) noexcept {
  if (out->num_ints < kMaxInts) out->ints[out->num_ints++] = v;
}

void AddFloat(Answer* out, float v) noexcept {
  if (out->num_floats < kMaxFloats) out->floats[out->num_floats++] = v;
}

void RecordTable::Record(QueryFn fn, std::int64_t id, const char* name, const Answer& answer) noexcept {
  for (Entry& e : entries_) {
    std::uint32_t state = e.state.load(std::memory_order_acquire);
    if (state == kEmpty) {
      std::uint32_t expected = kEmpty;
      if (e.state.compare_exchange_strong(expected, kClaiming, std::memory_order_acq_rel)) {
        e.fn = fn;
        e.id = id;
        e.name_truncated = CopyBounded(e.name, kNameBytes, name);
        e.first = answer;
        e.last = answer;
        e.calls.store(1, std::memory_order_relaxed);
        e.state.store(kReady, std::memory_order_release);
        return;
      }
      state = expected;
    }
    for (int i = 0; state == kClaiming && i < kClaimSpin; ++i) state = e.state.load(std::memory_order_acquire);
    if (state != kReady) {
      // Another thread is still filling this slot: its key is unknown, so move on. A query can then
      // occupy two slots; the dump shows both.
      contended_.fetch_add(1, std::memory_order_relaxed);
      continue;
    }
    if (e.fn != fn || e.id != id || !SameName(e.name, e.name_truncated, name)) continue;
    e.calls.fetch_add(1, std::memory_order_relaxed);
    bool held = false;
    if (e.busy.compare_exchange_strong(held, true, std::memory_order_acquire)) {
      e.last = answer;
      e.busy.store(false, std::memory_order_release);
    } else {
      contended_.fetch_add(1, std::memory_order_relaxed);
    }
    return;
  }
  overflow_.fetch_add(1, std::memory_order_relaxed);
}

std::size_t RecordTable::Snapshot(RecordSnapshot* out, std::size_t capacity) const noexcept {
  std::size_t n = 0;
  for (const Entry& e : entries_) {
    if (n == capacity) break;
    if (e.state.load(std::memory_order_acquire) != kReady) continue;
    RecordSnapshot& s = out[n++];
    s.fn = e.fn;
    s.id = e.id;
    std::memcpy(s.name, e.name, kNameBytes);
    s.name_truncated = e.name_truncated;
    s.first = e.first;
    s.last = e.first;
    s.last_current = false;
    for (int attempt = 0; attempt < 1000; ++attempt) {
      bool held = false;
      if (e.busy.compare_exchange_strong(held, true, std::memory_order_acquire)) {
        s.last = e.last;
        e.busy.store(false, std::memory_order_release);
        s.last_current = true;
        break;
      }
    }
    s.calls = e.calls.load(std::memory_order_relaxed);
  }
  return n;
}

namespace {
// Namespace scope with a constexpr constructor and a trivial destructor: constant-initialised, so no
// .init_array entry and no atexit registration (tools/check_quest_static_init.sh checks the objects).
RecordTable g_records;
}  // namespace

RecordTable& Records() noexcept { return g_records; }

}  // namespace nevr_quest::hwdump
