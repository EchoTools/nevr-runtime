// NRadEngine::CFactory<NRadEngine::CGShaderSetResource>::CreateInstance
// Quest libr15.so VA 0x1a0707c; ReVault binary 576.
// ReVault raw parent 100260851, reconstruction node 100600499; grade M: all 73 instructions and the four default-static branches traced; the CAllocator slot +0x10 is unnamed and the return type is inferred.
// @0x1a0707c — NRadEngine::CFactory<NRadEngine::CGShaderSetResource>::CreateInstance
/* @confidence: M */
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>

// Vptr values stored into the object: the address points (vtable + 0x10) of the CPresence, CGShaderSetResource and
// CPresenceInit vtables (Ghidra labels PTR__CPresence_0372ecc8, PTR__CGShaderSetResource_0375c7d8 and
// PTR__CPresenceInit_0372f298).
extern const std::byte g_vptr_CPresence[];
extern const std::byte g_vptr_CGShaderSetResource[];
extern const std::byte g_vptr_CPresenceInit[];
// CGShaderSetResource::DefaultInit()::init (0x18 bytes; guard symbol _ZGVZN10NRadEngine19CGShaderSetResource11DefaultInitEvE4init).
extern std::byte g_default_init_object[0x18];
extern std::byte g_default_init_guard[8];

extern "C" int __cxa_guard_acquire(std::byte* guard);   // thunk 0x00ff6770
extern "C" void __cxa_guard_release(std::byte* guard);  // thunk 0x01051c60

namespace {

// The default static's id words: +0x08 is the 8 bytes at 0x02d361d0 (adrp 0x2d36000, add #0x1d1, aligned down to 8),
// +0x10 the 8 bytes at 0x02d61e58 (adrp 0x2d61000, add #0xe5b, aligned down to 8).
constexpr std::uint64_t kDefaultInitWord1 = 0xa71a14664bd4e047ULL;
constexpr std::uint64_t kDefaultInitWord2 = 0xbbd813f7ad760c69ULL;
// The 16-byte constant at 0x02d35df0, stored at +0x60 of the object: {7, 0}.
constexpr std::uint64_t kHeaderWords[2] = {7, 0};

std::uint64_t LoadU64(const std::byte* base, std::size_t offset) {
    std::uint64_t value = 0;
    std::memcpy(&value, base + offset, sizeof value);
    return value;
}

const std::byte* LoadPtr(const std::byte* base, std::size_t offset) {
    const std::byte* value = nullptr;
    std::memcpy(&value, base + offset, sizeof value);
    return value;
}

void StoreU64(std::byte* base, std::size_t offset, std::uint64_t value) {
    std::memcpy(base + offset, &value, sizeof value);
}

void StorePtr(std::byte* base, std::size_t offset, const void* value) {
    std::memcpy(base + offset, &value, sizeof value);
}

}  // namespace

