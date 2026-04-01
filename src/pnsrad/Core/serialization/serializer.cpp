// @module: pnsrad.dll
// @source: d:\projects\rad\dev\src\engine\libs\io\cserializer.cpp
// @confidence: H — from Ghidra decompilation of base class error stubs

#include "src/pnsrad/Core/serialization/serializer.h"

// Forward declaration — engine error reporting (CBaseErr constructor raises and returns error code)
// @addr: referenced throughout as CBaseErr::CBaseErr(file, line, hash, message)
extern uint64_t CBaseErr_Report(const char* file, int line, uint64_t hash, const char* message);

// @addr: 0x180384fb0 — global error hash used by all serializer error stubs
static const uint64_t g_serializer_error_hash = 0; // actual value is a runtime-resolved DAT

namespace NRadEngine {

static const char* const kSerializerSource = "d:\\projects\\rad\\dev\\src\\engine\\libs\\io\\cserializer.cpp";

// @0x180095ca0
CSerializer::~CSerializer() {
    // Base destructor: sets vtable pointer back to CSerializer::vftable.
    // In C++ this happens implicitly.
}

// @0x1800a3c20
void CSerializer::SerializeString(CString* value) {
    // Base calls FUN_18008be00 (CString::GetCStr) if string has data (length at +0x30 != 0),
    // then raises error.
    CBaseErr_Report(kSerializerSource, 0x75, g_serializer_error_hash,
                    "can't serialize CString \"%s\"");
}

// @0x1800a3b60
void CSerializer::SerializeFloat(float* /*value*/, uint64_t /*param2*/, uint64_t /*param3*/) {
    CBaseErr_Report(kSerializerSource, 0x6e, g_serializer_error_hash,
                    "can't serialize real \"%f\"");
}

// @0x1800a3a70
void CSerializer::SerializeUint8(uint8_t* /*value*/, uint64_t /*param2*/, uint64_t /*param3*/) {
    CBaseErr_Report(kSerializerSource, 0x67, g_serializer_error_hash,
                    "can't serialize uint8 \"%d\"");
}

// @0x1800a3a40
void CSerializer::SerializeInt8(int8_t* /*value*/, uint64_t /*param2*/, uint64_t /*param3*/) {
    CBaseErr_Report(kSerializerSource, 0x60, g_serializer_error_hash,
                    "can't serialize int8 \"%d\"");
}

// @0x1800a3ad0
void CSerializer::SerializeUint16(uint16_t* /*value*/, uint64_t /*param2*/, uint64_t /*param3*/) {
    CBaseErr_Report(kSerializerSource, 0x59, g_serializer_error_hash,
                    "can't serialize uint16 \"%d\"");
}

// @0x1800a3aa0
void CSerializer::SerializeInt16(int16_t* /*value*/, uint64_t /*param2*/, uint64_t /*param3*/) {
    CBaseErr_Report(kSerializerSource, 0x52, g_serializer_error_hash,
                    "can't serialize int16 \"%d\"");
}

// @0x1800a3ba0
void CSerializer::SerializeUint24(uint32_t* /*value*/, uint64_t /*param2*/, uint64_t /*param3*/) {
    CBaseErr_Report(kSerializerSource, 0x4b, g_serializer_error_hash,
                    "can't serialize uint24 \"%u\"");
}

// @0x1800a3b30
void CSerializer::SerializeUint32(uint32_t* /*value*/, uint64_t /*param2*/, uint64_t /*param3*/) {
    CBaseErr_Report(kSerializerSource, 0x44, g_serializer_error_hash,
                    "can't serialize uint32 \"%u\"");
}

// @0x1800a3b00
void CSerializer::SerializeInt32(int32_t* /*value*/, uint64_t /*param2*/, uint64_t /*param3*/) {
    CBaseErr_Report(kSerializerSource, 0x3d, g_serializer_error_hash,
                    "can't serialize int32 \"%d\"");
}

// @0x1800a3cd0
void CSerializer::SerializeUint64(uint64_t* /*value*/, uint64_t /*param2*/, uint64_t /*param3*/) {
    CBaseErr_Report(kSerializerSource, 0x36, g_serializer_error_hash,
                    "can't serialize uint64 \"%llu\"");
}

// @0x1800a3ca0
void CSerializer::SerializeInt64(int64_t* /*value*/, uint64_t /*param2*/, uint64_t /*param3*/) {
    CBaseErr_Report(kSerializerSource, 0x2f, g_serializer_error_hash,
                    "can't serialize int64 \"%lld\"");
}

// @0x1800a3bf0
void CSerializer::SerializeSoundSymbol(uint32_t* /*value*/, uint64_t /*param2*/, uint64_t /*param3*/) {
    CBaseErr_Report(kSerializerSource, 0x28, g_serializer_error_hash,
                    "can't serialize CSoundSymbol \"%#x\"");
}

// @0x1800a3c70
void CSerializer::SerializeSymbol64(uint64_t* /*value*/, uint64_t /*param2*/, uint64_t /*param3*/) {
    CBaseErr_Report(kSerializerSource, 0x20, g_serializer_error_hash,
                    "can't serialize symbol \"%#llx\"");
}

// @0x1800a3d00
void CSerializer::SerializeCPointer(int64_t /*count*/, void** /*data*/) {
    CBaseErr_Report(kSerializerSource, 0x19, g_serializer_error_hash,
                    "can't serialize cpointer \"%p\"");
}

// @0x1800a3d30
void CSerializer::SerializeFlag(uint32_t* /*value*/, uint64_t /*param2*/, uint64_t /*param3*/) {
    CBaseErr_Report(kSerializerSource, 0x7c, g_serializer_error_hash,
                    "can't serialize flag \"%u\"");
}

// Default implementations for structured serialization (no-ops in base)
uint64_t CSerializer::BeginObject(uint64_t /*name*/) { return 0; }
void CSerializer::EndObject() {}
void CSerializer::BeginDocument() {}

} // namespace NRadEngine
