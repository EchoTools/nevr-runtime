// NRadEngine::CJson::IsArray
// Quest libr15.so VA 0x10a3dc8; ReVault binary 576.
// ReVault raw parent 100208576, reconstruction node 100600485; grade H: all 37 instructions (stored size 148) checked; both allocator-selection branches, the NULL and non-NULL node paths and the type==1 test match; NavigatePath arguments (path, this+0, 0, this+8) match the call site.
// @0x10a3dc8 — NRadEngine::CJson::IsArray
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
constexpr std::int32_t kJsonArray = 1;

// Path cache stored at CJson+0x08, a bucket/chunk table; NavigatePath reads it at fixed offsets only:
//   +0x00  pointer to 4096 uint32_t bucket heads, indexed by the low 12 bits of the symbol value
//   +0x18  pointer to the chunk table: one pointer per chunk, 8 bytes each
//   +0x60  uint32_t entries per chunk
// An entry is 0x20 bytes: +0x00 uint32_t index of the next entry (0xffffffff ends the chain), +0x10 uint64_t symbol value,
// +0x18 json_t* value. IsArray passes the pointer through and does not read it.
struct CJsonCache;

namespace NRadEngine {

struct CMemoryContext {
    static CAllocator* CurrentPtr();                 // NRadEngine::CMemoryContext::CurrentPtr(void), 0x12cae90 (stub 0xffbc50)
    static void SetCurrent(CAllocator* allocator);   // NRadEngine::CMemoryContext::SetCurrent(CAllocator*), 0x12cafac (stub 0xfd3590)
};

class CJson {
 public:
    // Raw layout used here, none of it recovered as fields: +0x00 root node (json_t*), +0x08 path cache (CJsonCache*).
    bool IsArray(const char* path) const;
    // CJson::NavigatePath, 0x10a0950 (direct call; static, so no this argument). Name from the echovr.exe counterpart
    // CJson_NavigatePath (0x1405fcea0); libr15 leaves 0x10a0950 unnamed. Signature as settled by cjson_navigate_path.cpp
    // (node 100600511): the third argument is an int, tested over all 32 bits.
    static const json_t* NavigatePath(const char* path, const json_t* root, int required, CJsonCache* cache);
};

}  // namespace NRadEngine

bool NRadEngine::CJson::IsArray(const char* path) const {
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
    bool is_array = false;
    if (node != nullptr) {
        std::int32_t type = 0;
        std::memcpy(&type, node, sizeof type);
        is_array = type == kJsonArray;
    }

    NRadEngine::CMemoryContext::SetCurrent(saved);
    return is_array;
}
