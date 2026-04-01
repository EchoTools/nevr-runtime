#ifndef PNSRAD_CORE_SERIALIZATION_JSON_SERIALIZER_H
#define PNSRAD_CORE_SERIALIZATION_JSON_SERIALIZER_H

// @module: pnsrad.dll
// @source: d:\projects\rad\dev\src\engine\libs\io\cserializer.cpp
// @confidence: H — from Ghidra decompilations at 0x1800a4710..0x1800a4f20

#include "src/pnsrad/Core/serialization/stream_serializer.h"
#include <cstdint>

namespace NRadEngine {

class CStream;

// CJsonStreamSerializerWrite: writes JSON-formatted serialization to a CStream.
//
// Tracks nesting depth to emit commas between array/object elements.
// The m_depth field (at +0x150) tracks the current array element count;
// WriteSeparator() (at 0x1800a4dd0) emits commas and element separators
// between consecutive values.
//
// Output format:
//   Integers: bare numbers (e.g. "42")
//   Strings: quoted (e.g. "\"hello\"")
//   Hex values: quoted hex (e.g. "\"0x1a\"")
//   Floats: bare decimal (e.g. "3.140000")
//
// Layout (identical to CTextStreamSerializerWrite):
//   +0x00: vtable*
//   +0x08: uint64_t m_mode (1 = write)
//   +0x10: CStream* m_stream
//   +0x18: name stack buffer context
//   +0x38: uint64_t m_initial_capacity (= 0x20)
//   +0x40: uint64_t m_stack_capacity
//   +0x48: uint64_t m_stack_count
//   +0x50: char m_field_name[256]
//   +0x150: uint64_t m_depth (element counter for comma insertion)
//
// @addr: vtable at 0x1802ad4a0 (CJsonStreamSerializerWrite::vftable)
// @size: 0x158
class CJsonStreamSerializerWrite : public CStreamSerializer {
public:
    // @0x1800a4710
    CJsonStreamSerializerWrite(CStream* stream);

    // @0x1800a4870
    virtual ~CJsonStreamSerializerWrite() override;

    // @0x1800a4c30
    virtual void SerializeString(CString* value) override;

    // @0x1800a4b50
    virtual void SerializeFloat(float* value, uint64_t param2, uint64_t param3) override;

    // @0x1800a4aa0
    virtual void SerializeUint8(uint8_t* value, uint64_t param2, uint64_t param3) override;

    // @0x1800a4a90
    virtual void SerializeInt8(int8_t* value, uint64_t param2, uint64_t param3) override;

    // @0x1800a4ac0
    virtual void SerializeUint16(uint16_t* value, uint64_t param2, uint64_t param3) override;

    // @0x1800a4ab0
    virtual void SerializeInt16(int16_t* value, uint64_t param2, uint64_t param3) override;

    // SerializeUint24 — inherited error stub from CSerializer (@0x1800a3ba0)

    // @0x1800a4ae0 — with WriteSeparator
    virtual void SerializeUint32(uint32_t* value, uint64_t param2, uint64_t param3) override;

    // @0x1800a4ad0 — deref + delegate to WriteInt32
    virtual void SerializeInt32(int32_t* value, uint64_t param2, uint64_t param3) override;

    // @0x1800a4d50 (format: "%llu")
    virtual void SerializeUint64(uint64_t* value, uint64_t param2, uint64_t param3) override;

    // @0x1800a4ce0 (format: "%lld")
    virtual void SerializeInt64(int64_t* value, uint64_t param2, uint64_t param3) override;

    // @0x1800a4bc0 (format: "\"%#x\"")
    virtual void SerializeSoundSymbol(uint32_t* value, uint64_t param2, uint64_t param3) override;

    // @0x1800a4c70 (format: "\"%#llx\"")
    virtual void SerializeSymbol64(uint64_t* value, uint64_t param2, uint64_t param3) override;

    // @0x1800a4dc0
    virtual void SerializeCPointer(int64_t count, void** data) override;

    // SerializeFlag — inherited error stub from CSerializer (@0x1800a3d30)

    // @0x1800a4960 — writes field name as JSON key, updates nesting
    virtual uint64_t BeginObject(uint64_t name) override;

    // EndObject — not overridden in JSON serializer (uses base no-op)

    // @0x1800a4940 — zeroes field name buffer
    virtual void BeginDocument() override;

private:
    // @0x1800a4ea0 — shared int formatting with WriteSeparator
    void WriteInt32(uint64_t value, uint64_t param2, uint64_t param3);

    // @0x1800a4f20 — write quoted string value with WriteSeparator
    void WriteQuotedString(const char* value);

    // @0x1800a4dd0 — emit comma/separator between JSON elements
    // Compares m_stack_count vs m_depth to decide separator type:
    //   m_depth < m_stack_count → write "}\n" (end nested, data at 0x1802ad598)
    //   m_depth > m_stack_count → write ",\n" (next element, data at 0x1802ad59c)
    //   m_depth == m_stack_count → write "]\n" (end array, data at 0x1802ad5a4)
    // Then updates m_depth, writes field name + ":\n" (data at 0x1802ad5a8)
    // Returns 0 on success.
    int32_t WriteSeparator();

    // Data layout (same as CTextStreamSerializerWrite)
    uint8_t  m_name_stack[0x20]; // +0x18
    uint64_t m_initial_capacity; // +0x38
    uint64_t m_stack_capacity;   // +0x40
    uint64_t m_stack_count;      // +0x48
    char     m_field_name[256];  // +0x50
    uint64_t m_depth;            // +0x150: element counter for separator logic
};

} // namespace NRadEngine

#endif // PNSRAD_CORE_SERIALIZATION_JSON_SERIALIZER_H
