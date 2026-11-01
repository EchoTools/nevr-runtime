#include "wwise_post.hpp"

namespace NRadEngine::AudioPost {

// @0x2a4b910 — FindWwiseEventSymbol
/* @confidence: M — preserves the locked lookup and reference acquisition. */
void* FindWwiseEventSymbol(std::uintptr_t registry, std::uint32_t event) {
    std::array<std::byte, 8> lookup_scope{};
    BeginWwiseSymbolLookup(lookup_scope.data(), registry);
    auto* symbol = LookupWwiseSymbol(registry + 0x28, event);
    if (symbol != nullptr) {
        AcquireWwiseSymbolReference(symbol);
    }
    EndWwiseSymbolLookup(lookup_scope.data());
    return symbol;
}

}  // namespace NRadEngine::AudioPost
