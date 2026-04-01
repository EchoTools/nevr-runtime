#ifndef PNSRAD_CORE_SERIALIZATION_STREAM_SERIALIZER_H
#define PNSRAD_CORE_SERIALIZATION_STREAM_SERIALIZER_H

// @module: pnsrad.dll
// @source: d:\projects\rad\dev\src\engine\libs\io\cserializer.cpp
// @confidence: H — from Ghidra constructor decompilations

#include "src/pnsrad/Core/serialization/serializer.h"
#include <cstdint>

namespace NRadEngine {

class CStream;

// CStreamSerializer: intermediate class that binds a CSerializer to a CStream.
//
// Layout (from constructors at 0x1800a3d60 and 0x1800a4710):
//   +0x00: vtable*
//   +0x08: uint64_t m_mode (inherited from CSerializer, set to 1 for write)
//   +0x10: CStream* m_stream
//
// The CStreamSerializer vtable inherits all error-raising stubs from CSerializer.
// Concrete subclasses (CTextStreamSerializerWrite, CJsonStreamSerializerWrite)
// override the type methods to format and write through m_stream.
//
// @addr: vtable at 0x1802ad238 (CStreamSerializer::vftable)
// @size: 0x18
class CStreamSerializer : public CSerializer {
public:
    // @0x1800a3e10 — scalar deleting destructor
    // Sets vtable back to CSerializer::vftable, then optionally deletes.
    virtual ~CStreamSerializer() override;

    CStream* m_stream; // +0x10
};

} // namespace NRadEngine

#endif // PNSRAD_CORE_SERIALIZATION_STREAM_SERIALIZER_H
