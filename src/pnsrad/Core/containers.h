#ifndef PNSRAD_CORE_CONTAINERS_H
#define PNSRAD_CORE_CONTAINERS_H

/* @module: pnsrad.dll */
/* @purpose: Array/container utility functions for CArray<CJsonNode> and similar
 *           element-stride-0x10 containers.
 *
 * The pnsrad plugin uses CArray (RadBuffer layout, 0x38 bytes) as its primary
 * dynamic container. Element type is a 0x10-byte pair used for JSON nodes and
 * network service state. These functions handle Reserve, Clear, and
 * Destroy-and-Reinit operations on such arrays.
 *
 * Layout reference (from Types.h RadBuffer):
 *   +0x00: void*    data_ptr       — element buffer
 *   +0x08: int64_t  byte_capacity  — (BufferContext field)
 *   +0x10: void*    allocator      — allocator instance
 *   +0x18: int32_t  alignment      — (BufferContext field)
 *   +0x1c: uint32_t flags          — (BufferContext field)
 *   +0x20: uint64_t min_grow       — minimum growth increment
 *   +0x28: uint64_t capacity       — allocated element slots
 *   +0x30: uint64_t count          — current element count
 */

#include <cstdint>
#include <cstddef>

namespace NRadEngine {

// CJsonNodePair: 0x10-byte element used in CArray containers throughout pnsrad.
// Initialized to {0, 0} by CJsonNodePair_Init.
// Destroyed by CJsonNodePair_Destroy which frees cached JSON DB and resets path.
//
// @confidence: H — from CNSUser, CNSIParty constructors/destructors
#pragma pack(push, 8)
struct CJsonNodePair {
    /* @offset: 0x00 */ void*   json_root; // JSON tree root (or null)
    /* @offset: 0x08 */ void*   json_db;   // Cached JSON database (freed on destroy)
};
#pragma pack(pop)
static_assert(sizeof(CJsonNodePair) == 0x10);
static_assert(offsetof(CJsonNodePair, json_root) == 0x00);
static_assert(offsetof(CJsonNodePair, json_db) == 0x08);

// @0x180097500 — CJsonNodePair::Init
// @confidence: H — zeros both fields. Called by CNSUser ctor, CArray<CJsonNodePair>::Reserve.
// Used in: CNSUser::CNSUser (4x), CNSIParty::CNSIParty,
//          CArray<CJsonNodePair>::Reserve @0x180082260
void CJsonNodePair_Init(CJsonNodePair* self);

// @0x180097650 — CJsonNodePair::Destroy
// @confidence: H — frees json_db via CJson::ReleaseCache, resets path via FUN_1800980d0.
// Used in: ~CNSUser, ~CNSIParty, CArray::Clear @0x180086920, CArray::DestroyReverse @0x18007f5d0
void CJsonNodePair_Destroy(CJsonNodePair* self);

// @0x18009ddf0 — CJsonNodePair_FreeDB (internal)
// @confidence: H — frees the JSON DB object at self->json_db if non-null.
// Acquires allocator lock, calls FUN_180097510 to clean up the hashtable,
// then frees via allocator vtable+0x30.
void CJsonNodePair_FreeDB(CJsonNodePair* self);

// @0x1800980d0 — CJsonNodePair_SetPath (JSON path setter)
// @confidence: H — if json_db is null, delegates to CJson::DeleteAtPath (json path set).
// If json_db is non-null (cached/read-only), logs error and aborts.
uint64_t CJsonNodePair_SetPath(CJsonNodePair* self, const char* path, uint32_t create_flag);

// @0x180082260 — CArray<CJsonNodePair>::Reserve
// @confidence: H — grows array buffer, initializes new elements with CJsonNodePair_Init.
// Algorithm:
//   old_count = count
//   if capacity < count + additional:
//     grow = max(min_grow, capacity, additional)
//     Realloc(data_ptr, (capacity + grow) * 0x10)
//     capacity += grow
//   count += additional
//   for i in [old_count, count): Init(data[i])
//   return old_count
uint64_t CArrayJsonNodePair_Reserve(void* array, uint64_t additional);

// @0x18007f5d0 — CArray<CJsonNodePair>::DestroyReverse
// @confidence: H — iterates elements in reverse calling CJsonNodePair_Destroy,
// then frees the buffer via allocator vtable+0x30.
// Used in: ~CNSIParty, ~CNSUser
void CArrayJsonNodePair_DestroyReverse(void* array);

// @0x180086920 — CArray<CJsonNodePair>::Clear
// @confidence: H — iterates elements forward calling CJsonNodePair_Destroy on non-null,
// then sets count to 0. Does NOT free the buffer (retains capacity).
// Acquires allocator lock around the loop.
void CArrayJsonNodePair_Clear(void* array);

// @0x18008c850 — CArray<CJsonNodePair>::DestroyAndReinit
// @confidence: H — destroys all elements in reverse, frees buffer,
// then allocates a new buffer of param_1 elements and initializes them.
// Replaces the allocator reference with param_2.
void CArrayJsonNodePair_DestroyAndReinit(void* array, int64_t new_count, void* new_allocator);

} // namespace NRadEngine

#endif // PNSRAD_CORE_CONTAINERS_H
