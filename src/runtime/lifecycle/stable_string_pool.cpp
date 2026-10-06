#include "runtime/lifecycle/stable_string_pool.h"

#include <limits>
#include <new>
#include <string>

namespace nevr_runtime::lifecycle {
namespace {

InternResult Result(InternStatus status, const char* pointer, std::size_t count, std::size_t bytes) {
  return {status, pointer, count, bytes};
}

}  // namespace

StableStringPool::StableStringPool(StableStringPoolLimits limits, AllocationProbe probe, void* probeContext)
    : limits_(limits), values_(std::less<>{}, StableStringPoolAllocator<std::string>{probe, probeContext}) {}

InternResult StableStringPool::Intern(std::string_view value) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (value.find('\0') != std::string_view::npos) {
    return Result(InternStatus::kEmbeddedNul, nullptr, values_.size(), liveBytes_);
  }

  const auto existing = values_.find(value);
  if (existing != values_.end()) {
    return Result(InternStatus::kSuccess, existing->c_str(), values_.size(), liveBytes_);
  }

  if (value.size() > limits_.maxStringPayloadBytes || value.size() == std::numeric_limits<std::size_t>::max()) {
    return Result(InternStatus::kStringTooLong, nullptr, values_.size(), liveBytes_);
  }
  const std::size_t bytes = value.size() + 1;  // include the published NUL terminator
  if (values_.size() >= limits_.maxCount) {
    return Result(InternStatus::kPoolCountExceeded, nullptr, values_.size(), liveBytes_);
  }
  if (bytes > limits_.maxLiveBytes || liveBytes_ > limits_.maxLiveBytes - bytes) {
    return Result(InternStatus::kPoolBytesExceeded, nullptr, values_.size(), liveBytes_);
  }
  try {
    const auto [insertedAt, inserted] = values_.emplace(value);
    if (!inserted) {
      return Result(InternStatus::kSuccess, insertedAt->c_str(), values_.size(), liveBytes_);
    }
    liveBytes_ += bytes;
    return Result(InternStatus::kSuccess, insertedAt->c_str(), values_.size(), liveBytes_);
  } catch (const std::bad_alloc&) {
    return Result(InternStatus::kAllocationFailure, nullptr, values_.size(), liveBytes_);
  }
}

InternResult InternStableCStr(std::string_view value) {
  try {
    // This pointer is intentionally never deleted: pointers returned from this
    // module may outlive a DSO unload and must remain readable until process exit.
    static StableStringPool* const pool = new StableStringPool();
    return pool->Intern(value);
  } catch (const std::bad_alloc&) {
    return Result(InternStatus::kAllocationFailure, nullptr, 0, 0);
  }
}

const char* InternStatusName(InternStatus status) noexcept {
  switch (status) {
    case InternStatus::kSuccess: return "success";
    case InternStatus::kEmbeddedNul: return "embedded_nul";
    case InternStatus::kStringTooLong: return "string_limit";
    case InternStatus::kPoolCountExceeded: return "count_limit";
    case InternStatus::kPoolBytesExceeded: return "pool_bytes_limit";
    case InternStatus::kAllocationFailure: return "allocation_failure";
  }
  return "unknown";
}

}  // namespace nevr_runtime::lifecycle
