#include "wwise_post.hpp"

namespace NRadEngine::AudioPost {

// @0x2a4b6d0 — PostWwiseEventCommand
/* @confidence: M — preserves command fields and branches; engine callback types remain inferred. */
std::uint32_t PostWwiseEventCommand(std::uint32_t event, std::uint64_t game_object,
                                    std::uint32_t flags, SoundEventCallback callback,
                                    void* cookie, const void* callback_info,
                                    std::uint32_t playing_id) {
    auto* symbol = FindWwiseEventSymbol(DAT_038cec70 + 0xc0, event);
    if (symbol == nullptr) {
        return 0;
    }

    std::array<std::byte, 8> command_scope{};
    BeginAudioCommandScope(command_scope.data(), 1, WwisePostCommandSize());
    auto* command = GetAudioCommandScope(command_scope.data());
    StoreAudioCommandReference(command + 0x28, symbol);
    command = GetAudioCommandScope(command_scope.data());
    *reinterpret_cast<std::uint32_t*>(command + 0x30) = event;

    command = GetAudioCommandScope(command_scope.data());
    if (callback_info == nullptr) {
        *reinterpret_cast<std::uint64_t*>(command + 0x1c) = 0;
        command = GetAudioCommandScope(command_scope.data());
        *reinterpret_cast<std::uint32_t*>(command + 0x24) = 0;
        command = GetAudioCommandScope(command_scope.data());
        InitializeCallbackArgument(command + 0x14, nullptr);
    } else {
        const auto* callback_words = static_cast<const std::uint64_t*>(callback_info);
        *reinterpret_cast<std::uint32_t*>(command + 0x24) =
            *reinterpret_cast<const std::uint32_t*>(callback_words + 2);
        *reinterpret_cast<std::uint64_t*>(command + 0x1c) = callback_words[1];
        *reinterpret_cast<std::uint64_t*>(command + 0x14) = callback_words[0];
    }

    const auto command_id = NextWwiseCommandId(&DAT_038cee60);
    command = GetAudioCommandScope(command_scope.data());
    *reinterpret_cast<std::uint32_t*>(command + 0x0c) = command_id;
    command = GetAudioCommandScope(command_scope.data());
    *reinterpret_cast<std::uint32_t*>(command + 0x10) = playing_id;
    command = GetAudioCommandScope(command_scope.data());
    *reinterpret_cast<std::uint64_t*>(command + 0x04) = game_object;
    command = GetAudioCommandScope(command_scope.data());
    auto* reference = command + 0x28;
    auto* symbol_reference = LoadAudioCommandPointer(
        reinterpret_cast<void* const*>(reference));
    const auto thread_token =
        LoadAudioCommandWordAtOffset10(symbol_reference);
    const auto result = SubmitWwisePostCommand(
        DAT_038cecc0, command + 0x04, callback, cookie, flags,
        thread_token);
    if (result == 1) {
        command = GetAudioCommandScope(command_scope.data());
        const auto submitted_id =
            *reinterpret_cast<std::uint32_t*>(command + 0x0c);
        CommitAudioCommandScope(command_scope.data());
        return submitted_id;
    }

    using ReleaseSymbol = void (*)(void*);
    auto** symbol_vtable = *reinterpret_cast<void***>(symbol);
    reinterpret_cast<ReleaseSymbol>(symbol_vtable[3])(symbol);
    command = GetAudioCommandScope(command_scope.data());
    *reinterpret_cast<std::uint16_t*>(command + 0x02) = 0x3a;
    CommitAudioCommandScope(command_scope.data());
    return 0;
}

}  // namespace NRadEngine::AudioPost
