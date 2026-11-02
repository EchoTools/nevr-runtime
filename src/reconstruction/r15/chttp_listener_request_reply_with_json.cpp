// Quest libr15.so VA 0x1094154; ReVault binary libr15.so.
// ReVault raw parent 100208242, reconstruction node 100600598; grade M.
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
    char* GetString();
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
    void EncodeToCompact(CString* output, std::uint32_t flags,
                         const char* prefix) const;
};

struct evhttp_request;
struct evkeyvalq;
struct evbuffer;

class CHttpListenerRequest {
public:
    void ReplyWithJson(const CJson* json, std::uint32_t status);
};

namespace {

constexpr int kAllocatorTlsKey = 0x74682820;
constexpr char kJsonContentType[] = "application/json; charset=utf-8";
constexpr char kEmptyResponseReason[] = "";

void StoreU64(std::byte* object, std::size_t offset, std::uint64_t value) {
    std::memcpy(object + offset, &value, sizeof(value));
}

std::uint8_t LoadFlags(const CMemBlock& block) {
    std::uint8_t flags = 0;
    std::memcpy(&flags, reinterpret_cast<const std::byte*>(&block) + 0x1c,
                sizeof(flags));
    return flags;
}

std::uint64_t LoadU64(const CMemBlock& block, std::size_t offset) {
    std::uint64_t value = 0;
    std::memcpy(&value, reinterpret_cast<const std::byte*>(&block) + offset,
                sizeof(value));
    return value;
}

}  // namespace

extern "C" {
evkeyvalq* evhttp_request_get_output_headers(evhttp_request* request);
int evhttp_add_header(evkeyvalq* headers, const char* key, const char* value);
evbuffer* evhttp_request_get_output_buffer(evhttp_request* request);
int evbuffer_add(evbuffer* buffer, const void* data, std::size_t length);
void evhttp_send_reply(evhttp_request* request, int status, const char* reason,
                       evbuffer* buffer);
}

// @0x1094154 — NRadEngine::CHttpListenerRequest::ReplyWithJson
/* @confidence: M — all 70 instructions and both block-length branches match; the allocator TLS key is the exact word loaded from 0x38611b8, whose role is uncertain. */
void CHttpListenerRequest::ReplyWithJson(const CJson* json,
                                         std::uint32_t status) {
    CAllocator* const current_allocator = CMemoryContext::CurrentPtr();
    CMemBlock body(0, 0, current_allocator);
    auto* const bytes = reinterpret_cast<std::byte*>(&body);
    StoreU64(bytes, 0x20, 0x20);
    StoreU64(bytes, 0x28, 0);
    StoreU64(bytes, 0x30, 0);

    CAllocator* const request_allocator =
        static_cast<CAllocator*>(CSysThread::GetTLS(kAllocatorTlsKey));
    body.SetAllocator(request_allocator);

    json->EncodeToCompact(reinterpret_cast<CString*>(&body), 0,
                          kEmptyResponseReason);

    auto* const request = reinterpret_cast<evhttp_request*>(this);
    evkeyvalq* const headers = evhttp_request_get_output_headers(request);
    evhttp_add_header(headers, "Content-Type", kJsonContentType);

    evbuffer* const output = evhttp_request_get_output_buffer(request);
    const char* data = kEmptyResponseReason;
    std::uint64_t length = 0;
    if (LoadU64(body, 0x30) != 0) {
        data = body.GetString();
        const auto current_length = LoadU64(body, 0x30);
        if (current_length != 0) {
            length = current_length - 1;
        }
    }
    evbuffer_add(output, data, static_cast<std::size_t>(length));
    evhttp_send_reply(request, static_cast<int>(status),
                      kEmptyResponseReason, output);

    if ((LoadFlags(body) & 0x06U) != 0) {
        body.Detach();
    }
}

}  // namespace NRadEngine
