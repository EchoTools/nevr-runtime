#include "wwise_post.hpp"

namespace NRadEngine::AudioPost {

// @0x2649264 — CWwiseEngine::PostSoundEvent
/* @confidence: M — control flow matches the raw body; object and callback ABI types remain inferred. */
int PostSoundEvent(void* self, std::uint32_t event, std::uint32_t game_object,
                   const std::uint32_t* flags, SoundEventCallback callback,
                   void* cookie, std::uint32_t seek_ms, std::int32_t* playing_id) {
    const auto event_object = static_cast<std::uint64_t>(game_object);
    auto result = static_cast<std::int32_t>(
        PostWwiseEvent(event, event_object, *flags, callback, cookie, 0, 0, 0));
    *playing_id = result;

    if (result == 0) {
        if (*reinterpret_cast<const std::int64_t*>(
                static_cast<const std::byte*>(self) + 0x80) > 0) {
            auto* task_group = GetCurrentTaskGroup(DAT_03864210);
            if (*reinterpret_cast<const std::int64_t*>(
                    static_cast<const std::byte*>(self) + 0x80) > 0) {
                if (task_group == nullptr) {
                    do {
                    } while (*reinterpret_cast<const std::int64_t*>(
                                 static_cast<const std::byte*>(self) + 0x80) > 0);
                } else {
                    do {
                        ExecuteTask(task_group, 0);
                    } while (*reinterpret_cast<const std::int64_t*>(
                                 static_cast<const std::byte*>(self) + 0x80) > 0);
                }
            }
            result = static_cast<std::int32_t>(
                PostWwiseEvent(event, event_object, *flags, callback, cookie, 0, 0, 0));
            *playing_id = result;
            if (result != 0) {
                goto seek;
            }
        }

        *playing_id = -1;
        alignas(8) std::byte suppress_error_storage[8]{};
        InitializeSuppressErrorBreakSelect(suppress_error_storage, 1);
        const auto* event_name = SoundSymbolString(event);
        if (event_name == nullptr) {
            result = CreateAndAddError(
                "d:\\projects\\rad\\dev\\src\\engine\\libs\\audio\\wwise\\cwwiseutils.cpp",
                0x51, 0xf986c58c7b3d4fbULL,
                "Error %s. It is not a valid wwise identifier or registered sound symbol. Either the debug data for sound symbols id not loaded (like in a final build) or someone is doing something very shady with sound symbols.",
                "playing sound");
            DestroySuppressErrorBreakSelect(suppress_error_storage);
        } else {
            result = CreateAndAddError(
                "d:\\projects\\rad\\dev\\src\\engine\\libs\\audio\\wwise\\cwwiseutils.cpp",
                0x4a, 0xf986c58c7b3d4fbULL,
                "Error %s '%s'. Make sure that there is a %s with that name in wwise and that its bank is loaded.",
                "playing sound", event_name, "event");
            DestroySuppressErrorBreakSelect(suppress_error_storage);
        }
        if (result != 0) {
            return result;
        }
    }

seek:
    if (seek_ms == 0) {
        return 0;
    }

    const auto seek_result = QueueWwiseSeek(event, event_object, seek_ms, 0, 0);
    if (seek_result == 1) {
        return 0;
    }
    const auto first_error = TranslateWwiseResult(
        "Wwise error", seek_result,
        "d:\\projects\\rad\\dev/src/engine/libs\\audio/wwise/wwiseresulttranslator.h",
        0x28);
    OutputError(first_error, 8);
    return TranslateWwiseResult(
        "res", seek_result,
        "d:\\projects\\rad\\dev\\src\\engine\\libs\\audio\\wwise\\cwwiseengine.cpp",
        0x2a0);
}

}  // namespace NRadEngine::AudioPost
