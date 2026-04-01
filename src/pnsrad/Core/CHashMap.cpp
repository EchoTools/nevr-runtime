#include "src/pnsrad/Core/CHashMap.h"
#include <cstring>

namespace NRadEngine {

// Entry offsets within the 0x20-byte entry structure
static constexpr uint32_t ENTRY_NEXT_CHAIN  = 0x00;  // uint32_t
static constexpr uint32_t ENTRY_REF_COUNT   = 0x04;  // uint32_t
static constexpr uint32_t ENTRY_PREV_ORDER  = 0x08;  // uint32_t
static constexpr uint32_t ENTRY_NEXT_ORDER  = 0x0C;  // uint32_t
static constexpr uint32_t ENTRY_KEY         = 0x10;  // uint64_t
static constexpr uint32_t ENTRY_VALUE       = 0x18;  // uint64_t

static constexpr uint32_t END_OF_CHAIN = 0xFFFFFFFF;
static constexpr uint32_t ENTRY_SIZE   = 0x20;
static constexpr uint32_t BUCKET_COUNT = 0x1000;

// Helper: resolve entry index to pointer within paged storage
// @0x180097170 (inlined) — index-to-pointer resolution for paged storage
static uint8_t* ResolveEntry(CHashMap* map, uint32_t index) {
    uintptr_t buf_data = static_cast<uintptr_t>(*reinterpret_cast<int64_t*>(&map->buf_ctx[0]));  // buf_ctx.data_ptr
    uint32_t page_idx = index / map->entries_per_page;
    uint32_t slot_idx = index % map->entries_per_page;
    auto page_ptr = *reinterpret_cast<int64_t*>(buf_data + page_idx * 8);
    return reinterpret_cast<uint8_t*>(page_ptr + slot_idx * ENTRY_SIZE);
}

// @0x180097170 — CHashMap::FindOrInsert
// @confidence: H
uint32_t* CHashMap_FindOrInsert(CHashMap* map, uint16_t* key, uint32_t* found_flag, uint64_t* out_index) {
    void* lock = get_tls_lock(reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(map) + 0x18));
    void* tls = get_tls_context();
    release_lock(lock);

    // Hash into bucket (lower 12 bits of 8-byte key)
    uint64_t full_key = *reinterpret_cast<uint64_t*>(key);
    uint32_t bucket_idx = static_cast<uint32_t>(full_key & 0xFFF);
    auto* bucket_array = reinterpret_cast<uint32_t*>(map->bucket_array);
    uint32_t* chain_ptr = &bucket_array[bucket_idx];
    uint32_t entry_idx = *chain_ptr;

    // Search chain for matching key
    if (entry_idx != END_OF_CHAIN) {
        uint32_t* prev_ptr = chain_ptr;
        do {
            uint8_t* entry = ResolveEntry(map, entry_idx);
            if (*reinterpret_cast<uint64_t*>(entry + ENTRY_KEY) == full_key) {
                *found_flag = 1;
                auto* value_ptr = reinterpret_cast<uint32_t*>(entry + ENTRY_VALUE);
                entry_idx = *prev_ptr;
                *out_index = static_cast<uint64_t>(entry_idx);
                release_lock(tls);
                return value_ptr;
            }
            entry_idx = *reinterpret_cast<uint32_t*>(entry + ENTRY_NEXT_CHAIN);
            prev_ptr = reinterpret_cast<uint32_t*>(entry + ENTRY_NEXT_CHAIN);
        } while (entry_idx != END_OF_CHAIN);
    }

    // Not found — insert new entry from free list
    *found_flag = 0;
    entry_idx = map->free_head;
    if (entry_idx == END_OF_CHAIN) {
        expand_pages(map);
        entry_idx = map->free_head;
    }

    // Remove from free list
    *chain_ptr = entry_idx;
    uint8_t* new_entry = ResolveEntry(map, entry_idx);
    map->free_head = *reinterpret_cast<uint32_t*>(new_entry + ENTRY_NEXT_CHAIN);
    map->free_count--;

