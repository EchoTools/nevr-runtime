#ifndef PNSRAD_SERIALIZATION_JSON_TRAVERSAL_H
#define PNSRAD_SERIALIZATION_JSON_TRAVERSAL_H

// @module: pnsrad.dll
// @purpose: CJsonTraversal — JSON document traversal engine.
//
// CJsonTraversal is the core traversal engine that walks a CJson document tree
// and dispatches to a CTraversal visitor's virtual methods for each node. It
// determines node types (object, array, string, integer, boolean, null) and
// calls the corresponding vtable slot on the CTraversal.
//
// Three main entry points:
//   Execute()      — walks the root of a JSON document        (@0x18009ede0)
//   ExecuteNamed() — walks starting from a named field        (@0x18009ef20)
//   VisitField()   — visits a single field (recursive helper) (@0x18009c3f0)
//
// CJsonTraversal is used as a stack-allocated inline object. Its "constructor"
// (SetFlags @ 0x18009fc30) just sets flag bits, no heap allocation.

#include "pnsrad/serialization/traversal.h"

namespace NRadEngine {

// CJsonTraversal — wraps a CTraversal and drives the JSON walk.
//
// The CJsonTraversal IS a CTraversal (first member is the CTraversal base).
// In practice, the vtable pointer at +0x00 points to a CJsonTraversal-specific
// vtable (or a derived class vtable like CNSUser::CTraversal::vftable).
//
// SetFlags (@0x18009fc30) clears bit 2, sets bit 1 in the flags field at +0x18.
// Execute/ExecuteNamed populate source (+0x08) and target (+0x10) then walk.
//
// @size: 0x20 (same as CTraversal — no additional fields)
// @confidence: H
struct CJsonTraversal : CTraversal {
    // No additional fields beyond CTraversal base.
    // The flags field at +0x18 controls traversal behavior:
    //   bit 0: if set, call FUN_1800a1040(0) after each field visit (flush/sync)
    //   bit 1: write-mode (set by SetFlags)
    //   bit 2: cleared by SetFlags
};
static_assert(sizeof(CJsonTraversal) == 0x20, "CJsonTraversal must be 0x20 bytes");

// --- CJsonTraversal free functions (engine-level) ---

// @addr: 0x18009fc30 (pnsrad.dll)
// @original: NRadEngine::CJsonTraversal::SetFlags
// Initializes traversal flags: clears bit 2, sets bit 1.
// Returns `this` for chaining.
// @confidence: H
void* CJsonTraversal_SetFlags(CJsonTraversal* self);

// @addr: 0x18009ede0 (pnsrad.dll)
// @original: NRadEngine::CJsonTraversal::Execute
// Sets source and target, then walks the root of the target JSON document,
// dispatching each field to the visitor's vtable based on node type.
// @confidence: H
void CJsonTraversal_Execute(CJsonTraversal* self, CJson* source, void* target);

// @addr: 0x18009ef20 (pnsrad.dll)
// @original: NRadEngine::CJsonTraversal::ExecuteNamed
// Like Execute but begins traversal from a named field within the document.
// @confidence: H
void CJsonTraversal_ExecuteNamed(CJsonTraversal* self, CJson* source, void* target, const char* field);

// @addr: 0x18009c3f0 (pnsrad.dll)
// @original: NRadEngine::CJsonTraversal::VisitField
// Visits a single field: determines node type via GetNodeType, then dispatches
// to the appropriate vtable method. If bit 0 of flags is set, calls
// FUN_1800a1040(0) for sync/flush after dispatch.
// @confidence: H
void CJsonTraversal_VisitField(CJsonTraversal* self, const char* key, const char* path);

} // namespace NRadEngine

#endif // PNSRAD_SERIALIZATION_JSON_TRAVERSAL_H
