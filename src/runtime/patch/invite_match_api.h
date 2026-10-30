#pragma once
// #966 S4: the frame path's view of the invite service, with no worker type and no exception path, so the Quest's hook-side
// translation unit can include it. Implemented in invite_service.cpp (the exception TU), on top of the service's answers.
#include <cstddef>

namespace nevr_invite_match {

/// The longest match id the frame path takes. Longer ids are refused (taken as empty, so the join logs them as an invalid id).
inline constexpr std::size_t kMaxMatchIdBytes = 63;

/// True while the invite service is started (the network.invite_codes feature, nevr_invite_service::Active).
bool ServiceActive() noexcept;

/// Takes the next resolved match id the service answered with (a Resolve that succeeded and parsed). The text is copied to `out`
/// (not NUL-terminated) and its length set in `length`; an empty id is returned as length 0 and still counts as one answer.
/// False when no resolved answer is waiting. Called on the game thread once per frame.
bool TakeResolvedMatch(char* out, std::size_t capacity, std::size_t* length) noexcept;

}  // namespace nevr_invite_match
