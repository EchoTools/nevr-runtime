#include "wwise_post.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace NRadEngine::AudioPost {

// @0x2a4b5e0 — CreateCallbackSource
/* @confidence: M — copies each 32-byte callback record and deep-copies its optional string; allocator boundary is inferred. */
std::uint64_t CreateCallbackSource(std::int32_t count, std::uint64_t records) {
    const auto record_count = static_cast<std::uint32_t>(count);
    const auto byte_count =
        static_cast<std::size_t>((record_count << 5U) | 8U);
    auto* source = static_cast<std::byte*>(std::malloc(byte_count));
    if (source == nullptr) {
        return 0;
    }

    *reinterpret_cast<std::int32_t*>(source) = 1;
    *reinterpret_cast<std::uint32_t*>(source + 4) = record_count;
    const auto* input = reinterpret_cast<const std::uint64_t*>(records);
    auto* output = reinterpret_cast<std::uint64_t*>(source + 8);
    for (std::uint32_t i = 0; i < record_count; ++i) {
        const auto* input_record = input + i * 4U;
        auto* output_record = output + i * 4U;
        output_record[0] = input_record[0];
        output_record[1] = input_record[1];
        output_record[2] = input_record[2];
        output_record[3] = input_record[3];
        if (input_record[1] != 0) {
            const auto* text = reinterpret_cast<const char*>(input_record[1]);
            const auto text_size = std::strlen(text) + 1U;
            auto* copied_text = static_cast<char*>(std::malloc(text_size));
            if (copied_text == nullptr) {
                *reinterpret_cast<std::uint32_t*>(source + 4) = i;
                ReleaseCallbackSource(reinterpret_cast<std::uintptr_t>(source));
                return 0;
            }
            std::memcpy(copied_text, text, text_size);
            output_record[1] = reinterpret_cast<std::uintptr_t>(copied_text);
        }
    }
    return reinterpret_cast<std::uintptr_t>(source);
}

}  // namespace NRadEngine::AudioPost
