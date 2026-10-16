#pragma once
// Self-check "party_member_leave" (#403, docs/engine/remote-log.md): placeholder, filled in by the next commit.

#include <cstddef>
#include <cstdint>

namespace nevr_party_leave_check {

inline void OnServerMessage(std::uint64_t, const std::uint8_t*, std::size_t) {}
inline void ResetForTest() {}

}  // namespace nevr_party_leave_check
