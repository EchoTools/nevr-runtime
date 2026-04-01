#ifndef PNSRAD_CORE_SERIALIZATION_TEXT_SERIALIZER_H
#define PNSRAD_CORE_SERIALIZATION_TEXT_SERIALIZER_H

// @module: pnsrad.dll
// @source: d:\projects\rad\dev\src\engine\libs\io\cserializer.cpp
// @confidence: H — from Ghidra decompilations at 0x1800a3d60..0x1800a4670

#include "src/pnsrad/Core/serialization/stream_serializer.h"
#include <cstdint>

namespace NRadEngine {

class CStream;

// CTextStreamSerializerWrite: writes human-readable text serialization to a CStream.
//
// Output format (when m_has_field_name is set):
//   "field_name: value\n"
// Output format (when m_has_field_name is not set):
//   "value\n"
//
// BeginObject pushes the current object name onto a stack and increases
// indentation depth. EndObject pops and decreases depth.
//
// Layout:
//   +0x00: vtable*
//   +0x08: uint64_t m_mode (1 = write)
//   +0x10: CStream* m_stream
//   +0x18: name stack buffer context (initialized by init_buffer_context)
//          Contains: data_ptr (+0x18), flags (+0x34), capacity_initial (+0x38),
//                    capacity (+0x40), count (+0x48)
//   +0x50: char m_field_name[256] — current field name buffer
//   +0x150: int32_t m_has_field_name — nonzero when a field name is pending
//
// @addr: vtable at 0x1802ad320 (CTextStreamSerializerWrite::vftable)
// @size: 0x158 (0x150 + 4 aligned to 8 = 0x158)
class CTextStreamSerializerWrite : public CStreamSerializer {
public:
    // @0x1800a3d60
    CTextStreamSerializerWrite(CStream* stream, int32_t has_field_name);

    // @0x1800a3e40
    virtual ~CTextStreamSerializerWrite() override;

    // @0x1800a43e0
    virtual void SerializeString(CString* value) override;

    // @0x1800a42c0
    virtual void SerializeFloat(float* value, uint64_t param2, uint64_t param3) override;

    // @0x1800a41f0
    virtual void SerializeUint8(uint8_t* value, uint64_t param2, uint64_t param3) override;

    // @0x1800a41e0
    virtual void SerializeInt8(int8_t* value, uint64_t param2, uint64_t param3) override;

    // @0x1800a4210
    virtual void SerializeUint16(uint16_t* value, uint64_t param2, uint64_t param3) override;

    // @0x1800a4200
    virtual void SerializeInt16(int16_t* value, uint64_t param2, uint64_t param3) override;

    // SerializeUint24 — inherited error stub from CSerializer (@0x1800a3ba0)

    // @0x1800a4230 (format: "%u\n" or "%s: %u\n")
    virtual void SerializeUint32(uint32_t* value, uint64_t param2, uint64_t param3) override;

    // @0x1800a4220 (deref + delegate to WriteInt32)
    virtual void SerializeInt32(int32_t* value, uint64_t param2, uint64_t param3) override;

    // @0x1800a4540 (format: "%llu\n" or "%s: %llu\n")
    virtual void SerializeUint64(uint64_t* value, uint64_t param2, uint64_t param3) override;

    // @0x1800a44b0 (format: "%lld\n" or "%s: %lld\n")
    virtual void SerializeInt64(int64_t* value, uint64_t param2, uint64_t param3) override;

    // @0x1800a4350 (format: "%#x\n" or "%s: %#x\n")
    virtual void SerializeSoundSymbol(uint32_t* value, uint64_t param2, uint64_t param3) override;

    // @0x1800a4420 (format: "%#llx\n" or "%s: %#llx\n")
    virtual void SerializeSymbol64(uint64_t* value, uint64_t param2, uint64_t param3) override;

    // @0x1800a45d0
    virtual void SerializeCPointer(int64_t count, void** data) override;

    // SerializeFlag — inherited error stub from CSerializer (@0x1800a3d30)

    // @0x1800a4070 — pushes name onto stack, indents, writes "name {\n"
    virtual uint64_t BeginObject(uint64_t name) override;

    // @0x1800a4040 — pops name stack, decrements depth
    virtual void EndObject() override;

    // @0x1800a3f50 — zeroes field name buffer, writes indentation + document header
    virtual void BeginDocument() override;

private:
    // @0x1800a45e0 — shared int formatting: snprintf into buffer, write to stream
    void WriteInt32(int32_t value, uint64_t param2, uint64_t param3);

    // @0x1800a4670 — write field name prefix + string value + newline
    void WriteStringValue(const char* value);

    // @0x1800a3eb0 — append a character to m_field_name with bounds checking
    void AppendToFieldName(char ch);

    // Name stack: dynamic array of const char* pushed on BeginObject, popped on EndObject.
    // Initialized by init_buffer_context (@0x18008b630) in the constructor.
    uint8_t  m_name_stack[0x20]; // +0x18: buffer context (data_ptr, size, allocator, capacity, etc.)
    uint64_t m_initial_capacity; // +0x38: initial stack capacity (= 0x20)
    uint64_t m_stack_capacity;   // +0x40: current allocated capacity
    uint64_t m_stack_count;      // +0x48: current depth / number of pushed names
    char     m_field_name[256];  // +0x50: current field name buffer (0x100 bytes)
    int32_t  m_has_field_name;   // +0x150: nonzero if m_field_name contains a pending name
    int32_t  m_pad154;           // +0x154: padding to 8-byte alignment
};

} // namespace NRadEngine

#endif // PNSRAD_CORE_SERIALIZATION_TEXT_SERIALIZER_H
