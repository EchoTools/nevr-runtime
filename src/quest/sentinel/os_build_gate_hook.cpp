#include "os_build_gate_hook.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

#if defined(__ANDROID__)
#include <sys/system_properties.h>
#endif

#include "hook_install.h"
#include "hook_log.h"

namespace nevr_quest::os_build_gate {

namespace {

constexpr const char* kUnmeasured = "unmeasured";
constexpr const char* kBuildProperty = "ro.build.version.incremental";
constexpr std::size_t kPropertyValueMax = 92;  // PROP_VALUE_MAX

int DefaultReader(const char* name, char* value) noexcept {
#if defined(__ANDROID__)
  return __system_property_get(name, value);
#else
  static_cast<void>(name);
  static_cast<void>(value);
  return 0;
#endif
}

sentinel::GotHook g_hook;
std::atomic<PropertyReader> g_reader{&DefaultReader};
std::atomic<std::uint64_t> g_floor{0};
std::atomic<unsigned long long> g_calls{0};
std::atomic<bool> g_read{false};
std::atomic<std::uint64_t> g_osBuild{0};
std::atomic<const char*> g_reason{kUnmeasured};

// The value of one digit character in base 36, or -1 when it is not a letter or a digit.
int DigitValue(char c) noexcept {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'z') return c - 'a' + 10;
  if (c >= 'A' && c <= 'Z') return c - 'A' + 10;
  return -1;
}

// CSysInfo::OSBuildVersion. The game's only caller is CR15NetGame::Initialize, which refuses the headset when this returns
// 1 with a number below the floor. The hook answers 0 ("no number") every time, so Initialize goes on to its next check.
// The number is measured from the property here, on the first call; the game's function is never called.
int Handler(Thunk::Fn /*original: never called*/, std::uint64_t* /*version*/) noexcept {
  if (g_calls.fetch_add(1, std::memory_order_acq_rel) != 0) return 0;  // measured once
  char value[kPropertyValueMax] = {};
  const PropertyReader reader = g_reader.load(std::memory_order_acquire);
  const int length = reader(kBuildProperty, value);
  std::uint64_t osBuild = 0;
  const Reading reading = ParseOsBuild(value, length, &osBuild);
  const bool read = reading == Reading::kNumber;
  if (!read) osBuild = 0;
  g_read.store(read, std::memory_order_relaxed);
  g_osBuild.store(osBuild, std::memory_order_relaxed);
  g_reason.store(ReadingName(reading), std::memory_order_release);
  const Seen seen = CurrentSeen();
  sentinel::LogFields(sentinel::LogLevel::kInfo, "os_build_gate",
                      {{"answer", "no_number"},
                       {"os_build_read", seen.read},
                       {"os_build", seen.os_build},
                       {"floor", seen.floor},
                       {"game_would_refuse", seen.would_refuse},
                       {"reason", seen.reason}});
  return 0;
}

NEVR_HOOK_RECORD(kHook, Thunk, &Handler);

}  // namespace

const char* ReadingName(Reading reading) noexcept {
  switch (reading) {
    case Reading::kNumber:
      return "number";
    case Reading::kNoProperty:
      return "no_property";
    case Reading::kTooLong:
      return "too_long";
    case Reading::kNotANumber:
      return "not_a_number";
  }
  return "unknown";
}

Reading ParseOsBuild(const char* value, int length, std::uint64_t* out) noexcept {
  if (length <= 0) return Reading::kNoProperty;
  if (length > 20) return Reading::kTooLong;
  const char* p = value;
  while (*p == ' ' || *p == '\t') ++p;
  std::uint64_t base = 10;
  if (p[0] == '0' && (p[1] | 0x20) == 'x') {
    base = 16;
    p += 2;
  } else if (p[0] == '+') {
    p += 1;
  }
  std::uint64_t v = 0;
  int digits = 0;
  for (; *p != '\0'; ++p) {
    const int d = DigitValue(*p);
    if (d < 0 || static_cast<std::uint64_t>(d) >= base) return Reading::kNotANumber;
    v = v * base + static_cast<std::uint64_t>(d);  // wraps at 64 bits, as the game's accumulation does
    ++digits;
  }
  if (digits == 0) return Reading::kNotANumber;
  *out = v;
  return Reading::kNumber;
}

void SetPropertyReaderForTest(PropertyReader reader) noexcept {
  const PropertyReader effective = reader != nullptr ? reader : static_cast<PropertyReader>(&DefaultReader);
  g_reader.store(effective, std::memory_order_release);
}

