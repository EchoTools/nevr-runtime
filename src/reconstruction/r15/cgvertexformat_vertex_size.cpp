// NRadEngine::CGVertexFormat::VertexSize
// Quest libr15.so VA 0x2909538; ReVault binary 576.
// ReVault raw parent 100330670, reconstruction node 100600765; grade H: all 77 instructions (0x2909538-0x2909668) decoded and control flow traced; 0 callees, 0 globals, 0 placeholders; element bytes +1/+5/+6 kept raw.
// @0x2909538 — NRadEngine::CGVertexFormat::VertexSize
/* @confidence: H */
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace NRadEngine {

// Element records are 8 bytes, record r at storage + 8 * r. VertexSize reads bytes +1, +5 and +6 of
// each record and the 64-bit element count at +0x120; every other byte is left raw.
struct alignas(8) CGVertexFormat {
    unsigned char storage[0x128];

    // 0x2909538 (thunk 0x1036240). The argument is matched against byte +6 of each record.
    std::uint64_t VertexSize(std::uint64_t stream) const;
};
static_assert(sizeof(CGVertexFormat) == 0x128, "CGVertexFormat is 0x128 bytes");
static_assert(offsetof(CGVertexFormat, storage) == 0, "storage starts the object");
static_assert(0x120 + sizeof(std::uint64_t) == sizeof(CGVertexFormat), "the element count is the last word");

// Candidate of a record: byte +1 plus byte +5. A record matches when its byte +6 equals the argument.
// The pair loop keeps two maxima (x8 for even records, x11 for odd records) and merges them; an odd
// count's last record is handled by the tail loop after the pairs.
std::uint64_t CGVertexFormat::VertexSize(std::uint64_t stream) const {
    // 0x2909538: ldr x9, [x0, #0x120]
    std::uint64_t count = 0;
    std::memcpy(&count, storage + 0x120, sizeof count);

    const auto candidate = [this](std::uint64_t r) -> std::uint64_t {
        return std::uint64_t{storage[8 * r + 1]} + std::uint64_t{storage[8 * r + 5]};
    };
    // 64-bit compare (cmp x15, x1): a stream above 255 matches no record.
    const auto matches = [this, stream](std::uint64_t r) -> bool {
        return std::uint64_t{storage[8 * r + 6]} == stream;
    };

    // 0x290953c: count == 0 returns 0.
    if (count == 0) {
        return 0;  // 0x2909554..0x2909558
    }
    std::uint64_t best = 0;      // x8
    std::uint64_t best_odd = 0;  // x11
    std::uint64_t paired = 0;    // x10: records covered by the pair loop
    // 0x2909540..0x2909550: count == 1 skips the pair loop and goes straight to the tail.
    if (count != 1) {
        // 0x290955c: x10 = count & ~1
        paired = count & ~std::uint64_t{1};
        // 0x2909584..0x29095a8: pair loop; record r (even) feeds x8, record r + 1 feeds x11.
        for (std::uint64_t r = 0; r < paired; r += 2) {
            if (matches(r)) {
                best = std::max(best, candidate(r));
            }
            if (matches(r + 1)) {
                best_odd = std::max(best_odd, candidate(r + 1));
            }
        }
        // 0x2909608..0x290960c: x8 = max(x8, x11)
        best = std::max(best, best_odd);
        // 0x2909610..0x2909614: count == paired returns x8.
        if (count == paired) {
            return best;  // 0x2909664..0x2909668
        }
    }
    // 0x2909618..0x2909660: tail records paired .. count - 1, one at a time.
    for (std::uint64_t r = paired; r < count; ++r) {
        if (matches(r)) {
            best = std::max(best, candidate(r));
        }
    }
    return best;  // 0x2909664..0x2909668
}

}  // namespace NRadEngine
