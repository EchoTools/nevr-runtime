// NRadEngine::ClearFileRequestData
// Quest libr15.so VA 0x2b9759c; ReVault binary 576.
// ReVault raw parent 100350241, reconstruction node 100600764; grade H: body is the Fill call (0, 0x470, through thunk 0x1047300) and the stores at +0x418, +0x10 and +0x46c; all 11 instructions checked, the frame is not written; 0 placeholders; NRadEngine::SFileRequestData is raw 0x470-byte storage, ReVault records no layout for it.
// @0x2b9759c — NRadEngine::ClearFileRequestData
/* @confidence: H */
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace NRadEngine {

// Quest libr15.so VA 0x12cafcc: ReVault name Fill, Ghidra CMemory::Fill. The call at 0x2b975ac
// reaches it through the thunk at 0x1047300. Same signature as memory_fill.cpp in the merged tree.
namespace CMemory {
void Fill(void* destination, unsigned char value, std::uint64_t count);
}  // namespace CMemory

// Request block passed by reference. 0x470 bytes: the length cleared at 0x2b975a0, and the size
// in ReVault's comment on this function and in the merged FillResourceFileRequest.cpp. No
// layout is recorded in ReVault, so the struct is raw storage; the members used are below.
struct SFileRequestData {
    alignas(8) std::byte bytes[0x470];
};
static_assert(sizeof(SFileRequestData) == 0x470);

namespace {

// Stores a value at a byte offset, as the str instructions at 0x2b975b4, 0x2b975b8 and 0x2b975bc do.
template <typename T>
void store_at(SFileRequestData& request, std::size_t offset, T value) {
    std::memcpy(request.bytes + offset, &value, sizeof value);
}

}  // namespace

// 0x2b9759c: clears the request, then sets three fields. The prologue, epilogue and register
// saves are the compiler's, not written.
void ClearFileRequestData(SFileRequestData& request) {
    // 0x2b975a0-0x2b975ac: Fill(request, 0, 0x470), a direct call to the thunk at 0x1047300.
    CMemory::Fill(&request, 0, 0x470);
    // 0x2b975b0-0x2b975b8: -1 (64-bit) at +0x418, then -1 at +0x10.
    store_at<std::int64_t>(request, 0x418, -1);
    store_at<std::int64_t>(request, 0x10, -1);
    // 0x2b975bc: zero (32-bit) at +0x46c.
    store_at<std::int32_t>(request, 0x46c, 0);
}

}  // namespace NRadEngine
