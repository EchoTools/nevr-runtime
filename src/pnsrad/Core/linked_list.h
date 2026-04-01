#ifndef PNSRAD_CORE_LINKED_LIST_H
#define PNSRAD_CORE_LINKED_LIST_H

/* @module: pnsrad.dll */
/* @purpose: CLinkedList<T> — vtable-polymorphic array-of-T container.
 *
 * Despite the name, CLinkedList in the RAD Engine is an array-backed container
 * with virtual dispatch for destruction. Each instantiation gets its own vtable.
 * The "linked list" name likely refers to the engine's internal type registry
 * naming, not the data structure.
 *
 * Evidence:
 *   - CLinkedList<CPresence> vtable at 0x180373078 (pnsrad.dll)
 *   - Constructor/static-init at 0x180221e70 sets vftable ptr
 *   - Virtual destructor at 0x1800a5180 resets vftable and optionally frees
 *   - Quest ARM64 CLinkedListInit at 0x1401d5170 zeros 4 qwords then sets allocator
 *   - Quest CLinkedListModHeader_Clear at 0x14045f170 iterates elements calling dtors
 *   - CSceneGraph.h documents layout: vftable + head + tail (0x18 bytes)
 *   - CR15NetPhysicsCS documents: vftable + head + tail + count (0x20 bytes)
 *
 * Layout (Quest evidence, 0x20 bytes):
 *   +0x00: void* allocator    — TLS allocator instance
 *   +0x08: T*    data         — element buffer
 *   +0x10: int64_t capacity   — allocated element slots
 *   +0x18: int64_t count      — current element count
 *
 * Layout (pnsrad vtable variant, 0x18 bytes minimum):
 *   +0x00: void* vftable      — per-instantiation vtable
 *   +0x08: T*    data         — element buffer (or head pointer)
 *   +0x10: int64_t count      — element count (or tail pointer)
 */

#include <cstdint>
#include <cstddef>

namespace NRadEngine {

// CLinkedListBase: non-templated base with allocator-managed storage.
// Matches the Quest CLinkedListInit layout at @0x1401d5170.
//
// @addr: 0x1401d5170 (Quest CLinkedListInit)
// @confidence: H — layout from Quest decompilation + CR15NetPhysicsCS reconstruction
#pragma pack(push, 8)
struct CLinkedListBase {
    /* @offset: 0x00 */ void*   allocator; // TLS allocator (set by Init)
    /* @offset: 0x08 */ void*   data;      // element buffer
    /* @offset: 0x10 */ int64_t capacity;  // allocated slots
    /* @offset: 0x18 */ int64_t count;     // current element count
};
#pragma pack(pop)
static_assert(sizeof(CLinkedListBase) == 0x20);
static_assert(offsetof(CLinkedListBase, allocator) == 0x00);
static_assert(offsetof(CLinkedListBase, data) == 0x08);
static_assert(offsetof(CLinkedListBase, capacity) == 0x10);
static_assert(offsetof(CLinkedListBase, count) == 0x18);

// @0x1401d5170 — CLinkedList::Init [Quest: confirmed]
// Zeros all fields, frees any existing allocation via allocator vtable+0x30,
// then assigns TLS allocator to +0x00.
void CLinkedListBase_Init(CLinkedListBase* self);

// CLinkedListVT<T>: vtable-bearing variant used when the container is
// embedded in a polymorphic object (e.g., CSceneGraph camera list).
//
// pnsrad.dll uses this for CLinkedList<CPresence>.
// The vtable provides a virtual destructor that can clean up elements.
//
// @addr: 0x180221e70 (static init), 0x1800a5180 (virtual dtor)
// @confidence: H — from Ghidra RTTI + decompilation
#pragma pack(push, 8)
template <typename T>
struct CLinkedListVT {
    /* @offset: 0x00 */ void*   vftable;  // per-instantiation vtable
    /* @offset: 0x08 */ T*      data;     // element buffer (head)
    /* @offset: 0x10 */ int64_t count;    // element count (tail)
};
#pragma pack(pop)

// @0x1800a5180 — CLinkedList<CPresence>::~CLinkedList (virtual dtor)
// @confidence: H
// Sets vftable back to CLinkedList<CPresence>::vftable.
// If (param_1 & 1), calls operator delete (scalar deleting destructor pattern).

// @0x180221e70 — CLinkedList<CPresence>::CLinkedList (static init / ctor)
// @confidence: H
// Sets PTR_vftable_180373078 = &CLinkedList<CPresence>::vftable.

} // namespace NRadEngine

#endif // PNSRAD_CORE_LINKED_LIST_H
