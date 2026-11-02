// Quest libr15.so VA 0x10940b4; ReVault binary libr15.so.
// ReVault raw parent 100208241, reconstruction node 100600597; grade M.
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace NRadEngine {

class CAllocator;
class CString;

struct CMemBlock {
    CMemBlock(std::uint64_t size, std::uint64_t alignment,
              CAllocator* allocator);
    void SetAllocator(CAllocator* allocator);
    void Detach();
    ~CMemBlock();

    alignas(8) std::byte raw[0x38];
};
static_assert(sizeof(CMemBlock) == 0x38);

class CMemoryContext {
public:
    static CAllocator* CurrentPtr();
};

class CSysThread {
public:
    static void* GetTLS(int key);
};

class CJson {
public:
    std::uint32_t DecodeFrom(CString* text, std::uint32_t flags);
};

class CHttpListenerRequest {
public:
    std::uint32_t Body(CJson* json);
    void Body(CString* output);
};

namespace {

constexpr int kAllocatorTlsKey = 0x74682820;

void StoreU64(std::byte* object, std::size_t offset, std::uint64_t value) {
    std::memcpy(object + offset, &value, sizeof(value));
}

std::uint8_t LoadFlags(const CMemBlock& block) {
    std::uint8_t flags = 0;
    std::memcpy(&flags, reinterpret_cast<const std::byte*>(&block) + 0x1c,
                sizeof(flags));
    return flags;
}

}  // namespace

// @0x10940b4 — NRadEngine::CHttpListenerRequest::Body
/* @confidence: M — all 40 instructions and cleanup branches are accounted for; the TLS key is the exact word loaded from the referenced bytes at 0x38611b8, whose role is uncertain. */
std::uint32_t CHttpListenerRequest::Body(CJson* json) {
    CAllocator* const current_allocator = CMemoryContext::CurrentPtr();
    CMemBlock body(0, 0, current_allocator);
    auto* const bytes = reinterpret_cast<std::byte*>(&body);
    StoreU64(bytes, 0x20, 0x20);
    StoreU64(bytes, 0x28, 0);
    StoreU64(bytes, 0x30, 0);

    CAllocator* const request_allocator =
        static_cast<CAllocator*>(CSysThread::GetTLS(kAllocatorTlsKey));
    body.SetAllocator(request_allocator);
    Body(reinterpret_cast<CString*>(&body));

    const std::uint32_t result =
        json->DecodeFrom(reinterpret_cast<CString*>(&body), 0);
    if ((LoadFlags(body) & 0x06U) != 0) {
        body.Detach();
    }
    return result;
}

}  // namespace NRadEngine
