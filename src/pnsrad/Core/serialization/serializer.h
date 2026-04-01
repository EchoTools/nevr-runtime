#ifndef PNSRAD_CORE_SERIALIZATION_SERIALIZER_H
#define PNSRAD_CORE_SERIALIZATION_SERIALIZER_H

// @module: pnsrad.dll
// @source: d:\projects\rad\dev\src\engine\libs\io\cserializer.cpp
// @confidence: H — from Ghidra vtable analysis + error strings

#include <cstdint>

namespace NRadEngine {

class CString;

// CSerializer: abstract base class for type-directed serialization.
//
// Each virtual method handles one primitive type. The base implementation
// raises CBaseErr for every type, forcing subclasses to override only the
// types they support.
//
// Vtable layout (from Ghidra vfunction numbering):
//   vfunc1  — scalar deleting destructor
//   vfunc2  — SerializeString (CString)
//   vfunc3  — SerializeFloat
//   vfunc4  — SerializeUint8
//   vfunc5  — SerializeInt8
//   vfunc6  — SerializeUint16
//   vfunc7  — SerializeInt16
//   vfunc8  — SerializeUint24
//   vfunc9  — SerializeUint32
//   vfunc10 — SerializeInt32
//   vfunc11 — SerializeUint64
//   vfunc12 — SerializeInt64
//   vfunc13 — SerializeSoundSymbol (hex32)
//   vfunc14 — SerializeSymbol64 (hex64)
//   vfunc15 — SerializeCPointer
//   vfunc16 — SerializeFlag
//   vfunc20 — BeginObject
//   vfunc21 — EndObject
//   vfunc22 — BeginDocument

// @addr: 0x180095ca0 (~CSerializer), 0x180095cc0 (~CSerializer variant 2)
// @size: 0x10 (base only: vtable + mode)
class CSerializer {
public:
    // @0x180095ca0
    virtual ~CSerializer();

    // @0x1800a3c20 (base raises error: "can't serialize CString \"%s\"")
    virtual void SerializeString(CString* value);

    // @0x1800a3b60 (base raises error: "can't serialize real \"%f\"")
    virtual void SerializeFloat(float* value, uint64_t param2, uint64_t param3);

    // @0x1800a3a70 (base raises error: "can't serialize uint8 \"%d\"")
    virtual void SerializeUint8(uint8_t* value, uint64_t param2, uint64_t param3);

    // @0x1800a3a40 (base raises error: "can't serialize int8 \"%d\"")
    virtual void SerializeInt8(int8_t* value, uint64_t param2, uint64_t param3);

    // @0x1800a3ad0 (base raises error: "can't serialize uint16 \"%d\"")
    virtual void SerializeUint16(uint16_t* value, uint64_t param2, uint64_t param3);

    // @0x1800a3aa0 (base raises error: "can't serialize int16 \"%d\"")
    virtual void SerializeInt16(int16_t* value, uint64_t param2, uint64_t param3);

    // @0x1800a3ba0 (base raises error: "can't serialize uint24 \"%u\"")
    virtual void SerializeUint24(uint32_t* value, uint64_t param2, uint64_t param3);

    // @0x1800a3b30 (base raises error: "can't serialize uint32 \"%u\"")
    virtual void SerializeUint32(uint32_t* value, uint64_t param2, uint64_t param3);

    // @0x1800a3b00 (base raises error: "can't serialize int32 \"%d\"")
    virtual void SerializeInt32(int32_t* value, uint64_t param2, uint64_t param3);

    // @0x1800a3cd0 (base raises error: "can't serialize uint64 \"%llu\"")
    virtual void SerializeUint64(uint64_t* value, uint64_t param2, uint64_t param3);

    // @0x1800a3ca0 (base raises error: "can't serialize int64 \"%lld\"")
    virtual void SerializeInt64(int64_t* value, uint64_t param2, uint64_t param3);

    // @0x1800a3bf0 (base raises error: "can't serialize CSoundSymbol \"%#x\"")
    virtual void SerializeSoundSymbol(uint32_t* value, uint64_t param2, uint64_t param3);

    // @0x1800a3c70 (base raises error: "can't serialize symbol \"%#llx\"")
    virtual void SerializeSymbol64(uint64_t* value, uint64_t param2, uint64_t param3);

    // @0x1800a3d00 (base raises error: "can't serialize cpointer \"%p\"")
    virtual void SerializeCPointer(int64_t count, void** data);

    // @0x1800a3d30 (base raises error: "can't serialize flag \"%u\"")
    virtual void SerializeFlag(uint32_t* value, uint64_t param2, uint64_t param3);

    // vfunc17-19: not present in serializer classes (gap in numbering)

    // @0x1800a4070 (CTextStream), @0x1800a4960 (CJsonStream)
    virtual uint64_t BeginObject(uint64_t name);

    // @0x1800a4040 (CTextStream)
    virtual void EndObject();

    // @0x1800a3f50 (CTextStream), @0x1800a4940 (CJsonStream)
    virtual void BeginDocument();

    uint64_t m_mode; // +0x08: 1 = write mode
};

} // namespace NRadEngine

#endif // PNSRAD_CORE_SERIALIZATION_SERIALIZER_H
