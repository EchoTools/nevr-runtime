// @module: pnsrad.dll
// @purpose: CProfileJsonTraversal — Profile copy traversal implementation.
//
// CProfileJsonTraversal's single override copies JSON values from source to
// target during profile synchronization.

#include "pnsrad/serialization/profile_traversal.h"

#include <cstdint>

namespace NRadEngine {

// @0x1800980d0 — Sets a value at a path in a CJson document.
extern int CJson_SetValue(void* json, void* value, uint32_t mode, ...);

// @addr: 0x18008fa00 (pnsrad.dll)
// @original: NRadEngine::CProfileJsonTraversal::CopyData (vfunction5)
// Copies source value into self->source (the target profile CJson).
// Decompilation: FUN_1800980d0(this[1].vftablePtr, param_1, 0, param_3)
// where this[1].vftablePtr is CJson* at +0x08 (the source field).
// @confidence: H
void CProfileJsonTraversal_CopyData(CProfileJsonTraversal* self, void* srcPath, uint64_t, uint64_t) {
    CJson_SetValue(self->source, srcPath, 0);
}

} // namespace NRadEngine
