#include "runtime/combat/write_gate.h"

#include <cstdint>

#include "core/logging.h"
#include "runtime/hook/hook_guard.h"
#include "runtime/hook/write_ranges.h"

namespace nevr_combat {

bool GateWrite(const void* start, std::size_t length, const char* what) {
  const char* const name = what != nullptr ? what : "(unnamed)";
  const auto va = static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(start));
  nevr_hook_guard::Overlap hit{};
  if (nevr_hook_guard::FindOverlap(start, length, &hit)) {
    Log(EchoVR::LogLevel::Warning,
        "[NEVR.COMBAT] overlap refused what=%s va=0x%llX len=%llu owner=%s owner_kind=%s owner_va=0x%llX "
        "owner_len=%llu (the runtime owns that range; the write is not made and its feature stays off)",
        name, va, static_cast<unsigned long long>(length), hit.name != nullptr ? hit.name : "(unnamed)", hit.kind,
        static_cast<unsigned long long>(hit.start), static_cast<unsigned long long>(hit.length));
    return false;
  }
  const int dropped = nevr_write_ranges::DroppedCount() + nevr_hook_guard::DroppedCount();
  if (dropped > 0) {
    // The registry is full: what it holds was checked, what it dropped was not. Said with every write it lets through.
    Log(EchoVR::LogLevel::Warning,
        "[NEVR.COMBAT] overlap check incomplete what=%s va=0x%llX dropped_records=%d (the registry was full; ranges "
        "it dropped were not checked)",
        name, va, dropped);
  }
  return true;
}

}  // namespace nevr_combat
