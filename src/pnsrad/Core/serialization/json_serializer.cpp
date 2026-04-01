// @module: pnsrad.dll
// @source: d:\projects\rad\dev\src\engine\libs\io\cserializer.cpp
// @confidence: H — from Ghidra decompilations at 0x1800a4710..0x1800a4f20

#include "src/pnsrad/Core/serialization/json_serializer.h"
#include "src/pnsrad/Core/serialization/engine_fwd.h"
#include <cstdint>
#include <cstring>

// @addr: 0x180095c20 — enter critical section (lock)
extern void EnterLock(void* lock_buf);
// @addr: 0x180095cb0 — leave critical section (unlock)
extern void LeaveLock();
// @addr: 0x180096a20 — report error from result code
extern void ReportError(const int32_t* result);

namespace NRadEngine {

// @0x1800a4710
CJsonStreamSerializerWrite::CJsonStreamSerializerWrite(CStream* stream) {
    m_mode = 1;
    m_stream = stream;

    void* allocator = GetDefaultAllocator();
    InitBufferContext(&m_name_stack, 0, 0, allocator);

    m_initial_capacity = 0x20;
    m_stack_capacity = 0;
    m_stack_count = 0;
    memset(m_field_name, 0, sizeof(m_field_name));
    m_depth = 0;
}

// @0x1800a4870
CJsonStreamSerializerWrite::~CJsonStreamSerializerWrite() {
    // Close any unclosed nesting levels by writing newline separators
    uint64_t level = 1;
    if (1 < m_depth) {
        uint8_t lock_buf[8];
        do {
            EnterLock(lock_buf);
            int32_t result = StreamWrite(m_stream, 2, "\n");
            if (result != 0) {
                ReportError(&result);
            }
            LeaveLock();
            level++;
        } while (level < m_depth);
    }

    CleanupBufferContext(&m_name_stack);
}

// @0x1800a4dd0
int32_t CJsonStreamSerializerWrite::WriteSeparator() {
    uint64_t stack_count = m_stack_count;

    int32_t result;
    if (m_depth < stack_count) {
        result = StreamWrite(m_stream, 3, "}\n");
    } else if (stack_count < m_depth) {
        result = StreamWrite(m_stream, 4, ",\n");
    } else {
        result = StreamWrite(m_stream, 3, "]\n");
    }

    if (result != 0) {
        return result;
    }

    m_depth = stack_count;

    int64_t name_len = StrLenFull(m_field_name);
    result = StreamWrite(m_stream, name_len, m_field_name);
    if (result == 0) {
        result = StreamWrite(m_stream, 3, ":\n");
    }

    return result;
}

// @0x1800a4ea0
void CJsonStreamSerializerWrite::WriteInt32(uint64_t value, uint64_t /*param2*/, uint64_t /*param3*/) {
    int32_t sep_result = WriteSeparator();
    if (sep_result != 0) {
        return;
    }

    char buf[128];
    memset(buf, 0, sizeof(buf));
    StrFormatWide(buf, "%d", static_cast<uint32_t>(value & 0xFFFFFFFF));
    int64_t len = StrLen(buf, sizeof(buf));
    StreamWrite(m_stream, len, buf);
}

// @0x1800a4f20
void CJsonStreamSerializerWrite::WriteQuotedString(const char* value) {
    int32_t sep_result = WriteSeparator();
    if (sep_result != 0) {
        return;
    }

    int32_t result = StreamWrite(m_stream, 1, "\"");
    if (result != 0) {
        return;
    }

    int64_t len = StrLenFull(value);
    result = StreamWrite(m_stream, len, value);
    if (result != 0) {
        return;
    }

    StreamWrite(m_stream, 1, "\"");
}

// @0x1800a4c30
void CJsonStreamSerializerWrite::SerializeString(CString* value) {
    if (*reinterpret_cast<int64_t*>(reinterpret_cast<uint8_t*>(value) + 0x30) == 0) {
        WriteQuotedString("");
    } else {
        const char* str = CStringGetCStr(value);
        WriteQuotedString(str);
    }
}

// @0x1800a4b50
void CJsonStreamSerializerWrite::SerializeFloat(float* value, uint64_t /*param2*/, uint64_t /*param3*/) {
    char buf[64];
    memset(buf, 0, sizeof(buf));
    StrFormat(buf, "%f", static_cast<double>(*value));
    int64_t len = StrLen(buf, sizeof(buf));
    StreamWrite(m_stream, len, buf);
}

// @0x1800a4aa0
void CJsonStreamSerializerWrite::SerializeUint8(uint8_t* value, uint64_t param2, uint64_t param3) {
    WriteInt32(static_cast<uint64_t>(*value), param2, param3);
}

// @0x1800a4a90
void CJsonStreamSerializerWrite::SerializeInt8(int8_t* value, uint64_t param2, uint64_t param3) {
    WriteInt32(static_cast<uint64_t>(static_cast<uint32_t>(static_cast<int32_t>(*value))), param2, param3);
}

// @0x1800a4ac0
void CJsonStreamSerializerWrite::SerializeUint16(uint16_t* value, uint64_t param2, uint64_t param3) {
    WriteInt32(static_cast<uint64_t>(*value), param2, param3);
}

// @0x1800a4ab0
void CJsonStreamSerializerWrite::SerializeInt16(int16_t* value, uint64_t param2, uint64_t param3) {
    WriteInt32(static_cast<uint64_t>(static_cast<uint32_t>(static_cast<int32_t>(*value))), param2, param3);
}

// @0x1800a4ae0
void CJsonStreamSerializerWrite::SerializeUint32(uint32_t* value, uint64_t /*param2*/, uint64_t /*param3*/) {
    int32_t sep_result = WriteSeparator();
    if (sep_result != 0) {
        return;
    }

    char buf[64];
    memset(buf, 0, sizeof(buf));
    StrFormat(buf, "%u", static_cast<uint64_t>(*value));
    int64_t len = StrLen(buf, sizeof(buf));
    StreamWrite(m_stream, len, buf);
}

// @0x1800a4ad0
void CJsonStreamSerializerWrite::SerializeInt32(int32_t* value, uint64_t param2, uint64_t param3) {
    WriteInt32(static_cast<uint64_t>(*value), param2, param3);
}

// @0x1800a4d50
void CJsonStreamSerializerWrite::SerializeUint64(uint64_t* value, uint64_t /*param2*/, uint64_t /*param3*/) {
    int32_t sep_result = WriteSeparator();
    if (sep_result != 0) {
        return;
    }

    char buf[64];
    memset(buf, 0, sizeof(buf));
    StrFormat(buf, "%llu", *value);
    int64_t len = StrLen(buf, sizeof(buf));
    StreamWrite(m_stream, len, buf);
}

// @0x1800a4ce0
void CJsonStreamSerializerWrite::SerializeInt64(int64_t* value, uint64_t /*param2*/, uint64_t /*param3*/) {
    int32_t sep_result = WriteSeparator();
    if (sep_result != 0) {
        return;
    }

    char buf[64];
    memset(buf, 0, sizeof(buf));
    StrFormat(buf, "%lld", *value);
    int64_t len = StrLen(buf, sizeof(buf));
    StreamWrite(m_stream, len, buf);
}

// @0x1800a4bc0
void CJsonStreamSerializerWrite::SerializeSoundSymbol(uint32_t* value, uint64_t /*param2*/, uint64_t /*param3*/) {
    int32_t sep_result = WriteSeparator();
    if (sep_result != 0) {
        return;
    }

    char buf[64];
    memset(buf, 0, sizeof(buf));
    StrFormat(buf, "\"%#x\"", static_cast<uint64_t>(*value));
    int64_t len = StrLen(buf, sizeof(buf));
    StreamWrite(m_stream, len, buf);
}

// @0x1800a4c70
void CJsonStreamSerializerWrite::SerializeSymbol64(uint64_t* value, uint64_t /*param2*/, uint64_t /*param3*/) {
    int32_t sep_result = WriteSeparator();
    if (sep_result != 0) {
        return;
    }

    char buf[64];
    memset(buf, 0, sizeof(buf));
    StrFormat(buf, "\"%#llx\"", *value);
    int64_t len = StrLen(buf, sizeof(buf));
    StreamWrite(m_stream, len, buf);
}

// @0x1800a4dc0
void CJsonStreamSerializerWrite::SerializeCPointer(int64_t count, void** data) {
    if (count == 0) {
        return;
    }
    WriteQuotedString(static_cast<const char*>(*data));
}

// @0x1800a4960
uint64_t CJsonStreamSerializerWrite::BeginObject(uint64_t name) {
    const char* name_str = SymbolToString(&name);

    PushNameStack(&m_name_stack, &m_stack_capacity, &m_stack_count,
                  m_initial_capacity, name_str);

    memset(m_field_name, 0, sizeof(m_field_name));
    CopyNameToFieldBuffer(m_field_name, sizeof(m_field_name), name_str);

    return 1;
}

// @0x1800a4940
void CJsonStreamSerializerWrite::BeginDocument() {
    memset(m_field_name, 0, sizeof(m_field_name));
}

} // namespace NRadEngine
