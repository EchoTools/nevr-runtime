#pragma once
// What libr15 asks the system for (#335, part B): the GOT hooks in hwdump_hooks.cpp record each distinct
// (function, id or name) query with its call count and its first and last answer.
//
// Safe on the game's call path: no allocation, no lock, no exception, no logging (the hook frames must stay
// personality-free, callback_thunk.h). The table is a fixed array with constant initialisation, so it adds
// no .init_array entry (tools/check_quest_static_init.sh). A full table counts the overflow and drops the
// record; the call itself is never affected.

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace nevr_quest::hwdump {

enum class QueryFn : std::uint8_t {
  kSystemPropertyGet,
  kSysconf,
  kUname,
  kGethostname,
  kConfigGetLanguage,
  kConfigGetCountry,
  kVrapiInitialize,
  kVrapiGetSystemPropertyInt,
  kVrapiGetSystemPropertyFloat,
  kVrapiGetSystemPropertyFloatArray,
  kVrapiGetSystemStatusInt,
  kVrapiGetPropertyInt,
  kVrapiSetPropertyInt,
  kVrapiSetDisplayRefreshRate,
  kVrapiSetClockLevels,
  kVrapiSetExtraLatencyMode,
  kVrapiGetInstanceExtensionsVulkan,
  kVrapiGetDeviceExtensionsVulkan,
  kCount,
};

// The import name, for the dump.
const char* QueryFnName(QueryFn fn) noexcept;

inline constexpr std::size_t kNameBytes = 96;
inline constexpr std::size_t kTextBytes = 200;
inline constexpr std::size_t kMaxInts = 8;
inline constexpr std::size_t kMaxFloats = 16;

// One answer. Each hook fills the parts its function returns.
struct Answer {
  char text[kTextBytes] = {};  // NUL-terminated; e.g. a property value or a utsname field list
  bool has_text = false;
  bool text_truncated = false;
  std::int64_t ints[kMaxInts] = {};
  std::uint8_t num_ints = 0;
  float floats[kMaxFloats] = {};
  std::uint8_t num_floats = 0;
};

// Copies `s` into `out` (bounded, always NUL-terminated); sets has_text and text_truncated.
void SetText(Answer* out, const char* s) noexcept;
void AddInt(Answer* out, std::int64_t v) noexcept;
void AddFloat(Answer* out, float v) noexcept;

struct RecordSnapshot {
  QueryFn fn = QueryFn::kCount;
  std::int64_t id = 0;
  char name[kNameBytes] = {};
  bool name_truncated = false;
  std::uint64_t calls = 0;
  Answer first;
  Answer last;
  bool last_current = true;  // false when writers held `last` for every attempt to copy it
};

inline constexpr std::size_t kMaxRecords = 128;

class RecordTable {
 public:
  constexpr RecordTable() = default;
  RecordTable(const RecordTable&) = delete;
  RecordTable& operator=(const RecordTable&) = delete;

  // Records one call: finds or claims the entry for (fn, id, name) and updates it. `name` may be null.
  void Record(QueryFn fn, std::int64_t id, const char* name, const Answer& answer) noexcept;

  // Copies the ready entries into `out` (at most `capacity`); returns how many were copied.
  std::size_t Snapshot(RecordSnapshot* out, std::size_t capacity) const noexcept;

  std::uint64_t overflow() const noexcept { return overflow_.load(std::memory_order_relaxed); }
  std::uint64_t contended() const noexcept { return contended_.load(std::memory_order_relaxed); }

 private:
  enum : std::uint32_t { kEmpty = 0, kClaiming = 1, kReady = 2 };
  struct Entry {
    std::atomic<std::uint32_t> state{kEmpty};
    QueryFn fn = QueryFn::kCount;
    std::int64_t id = 0;
    char name[kNameBytes] = {};
    bool name_truncated = false;
    std::atomic<std::uint64_t> calls{0};
    Answer first;
    // `last` is only touched while holding `busy`: a writer that finds it held skips the update (the call
    // is still counted), and the reader retries a bounded number of times.
    mutable std::atomic<bool> busy{false};
    Answer last;
  };

  Entry entries_[kMaxRecords];
  std::atomic<std::uint64_t> overflow_{0};
  std::atomic<std::uint64_t> contended_{0};
};

// The process-wide table the hooks write and the dump reads.
RecordTable& Records() noexcept;

}  // namespace nevr_quest::hwdump
