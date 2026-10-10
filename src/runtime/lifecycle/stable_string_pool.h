// Process-stable C-string storage for the config-string accessors. The PCVR runtime
// links it; the Quest build compiles it into its test target only.
//
// Storage and lifetime:
// - InternStableCStr stores each distinct value once and returns a NUL-terminated,
//   immutable pointer. Equal values return the same pointer and consume no quota.
// - The owner of the process-wide pool is heap-allocated and never destroyed. No
//   DSO or static destructor reclaims it, so a returned pointer stays readable
//   through module unload until process exit. Game code may hold these pointers
//   for as long as it likes, so nothing in the pool is ever freed.
// - Nothing clears or shrinks the pool, including on detach. Secret-bearing
//   configured strings (the game-native config JSON carries the server key)
//   therefore stay resident until process exit; their values are never logged.
// - Reloading the owning module after a pointer has been published is not
//   supported. The failure path terminates through ForceFatalExit and Log, which
//   the Quest build does not provide yet (issue #158, tranche 1c).
// - A value containing an embedded NUL is rejected rather than truncated.
//
// Limits (kStableStringMaxCount, kStableStringMaxPayloadBytes, kStablePoolMaxBytes):
// - at most 1024 distinct strings;
// - at most 1 MiB of payload per string, counted without its terminating NUL;
// - at most 16 MiB of live string bytes in total, counting every terminating NUL;
// - set-node and allocator overhead are not counted.
// Each module that compiles this source owns its own pool and quota, and a module
// reload creates a new pool: the limits are per loaded module, not per process.
//
// Failure: exceeding a limit or failing to allocate returns a non-success
// InternStatus and publishes nothing; no escaped value is ever evicted. The
// C accessors in service_config.cpp treat any such status as terminal (they log
// the status name and the pool counts, then call ForceFatalExit) because a null
// return already means "absent" to their callers.
#pragma once

#include <cstddef>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <string_view>

namespace nevr::lifecycle {

inline constexpr std::size_t kStableStringMaxCount = 1024;
inline constexpr std::size_t kStableStringMaxPayloadBytes = 1024 * 1024;
inline constexpr std::size_t kStablePoolMaxBytes = 16 * 1024 * 1024;

enum class InternStatus {
  kSuccess,
  kEmbeddedNul,
  kStringTooLong,
  kPoolCountExceeded,
  kPoolBytesExceeded,
  kAllocationFailure,
};

struct InternResult {
  InternStatus status;
  const char* pointer;
  std::size_t stringCount;
  std::size_t liveBytes;
};

struct StableStringPoolLimits {
  std::size_t maxCount = kStableStringMaxCount;
  std::size_t maxStringPayloadBytes = kStableStringMaxPayloadBytes;
  std::size_t maxLiveBytes = kStablePoolMaxBytes;
};

// Optional allocator hook used to inject std::bad_alloc from the set node
// allocation boundary in tests. Production pools leave it unset.
using AllocationProbe = void (*)(void* context);

template <typename T>
class StableStringPoolAllocator {
 public:
  using value_type = T;

  explicit StableStringPoolAllocator(AllocationProbe probe = nullptr, void* context = nullptr) noexcept
      : probe_(probe), context_(context) {}

  template <typename U>
  StableStringPoolAllocator(const StableStringPoolAllocator<U>& other) noexcept
      : probe_(other.probe()), context_(other.context()) {}

  T* allocate(std::size_t count) {
    if (probe_ != nullptr) probe_(context_);
    return std::allocator<T>{}.allocate(count);
  }

  void deallocate(T* pointer, std::size_t count) noexcept {
    std::allocator<T>{}.deallocate(pointer, count);
  }

  AllocationProbe probe() const noexcept { return probe_; }
  void* context() const noexcept { return context_; }

 private:
  template <typename>
  friend class StableStringPoolAllocator;

  AllocationProbe probe_;
  void* context_;
};

template <typename T, typename U>
bool operator==(const StableStringPoolAllocator<T>& left,
                const StableStringPoolAllocator<U>& right) noexcept {
  return left.probe() == right.probe() && left.context() == right.context();
}

template <typename T, typename U>
bool operator!=(const StableStringPoolAllocator<T>& left,
                const StableStringPoolAllocator<U>& right) noexcept {
  return !(left == right);
}

class StableStringPool {
 public:
  explicit StableStringPool(StableStringPoolLimits limits = {}, AllocationProbe probe = nullptr,
                            void* probeContext = nullptr);

  InternResult Intern(std::string_view value);

 private:
  using Values = std::set<std::string, std::less<>, StableStringPoolAllocator<std::string>>;

  const StableStringPoolLimits limits_;
  std::mutex mutex_;
  Values values_;
  std::size_t liveBytes_ = 0;
};

/// Intern a value in this module generation's process-lifetime pool. Its heap
/// owner is deliberately leaked so unloading this module never reclaims bytes
/// already returned to game or module callers.
InternResult InternStableCStr(std::string_view value);

const char* InternStatusName(InternStatus status) noexcept;

}  // namespace nevr::lifecycle