    // Initialize entry
    *reinterpret_cast<uint64_t*>(new_entry + ENTRY_KEY) = full_key;
    *reinterpret_cast<uint32_t*>(new_entry + ENTRY_NEXT_CHAIN) = END_OF_CHAIN;
    *reinterpret_cast<uint32_t*>(new_entry + ENTRY_REF_COUNT) = 1;
    *reinterpret_cast<uint32_t*>(new_entry + ENTRY_PREV_ORDER) = map->order_head;
    *reinterpret_cast<uint32_t*>(new_entry + ENTRY_NEXT_ORDER) = END_OF_CHAIN;

    // Link into insertion-order list
    map->order_head = entry_idx;
    uint32_t prev_order = *reinterpret_cast<uint32_t*>(new_entry + ENTRY_PREV_ORDER);
    if (prev_order != END_OF_CHAIN) {
        uint8_t* prev_entry = ResolveEntry(map, prev_order);
        *reinterpret_cast<uint32_t*>(prev_entry + ENTRY_NEXT_ORDER) = entry_idx;
    }

    map->entry_count++;

    auto* value_ptr = reinterpret_cast<uint32_t*>(new_entry + ENTRY_VALUE);
    value_ptr[0] = 0;
    value_ptr[1] = 0;

    *out_index = static_cast<uint64_t>(entry_idx);
    release_lock(tls);
    return value_ptr;
}

// @0x1800972F0 — CHashMap::Init
// @confidence: H
void* CHashMap_Init(CHashMap* map, int64_t* params) {
    auto* allocator = reinterpret_cast<void*>(params[2]);
    map->bucket_capacity = BUCKET_COUNT;
    map->allocator_ctx = allocator;

    // Get allocator vtable and call alloc
    uint64_t alloc_result = 0;
    if (allocator != nullptr) {
        // vtable[1](allocator) — allocate bucket array
        auto** vtable = *reinterpret_cast<void***>(allocator);
        using AllocFn = uint64_t(*)(void*);
        alloc_result = reinterpret_cast<AllocFn>(reinterpret_cast<uintptr_t*>(vtable)[1])(allocator);
    }
    map->bucket_array = reinterpret_cast<void*>(alloc_result);

    // Init embedded buffer context for page array
    auto* buf_ctx_ptr = reinterpret_cast<void*>(&map->buf_ctx[0]);
    init_buffer_context(buf_ctx_ptr, 0, 0, allocator);

    map->entry_size = ENTRY_SIZE;
    map->page_capacity = 0;
    map->page_count = 0;
    map->free_head = END_OF_CHAIN;
    map->free_count = 0;

    // Determine entries_per_page
    int64_t epp = params[1];
    if (epp == 0) {
        epp = (params[0] != 0) ? params[0] : 1;
    }
    map->entries_per_page = static_cast<uint32_t>(epp);
    map->page_byte_size = map->entries_per_page << 5;  // * 0x20

    // Fill bucket array with 0xFF (all END_OF_CHAIN)
    memset_wrapper(map->bucket_array, 0xFF, BUCKET_COUNT * 4);

    // Pre-allocate pages
    uint64_t pages_needed = (static_cast<uint64_t>(params[0]) - 1 + map->entries_per_page)
                            / map->entries_per_page;
    for (uint64_t i = 0; i < pages_needed; i++) {
        void* lock = get_tls_lock(buf_ctx_ptr);
        void* tls = get_tls_context();
        release_lock(lock);

        // Allocate page via allocator vtable
        auto* alloc_ctx = reinterpret_cast<void*>(get_tls_context());
        auto** avt = *reinterpret_cast<void***>(alloc_ctx);
        using AllocSizeFn = uint8_t*(*)(void*, uint32_t);
        uint8_t* page = reinterpret_cast<AllocSizeFn>(reinterpret_cast<uintptr_t*>(avt)[1])(alloc_ctx, map->page_byte_size);

        // Grow page array if needed
        if (map->page_capacity <= map->page_count) {
            uint64_t grow = map->page_capacity;
            if (grow <= map->entry_size) {
                grow = map->entry_size;
            }
            grow_buffer(buf_ctx_ptr, (map->page_capacity + grow) * 8, 1);
            map->page_capacity += grow;
        }

        // Store page pointer
        map->page_count++;
        auto* page_array = *reinterpret_cast<int64_t**>(&map->buf_ctx[0]);
        *reinterpret_cast<uint8_t**>(reinterpret_cast<uintptr_t>(page_array) + (map->page_count - 1) * 8) = page;

        // Build free list in reverse order within this page
        uint32_t base_index = static_cast<uint32_t>(map->page_count) * map->entries_per_page;
        map->free_count += map->entries_per_page;
        uint8_t* slot = page + static_cast<uint64_t>(map->entries_per_page) * ENTRY_SIZE;
        while (slot != page) {
            slot -= ENTRY_SIZE;
            *reinterpret_cast<uint32_t*>(slot + ENTRY_REF_COUNT) = 0;
            *reinterpret_cast<uint64_t*>(slot + ENTRY_KEY) = 0xFFFFFFFFFFFFFFFFULL;
            base_index--;
            *reinterpret_cast<uint32_t*>(slot + ENTRY_NEXT_CHAIN) = map->free_head;
            map->free_head = base_index;
        }

        release_lock(tls);
    }

    return map;
}

