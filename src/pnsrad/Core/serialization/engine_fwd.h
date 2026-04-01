#ifndef PNSRAD_CORE_SERIALIZATION_ENGINE_FWD_H
#define PNSRAD_CORE_SERIALIZATION_ENGINE_FWD_H

// @module: pnsrad.dll
// @purpose: Forward declarations for engine utility functions used by serializers.
// Centralizes extern declarations that would otherwise be duplicated across
// text_serializer.cpp and json_serializer.cpp.

#include <cstdint>

namespace NRadEngine {
class CStream;
} // namespace NRadEngine

// @addr: 0x180093120 — strlen with max length
extern int64_t StrLen(const char* str, int64_t max_len);
// @addr: 0x180093100 — strlen (unbounded)
extern int64_t StrLenFull(const void* str);
// @addr: 0x1800899f0 — strncpy
extern void StrCopy(void* dst, const void* src, uint64_t max_len);
// @addr: 0x18008b2b0 — snprintf into buffer (64-byte variant)
extern void StrFormat(char* dst, const char* fmt, ...);
// @addr: 0x1800959b0 — snprintf into buffer (128-byte variant)
extern void StrFormatWide(char* dst, const char* fmt, ...);
// @addr: 0x180089ad0 — CSymbol64::GetString (returns const char*)
extern const char* SymbolToString(const uint64_t* symbol);
// @addr: 0x18008be00 — CString::GetCStr
extern const char* CStringGetCStr(void* cstring);
// @addr: 0x180089700 — get default allocator
extern void* GetDefaultAllocator();
// @addr: 0x18008b630 — init_buffer_context (dynamic array init)
extern void InitBufferContext(void* ctx, uint64_t initial_size, uint64_t initial_capacity, void* allocator);
// @addr: 0x18008c3b0 — grow buffer context
extern void GrowBufferContext(void* ctx, uint64_t new_size, int flag);
// @addr: 0x18008bb90 — flush buffer context
extern void FlushBufferContext(void* ctx);
// @addr: 0x18008b730 — destroy buffer context
extern void DestroyBufferContext(void* ctx);

// CStream vtable slot for Write: vtable+0x48.
// Signature: int32_t Write(CStream* stream, int64_t count, const void* data)
// Returns 0 on success.
inline int32_t StreamWrite(NRadEngine::CStream* stream, int64_t count, const void* data) {
    auto vtable = *reinterpret_cast<void***>(stream);
    auto write_fn = reinterpret_cast<int32_t(*)(NRadEngine::CStream*, int64_t, const void*)>(
        reinterpret_cast<uintptr_t*>(vtable)[9]); // 0x48 / 8 = 9
    return write_fn(stream, count, data);
}

// Retrieve buffer context flags at internal offset +0x14.
// Used by destructors to decide whether FlushBufferContext is needed.
inline uint32_t GetBufferContextFlags(const void* name_stack) {
    return *reinterpret_cast<const uint32_t*>(
        reinterpret_cast<const uint8_t*>(name_stack) + 0x14);
}

// Shared destructor cleanup: flush (if flagged) + destroy buffer context.
inline void CleanupBufferContext(void* name_stack) {
    uint32_t flags = GetBufferContextFlags(name_stack);
    if ((flags & 4) != 0 || (flags & 2) != 0) {
        FlushBufferContext(name_stack);
    }
    DestroyBufferContext(name_stack);
}

// Copy a name string into a fixed-size field name buffer with bounds checking
// and null-termination. Zeroes any remaining bytes after the string.
// @param field_name: destination buffer
// @param buf_size: size of destination buffer
// @param name_str: source string to copy
inline void CopyNameToFieldBuffer(char* field_name, uint64_t buf_size, const char* name_str) {
    int64_t name_len = StrLen(name_str, -1);
    int64_t field_len = StrLen(field_name, static_cast<int64_t>(buf_size));
    uint64_t remaining = buf_size - static_cast<uint64_t>(field_len);
    char* dest = field_name + field_len;

    if (remaining < static_cast<uint64_t>(name_len)) {
        StrCopy(dest, name_str, remaining);
    } else {
        StrCopy(dest, name_str, static_cast<uint64_t>(name_len));
        if (static_cast<uint64_t>(name_len) < remaining) {
            dest[name_len] = '\0';
        }
    }
    field_name[buf_size - 1] = '\0';

    int64_t total_len = StrLen(field_name, static_cast<int64_t>(buf_size));
    uint64_t next = static_cast<uint64_t>(total_len) + 1;
    if (next < buf_size) {
        __builtin_memset(field_name + next, 0, buf_size - next);
    }
}

// Grow the name stack if at capacity, push a name entry.
// Returns the new stack count.
inline uint64_t PushNameStack(void* name_stack, uint64_t* stack_capacity,
                               uint64_t* stack_count, uint64_t initial_capacity,
                               const char* name_str) {
    uint64_t count = *stack_count;
    uint64_t capacity = *stack_capacity;

    if (capacity <= count) {
        uint64_t grow = capacity;
        if (capacity <= initial_capacity) {
            grow = initial_capacity;
        }
        GrowBufferContext(name_stack, (capacity + grow) * 8, 1);
        *stack_capacity += grow;
        count = *stack_count;
    }

    *stack_count = count + 1;
    char** stack_data = *reinterpret_cast<char***>(name_stack);
    stack_data[count] = const_cast<char*>(name_str);

    return count + 1;
}

#endif // PNSRAD_CORE_SERIALIZATION_ENGINE_FWD_H
