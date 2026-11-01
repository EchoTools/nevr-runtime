#include "wwise_post.hpp"

namespace NRadEngine::AudioPost {

// @0x2a4b500 — PostWwiseEvent
/* @confidence: M — preserves both raw branches; callback wrapper types remain inferred. */
std::uint64_t PostWwiseEvent(std::uint32_t event, std::uint64_t game_object,
                             std::uint32_t flags, SoundEventCallback callback,
                             void* cookie, std::int32_t callback_kind,
                             std::uint64_t callback_user,
                             std::uint32_t playing_id) {
    if (callback_kind == 0) {
        return PostWwiseEventCommand(event, game_object, flags, callback, cookie,
                                     nullptr, playing_id);
    }

    std::array<std::byte, 8> callback_info{};
    std::uint64_t callback_source = 0;
    callback_source = CreateCallbackSource(callback_kind, callback_user);
    InitializeCallbackArgument(
        callback_info.data(), reinterpret_cast<const void*>(callback_source));
    const auto is_empty = CallbackInfoEqualsValue(callback_info.data(), 0);
    std::uint64_t result = 0;
    if (!is_empty) {
        result = PostWwiseEventCommand(event, game_object, flags, callback, cookie,
                                       callback_info.data(), playing_id);
        if (static_cast<std::int32_t>(result) == 0) {
            auto* stored_source = CallbackInfoSource(callback_info.data());
            ReleaseCallbackSource(reinterpret_cast<std::uintptr_t>(stored_source));
            result = 0;
        }
    }
    return result;
}

}  // namespace NRadEngine::AudioPost