// @0x1800974D0 — CHashMapRef::Construct (ref-counted handle)
// @confidence: H
void* CHashMapRef_Construct(void* ref, uint64_t* value) {
    auto* r = reinterpret_cast<uint64_t*>(ref);
    r[0] = 0;
    r[1] = 0;
    // json_incref equivalent — increments refcount on json value
    extern int* json_incref(void* val);  // @0x1801c2d60
    auto* val = json_incref(reinterpret_cast<void*>(*value));
    r[0] = reinterpret_cast<uint64_t>(val);
    return ref;
}

// @0x180097500 — CHashMapRef::Default
// @confidence: H
void* CHashMapRef_Default(void* ref) {
    auto* r = reinterpret_cast<uint64_t*>(ref);
    r[0] = 0;
    r[1] = 0;
    return ref;
}

// @0x180097510 — CHashMap::Destroy
// @confidence: H
void CHashMap_Destroy(CHashMap* map) {
    auto* buf_ctx_ptr = reinterpret_cast<void*>(&map->buf_ctx[0]);

    // Walk insertion-order list and release all entries
    void* lock = get_tls_lock(buf_ctx_ptr);
    void* tls1 = get_tls_context();
    release_lock(lock);

    uint32_t idx = map->order_head;
    while (idx != END_OF_CHAIN) {
        uint8_t* entry = ResolveEntry(map, idx);
        idx = *reinterpret_cast<uint32_t*>(entry + ENTRY_PREV_ORDER);
        map->order_head = idx;
    }

    // Free all pages
    lock = get_tls_lock(buf_ctx_ptr);
    void* tls2 = get_tls_context();
    release_lock(lock);

    for (uint64_t i = 0; i < map->page_count; i++) {
        auto* alloc = reinterpret_cast<void*>(get_tls_context());
        auto** avt = *reinterpret_cast<void***>(alloc);
        auto* page_array = *reinterpret_cast<int64_t**>(&map->buf_ctx[0]);
        auto* page = reinterpret_cast<void*>(*reinterpret_cast<int64_t*>(reinterpret_cast<uintptr_t>(page_array) + i * 8));
        // vtable[6](allocator, page) — free
        using FreeFn = void(*)(void*, void*);
        reinterpret_cast<FreeFn>(reinterpret_cast<uintptr_t*>(avt)[6])(alloc, page);
        *reinterpret_cast<int64_t*>(reinterpret_cast<uintptr_t>(page_array) + i * 8) = 0;
    }

    // Reset map state
    map->free_count = 0;
    map->free_head = END_OF_CHAIN;
    map->page_count = 0;
    release_lock(tls2);
    release_lock(tls1);

    // Destroy embedded buffer context
    uint32_t flags = *reinterpret_cast<uint32_t*>(&map->buf_ctx[0x1C]);  // buf_ctx.flags
    if ((flags & 4) != 0 || (flags & 2) != 0) {
        reset_buffer(buf_ctx_ptr);
    }
    destroy_buffer(buf_ctx_ptr);

    // Free bucket array
    if (map->bucket_capacity != 0 && map->allocator_ctx != nullptr) {
        auto** avt = *reinterpret_cast<void***>(map->allocator_ctx);
        using FreeFn = void(*)(void*, void*);
        reinterpret_cast<FreeFn>(reinterpret_cast<uintptr_t*>(avt)[6])(map->allocator_ctx, map->bucket_array);
    }
}

} // namespace NRadEngine
