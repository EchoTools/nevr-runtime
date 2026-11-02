// NRadEngine::CGVertexFormat::AppendElementWithAlias
// Quest libr15.so VA 0x290a694; ReVault binary 576.
// ReVault raw parent 100330680, reconstruction node 100600767; grade H: all 131 instructions (0x290a694-0x290a89c, the only ret at 0x290a868) checked against the listing; the function makes no call and reads no global, so 0 placeholders; the object is raw bytes (elements at +0, count at +0x120, size 0x128 from the merged tree) because ReVault holds no CGVertexFormat layout.
// @0x290a694 — NRadEngine::CGVertexFormat::AppendElementWithAlias
/* @confidence: H */

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace NRadEngine {

class CGVertexFormat {
public:
    // Enumerators are not recovered; the listing passes both as 32-bit values (w1, w2, w3).
    enum EUsage : std::uint32_t {};
    enum EType : std::uint32_t {};

    // NRadEngine::CGVertexFormat::AppendElementWithAlias(EUsage, EUsage, EType, unsigned long long,
    // unsigned long long, unsigned long long, unsigned int) @ 0x290a694 (thunk 0x1008350).
    // Entry registers: x0 this, w1 usage, w2 alias_usage, w3 type, x4 arg4, x5 arg5, x6 arg6, w7 arg7.
    // The parameter names arg4..arg7 are not recovered; their roles are in the body.
    void AppendElementWithAlias(EUsage usage, EUsage alias_usage, EType type, std::uint64_t arg4,
                                std::uint64_t arg5, std::uint64_t arg6, std::uint32_t arg7);

private:
    // 0x128 bytes. +0x000..+0x11f: element slots of 8 bytes each (the constructor at 0x2909098
    // initialises 36 of them); +0x120: the element count (uint64), stored as zero by the constructor.
    alignas(8) std::byte bytes[0x128];
};
static_assert(sizeof(CGVertexFormat) == 0x128, "CGVertexFormat is 0x128 bytes");

namespace {

constexpr std::size_t kCountOffset = 0x120;  // ldr x0, [x0, #0x120]: the element count
constexpr std::size_t kElementSize = 8;      // slots are indexed with LSL #3

template <typename T>
T ReadAt(const std::byte* base, std::size_t offset) {
    T value;
    std::memcpy(&value, base + offset, sizeof value);
    return value;
}

template <typename T>
void WriteAt(std::byte* base, std::size_t offset, T value) {
    std::memcpy(base + offset, &value, sizeof value);
}

std::uint64_t ByteAt(const std::byte* base, std::size_t offset) {
    return std::to_integer<std::uint64_t>(base[offset]);
}

}  // namespace

// @0x290a694 — NRadEngine::CGVertexFormat::AppendElementWithAlias
/* @confidence: H */
void CGVertexFormat::AppendElementWithAlias(EUsage usage, EUsage alias_usage, EType type,
                                            std::uint64_t arg4, std::uint64_t arg5,
                                            std::uint64_t arg6, std::uint32_t arg7) {
    const std::uint64_t count = ReadAt<std::uint64_t>(bytes, kCountOffset);  // ldr x0, [x0, #0x120]
    const std::uint32_t type_value = type;

    // Element layout (byte n of the 64-bit slot), from the stores at 0x290a7ec-0x290a818:
    //   byte 0 usage            (or alias_usage for the second slot)
    //   byte 1 start offset     computed below
    //   byte 2 type
    //   byte 3 arg4 (low byte)
    //   byte 4 arg5 (low byte)
    //   byte 5 size             computed from type and arg4 below
    //   byte 6 arg6 (low byte)  the value the existing slots are matched on
    //   byte 7 arg7 (low byte)  raised to at least 1 when usage is 10

    // 0x290a6a4-0x290a7c0: the listing unrolls this by two (separate maxima for even and odd slots,
    // merged at 0x290a768) and finishes an odd count with the loop at 0x290a784. It is one maximum
    // over the slots whose byte 6 equals arg6 of (byte 1 + byte 5); it starts at 0, which is the
    // result for a count of 0 (cbz at 0x290a6a4).
    std::uint32_t end = 0;
    for (std::uint64_t index = 0; index < count; ++index) {
        const std::size_t slot = static_cast<std::size_t>(index) * kElementSize;
        if (ByteAt(bytes, slot + 6) == arg6) {
            const std::uint32_t slot_end =
                static_cast<std::uint32_t>(ByteAt(bytes, slot + 1) + ByteAt(bytes, slot + 5));
            if (end < slot_end) {
                end = slot_end;
            }
        }
    }

    // 0x290a7c4-0x290a7d0: cmp w7, #1 / csinc w10, w7, wzr, gt (signed), then cmp w1, #0xa / csel.
    std::uint32_t byte7 = arg7;
    if (usage == 10 && static_cast<std::int32_t>(arg7) < 2) {
        byte7 = 1;
    }

    // 0x290a7d4-0x290a89c: size from the type (w3 - 2 unsigned below 5, 7, 8, else w3 - 9 below 2).
    std::uint64_t size = arg4;
    if (type_value >= 2) {
        if (type_value - 2 < 5) {
            size = arg4 << 1;
        } else if (type_value == 7) {
            size = 4;
        } else if (type_value == 8) {
            size = arg4 << 2;
        } else {
            size = std::uint64_t{type_value - 9 < 2} << 2;
        }
    }

    // 0x290a7ec-0x290a810: the slot without byte 0; each bfm inserts the low byte of its source.
    const std::uint64_t element = (arg6 & 0xff) << 48 | std::uint64_t{byte7} << 56 |
                                  std::uint64_t{(type_value & 0xff) << 16} | (arg4 & 0xff) << 24 |
                                  (arg5 & 0xff) << 32 | std::uint64_t{(end << 8) & 0xff00u} |
                                  (size & 0xff) << 40;

    // 0x290a818: str x10, [x8, x0, LSL #3]
    WriteAt<std::uint64_t>(bytes, static_cast<std::size_t>(count) * kElementSize,
                           element | (static_cast<std::uint64_t>(usage) & 0xff));

    // 0x290a81c-0x290a824: reload the count and add one (the store above may alias it).
    const std::uint64_t next = ReadAt<std::uint64_t>(bytes, kCountOffset) + 1;
    WriteAt<std::uint64_t>(bytes, kCountOffset, next);

    // 0x290a82c-0x290a848: a slot (the one just written included) whose byte 0 equals alias_usage
    // ends the call with the alias already present. The 32-bit compare against w2 can only match
    // when alias_usage fits in a byte.
    for (std::uint64_t index = 0; index < next; ++index) {
        if (ByteAt(bytes, static_cast<std::size_t>(index) * kElementSize) ==
            static_cast<std::uint64_t>(alias_usage)) {
            return;
        }
    }

    // 0x290a84c-0x290a860: the same slot with byte 0 = alias_usage, appended at the cached count.
    WriteAt<std::uint64_t>(bytes, static_cast<std::size_t>(next) * kElementSize,
                           element | (static_cast<std::uint64_t>(alias_usage) & 0xff));
    WriteAt<std::uint64_t>(bytes, kCountOffset, ReadAt<std::uint64_t>(bytes, kCountOffset) + 1);
}

}  // namespace NRadEngine
