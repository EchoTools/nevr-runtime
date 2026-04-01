// @module: pnsrad.dll
// @source: d:\projects\rad\dev\src\engine\libs\io\cserializer.cpp
// @confidence: H — from Ghidra decompilations at 0x1800a3d60..0x1800a4670

#include "src/pnsrad/Core/serialization/text_serializer.h"
#include "src/pnsrad/Core/serialization/engine_fwd.h"
#include <cstdint>
#include <cstring>

// @addr: 0x1800958f0 — stream write helper (writes string to stream)
extern void StreamWriteString(char* buffer, const void* data, int64_t len);

namespace NRadEngine {

// @0x1800a3d60
CTextStreamSerializerWrite::CTextStreamSerializerWrite(CStream* stream, int32_t has_field_name) {
    m_mode = 1;
    m_stream = stream;

    void* allocator = GetDefaultAllocator();
    InitBufferContext(&m_name_stack, 0, 0, allocator);

    m_initial_capacity = 0x20;
    m_stack_capacity = 0;
    m_stack_count = 0;
    memset(m_field_name, 0, sizeof(m_field_name));
    m_has_field_name = has_field_name;
}

// @0x1800a3e40
CTextStreamSerializerWrite::~CTextStreamSerializerWrite() {
    CleanupBufferContext(&m_name_stack);
}

// @0x1800a3eb0 — append a single char to m_field_name, null-terminate, zero-fill remainder
void CTextStreamSerializerWrite::AppendToFieldName(char ch) {
    int64_t len = StrLen(m_field_name, sizeof(m_field_name));
    char* pos = m_field_name + len;

    if (static_cast<uint64_t>(-len) == sizeof(m_field_name)) {
        StrCopy(pos, &ch, 0);
    } else {
        StrCopy(pos, &ch, 1);
        if (1 < static_cast<uint64_t>(sizeof(m_field_name) - len)) {
            pos[1] = '\0';
        }
    }

    m_field_name[sizeof(m_field_name) - 1] = '\0';

    uint64_t used = static_cast<uint64_t>(len) + 2;
    if (used < sizeof(m_field_name)) {
        memset(m_field_name + used, 0, sizeof(m_field_name) - used);
    }
}

// @0x1800a45e0 — format a 32-bit integer and write to stream
void CTextStreamSerializerWrite::WriteInt32(int32_t value, uint64_t /*param2*/, uint64_t /*param3*/) {
    char buf[128];
    memset(buf, 0, sizeof(buf));

    if (m_has_field_name == 0) {
        StrFormatWide(buf, "%d\n", static_cast<uint32_t>(value));
    } else {
        StrFormatWide(buf, "%s: %d\n", m_field_name, static_cast<uint32_t>(value));
    }

    int64_t len = StrLen(buf, sizeof(buf));
    StreamWrite(m_stream, len, buf);
}

// @0x1800a4670 — write field name prefix (if any) then string value with newline
void CTextStreamSerializerWrite::WriteStringValue(const char* value) {
    if (m_has_field_name != 0) {
        int64_t name_len = StrLenFull(m_field_name);
        int32_t result = StreamWrite(m_stream, name_len, m_field_name);
        if (result != 0) {
            return;
        }
        result = StreamWrite(m_stream, 2, ": ");
        if (result != 0) {
            return;
        }
    }

    int64_t value_len = StrLenFull(value);
    int32_t result = StreamWrite(m_stream, value_len, value);
    if (result == 0) {
        StreamWrite(m_stream, 1, "\n");
    }
}

// @0x1800a43e0
void CTextStreamSerializerWrite::SerializeString(CString* value) {
    if (*reinterpret_cast<int64_t*>(reinterpret_cast<uint8_t*>(value) + 0x30) == 0) {
        WriteStringValue("");
    } else {
        const char* str = CStringGetCStr(value);
        WriteStringValue(str);
    }
}

// @0x1800a42c0
void CTextStreamSerializerWrite::SerializeFloat(float* value, uint64_t /*param2*/, uint64_t /*param3*/) {
    char buf[64];
    memset(buf, 0, sizeof(buf));

    if (m_has_field_name == 0) {
        StrFormat(buf, "%f\n", static_cast<double>(*value));
    } else {
        StrFormat(buf, "%s: %f\n", m_field_name, static_cast<double>(*value));
    }

    int64_t len = StrLen(buf, sizeof(buf));
    StreamWrite(m_stream, len, buf);
}

// @0x1800a41f0
void CTextStreamSerializerWrite::SerializeUint8(uint8_t* value, uint64_t param2, uint64_t param3) {
    WriteInt32(static_cast<int32_t>(*value), param2, param3);
}

// @0x1800a41e0
void CTextStreamSerializerWrite::SerializeInt8(int8_t* value, uint64_t param2, uint64_t param3) {
    WriteInt32(static_cast<int32_t>(*value), param2, param3);
}

// @0x1800a4210
void CTextStreamSerializerWrite::SerializeUint16(uint16_t* value, uint64_t param2, uint64_t param3) {
    WriteInt32(static_cast<int32_t>(*value), param2, param3);
}

// @0x1800a4200
void CTextStreamSerializerWrite::SerializeInt16(int16_t* value, uint64_t param2, uint64_t param3) {
    WriteInt32(static_cast<int32_t>(*value), param2, param3);
}

// @0x1800a4230
void CTextStreamSerializerWrite::SerializeUint32(uint32_t* value, uint64_t /*param2*/, uint64_t /*param3*/) {
    char buf[64];
    memset(buf, 0, sizeof(buf));

    if (m_has_field_name == 0) {
        StrFormat(buf, "%u\n", static_cast<uint64_t>(*value));
    } else {
        StrFormat(buf, "%s: %u\n", m_field_name, static_cast<uint64_t>(*value));
    }

    int64_t len = StrLen(buf, sizeof(buf));
    StreamWrite(m_stream, len, buf);
}

// @0x1800a4220
void CTextStreamSerializerWrite::SerializeInt32(int32_t* value, uint64_t param2, uint64_t param3) {
    WriteInt32(*value, param2, param3);
}

// @0x1800a4540
void CTextStreamSerializerWrite::SerializeUint64(uint64_t* value, uint64_t /*param2*/, uint64_t /*param3*/) {
    char buf[64];
    memset(buf, 0, sizeof(buf));

    if (m_has_field_name == 0) {
        StrFormat(buf, "%llu\n", *value);
    } else {
        StrFormat(buf, "%s: %llu\n", m_field_name, *value);
    }

    int64_t len = StrLen(buf, sizeof(buf));
    StreamWrite(m_stream, len, buf);
}

// @0x1800a44b0
void CTextStreamSerializerWrite::SerializeInt64(int64_t* value, uint64_t /*param2*/, uint64_t /*param3*/) {
    char buf[64];
    memset(buf, 0, sizeof(buf));

    if (m_has_field_name == 0) {
        StrFormat(buf, "%lld\n", *value);
    } else {
        StrFormat(buf, "%s: %lld\n", m_field_name, *value);
    }

    int64_t len = StrLen(buf, sizeof(buf));
    StreamWrite(m_stream, len, buf);
}

// @0x1800a4350
void CTextStreamSerializerWrite::SerializeSoundSymbol(uint32_t* value, uint64_t /*param2*/, uint64_t /*param3*/) {
    char buf[64];
    memset(buf, 0, sizeof(buf));

    if (m_has_field_name == 0) {
        StrFormat(buf, "%#x\n", static_cast<uint64_t>(*value));
    } else {
        StrFormat(buf, "%s: %#x\n", m_field_name, static_cast<uint64_t>(*value));
    }

    int64_t len = StrLen(buf, sizeof(buf));
    StreamWrite(m_stream, len, buf);
}

// @0x1800a4420
void CTextStreamSerializerWrite::SerializeSymbol64(uint64_t* value, uint64_t /*param2*/, uint64_t /*param3*/) {
    char buf[64];
    memset(buf, 0, sizeof(buf));

    if (m_has_field_name == 0) {
        StrFormat(buf, "%#llx\n", *value);
    } else {
        StrFormat(buf, "%s: %#llx\n", m_field_name, *value);
    }

    int64_t len = StrLen(buf, sizeof(buf));
    StreamWrite(m_stream, len, buf);
}

// @0x1800a45d0
void CTextStreamSerializerWrite::SerializeCPointer(int64_t count, void** data) {
    if (count == 0) {
        return;
    }
    WriteStringValue(static_cast<const char*>(*data));
}

// @0x1800a4070
uint64_t CTextStreamSerializerWrite::BeginObject(uint64_t name) {
    const char* name_str = SymbolToString(&name);

    PushNameStack(&m_name_stack, &m_stack_capacity, &m_stack_count,
                  m_initial_capacity, name_str);

    int64_t buf_len = StrLen(m_field_name, sizeof(m_field_name));
    if (buf_len != 0) {
        AppendToFieldName('\n');
    }

    for (uint64_t i = 0; i < m_stack_count; i++) {
        AppendToFieldName(' ');
    }

    CopyNameToFieldBuffer(m_field_name, sizeof(m_field_name), name_str);

    StreamWriteString(m_field_name, name_str, -1);

    return 1;
}

// @0x1800a4040
void CTextStreamSerializerWrite::EndObject() {
    if (m_stack_count != 0) {
        m_stack_count--;
    }
    memset(m_field_name, 0, sizeof(m_field_name));
}

// @0x1800a3f50
void CTextStreamSerializerWrite::BeginDocument() {
    memset(m_field_name, 0, sizeof(m_field_name));

    for (uint64_t i = 0; i < m_stack_count; i++) {
        AppendToFieldName(' ');
    }

    // The original code appends a document start marker from a hashed string constant
    // at 0x1802ad480. The exact string value is not recoverable from the binary data
    // (it reads as hashed bytes, not ASCII).
    CopyNameToFieldBuffer(m_field_name, sizeof(m_field_name), "");
}

} // namespace NRadEngine
