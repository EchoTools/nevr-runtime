#ifndef PNSRAD_SERIALIZATION_TRAVERSAL_H
#define PNSRAD_SERIALIZATION_TRAVERSAL_H

// @module: pnsrad.dll
// @purpose: CTraversal — Abstract base for JSON field visitor pattern.
//
// CTraversal is a virtual dispatch interface for walking JSON key-value pairs.
// CJsonTraversal::Execute drives the walk by determining each node's type
// (via FUN_18009ef60 / GetNodeType) then dispatching to the appropriate vtable
// slot. Subclasses override these slots to handle each field type.
//
// Vtable slot dispatch map (from CJsonTraversal::Execute @ 0x18009ede0 and
// CJsonTraversal::VisitField @ 0x18009c3f0):
//   GetNodeType returns → vtable offset called
//   0 (object)          → +0x20  ProcessObject
//   1 (array)           → +0x18  ProcessArray
//   2 (string)          → +0x08  ProcessString
//   3 (integer)         → +0x10  ProcessInt
//   4 (bool true/false) → +0x00  ProcessBoolean
//   5 (array element)   → +0x28  ProcessArrayElement
//   6 (null/root)       → +0x30  ProcessNull
//
// RTTI: "?AVCTraversal@CNSUser@NRadEngine@@" at 0x142064d18 (echovr.exe)
// Source: d:\projects\rad\dev\src\engine\libs\netservice\cnsuser.cpp

#include <cstdint>

namespace NRadEngine {

class CJson;

// @addr: vtable referenced in CJsonTraversal::Execute @ 0x18009ede0
// @size: 0x20 (base: vtable + source + target + flags)
// @confidence: H
struct CTraversal {
    struct VTable {
        void (*ProcessBoolean)(CTraversal* self, const char* key, const char* value, ...); // +0x00
        void (*ProcessString)(CTraversal* self, const char* key, const char* value, ...);  // +0x08
        void (*ProcessInt)(CTraversal* self, const char* key, const char* value, ...);     // +0x10
        void (*ProcessArray)(CTraversal* self, const char* key, const char* value, ...);   // +0x18
        void (*ProcessObject)(CTraversal* self, const char* key, const char* value, ...);  // +0x20
        void (*ProcessArrayElement)(CTraversal* self, const char* key, const char* value, ...); // +0x28
        void (*ProcessNull)(CTraversal* self, const char* key, const char* value, ...);    // +0x30
    };

    /* @offset: 0x00 */ VTable*   vtable;
    /* @offset: 0x08 */ CJson*    source;    // JSON data being traversed
    /* @offset: 0x10 */ void*     target;    // target JSON or receiver object
    /* @offset: 0x18 */ uint64_t  flags;     // bit 0: recursive, bit 1: write-mode, bit 2: ?
};
static_assert(sizeof(CTraversal) == 0x20, "CTraversal must be 0x20 bytes");

} // namespace NRadEngine

#endif // PNSRAD_SERIALIZATION_TRAVERSAL_H