// The floor from a libr15 image, read only after its build ID is the pinned one. `*source` names the outcome.
std::uint64_t ReadPinnedFloorFrom(const sentinel::ElfImage& image, const char** source) noexcept {
  char id[64] = {};
  if (!sentinel::ReadBuildId(image, id, sizeof(id))) {
    *source = "no_build_id";
    return 0;
  }
  if (std::strcmp(id, sentinel::pinned::LibR15PatchedBuildId()) != 0) {
    *source = "build_id_mismatch";
    return 0;
  }
  std::uint64_t floor = 0;
  std::memcpy(&floor, reinterpret_cast<const unsigned char*>(image.base) + sentinel::pinned::kMinOSBuildVersionVaddr,
              sizeof(floor));
  *source = "pinned";
  return floor;
}

const char* ReadOriginalBodyFrom(const sentinel::ElfImage& image, const char** kind) noexcept {
  char id[64] = {};
  if (!sentinel::ReadBuildId(image, id, sizeof(id))) {
    *kind = "no_build_id";
    return *kind;
  }
  if (std::strcmp(id, sentinel::pinned::LibR15PatchedBuildId()) != 0) {
    *kind = "build_id_mismatch";
    return *kind;
  }
  std::uint32_t word = 0;
  std::memcpy(&word, reinterpret_cast<const unsigned char*>(image.base) + sentinel::pinned::kOSBuildVersionVaddr,
              sizeof(word));
  // sub sp, sp, #0xa0 (the stock body, which queries the property) and mov x0, #0 (the body the community patch writes).
  if (word == 0xd10283ffU) {
    *kind = "queries_property";
  } else if (word == 0xd2800000U) {
    *kind = "returns_zero";
  } else {
    *kind = "other_code";
  }
  return *kind;
}

namespace {

// The floor from the loaded libr15. `*source` names the outcome.
std::uint64_t ReadPinnedFloor(const char** source) noexcept {
  sentinel::ElfImage image;
  if (!sentinel::FindLoadedImage(sentinel::pinned::kLibR15, &image)) {
    *source = "module_not_loaded";
    return 0;
  }
  return ReadPinnedFloorFrom(image, source);
}

// The original's classification from the loaded libr15. `*kind` names the outcome.
const char* ReadOriginalBody(const char** kind) noexcept {
  sentinel::ElfImage image;
  if (!sentinel::FindLoadedImage(sentinel::pinned::kLibR15, &image)) {
    *kind = "module_not_loaded";
    return *kind;
  }
  return ReadOriginalBodyFrom(image, kind);
}

void Arm(std::uint64_t floor) noexcept {
  g_floor.store(floor, std::memory_order_release);
  g_calls.store(0, std::memory_order_relaxed);
  g_read.store(false, std::memory_order_relaxed);
  g_osBuild.store(0, std::memory_order_relaxed);
  g_reason.store(kUnmeasured, std::memory_order_relaxed);
  Thunk::Arm(kHook);
}

}  // namespace

Seen CurrentSeen() noexcept {
  const bool read = g_read.load(std::memory_order_relaxed);
  const std::uint64_t osBuild = g_osBuild.load(std::memory_order_relaxed);
  const std::uint64_t floor = g_floor.load(std::memory_order_acquire);
  const char* reason = g_reason.load(std::memory_order_acquire);
  return {g_calls.load(std::memory_order_acquire), read, osBuild, floor, read && floor != 0 && osBuild < floor, reason};
}

bool InstallAt(const sentinel::GotTarget& target, std::uint64_t floor, const char* floorSource, const char* originalBody,
               sentinel::ImageLookup lookup) noexcept {
  Arm(floor);
  const sentinel::GotStatus status = sentinel::InstallThunk<Thunk>(g_hook, target, lookup);
  const bool ok = status == sentinel::GotStatus::kOk;
  sentinel::LogFields(ok ? sentinel::LogLevel::kInfo : sentinel::LogLevel::kError, "os_build_gate_install",
                      {{"result", ok ? "installed" : "refused"},
                       {"status", sentinel::GotStatusName(status)},
                       {"slot", sentinel::pinned::kOSBuildVersionSymbol},
                       {"floor", floor},
                       {"floor_source", floorSource},
                       {"original_body", originalBody}});
  return ok;
}

bool Install() noexcept {
  const char* source = "unknown";
  const std::uint64_t floor = ReadPinnedFloor(&source);
  const char* kind = "unknown";
  const char* originalBody = ReadOriginalBody(&kind);
  return InstallAt(sentinel::pinned::LibR15OSBuildVersion(), floor, source, originalBody);
}

void ArmForTest(std::uint64_t floor) noexcept { Arm(floor); }

void ResetForTest() noexcept {
  g_floor.store(0, std::memory_order_relaxed);
  g_calls.store(0, std::memory_order_relaxed);
  g_read.store(false, std::memory_order_relaxed);
  g_osBuild.store(0, std::memory_order_relaxed);
  g_reason.store(kUnmeasured, std::memory_order_relaxed);
}

}  // namespace nevr_quest::os_build_gate
