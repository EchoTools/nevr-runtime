// @module: pnsrad.dll
// @purpose: CJsonTraversal — JSON document traversal engine implementation.
//
// These are the core engine functions that drive JSON traversal. They walk a
// CJson document tree and dispatch to the CTraversal visitor's virtual methods.
//
// Source: d:\projects\rad\dev\src\engine\libs\json

#include "pnsrad/serialization/json_traversal.h"

#include <cstdint>

namespace NRadEngine {

// @0x18009f600 — Checks if a path exists in the JSON document.
extern bool CJson_HasNode(void* json, const char* path);

// @0x18009ef60 — Returns the cJSON node type at `path`.
// Return values map to CTraversal vtable slots:
//   0=object→+0x20, 1=array→+0x18, 2=string→+0x08,
//   3=integer→+0x10, 4/5=bool→+0x00, 6=null→+0x30
extern int64_t CJson_GetNodeType(void* json, const char* path);

// @0x1800a1040 — Post-field-visit sync, called when flags bit 0 is set.
extern void JsonEngine_Flush(int param);

extern const char g_emptyString; // @0x180222db4

// Dispatches to the correct vtable method based on cJSON node type, then
// optionally flushes. Shared by Execute (root walk) and VisitField (per-field).
static void DispatchByNodeType(CJsonTraversal* self, const char* key, const char* path) {
    int64_t nodeType = CJson_GetNodeType(self->target, path);

    switch (static_cast<int>(nodeType)) {
    case 0: self->vtable->ProcessObject(self, key, path);       break;
    case 1: self->vtable->ProcessArray(self, key, path);        break;
    case 2: self->vtable->ProcessString(self, key, path);       break;
    case 3: self->vtable->ProcessInt(self, key, path);          break;
    case 4: self->vtable->ProcessBoolean(self, key, path);      break;
    case 5: self->vtable->ProcessArrayElement(self, key, path); break;
    case 6: self->vtable->ProcessNull(self, key, path);         break;
    }

    if (self->flags & 0x01) {
        JsonEngine_Flush(0);
    }
}

// @addr: 0x18009fc30 (pnsrad.dll)
// @original: NRadEngine::CJsonTraversal::SetFlags
// Configures traversal for standard write-through mode.
// @confidence: H
void* CJsonTraversal_SetFlags(CJsonTraversal* self) {
    self->flags &= ~0x04ULL;
    self->flags |= 0x02ULL;
    return self;
}

// @addr: 0x18009ede0 (pnsrad.dll)
// @original: NRadEngine::CJsonTraversal::Execute
// Sets source and target, walks the root node, dispatches by type.
// @confidence: H
void CJsonTraversal_Execute(CJsonTraversal* self, CJson* source, void* target) {
    self->source = source;
    self->target = target;

    if (!CJson_HasNode(target, "")) {
        return;
    }

    DispatchByNodeType(self, &g_emptyString, &g_emptyString);
}

// @addr: 0x18009ef20 (pnsrad.dll)
// @original: NRadEngine::CJsonTraversal::ExecuteNamed
// Sets source and target, begins traversal from a named field.
// @confidence: H
void CJsonTraversal_ExecuteNamed(CJsonTraversal* self, CJson* source, void* target, const char* field) {
    self->source = source;
    self->target = target;

    if (!CJson_HasNode(target, field)) {
        return;
    }

    CJsonTraversal_VisitField(self, field, field);
}

// @addr: 0x18009c3f0 (pnsrad.dll)
// @original: NRadEngine::CJsonTraversal::VisitField
// Strips leading '|' path separator, determines node type, dispatches.
// @confidence: H
void CJsonTraversal_VisitField(CJsonTraversal* self, const char* key, const char* path) {
    const char* cleanPath = (*path == '|') ? path + 1 : path;
    const char* cleanKey  = (*key == '|')  ? key + 1  : key;

    DispatchByNodeType(self, cleanKey, cleanPath);
}

} // namespace NRadEngine
