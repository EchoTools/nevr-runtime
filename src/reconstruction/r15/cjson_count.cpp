// NRadEngine::CJson::Count
// Quest libr15.so VA 0x10a0d4c; ReVault binary 576.
// ReVault raw parent 100208527, reconstruction node 100600486; grade H: all 42 instructions (stored size 168) checked; both allocator-selection branches, the NULL node, the tag-0 and tag-1 paths and the zero fallthrough match; NavigatePath arguments match the call site.
// @0x10a0d4c — NRadEngine::CJson::Count
/* @confidence: H */
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace NRadEngine {
struct CAllocator;
}  // namespace NRadEngine

// mspace is the allocator pointer and mmode a 64-bit value (loaded with ldr x, compared with 1). Both are read through the
// GOT; the decompile prints their bare names, so no namespace is recovered for them.
extern NRadEngine::CAllocator* mspace;
extern std::uint64_t mmode;

// libjansson: json_t starts with its 4-byte json_type tag (JSON_OBJECT = 0, JSON_ARRAY = 1).
struct json_t;
constexpr std::int32_t kJsonObject = 0;
constexpr std::int32_t kJsonArray = 1;

// libjansson imports: json_object_size at 0x12c6a98 (stub 0x1017c40), json_array_size at 0x12c7040 (stub 0x1011fd0).
extern "C" std::size_t json_object_size(const json_t* object);
extern "C" std::size_t json_array_size(const json_t* array);

// Path cache stored at CJson+0x08 and passed to NavigatePath as its fourth argument: a bucket and chunk table, not read here.
struct CJsonCache;

namespace NRadEngine {

struct CMemoryContext {
    static CAllocator* CurrentPtr();                 // NRadEngine::CMemoryContext::CurrentPtr(void), 0x12cae90 (stub 0xffbc50)
    static void SetCurrent(CAllocator* allocator);   // NRadEngine::CMemoryContext::SetCurrent(CAllocator*), 0x12cafac (stub 0xfd3590)
};

class CJson {
 public:
    // Raw layout used here, none of it recovered as fields: +0x00 root node (json_t*), +0x08 path cache (CJsonCache*).
    std::size_t Count(const char* path) const;
    // CJson::NavigatePath, 0x10a0950 (direct call). Name from the echovr.exe counterpart CJson_NavigatePath (0x1405fcea0): same
    // parameters and the same "$ json path" messages; libr15 leaves 0x10a0950 unnamed.
    static const json_t* NavigatePath(const char* path, const json_t* root, int required, CJsonCache* cache);
};

}  // namespace NRadEngine

std::size_t NRadEngine::CJson::Count(const char* path) const {
    // Allocator selection as in the decompile: mspace when mmode is 1, else the current allocator. The current allocator
    // is saved and restored at the end.
    NRadEngine::CAllocator* selected = mspace;
    if (mmode != 1) {
        selected = NRadEngine::CMemoryContext::CurrentPtr();
    }
    NRadEngine::CAllocator* saved = NRadEngine::CMemoryContext::CurrentPtr();
    NRadEngine::CMemoryContext::SetCurrent(selected);

    const auto* object = reinterpret_cast<const std::byte*>(this);
    const json_t* root = nullptr;
    std::memcpy(&root, object + 0x00, sizeof root);
    CJsonCache* cache = nullptr;
    std::memcpy(&cache, object + 0x08, sizeof cache);

    const json_t* node = NRadEngine::CJson::NavigatePath(path, root, 0, cache);
    std::size_t count = 0;
    if (node != nullptr) {
        std::int32_t type = 0;
        std::memcpy(&type, node, sizeof type);
        if (type == kJsonObject) {
            count = json_object_size(node);
        } else if (type == kJsonArray) {
            count = json_array_size(node);
        }
    }

    NRadEngine::CMemoryContext::SetCurrent(saved);
    return count;
}
