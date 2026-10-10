// Process-stable C-string storage shared by PCVR and Quest adapters.
#pragma once

#include <cstddef>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <string_view>

namespace nevr_runtime::lifecycle {

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

}  // namespace nevr_runtime::lifecycle
