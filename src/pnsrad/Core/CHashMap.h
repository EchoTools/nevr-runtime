#ifndef PNSRAD_CORE_CHASHMAP_H
#define PNSRAD_CORE_CHASHMAP_H

/* @module: pnsrad.dll */
/* @purpose: Paged hash map with chained collision resolution */
/* @note: Used by CJson for path→value caching. Entry size = 0x20 bytes. */

#include <cstdint>
#include <cstddef>

namespace NRadEngine {

// Forward declarations for allocator/TLS helpers
extern void* get_tls_lock(void* buf_ctx);   // @0x18008b8b0
extern void* get_tls_context();              // @0x180089700
extern void  release_lock(void* lock);       // @0x180089a00
extern void  memset_wrapper(void* ptr, int val, uint64_t size);  // @0x1800897f0
extern void  init_buffer_context(void* ctx, uint64_t initial_size, uint64_t flags, void* allocator);  // @0x18008b630
extern void  grow_buffer(void* buf_ctx, uint64_t new_capacity, int flag);  // @0x18008c3b0
extern void  reset_buffer(void* buf_ctx);    // @0x18008bb90
extern void  destroy_buffer(void* buf_ctx);  // @0x18008b730
extern void  expand_pages(void* this_ptr);   // @0x18008a040

// ============================================================================
// CHashMap — paged chained hash map
// ============================================================================
// Entry layout (0x20 bytes per entry):
//   +0x00: uint32_t next_in_chain  (0xFFFFFFFF = end of chain)
//   +0x04: uint32_t ref_count
//   +0x08: uint32_t prev_in_order
//   +0x0C: uint32_t next_in_order
//   +0x10: uint64_t key            (full hash key, matched against lookup)
//   +0x18: uint64_t value          (user payload, 8 bytes)
//
// The hash table uses 4096 buckets (key & 0xFFF), with entries stored in
// fixed-size pages. Entries are 0x20 bytes each, `entries_per_page` items per page.
// An ordered doubly-linked list (prev_in_order/next_in_order) tracks insertion order.
// A free list (head at free_head) recycles deleted entries.

/* @addr: 0x180097170 — Lookup */
/* @size: 0x68 */
/* @confidence: H */
#pragma pack(push, 8)
struct CHashMap {
    void*    bucket_array;    // +0x00: pointer to 4096-entry bucket array (uint32_t[4096])
    uint64_t bucket_capacity; // +0x08: always 0x1000
    void*    allocator_ctx;   // +0x10: allocator context pointer (from constructor param)
    uint8_t  buf_ctx[0x20];   // +0x18: embedded BufferContext for page array storage
    uint64_t entry_size;      // +0x38: 0x20 (entry struct size)
    uint64_t page_capacity;   // +0x40: total allocated page pointer slots
    uint64_t page_count;      // +0x48: number of pages currently allocated
    uint32_t free_head;       // +0x50: head of free entry list (0xFFFFFFFF = empty)
    uint32_t order_head;      // +0x54: head of insertion-order list
    int32_t  free_count;      // +0x58: number of free entries remaining
    int32_t  entry_count;     // +0x5C: number of active entries
    uint32_t entries_per_page;// +0x60: entries per page (from constructor)
    uint32_t page_byte_size;  // +0x64: entries_per_page << 5 (entries_per_page * 0x20)
};
#pragma pack(pop)
static_assert(sizeof(CHashMap) == 0x68);
static_assert(offsetof(CHashMap, bucket_array) == 0x00);
static_assert(offsetof(CHashMap, bucket_capacity) == 0x08);
static_assert(offsetof(CHashMap, allocator_ctx) == 0x10);
static_assert(offsetof(CHashMap, entry_size) == 0x38);
static_assert(offsetof(CHashMap, page_capacity) == 0x40);
static_assert(offsetof(CHashMap, page_count) == 0x48);
static_assert(offsetof(CHashMap, free_head) == 0x50);
static_assert(offsetof(CHashMap, order_head) == 0x54);
static_assert(offsetof(CHashMap, free_count) == 0x58);
static_assert(offsetof(CHashMap, entry_count) == 0x5C);
static_assert(offsetof(CHashMap, entries_per_page) == 0x60);
static_assert(offsetof(CHashMap, page_byte_size) == 0x64);

// ============================================================================
// CHashMap operations
// ============================================================================

// @0x180097170 — CHashMap::FindOrInsert
// Looks up key in hash map; inserts a new entry if not found.
// param_2: pointer to 8-byte key (only lower 12 bits of key used for bucket index)
// param_3: [out] set to 1 if found, 0 if newly inserted
// param_4: [out] entry index
// Returns: pointer to the 8-byte value slot within the entry
// @confidence: H
uint32_t* CHashMap_FindOrInsert(CHashMap* map, uint16_t* key, uint32_t* found_flag, uint64_t* out_index);

// @0x1800972F0 — CHashMap::Init (constructor body)
// Initializes hash map from a parameter block:
//   param_1[0] = initial entry count hint
//   param_1[1] = entries_per_page (0 → use param_1[0] or default 1)
//   param_1[2] = allocator context pointer
// Allocates bucket array (4096 * 4 bytes), fills with 0xFF.
// Pre-allocates pages and builds free list in reverse order.
// @confidence: H
void* CHashMap_Init(CHashMap* map, int64_t* params);

// @0x1800974D0 — CHashMapRef::CHashMapRef (ref-counted handle)
// Stores a reference to an existing value via json_incref (@0x1801c2d60).
// @confidence: H
void* CHashMapRef_Construct(void* ref, uint64_t* value);

// @0x180097500 — CHashMapRef::CHashMapRef (default)
// Zero-initializes the 16-byte ref struct.
// @confidence: H
void* CHashMapRef_Default(void* ref);

// @0x180097510 — CHashMap::Destroy
// Walks insertion-order list, releases all entries (via allocator vtable +0x30 free).
// Destroys the BufferContext and bucket array.
// @confidence: H
void CHashMap_Destroy(CHashMap* map);

} // namespace NRadEngine

#endif // PNSRAD_CORE_CHASHMAP_H
