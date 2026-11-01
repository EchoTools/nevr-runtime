// @0x13fe8c4 — NRadEngine::NRadGame::NR15RestAPI::SAPIWrapper<SSessionRequest>::Execute
/* @confidence: M
 * The wrapper's dispatch conditions and request/response effects are reconstructed.
 * Project types are represented by the operations interface below; context field
 * offsets and the five forwarded context words are taken from the Quest raw body.
 */
#include <cstddef>
#include <cstdint>

struct SAPIWrapperOps {
    using JsonHandle = void*;
    using MemoryHandle = void*;

    virtual void* current_allocator();
    virtual MemoryHandle make_mem_block(std::size_t initial_size,
                                        std::size_t reserve,
                                        void* allocator);
    virtual JsonHandle make_json();
    virtual int request_body(void* request, JsonHandle body);
    virtual int extract_error_message(int body_result, bool include_offset,
                                      bool include_context, MemoryHandle output);
    virtual void append_text(MemoryHandle output, const char* text);
    virtual void append_block(MemoryHandle output, MemoryHandle text);
    virtual std::uint64_t game_type_hash(const void* game);
    virtual void session_request_execute(const void* context_words,
                                         JsonHandle body,
                                         JsonHandle response);
    virtual void json_set_int(JsonHandle response, std::uint32_t value);
    virtual void json_set_string(JsonHandle response, const char* value);
    virtual void reply_with_json(void* request, JsonHandle response);
};

struct ForwardedSessionContext {
    const void* words[5];
};

void SAPIWrapperSSessionRequestExecute(void* request,
                                       void* context,
                                       SAPIWrapperOps& ops) {
    void* allocator = ops.current_allocator();
    auto errorMessage = ops.make_mem_block(0, 0, allocator);
    auto response = ops.make_json();

    const auto* contextBytes = static_cast<const std::byte*>(context);
    const auto* api = *reinterpret_cast<const std::byte* const*>(contextBytes + 0x08);
    const int requestKind = *reinterpret_cast<const int*>(contextBytes);
    const auto* game = *reinterpret_cast<const void* const*>(contextBytes + 0x40);
    const bool featureEnabled =
        ((*reinterpret_cast<const std::uint8_t*>(api + 0x8c09) >> 3) & 1) != 0;
    bool delegatedRequest = false;

    if (featureEnabled && requestKind == 9) {
        const auto gameType = game != nullptr ? ops.game_type_hash(game) : 0;
        const bool restrictedSocialType =
            game != nullptr &&
            (gameType == 0x42d9cf9cfddcf76ULL ||
             gameType == 0x305d6e37c1589c45ULL ||
             gameType == 0x163956e6e431e617ULL);

        if (!restrictedSocialType) {
            delegatedRequest = true;
            auto body = ops.make_json();
            const int bodyResult = ops.request_body(request, body);
            if (bodyResult == 0) {
                // Raw stack layout passed to SSessionRequest::Execute:
                // +0x00=context+0x2b08, +0x08=context+0x2b10,
                // +0x10=*(context+0x08), +0x18=context, +0x20=context+0x2aa8.
                const ForwardedSessionContext forwarded{{
                    *reinterpret_cast<const void* const*>(contextBytes + 0x2b08),
                    *reinterpret_cast<const void* const*>(contextBytes + 0x2b10),
                    api,
                    context,
                    *reinterpret_cast<const void* const*>(contextBytes + 0x2aa8),
                }};
                ops.session_request_execute(forwarded.words, body, response);
            } else {
                auto parseText = ops.make_mem_block(0, 0, ops.current_allocator());
                if (ops.extract_error_message(bodyResult, true, true, parseText) != 0) {
                    ops.append_text(errorMessage,
                                    "Parsing error on json body parameters: ");
                    ops.append_block(errorMessage, parseText);
                }
                // The raw body constructs this diagnostic without adding it to the JSON.
            }
        }
    }

    // Accepted /session requests return delegated JSON with the status field.
    // Wrapper-level refusals additionally set err_description.
    ops.json_set_int(response, 0x02cb20fc);
    if (!delegatedRequest) {
        ops.json_set_string(response, "err_description");
    }
    ops.reply_with_json(request, response);
}
