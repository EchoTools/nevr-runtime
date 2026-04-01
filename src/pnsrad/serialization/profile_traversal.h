#ifndef PNSRAD_SERIALIZATION_PROFILE_TRAVERSAL_H
#define PNSRAD_SERIALIZATION_PROFILE_TRAVERSAL_H

// @module: pnsrad.dll
// @purpose: CProfileJsonTraversal — JSON traversal that copies data to a target CJson.
//
// CProfileJsonTraversal is a CJsonTraversal subclass used during user profile
// synchronization. Its only override (ProcessObject / vfunction5 @ 0x18008fa00)
// copies the source JSON field value into the target CJson using CJson_SetValue.
//
// It is always constructed inline on the stack. Usage pattern:
//   1. Set vtable to CProfileJsonTraversal::vftable
//   2. Call CJsonTraversal_SetFlags to configure write-through
//   3. Call CJsonTraversal_Execute to walk the source and copy fields to target
//
// Seen in:
//   - CNSUser profile update success handler @ 0x18008fcd0
//   - CNSUser reconnect profile reload @ 0x18008fcd0 (demo branch)
//
// @vtable: NRadEngine::CProfileJsonTraversal::vftable (referenced in decompilation)

#include "pnsrad/serialization/json_traversal.h"

namespace NRadEngine {

// @size: 0x20 (same as CJsonTraversal — no additional fields)
// @confidence: H
struct CProfileJsonTraversal : CJsonTraversal {
    // No additional fields. The vtable provides the CopyData override.
};
static_assert(sizeof(CProfileJsonTraversal) == 0x20, "CProfileJsonTraversal must be 0x20 bytes");

// @addr: 0x18008fa00 (pnsrad.dll)
// @original: NRadEngine::CProfileJsonTraversal::CopyData (vfunction5)
// Copies the value at a JSON path from the source document into the target
// document using CJson_SetValue (FUN_1800980d0).
// @confidence: H
void CProfileJsonTraversal_CopyData(CProfileJsonTraversal* self, void* srcPath, uint64_t unused1, uint64_t unused2);

} // namespace NRadEngine

#endif // PNSRAD_SERIALIZATION_PROFILE_TRAVERSAL_H
