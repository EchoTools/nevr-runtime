#ifndef PNSRAD_CORE_JSON_CJSONTRAVERSAL_H
#define PNSRAD_CORE_JSON_CJSONTRAVERSAL_H

/* @module: pnsrad.dll */
/* @purpose: CJsonTraversal — visitor pattern for JSON tree synchronization */
/* @note: Used by CNSUser::CTraversal and CProfileJsonTraversal to diff/merge
 *        JSON trees (e.g., syncing user profile data from server to local store).
 *        The traversal walks the source tree and calls typed virtual handlers
 *        (bool, int, real, string, array, object) on each leaf node. */

#include <cstdint>
#include "src/pnsrad/Core/json/CJson.h"

namespace NRadEngine {

// ============================================================================
// CJsonTraversal vtable layout
// ============================================================================
// +0x00: vfunction0 — handle boolean value (type 4)
// +0x08: vfunction1 — handle string value (type 1)  [vfunction4 in Ghidra]
// +0x10: vfunction2 — handle real/float value (type 3)  [vfunction3 in Ghidra]
// +0x18: vfunction3 — handle integer value (type 2)  [vfunction2 in Ghidra]
// +0x20: vfunction4 — handle object begin (type 6)  [vfunction1 in Ghidra]
// +0x28: vfunction5 — handle array begin (type 5)
// +0x30: vfunction6 — handle dict/key iteration (type 6)

// ============================================================================
// CJsonTraversal — base visitor for JSON tree operations
// ============================================================================
// Layout: 0x20 bytes per instance
//   +0x00: vtable pointer (CJsonTraversal_vftable*)
//   +0x08: CJson* destination (the JSON store being written to)
//   +0x10: CJson* source (the JSON store being read from)
//   +0x18: uint64_t flags (bit 0 = trigger post-processing after dispatch)

/* @addr: vtable at various locations */
/* @size: 0x20 */
/* @confidence: H */
struct CJsonTraversal {
    void*    vtable;    // +0x00: vtable pointer
    CJson*   dst;       // +0x08: destination JSON
    CJson*   src;       // +0x10: source JSON
    uint64_t flags;     // +0x18: control flags
};
static_assert(sizeof(CJsonTraversal) == 0x20);
static_assert(offsetof(CJsonTraversal, vtable) == 0x00);
static_assert(offsetof(CJsonTraversal, dst) == 0x08);
static_assert(offsetof(CJsonTraversal, src) == 0x10);
static_assert(offsetof(CJsonTraversal, flags) == 0x18);

// ============================================================================
// Dispatch function — routes to vtable handler based on node type
// ============================================================================

// @0x18009c3f0 — CJsonTraversal::Dispatch
// Determines the type of the source node at `src_path`, then calls the
// corresponding vtable handler with both the dst_path and src_path.
// Type mapping: 0→vfn4(object +0x20), 1→vfn3(string +0x18), 2→vfn1(int +0x08),
//               3→vfn2(real +0x10), 4→vfn0(bool +0x00), 5→vfn5(array +0x28),
//               6→vfn6(dict +0x30)
// If flags bit 0 is set, calls post_traversal_hook (@0x1800a1040) after dispatch.
// @confidence: H
void CJsonTraversal_Dispatch(CJsonTraversal* trav, const char* dst_path, const char* src_path);

// @0x18009ede0 — CJsonTraversal::TraverseRoot
// Stores dst_json and src_json, checks PathExists(""), then dispatches on root ("").
// Entry point for traversal of an entire JSON tree.
// param_1 (unknown) is stored directly into trav->dst (+0x08).
// @confidence: H
void CJsonTraversal_TraverseRoot(CJsonTraversal* trav, CJson* dst_json, CJson* src_json);

// @0x18009ef20 — CJsonTraversal::TraverseFromPath
// Stores dst_json and src_json, checks PathExists(path), then dispatches on `path`.
// @confidence: H
void CJsonTraversal_TraverseFromPath(CJsonTraversal* trav, CJson* dst_json, CJson* src_json, const char* path);

// @0x18009fc30 — CJsonTraversal::SetCopyMode
// Clears flag bit 2, sets flag bit 1. Used to switch traversal to copy mode.
// @confidence: H
void* CJsonTraversal_SetCopyMode(CJsonTraversal* trav);

// ============================================================================
// Default vtable implementations (inherited by CNSUser::CTraversal et al.)
// ============================================================================

// @0x18009C6C0 — vfunction1: handle boolean
// Reads boolean from source, writes to destination via CJson_SetBoolean.
// @confidence: H
void CJsonTraversal_HandleBoolean(CJsonTraversal* trav, const char* dst_path,
                                   const char* src_path, void* error_ctx);

// @0x18009C740 — vfunction2: handle integer
// Reads integer from source tree, writes to destination.
// Complex inline path navigation with TLS locking.
// @confidence: H
void CJsonTraversal_HandleInteger(CJsonTraversal* trav, const char* dst_path,
                                   const char* src_path, const char* error_ctx);

// @0x18009CFD0 — vfunction3: handle real/float
// Reads real value from source, writes to destination via CJson_SetReal.
// Also handles integer→double promotion for mixed-type nodes.
// @confidence: H
void CJsonTraversal_HandleReal(CJsonTraversal* trav, const char* dst_path,
                                const char* src_path, const char* error_ctx);

// @0x18009D690 — vfunction4: handle string
// Reads string from source, writes to destination via CJson_SetString.
// @confidence: H
void CJsonTraversal_HandleString(CJsonTraversal* trav, const char* dst_path,
                                  const char* src_path, const char* error_ctx);

// @0x18009C4F0 — vfunction6: handle array iteration
// Iterates array elements in source, dispatches on each element.
// Optionally iterates matching destination array in parallel (copy mode).
// If (flags&6)==2 (copy mode) OR TypeOf(dst,path)!=5, calls SetValue first.
// If (flags&6)==4, parallel-iterates the destination array.
// @confidence: H
void CJsonTraversal_HandleArray(CJsonTraversal* trav, const char* dst_path,
                                 const char* src_path, void* error_ctx);

// @0x18009CDD0 — vfunction7: handle object key iteration
// Uses CJson::FindNextArrayElement to get object iterator, FUN_1801c31c0 to get key name.
// Iterates object keys in source, builds "%s|%s" paths, dispatches on each key.
// @confidence: H
void CJsonTraversal_HandleObject(CJsonTraversal* trav, const char* dst_path,
                                  const char* src_path, void* error_ctx);

} // namespace NRadEngine

#endif // PNSRAD_CORE_JSON_CJSONTRAVERSAL_H
