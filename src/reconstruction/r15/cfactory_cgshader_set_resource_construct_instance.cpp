// NRadEngine::CFactory<NRadEngine::CGShaderSetResource>::ConstructInstance
// Quest libr15.so VA 0x1a06f4c; ReVault binary 576.
// ReVault raw parent 100260850, reconstruction node 100600480; grade M: all 76 instructions traced branch by branch; the +0x08 default word is the 8 bytes at 0x02d361d0, which equal the decompile literal, and the parameters are (void*, CPresenceInit*) per the mangled name.
// @0x1a06f4c — NRadEngine::CFactory<NRadEngine::CGShaderSetResource>::ConstructInstance
/* @confidence: M */
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>

extern const std::byte g_vptr_CPresence[];            // vptr value 0x0372ecc8 (PTR__CPresence_0372ecc8)
extern const std::byte g_vptr_CGShaderSetResource[];  // vptr value 0x0375c7d8 (PTR__CGShaderSetResource_0375c7d8)
extern const std::byte g_vptr_CPresenceInit[];        // vptr value 0x0372f298 (PTR__CPresenceInit_0372f298)
// Function-local static CGShaderSetResource::DefaultInit()::init (0x18 bytes) and its guard byte.
extern std::byte g_default_init_object[0x18];
extern std::byte g_default_init_guard[8];

extern "C" int __cxa_guard_acquire(std::byte* guard);   // thunk 0x00ff6770
extern "C" void __cxa_guard_release(std::byte* guard);  // thunk 0x01051c60

namespace {

// Id words of the default static: the word at +0x08 is the 8 bytes at 0x02d361d0, the word at +0x10 the 8 bytes at
// 0x02d61e58 (each the adrp/add target aligned down to 8). The +0x08 bytes equal the decompile literal.
constexpr std::uint64_t kDefaultInitWord1 = 0xa71a14664bd4e047ULL;
constexpr std::uint64_t kDefaultInitWord2 = 0xbbd813f7ad760c69ULL;
// The 16-byte constant at 0x02d35df0, stored at +0x60 of the object: {7, 0}.
constexpr std::uint64_t kHeaderWords[2] = {7, 0};

std::uint64_t LoadU64(const std::byte* base, std::size_t offset) {
    std::uint64_t value = 0;
    std::memcpy(&value, base + offset, sizeof value);
    return value;
}

void StoreU64(std::byte* base, std::size_t offset, std::uint64_t value) {
    std::memcpy(base + offset, &value, sizeof value);
}

void StorePtr(std::byte* base, std::size_t offset, const std::byte* value) {
    std::memcpy(base + offset, &value, sizeof value);
}

}  // namespace

namespace NRadEngine {

class CAllocator;
class CGShaderSetResource;
class CPresenceInit;

class CMemoryContext {
 public:
    // NRadEngine::CMemoryContext::Current, thunk 0x01028430; its 64-bit result is x0.
    static std::uint64_t Current();
    // NRadEngine::CMemoryContext::CurrentPtr, thunk 0x00ffbc50; its result is the allocator pointer (x0).
    static CAllocator* CurrentPtr();
};

class CMemBlock {
 public:
    // NRadEngine::CMemBlock::CMemBlock, thunk 0x00fedec0; the object is constructed in place at +0x78.
    CMemBlock(std::uint64_t arg1, std::uint64_t arg2, CAllocator* allocator);
};

template <typename T>
class CFactory;

template <>
class CFactory<CGShaderSetResource> {
 public:
    void* ConstructInstance(void* out, CPresenceInit* from);
};

// Layout of the built object (out, x1; x0 is the factory and is not read): +0x00 vptr, +0x20 and +0x28 the id words
// of the source, +0x30 the memory context, +0x38 -1, +0x60 a 16-byte header, +0x78 CMemBlock, +0x98 0x20.
void* CFactory<CGShaderSetResource>::ConstructInstance(void* out, CPresenceInit* from) {
    auto* const self = reinterpret_cast<std::byte*>(out);

    // from == null (x2 == 0): the default static is built once. Its guard byte is read; the acquire runs only while
    // bit 0 is clear, and a nonzero acquire builds the static and releases the guard.
    if (from == nullptr) {
        std::uint8_t guard_byte = 0;
        std::memcpy(&guard_byte, g_default_init_guard, sizeof guard_byte);
        if ((guard_byte & 1u) == 0 && __cxa_guard_acquire(g_default_init_guard) != 0) {
            StorePtr(g_default_init_object, 0x00, g_vptr_CPresenceInit);
            StoreU64(g_default_init_object, 0x08, kDefaultInitWord1);
            StoreU64(g_default_init_object, 0x10, kDefaultInitWord2);
            __cxa_guard_release(g_default_init_guard);
        }
    }

    StoreU64(self, 0x10, 0);
    StoreU64(self, 0x18, 0);
    StorePtr(self, 0x00, g_vptr_CPresence);
    StoreU64(self, 0x08, 0);
    const std::byte* const source =
        from != nullptr ? reinterpret_cast<const std::byte*>(from) : g_default_init_object;
    StoreU64(self, 0x20, LoadU64(source, 0x08));
    StoreU64(self, 0x28, LoadU64(source, 0x10));

    StoreU64(self, 0x30, CMemoryContext::Current());
    StoreU64(self, 0x38, ~std::uint64_t{0});
    StoreU64(self, 0x40, 0);
    StoreU64(self, 0x48, 0);
    StoreU64(self, 0x50, 0);
    StoreU64(self, 0x58, 0);
    StorePtr(self, 0x00, g_vptr_CGShaderSetResource);
    std::memcpy(self + 0x60, kHeaderWords, sizeof kHeaderWords);
    StoreU64(self, 0x70, 0);
    CAllocator* const allocator = CMemoryContext::CurrentPtr();
    new (self + 0x78) CMemBlock(0, 0, allocator);
    StoreU64(self, 0xa0, 0);
    StoreU64(self, 0xa8, 0);
    StoreU64(self, 0x98, 0x20);
    return out;
}

}  // namespace NRadEngine
