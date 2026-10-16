#pragma once
// Self-check "friend_request" (#405, docs/engine/remote-log.md): placeholder, filled in by the next commit.

#include <cstddef>
#include <cstdint>

namespace nevr_friend_request_check {

inline void OnServerMessage(std::uint64_t, const std::uint8_t*, std::size_t) {}
inline void ResetForTest() {}

}  // namespace nevr_friend_request_check