namespace NRadEngine {

struct CAllocator;
class CGShaderSetResource;
class CPresenceInit;

class CMemoryContext {
 public:
    // NRadEngine::CMemoryContext::Current (_ZN10NRadEngine14CMemoryContext7CurrentEv), thunk 0x01028430; x0.
    static CAllocator* Current();
    // NRadEngine::CMemoryContext::CurrentPtr (_ZN10NRadEngine14CMemoryContext10CurrentPtrEv), thunk 0x00ffbc50; x0.
    static CAllocator* CurrentPtr();
};

class CMemBlock {
 public:
    // NRadEngine::CMemBlock::CMemBlock(uint64, uint64, CAllocator*): complete-object symbol
    // _ZN10NRadEngine9CMemBlockC1EyyPNS_10CAllocatorE (a C2 symbol is also exported); thunk 0x00fedec0.
    CMemBlock(std::uint64_t arg1, std::uint64_t arg2, CAllocator* allocator);
};

template <typename T>
class CFactory;

template <>
class CFactory<CGShaderSetResource> {
 public:
    // _ZN10NRadEngine8CFactoryINS_19CGShaderSetResourceEE14CreateInstanceEPNS_13CPresenceInitE. The mangled name
    // carries no return type; the object returned is the CGShaderSetResource built here.
    CGShaderSetResource* CreateInstance(CPresenceInit* init);
};

// Layout of the object built here (returned in x0; x0 on entry is the factory and is not read): +0x00 vptr,
// +0x20 and +0x28 the id words of the source, +0x30 the memory context, +0x38 -1, +0x60 a 16-byte header,
// +0x78 CMemBlock, +0x98 0x20 (the res_heap_patch STEP site 0x1a07130). The allocation is 0xb0 bytes; the last
// word written is at +0xa8.
CGShaderSetResource* CFactory<CGShaderSetResource>::CreateInstance(CPresenceInit* init) {
    // 0x1a07088-0x1a07098: allocate(context, 0xb0) through the context's vtable slot +0x10.
    // UNRESOLVED: the name of that CAllocator virtual is not recovered.
    using AllocateFn = void* (*)(std::byte*, std::uint64_t);
    auto* const context = reinterpret_cast<std::byte*>(CMemoryContext::Current());
    AllocateFn allocate = nullptr;
    std::memcpy(&allocate, LoadPtr(context, 0x00) + 0x10, sizeof allocate);
    auto* const object = static_cast<std::byte*>(allocate(context, 0xb0));

    // 0x1a070a0: init != null skips the default static (cbnz x20 to 0x1a070bc).
    const std::byte* source = reinterpret_cast<const std::byte*>(init);
    if (init == nullptr) {
        // 0x1a070a4-0x1a070b8: ldarb of the guard byte; the acquire below runs only while bit 0 is clear.
        const std::uint8_t guard_byte =
            __atomic_load_n(reinterpret_cast<const std::uint8_t*>(g_default_init_guard), __ATOMIC_ACQUIRE);
        // 0x1a0714c-0x1a07158: __cxa_guard_acquire; a nonzero result builds the static and releases the guard.
        if ((guard_byte & 1u) == 0 && __cxa_guard_acquire(g_default_init_guard) != 0) {
            // 0x1a0715c-0x1a07190: vptr, then the two id words.
            StorePtr(g_default_init_object, 0x00, g_vptr_CPresenceInit);
            StoreU64(g_default_init_object, 0x08, kDefaultInitWord1);
            StoreU64(g_default_init_object, 0x10, kDefaultInitWord2);
            // 0x1a07198: __cxa_guard_release.
            __cxa_guard_release(g_default_init_guard);
        }
        source = g_default_init_object;
    }

    // 0x1a070bc-0x1a070cc: +0x10 and +0x18 zero; vptr CPresence at +0x00; +0x08 zero.
    StoreU64(object, 0x10, 0);
    StoreU64(object, 0x18, 0);
    StorePtr(object, 0x00, g_vptr_CPresence);
    StoreU64(object, 0x08, 0);
    // 0x1a070d0-0x1a070e0: the id words of the source.
    StoreU64(object, 0x20, LoadU64(source, 0x08));
    StoreU64(object, 0x28, LoadU64(source, 0x10));
    // 0x1a070e4-0x1a07100: memory context (a second call), then -1.
    StorePtr(object, 0x30, CMemoryContext::Current());
    StoreU64(object, 0x38, ~std::uint64_t{0});
    // 0x1a07108: sixteen zero bytes at +0x40 and +0x50.
    StoreU64(object, 0x40, 0);
    StoreU64(object, 0x48, 0);
    StoreU64(object, 0x50, 0);
    StoreU64(object, 0x58, 0);
    // 0x1a0710c-0x1a07114: vptr CGShaderSetResource; 16-byte header; +0x70 zero.
    StorePtr(object, 0x00, g_vptr_CGShaderSetResource);
    std::memcpy(object + 0x60, kHeaderWords, sizeof kHeaderWords);
    StoreU64(object, 0x70, 0);
    // 0x1a07118-0x1a0712c: CMemBlock constructed in place at +0x78, allocator CurrentPtr(), arguments 0 and 0.
    CAllocator* const allocator = CMemoryContext::CurrentPtr();
    new (object + 0x78) CMemBlock(0, 0, allocator);
    // 0x1a07134-0x1a07138: +0xa0 and +0xa8 zero; +0x98 = 0x20.
    StoreU64(object, 0xa0, 0);
    StoreU64(object, 0xa8, 0);
    StoreU64(object, 0x98, 0x20);
    // 0x1a0713c-0x1a07148: return the object.
    return reinterpret_cast<CGShaderSetResource*>(object);
}

}  // namespace NRadEngine
