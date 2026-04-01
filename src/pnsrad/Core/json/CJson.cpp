#include "src/pnsrad/Core/json/CJson.h"
#include "src/pnsrad/Core/CHashMap.h"
#include "src/pnsrad/Core/CBaseErr.h"
#include <cstring>

namespace NRadEngine {

// ============================================================================
// Global allocator state
// ============================================================================
void* g_json_allocator = nullptr;     // @0x180384d00 (DAT_180384d00)
int   g_json_allocator_mode = 0;      // @0x180384d08 (1=custom allocator set)
static void* g_json_allocator_ctx = nullptr;  // @0x180384cf8

// Empty string constant (DAT_180222db4 in pnsrad.dll)
static const char k_empty_string[] = "";

// ============================================================================
// Allocator Helpers
// ============================================================================
// The TLS context pattern appears throughout: get a lock, get a context, release.
// When g_json_allocator_mode == 1, use g_json_allocator; otherwise get_tls_context().

// @0x180097fb0 (inlined) — allocator selection pattern used throughout CJson
static void* GetAllocator() {
    if (g_json_allocator_mode == 1) {
        return g_json_allocator;
    }
    return get_tls_context();
}

// ============================================================================
// Path parsing helper: skip leading separator
// ============================================================================
// Advances past leading '|' or '[N]' prefix to get to the meaningful path start.
// Returns pointer to the content after the separator.
// @0x18009dec0 (inlined) — skip leading '|' or '[N]' prefix in path strings
static const char* SkipPathPrefix(const char* path) {
    if (path == nullptr) return nullptr;
    if (*path == '|') {
        return path + 1;
    }
    if (*path == '[') {
        const char* p = path + 1;
        char c = *p;
        while (c != '\0') {
            if (c == ']') return path + 1;
            if (static_cast<uint8_t>(c - '0') > 9) break;
            p++;
            c = *p;
        }
    }
    return path;
}

// ============================================================================
// Extract key segment from path
// ============================================================================
// Copies characters from path into `buf` until hitting '|', '[', or '\0'.
// Returns length of key and sets `path_ptr` to point past the delimiter.
// @0x18009aad0 (inlined) — extract key segment from path until delimiter
static int64_t ExtractKey(const char* path, char* buf, int buf_size) {
    memset_wrapper(buf, 0, buf_size);
    int64_t len = 0;
    char c = *path;
    const char* p = path;

    if (c == '\0') return 0;

    while (c != '\0') {
        if (c == '|') break;
        if (c == '[') {
            // Check if this is a valid array index
            const char* scan = p + 1;
            char sc = *scan;
            while (sc != '\0') {
                if (sc == ']') goto done;
                if (static_cast<uint8_t>(sc - '0') > 9) break;
                scan++;
                sc = *scan;
            }
        }
        buf[len] = *p;
        len++;
        p++;
        c = *p;
    }
done:
    buf[len] = '\0';
    return len;
}

// ============================================================================
// Lifecycle
// ============================================================================

// @0x180097650 — CJson::Clear
// @confidence: H
void CJson_Clear(CJson* json) {
    CJson_ReleaseCache(json);
    CJson_SetValue(json, k_empty_string, 1, nullptr);
}

// @0x180097680 — CJson::MoveAssign
// @confidence: H
void* CJson_MoveAssign(CJson* dst, CJson* src) {
    if (dst != src) {
        CJson_ReleaseCache(dst);
        CJson_SetValue(dst, k_empty_string, 1, nullptr);
        dst->root = src->root;
        dst->cache = src->cache;
        src->root = nullptr;
        src->cache = 0;
    }
    return dst;
}

// @0x18009ddc0 — CJson::Reset
// @confidence: H
void CJson_Reset(CJson* json) {
    CJson_ReleaseCache(json);
    CJson_SetValue(json, k_empty_string, 1, nullptr);
}

// @0x18009ddf0 — CJson::ReleaseCache
// @confidence: H
void CJson_ReleaseCache(CJson* json) {
    if (json->cache != 0) {
        void* alloc = GetAllocator();
        void* tls = get_tls_context();
        release_lock(alloc);

        void* tls2 = get_tls_context();
        void* alloc2 = get_tls_context();
        release_lock(alloc2);

        auto* alloc_ctx = reinterpret_cast<void**>(get_tls_context());
        auto** avt = *reinterpret_cast<void***>(alloc_ctx);
        using FreeFn = void(*)(void*, void*);
        auto free_fn = reinterpret_cast<FreeFn>(reinterpret_cast<uintptr_t*>(avt)[6]);

        auto* cache_map = reinterpret_cast<void*>(json->cache);
        if (cache_map != nullptr) {
            CHashMap_Destroy(reinterpret_cast<CHashMap*>(cache_map));
        }

        free_fn(alloc_ctx, cache_map);
        release_lock(tls2);
        json->cache = 0;
        release_lock(tls);
    }
}

// @0x180099f40 — CJson::BuildCache
// @confidence: H
void CJson_BuildCache(CJson* json) {
    if (json->root != nullptr && json->cache == 0) {
        void* alloc = GetAllocator();
        void* tls = get_tls_context();
        release_lock(alloc);

        // Allocate CHashMap (0x68 bytes) via allocator
        auto* alloc_ctx = get_tls_context();
        auto** avt = *reinterpret_cast<void***>(alloc_ctx);
        using AllocFn = void*(*)(void*, uint32_t);
        void* map_mem = reinterpret_cast<AllocFn>(reinterpret_cast<uintptr_t*>(avt)[1])(alloc_ctx, 0x68);

        // Initialize with 0 entries, 0x100 entries per page
        int64_t params[3];
        params[2] = reinterpret_cast<int64_t>(get_tls_context());
        params[0] = 0;
        params[1] = 0x100;
        auto* cache = reinterpret_cast<CHashMap*>(CHashMap_Init(reinterpret_cast<CHashMap*>(map_mem), params));

        CJson_PopulateCache(json, "", cache);
        json->cache = reinterpret_cast<int64_t>(cache);

        release_lock(tls);
    }
}

// ============================================================================
// Parsing
// ============================================================================

// @0x18009eaa0 — CJson::StripComments
// @confidence: H
void CJson_StripComments(char* buffer, uint64_t* length) {
    uint64_t len = *length;
    if (len == 0) return;

    bool in_block_comment = false;
    uint64_t read_pos = 0;
    uint8_t in_string = 0;  // 0 = not in string, else = quote character
    uint64_t write_pos = 0;

    while (read_pos < len) {
        char c = buffer[read_pos];

        switch (c) {
        case '"':
        case '\'':
            if (!in_block_comment) {
                buffer[write_pos] = c;
                write_pos++;
                if (in_string == 0) {
                    in_string = static_cast<uint8_t>(buffer[read_pos]);
                } else if (in_string == static_cast<uint8_t>(buffer[read_pos])) {
                    in_string = 0;
                }
            }
            break;

        case '\\':
            if (!in_block_comment) {
                buffer[write_pos] = c;
                read_pos++;
                buffer[write_pos + 1] = buffer[read_pos];
                write_pos += 2;
            }
            break;

        case '/':
            if (in_string != 0) {
                buffer[write_pos] = c;
                write_pos++;
            } else {
                char next = buffer[read_pos + 1];
                if (next == '/') {
                    // Line comment: skip until newline
                    do {
                        char nc = buffer[read_pos + 1];
                        read_pos++;
                        if (nc == '\n' || nc == '\0') break;
                    } while (true);
                } else if (next == '*') {
                    in_block_comment = true;
                    read_pos++;
                } else if (!in_block_comment) {
                    buffer[write_pos] = c;
                    write_pos++;
                }
            }
            break;

        case '*':
            if (in_string != 0) {
                buffer[write_pos] = c;
                write_pos++;
            } else if (in_block_comment) {
                if (buffer[read_pos + 1] == '/') {
                    in_block_comment = false;
                    read_pos++;
                }
            } else if (!in_block_comment) {
                buffer[write_pos] = c;
                write_pos++;
            }
            break;

        case ']':
        case '}':
            if (in_string != 0) {
                buffer[write_pos] = c;
                write_pos++;
            } else if (!in_block_comment) {
                // Remove trailing comma before closing bracket
                if (buffer[write_pos - 1] == ',') {
                    buffer[write_pos - 1] = c;
                } else {
                    buffer[write_pos] = c;
                    write_pos++;
                }
            }
            break;

        default:
            if (in_string != 0) {
                buffer[write_pos] = c;
                write_pos++;
            } else if (in_block_comment) {
                // skip
            } else {
                int ws = is_whitespace(c);
                if (ws == 0) {
                    buffer[write_pos] = buffer[read_pos];
                    write_pos++;
                }
            }
            break;
        }

        read_pos++;
    }

    buffer[write_pos] = '\0';
    *length = write_pos;
}

// @0x180099db0 — CJson::ParseBuffer
// @confidence: H
uint64_t CJson_ParseBuffer(CJson* json, const char* buffer, int64_t length, void* error_ctx) {
    // Trim trailing whitespace/nulls
    while (length > 0) {
        char c = buffer[length - 1];
        if (c != '\0' && is_whitespace(c) == 0) break;
        length--;
    }

    if (length == 0) {
        CJson_ReleaseCache(json);
        CJson_SetValue(json, k_empty_string, 1, error_ctx);
        return 0;
    }

    void* alloc = GetAllocator();
    void* tls = get_tls_context();
    release_lock(alloc);

    char error_buf[256];
    void* parsed = json_loads(const_cast<char*>(buffer), length, 1, error_buf);
    uint64_t result;

    if (parsed == nullptr) {
        // Report parse error — file: cjson.cpp, line: 0xa06, hash: 0x95f597e1fdf40bfc
        LogAndAbort(8, 0, "Json Format Error: %s - [%s] line: %llu column: %llu preview:\n%.*s");
        result = 0x95f597e1fdf40bfcULL & 0xFFFFFFFF;
    } else {
        CJson_ReleaseCache(json);
        CJson_SetValue(json, k_empty_string, 1, error_buf);
        json->root = parsed;
        result = 0;
    }

    release_lock(tls);
    return result;
}

// @0x180099d10 — CJson::ParseFromMemBlock
// @confidence: H
uint32_t CJson_ParseFromMemBlock(CJson* json, int64_t* mem_block, int strip_trailing, void* error_ctx) {
    // mem_block layout: [0]=data_ptr, ... [6]=length
    if (strip_trailing != 0 && mem_block[6] != 0) {
        uint64_t new_len = static_cast<uint64_t>(mem_block[6] - 1);
        // get_buffer_data equivalent
        extern void* mem_block_get_data(int64_t* block);  // @0x18008be00
        auto* data = reinterpret_cast<char*>(mem_block_get_data(mem_block));
        CJson_StripComments(data, &new_len);
        int64_t updated_len = static_cast<int64_t>(new_len + 1);
        mem_block[6] = updated_len;
        if (updated_len != 0) {
            data[updated_len - 1] = '\0';
        }
    }

    int64_t data_len = mem_block[6] - 1;
    const char* data_ptr;
    if (mem_block[6] == 0) {
        data_ptr = k_empty_string;
        data_len = 0;
    } else {
        extern void* mem_block_get_data(int64_t* block);
        data_ptr = reinterpret_cast<const char*>(mem_block_get_data(mem_block));
    }

    return static_cast<uint32_t>(CJson_ParseBuffer(json, data_ptr, data_len, error_ctx));
}

// ============================================================================
// Read: Path Navigation
// ============================================================================

// @0x18009dec0 — CJson::NavigatePath
// @confidence: H
void* CJson_NavigatePath(const char* path, void* root, uint32_t required, void* cache) {
    if (path == nullptr) {
        if (required != 0) {
            LogAndAbort(8, 0, "\n");
        }
        return nullptr;
    }

    // Skip path prefix
    const char* p = SkipPathPrefix(path);
    if (*p == '\0') {
        return root;
    }

    // Determine if root is object or array
    bool is_object = false;
    bool is_array = false;
    if (root != nullptr) {
        int type = *reinterpret_cast<int*>(root);
        if (type == 0) is_object = true;
        else if (type == 1) is_array = true;
    }

    if (!is_object && !is_array) {
        if (required != 0) {
            LogAndAbort(8, 0, "\n");
        }
        return nullptr;
    }

    // If cache is available, do hash lookup
    if (cache != nullptr) {
        uint64_t hash = hash_string(path, 0xFFFFFFFFFFFFFFFFULL, 0, -1, 1);
        auto* cache_map = reinterpret_cast<CHashMap*>(cache);
        uint32_t bucket_idx = static_cast<uint32_t>(hash & 0xFFF);
        auto* bucket_array = reinterpret_cast<uint32_t*>(cache_map->bucket_array);
        uint32_t entry_idx = bucket_array[bucket_idx];

        void* result = nullptr;
        while (entry_idx != 0xFFFFFFFF) {
            int64_t buf_data = *reinterpret_cast<int64_t*>(&cache_map->buf_ctx[0]);
            uint32_t page_idx = entry_idx / cache_map->entries_per_page;
            uint32_t slot_idx = entry_idx % cache_map->entries_per_page;
            int64_t page = *reinterpret_cast<int64_t*>(buf_data + page_idx * 8);
            auto* entry = reinterpret_cast<uint8_t*>(page + slot_idx * 0x20);

            if (*reinterpret_cast<uint64_t*>(entry + 0x10) == hash) {
                result = *reinterpret_cast<void**>(entry + 0x18);
                break;
            }
            entry_idx = *reinterpret_cast<uint32_t*>(entry);
        }

        if (required != 0 && result == nullptr) {
            char buf[1024];
            memset_wrapper(buf, 0, 0x400);
            snprintf_wrapper(buf, "$ json path: %s: not found in cache.", path);
            LogAndAbort(8, 0, "\n");
        }
        return result;
    }

    // No cache — navigate manually
    const char* remaining = nullptr;
    uint64_t index = 0;
    void* node;

    if (is_array) {
        node = CJson_NavigateArrayPath(path, root, "<root>", static_cast<uint64_t>(required),
                                        &remaining, &index);
    } else {
        node = CJson_NavigateObjectPath(path, root, "<root>", static_cast<uint64_t>(required),
                                         reinterpret_cast<int64_t*>(&remaining), &index);
    }

    if (node == nullptr) return nullptr;

    // Resolve final element
    int node_type = *reinterpret_cast<int*>(node);
    if (node_type != 0) {
        return CJson_GetArrayElementChecked(node, path, index, required);
    }
    return CJson_GetObjectFieldChecked(node, path, remaining, required);
}

// @0x18009bff0 — CJson::ParseArrayIndex
// @confidence: H
const char* CJson_ParseArrayIndex(const char* path, int64_t* out_index, int32_t* out_type) {
    *out_index = -1;
    char buf[1024];
    memset_wrapper(buf, 0, 0x400);

    // Copy digits until ']'
    char c = *path;
    int64_t len = 0;
    while (c != '\0') {
        if (c == ']') break;
        buf[len] = c;
        len++;
        path++;
        c = *path;
    }

    buf[len] = '\0';

    // Validate: must end with ']', content must be a valid number
    if (c == ']' && is_valid_number(buf, 10) != 0) {
        char next = *(path + 1);
        if (next == '\0' || next == '[' || next == '|') {
            int64_t idx = strtoll_wrapper(buf, 10);
            *out_index = idx;

            if (*(path + 1) == '|') {
                // Next segment is an object key
                *out_type = 6;
                return path + 2;
            }
            if (*(path + 1) == '[') {
                // Check for another array index
                char nc = *(path + 2);
                const char* scan = path + 2;
                while (nc != '\0') {
                    if (nc == ']') {
                        *out_type = 5;
                        return path + 2;
                    }
                    if (static_cast<uint8_t>(nc - '0') > 9) break;
                    scan++;
                    nc = *scan;
                }
            }
            *out_type = 0;
            return nullptr;
        }
    }

    return nullptr;
}

// ============================================================================
// Read: Value Getters
// ============================================================================

// @0x180097fb0 — CJson::GetBoolean
// @confidence: H
uint32_t CJson_GetBoolean(CJson* json, const char* path, uint32_t default_val, uint32_t required) {
    void* alloc = GetAllocator();
    void* tls = get_tls_context();
    release_lock(alloc);

    auto* cache = reinterpret_cast<void*>(json->cache);
    auto* node = reinterpret_cast<int*>(CJson_NavigatePath(path, json->root, required, cache));

    if (node == nullptr || (*node - 5) > 1) {
        // Not a boolean (type 5=true, 6=false)
        if (required != 0) {
            char buf[1024];
            memset_wrapper(buf, 0, 0x400);
            snprintf_wrapper(buf, "$ json path: [%s] not found or not a boolean.", path);
            LogAndAbort(8, 0, "\n");
        }
    } else {
        default_val = (*node == 5) ? 1 : 0;
    }

    release_lock(tls);
    return default_val;
}

// @0x18009afa0 — CJson::GetInt
// @confidence: H
int64_t CJson_GetInt(CJson* json, const char* path, int64_t default_val, uint32_t required) {
    void* alloc = GetAllocator();
    void* tls = get_tls_context();
    release_lock(alloc);

    auto* cache = reinterpret_cast<void*>(json->cache);
    auto* node = reinterpret_cast<int*>(CJson_NavigatePath(path, json->root, required, cache));

    if (node == nullptr || *node != 3) {
        if (required != 0) {
            char buf[1024];
            memset_wrapper(buf, 0, 0x400);
            snprintf_wrapper(buf, "$ json path: [%s] not found or not an integer.", path);
            LogAndAbort(8, 0, "\n");
        }
    } else {
        default_val = json_integer_value(node);
    }

    release_lock(tls);
    return default_val;
}

// @0x18009b140 — CJson::GetReal
// @confidence: H
double CJson_GetReal(CJson* json, const char* path, double default_val, uint32_t required) {
    void* alloc = GetAllocator();
    void* tls = get_tls_context();
    release_lock(alloc);

    auto* cache = reinterpret_cast<void*>(json->cache);
    auto* node = reinterpret_cast<int*>(CJson_NavigatePath(path, json->root, required, cache));

    if (node == nullptr) {
        if (required != 0) {
            char buf[1024];
            memset_wrapper(buf, 0, 0x400);
            snprintf_wrapper(buf, "$ json path: [%s] not found.", path);
            LogAndAbort(8, 0, "\n");
        }
    } else if (*node == 4) {
        default_val = json_real_value(node);
    } else if (*node == 3) {
        int64_t iv = json_integer_value(node);
        default_val = static_cast<double>(iv);
    } else if (required != 0) {
        char buf[1024];
        memset_wrapper(buf, 0, 0x400);
        snprintf_wrapper(buf, "$ json path: [%s] not a real.", path);
        LogAndAbort(8, 0, "\n");
    }

    release_lock(tls);
    return default_val;
}

// @0x18009ecd0 — CJson::GetString
// @confidence: H
const char* CJson_GetString(CJson* json, const char* path, const char* default_val, uint32_t required) {
    void* alloc = GetAllocator();
    void* tls = get_tls_context();
    release_lock(alloc);

    auto* cache = reinterpret_cast<void*>(json->cache);
    auto* node = reinterpret_cast<int*>(CJson_NavigatePath(path, json->root, required, cache));

    if (node == nullptr || *node != 2) {
        if (required != 0) {
            char buf[1024];
            memset_wrapper(buf, 0, 0x400);
            snprintf_wrapper(buf, "$ json path: [%s] not found or not a string.", path);
            LogAndAbort(8, 0, "\n");
        }
    } else {
        default_val = json_string_value(node);
    }

    release_lock(tls);
    return default_val;
}

// @0x18009dd20 — CJson::GetFloatDirect
// @confidence: H
float CJson_GetFloatDirect(CJson* json, void* key, float default_val) {
    if (key != nullptr) {
        // json_object_iter_value: returns the value for a jansson object iterator
        extern void* json_object_iter_value(void* iter);  // @0x1801c3210
        auto* node = reinterpret_cast<int*>(json_object_iter_value(key));
        if (node != nullptr) {
            if (*node == 4) {
                return static_cast<float>(json_real_value(node));
            }
            if (*node == 3) {
                int64_t iv = json_integer_value(node);
                return static_cast<float>(iv);
            }
        }
    }
    return default_val;
}

// @0x18009dda0 — CJson::GetFloatAtPath
// @confidence: H
float CJson_GetFloatAtPath(CJson* json, const char* path, float default_val, uint32_t required) {
    double d = CJson_GetReal(json, path, static_cast<double>(default_val), required);
    return static_cast<float>(d);
}

// @0x180099f20 — CJson::IsEmpty
// @confidence: H
bool CJson_IsEmpty(CJson* json) {
    void* result = CJson_FindFirstArrayElement(json, "");
    return result == nullptr;
}

// ============================================================================
// Read: Iteration
// ============================================================================

// @0x18009b920 — CJson::GetObjectKeyCount
// @confidence: H
int64_t CJson_GetObjectKeyCount(CJson* json, const char* path) {
    void* alloc = GetAllocator();
    void* tls = get_tls_context();
    release_lock(alloc);

    int64_t count = 0;
    if (path != nullptr) {
        count = json_object_iter_count(const_cast<char*>(path));
    }

    release_lock(tls);
    return count;
}

// ============================================================================
// Read: Path resolution helpers
// ============================================================================

// @0x18009ada0 — CJson::GetArrayElementChecked
// @confidence: H
void* CJson_GetArrayElementChecked(void* arr, const char* ctx_name, uint64_t index, int required) {
    void* elem = nullptr;
    if (arr != nullptr && *reinterpret_cast<int*>(arr) == 1) {
        elem = json_array_get(arr, index);
    }
    if (required != 0 && elem == nullptr) {
        char buf[1024];
        memset_wrapper(buf, 0, 0x400);
        if (ctx_name == nullptr || *ctx_name == '\0') ctx_name = "<unknown>";
        snprintf_wrapper(buf, "$ json path: index %llu not found in %s.", index, ctx_name);
        LogAndAbort(8, 0, "\n");
    }
    return elem;
}

// @0x18009ae70 — CJson::GetObjectFieldChecked
// @confidence: H
void* CJson_GetObjectFieldChecked(void* obj, const char* ctx_name, const char* key, int required) {
    void* field = nullptr;
    if (obj != nullptr && *reinterpret_cast<int*>(obj) == 0 && key != nullptr) {
        field = json_object_get(obj, key);
    }
    if (required != 0 && field == nullptr) {
        char buf[1024];
        memset_wrapper(buf, 0, 0x400);
        if (ctx_name == nullptr || *ctx_name == '\0') ctx_name = "<unknown>";
        snprintf_wrapper(buf, "$ json path: %s: { %s } not found.", ctx_name, key);
        LogAndAbort(8, 0, "\n");
    }
    return field;
}

// @0x18009a880 — CJson::NavigateArrayPath
// @confidence: H
void* CJson_NavigateArrayPath(const char* path, void* node, const char* ctx_name,
                               uint64_t required, const char** out_remaining, uint64_t* out_index) {
    int req = static_cast<int>(required & 0xFFFFFFFF);

    if (node == nullptr || *reinterpret_cast<int*>(node) != 1) {
        if (req != 0) {
            char buf[1024];
            memset_wrapper(buf, 0, 0x400);
            if (ctx_name == nullptr || *ctx_name == '\0') ctx_name = "<unknown>";
            snprintf_wrapper(buf, "$ json path: %s is not an array.", ctx_name);
            LogAndAbort(8, 0, "\n");
        }
        return nullptr;
    }

    int32_t next_type = 0;
    const char* remaining = CJson_ParseArrayIndex(path, reinterpret_cast<int64_t*>(out_index), &next_type);
    *out_remaining = remaining;
    uint64_t idx = *out_index;

    if (idx == 0xFFFFFFFFFFFFFFFFULL) {
        if (req != 0) {
            char buf[1024];
            memset_wrapper(buf, 0, 0x400);
            if (ctx_name == nullptr || *ctx_name == '\0') ctx_name = "<unknown>";
            snprintf_wrapper(buf, "$ json path: invalid index for array %s.", ctx_name);
            LogAndAbort(8, 0, "\n");
        }
        return nullptr;
    }

    if (next_type == 5) {
        // More array indices follow
        void* child = nullptr;
        if (*reinterpret_cast<int*>(node) == 1) {
            child = json_array_get(node, idx);
        }
        if (req != 0 && child == nullptr) {
            char buf[1024];
            memset_wrapper(buf, 0, 0x400);
            if (ctx_name == nullptr || *ctx_name == '\0') ctx_name = "<unknown>";
            snprintf_wrapper(buf, "$ json path: index %llu not found in %s.", idx, ctx_name);
            LogAndAbort(8, 0, "\n");
        }
        if (child != nullptr) {
            return CJson_NavigateArrayPath(*out_remaining, child, ctx_name, required, out_remaining, out_index);
        }
        return nullptr;
    }

    if (next_type == 6) {
        // Object key follows
        void* child = CJson_GetArrayElementChecked(node, ctx_name, idx, req);
        if (child != nullptr) {
            return CJson_NavigateObjectPath(*out_remaining, reinterpret_cast<int*>(child), ctx_name,
                                            required, reinterpret_cast<int64_t*>(out_remaining), out_index);
        }
        return nullptr;
    }

    // Terminal index — return the array and let caller resolve
    *out_remaining = path;
    return node;
}

// @0x18009aad0 — CJson::NavigateObjectPath
// @confidence: H
void* CJson_NavigateObjectPath(const char* path, void* node, const char* ctx_name,
                                uint64_t required, int64_t* out_remaining, uint64_t* out_index) {
    int req = static_cast<int>(required);

    if (node == nullptr || *reinterpret_cast<int*>(node) != 0) {
        if (req != 0) {
            char buf[1024];
            memset_wrapper(buf, 0, 0x400);
            if (ctx_name == nullptr || *ctx_name == '\0') ctx_name = "<unknown>";
            snprintf_wrapper(buf, "$ json path: %s is not an object.", ctx_name);
            LogAndAbort(8, 0, "\n");
        }
        return nullptr;
    }

    char key_buf[1024];
    memset_wrapper(key_buf, 0, 0x400);
    char c = *path;
    const char* p = path;
    int64_t key_len = 0;

    if (c != '\0') {
        while (c != '|') {
            if (c == '[') {
                const char* scan = p + 1;
                char sc = *scan;
                while (sc != '\0') {
                    if (sc == ']') goto extract_done;
                    if (static_cast<uint8_t>(sc - '0') > 9) break;
                    scan++;
                    sc = *scan;
                }
            }
            key_buf[key_len] = *p;
            key_len++;
            p++;
            c = *p;
            if (c == '\0') break;
        }
    }
extract_done:
    c = *p;
    key_buf[key_len] = '\0';

    if (c == '|') {
        // Object key followed by more path
        *out_remaining = reinterpret_cast<int64_t>(p + 1);
        void* child = nullptr;
        if (*reinterpret_cast<int*>(node) == 0) {
            child = json_object_get(node, key_buf);
        }
        if (req != 0 && child == nullptr) {
            char buf[1024];
            memset_wrapper(buf, 0, 0x400);
            if (ctx_name == nullptr || *ctx_name == '\0') ctx_name = "<unknown>";
            snprintf_wrapper(buf, "$ json path: %s: { %s } not found.", ctx_name, key_buf);
            LogAndAbort(8, 0, "\n");
        }
        if (child != nullptr) {
            return CJson_NavigateObjectPath(reinterpret_cast<const char*>(*out_remaining), child, key_buf,
                                            required, out_remaining, out_index);
        }
        return nullptr;
    }

    if (c == '[') {
        // Array index follows object key
        const char* scan = p + 1;
        char sc = *scan;
        while (sc != '\0') {
            if (sc == ']') {
                *out_remaining = reinterpret_cast<int64_t>(p + 1);
                void* child = nullptr;
                if (*reinterpret_cast<int*>(node) == 0) {
                    child = json_object_get(node, key_buf);
                }
                if (req != 0 && child == nullptr) {
                    char buf[1024];
                    memset_wrapper(buf, 0, 0x400);
                    if (ctx_name == nullptr || *ctx_name == '\0') ctx_name = "<unknown>";
                    snprintf_wrapper(buf, "$ json path: %s: { %s } not found.", ctx_name, key_buf);
                    LogAndAbort(8, 0, "\n");
                }
                if (child != nullptr) {
                    return CJson_NavigateArrayPath(reinterpret_cast<const char*>(*out_remaining), child, key_buf,
                                                    required, reinterpret_cast<const char**>(out_remaining), out_index);
                }
                return nullptr;
            }
            if (static_cast<uint8_t>(sc - '0') > 9) break;
            scan++;
            sc = *scan;
        }
    }

    // Terminal key
    *out_remaining = reinterpret_cast<int64_t>(path);
    return node;
}

// ============================================================================
// Write: Value Setters
// ============================================================================

// @0x1800980d0 — CJson::SetValue
// @confidence: H
uint64_t CJson_SetValue(CJson* json, const char* path, uint32_t required, void* error_ctx) {
    if (json->cache != 0) {
        // Cached = read-only, abort
        char buf[512];
        format_string(buf, "$ json path: %s: ERROR, json db is cached, read only.", path);
        LogAndAbort(8, 0, "\n");
    }

    void* alloc = GetAllocator();
    void* tls = get_tls_context();
    release_lock(alloc);

    auto* result = CJson_DeleteAtPath(path, json, required, error_ctx);

    release_lock(tls);
    return reinterpret_cast<uint64_t>(result) & 0xFFFFFFFF;
}

// @0x18009e150 — CJson::SetBoolean
// @confidence: H
void CJson_SetBoolean(CJson* json, const char* path, int value, void* error_ctx) {
    if (json->cache != 0) {
        char buf[1024];
        format_string(buf, "$ json path: %s: ERROR, json db is cached, read only.", path);
        LogAndAbort(8, 0, "\n");
    }

    void* alloc = GetAllocator();
    void* tls = get_tls_context();
    release_lock(alloc);

    uint64_t out_index = 0;
    char* out_key = nullptr;
    auto* parent = reinterpret_cast<int*>(CJson_NavigateForWrite(path, json, reinterpret_cast<int64_t*>(&out_key), &out_index));

    if (parent != nullptr) {
        if (*parent == 0) {
            // Object parent
            auto* existing = reinterpret_cast<int*>(json_object_get(parent, out_key));
            if (existing != nullptr && (*existing - 5) > 1) {
                char buf[1024];
                memset_wrapper(buf, 0, 0x400);
                snprintf_wrapper(buf, "$ json path: cannot change type of [%s] to boolean", path);
                LogAndAbort(8, 0, "\n");
            }
            json_object_del(parent, out_key);
            void* val = (value == 0) ? json_false_new() : json_true_new();
            json_object_set(parent, out_key, val);
        } else {
            // Array parent
            auto* existing = reinterpret_cast<int*>(json_array_get(parent, out_index));
            if (existing == nullptr || (*existing != 7 && (*existing - 5) > 1)) {
                char buf[1024];
                memset_wrapper(buf, 0, 0x400);
                snprintf_wrapper(buf, "$ json path: cannot change type of [%s] to boolean", path);
                LogAndAbort(8, 0, "\n");
            }
            void* val = (value == 0) ? json_false_new() : json_true_new();
            json_array_set(parent, out_index, val);
        }
    }

    release_lock(tls);
}

// @0x18009e3d0 — CJson::SetInt
// @confidence: H
void CJson_SetInt(CJson* json, const char* path, int64_t value, void* error_ctx) {
    if (json->cache != 0) {
        char buf[1024];
        format_string(buf, "$ json path: %s: ERROR, json db is cached, read only.", path);
        LogAndAbort(8, 0, "\n");
    }

    void* alloc = GetAllocator();
    void* tls = get_tls_context();
    release_lock(alloc);

    uint64_t out_index = 0;
    char* out_key = nullptr;
    auto* parent = reinterpret_cast<int*>(CJson_NavigateForWrite(path, json, reinterpret_cast<int64_t*>(&out_key), &out_index));

    if (parent != nullptr) {
        void* new_val = json_integer_new(value);
        if (*parent == 0) {
            auto* existing = reinterpret_cast<int*>(json_object_get(parent, out_key));
            if (existing != nullptr && ((*existing - 3) & 0xFFFFFFFB) != 0) {
                char buf[1024];
                memset_wrapper(buf, 0, 0x400);
                snprintf_wrapper(buf, "$ json path: cannot change type of [%s] to integer", path);
                LogAndAbort(8, 0, "\n");
            }
            json_object_del(parent, out_key);
            json_object_set(parent, out_key, new_val);
        } else {
            auto* existing = reinterpret_cast<int*>(json_array_get(parent, out_index));
            if (existing == nullptr || (*existing != 7 && *existing != 3)) {
                char buf[1024];
                memset_wrapper(buf, 0, 0x400);
                snprintf_wrapper(buf, "$ json path: cannot change type of [%s] to integer", path);
                LogAndAbort(8, 0, "\n");
            }
            json_array_set(parent, out_index, new_val);
        }
    }

    release_lock(tls);
}

// @0x18009e610 — CJson::SetReal
// @confidence: H
void CJson_SetReal(CJson* json, const char* path, double value, void* error_ctx) {
    if (json->cache != 0) {
        char buf[1024];
        format_string(buf, "$ json path: %s: ERROR, json db is cached, read only.", path);
        LogAndAbort(8, 0, "\n");
    }

    void* alloc = GetAllocator();
    void* tls = get_tls_context();
    release_lock(alloc);

    uint64_t out_index = 0;
    char* out_key = nullptr;
    auto* parent = reinterpret_cast<int*>(CJson_NavigateForWrite(path, json, reinterpret_cast<int64_t*>(&out_key), &out_index));

    if (parent != nullptr) {
        void* new_val = json_real_new(value);
        if (*parent == 0) {
            auto* existing = reinterpret_cast<int*>(json_object_get(parent, out_key));
            if (existing != nullptr && (((*existing - 3) & 0xFFFFFFFA) != 0 || *existing == 8)) {
                char buf[1024];
                memset_wrapper(buf, 0, 0x400);
                snprintf_wrapper(buf, "$ json path: cannot change type of [%s] to real", path);
                LogAndAbort(8, 0, "\n");
            }
            json_object_del(parent, out_key);
            json_object_set(parent, out_key, new_val);
        } else {
            auto* existing = reinterpret_cast<int*>(json_array_get(parent, out_index));
            if (existing == nullptr || (*existing != 7 && *existing != 4 && *existing != 3)) {
                char buf[1024];
                memset_wrapper(buf, 0, 0x400);
                snprintf_wrapper(buf, "$ json path: cannot change type of [%s] to real", path);
                LogAndAbort(8, 0, "\n");
            }
            json_array_set(parent, out_index, new_val);
        }
    }

    release_lock(tls);
}

// @0x18009e860 — CJson::SetString
// @confidence: H
void CJson_SetString(CJson* json, const char* path, const char* value, void* error_ctx) {
    if (json->cache != 0) {
        char buf[1024];
        format_string(buf, "$ json path: %s: ERROR, json db is cached, read only.", path);
        LogAndAbort(8, 0, "\n");
    }

    void* alloc = GetAllocator();
    void* tls = get_tls_context();
    release_lock(alloc);

    uint64_t out_index = 0;
    char* out_key = nullptr;
    auto* parent = reinterpret_cast<int*>(CJson_NavigateForWrite(path, json, reinterpret_cast<int64_t*>(&out_key), &out_index));

    if (parent != nullptr) {
        void* new_val = json_string_new(const_cast<void*>(reinterpret_cast<const void*>(value)));
        if (*parent == 0) {
            auto* existing = reinterpret_cast<int*>(json_object_get(parent, out_key));
            if (existing != nullptr && *existing != 7 && *existing != 2) {
                char buf[1024];
                memset_wrapper(buf, 0, 0x400);
                snprintf_wrapper(buf, "$ json path: cannot change type of [%s] to string", path);
                LogAndAbort(8, 0, "\n");
            }
            json_object_del(parent, out_key);
            json_object_set(parent, out_key, new_val);
        } else {
            auto* existing = reinterpret_cast<int*>(json_array_get(parent, out_index));
            if (existing == nullptr || (*existing != 7 && *existing != 2)) {
                char buf[1024];
                memset_wrapper(buf, 0, 0x400);
                snprintf_wrapper(buf, "$ json path: cannot change type of [%s] to string", path);
                LogAndAbort(8, 0, "\n");
            }
            json_array_set(parent, out_index, new_val);
        }
    }

    release_lock(tls);
}

// ============================================================================
// Write: Path creation
// ============================================================================

// @0x1800976e0 — CJson::EnsureArrayElement
// @confidence: H
void* CJson_EnsureArrayElement(void* arr, const char* ctx_name, uint64_t index) {
    auto* existing = reinterpret_cast<int*>(json_array_get(arr, index));
    if (existing == nullptr) {
        int64_t size = json_array_size(arr);
        for (int64_t i = (index - size) + 1; i > 0; i--) {
            void* null_val = json_null_new();
            json_array_append(arr, null_val);
        }
        json_array_get(arr, index);
    } else if (*existing != 7) {
        if (*existing == 1) return existing;  // Already an array
        char buf[1024];
        memset_wrapper(buf, 0, 0x400);
        if (ctx_name == nullptr || *ctx_name == '\0') ctx_name = "<unknown>";
        snprintf_wrapper(buf, "$ json path: cannot change type of %s..[%llu]", ctx_name, index);
        LogAndAbort(8, 0, "\n");
        return nullptr;
    }

    void* new_arr = json_array_new();
    int result = json_array_set(arr, index, new_arr);
    if (result != 0) {
        char buf[1024];
        memset_wrapper(buf, 0, 0x400);
        if (ctx_name == nullptr || *ctx_name == '\0') ctx_name = "<unknown>";
        snprintf_wrapper(buf, "$ json path: error adding array at %s..[%llu]", ctx_name, index);
        LogAndAbort(8, 0, "\n");
        return nullptr;
    }
    return json_array_get(arr, index);
}

// @0x180097850 — CJson::EnsureObjectAtArrayIndex
// @confidence: H
void* CJson_EnsureObjectAtArrayIndex(void* arr, const char* ctx_name, uint64_t index) {
    auto* existing = reinterpret_cast<int*>(json_array_get(arr, index));
    if (existing == nullptr) {
        int64_t size = json_array_size(arr);
        for (int64_t i = (index - size) + 1; i > 0; i--) {
            void* null_val = json_null_new();
            json_array_append(arr, null_val);
        }
        json_array_get(arr, index);
    } else if (*existing != 7) {
        if (*existing == 0) return existing;  // Already an object
        char buf[1024];
        memset_wrapper(buf, 0, 0x400);
        if (ctx_name == nullptr || *ctx_name == '\0') ctx_name = "<unknown>";
        snprintf_wrapper(buf, "$ json path: cannot change type of %s..[%llu]", ctx_name, index);
        LogAndAbort(8, 0, "\n");
        return nullptr;
    }

    void* new_obj = json_object_new();
    int result = json_array_set(arr, index, new_obj);
    if (result != 0) {
        char buf[1024];
        memset_wrapper(buf, 0, 0x400);
        if (ctx_name == nullptr || *ctx_name == '\0') ctx_name = "<unknown>";
        snprintf_wrapper(buf, "$ json path: error adding object at %s..[%llu]", ctx_name, index);
        LogAndAbort(8, 0, "\n");
        return nullptr;
    }
    return json_array_get(arr, index);
}

// @0x18009a7b0 — CJson::SetNodeSimple
// @confidence: H
void CJson_SetNodeSimple(CJson* json, const char* path, int required, const char* key) {
    CJson_SetNodeAtPath(json, path, required, 0, 0, key);
}

// @0x18009a7d0 — CJson::SetNodeWithBuffer
// @confidence: H
void* CJson_SetNodeWithBuffer(CJson* json, void* buf, int required, const char* key) {
    auto* alloc = get_tls_context();
    init_buffer_context(buf, 0, 0, alloc);
    *reinterpret_cast<uint64_t*>(reinterpret_cast<uintptr_t>(buf) + 0x20) = 0x20;
    *reinterpret_cast<uint64_t*>(reinterpret_cast<uintptr_t>(buf) + 0x28) = 0;
    *reinterpret_cast<uint64_t*>(reinterpret_cast<uintptr_t>(buf) + 0x30) = 0;

    extern void buf_ctx_copy_tls(void* buf, void* tls_data);  // @0x18008c4c0
    // DAT_180376638 is a TLS index used by the engine allocator; use get_tls_context() as equivalent
    void* tls_data = get_tls_context();
    buf_ctx_copy_tls(buf, tls_data);

    CJson_SetNodeAtPath(json, reinterpret_cast<const char*>(buf), required, 0, 0, key);
    return buf;
}

// ============================================================================
// Type Query
// ============================================================================

// @0x18009ef60 — CJson::TypeOf
// @confidence: H
int CJson_TypeOf(CJson* json, const char* path) {
    void* alloc = GetAllocator();
    void* tls = get_tls_context();
    release_lock(alloc);

    auto* cache = reinterpret_cast<void*>(json->cache);
    auto* root = json->root;

    if (path == nullptr) goto not_found;

    {
        const char* p = SkipPathPrefix(path);
        int result_type = 1;  // null/not-found
        void* node = root;

        if (*p == '\0') {
            // Path is empty — return type of root
            if (node != nullptr) {
                goto resolve_type;
            }
            goto not_found;
        }

        // Check root type
        if (root == nullptr) goto not_found;
        {
            int root_type = *reinterpret_cast<int*>(root);
            bool is_object = (root_type == 0);
            bool is_array = (root_type == 1);

            if (!is_object && !is_array) goto not_found;

            if (cache != nullptr) {
                // Hash lookup in cache
                uint64_t hash = hash_string(path, 0xFFFFFFFFFFFFFFFFULL, 0, -1, 1);
                auto* cache_map = reinterpret_cast<CHashMap*>(cache);
                auto* bucket_array = reinterpret_cast<uint32_t*>(cache_map->bucket_array);
                uint32_t entry_idx = bucket_array[static_cast<uint32_t>(hash & 0xFFF)];
                node = nullptr;

                while (entry_idx != 0xFFFFFFFF) {
                    int64_t buf_data = *reinterpret_cast<int64_t*>(&cache_map->buf_ctx[0]);
                    uint32_t pg = entry_idx / cache_map->entries_per_page;
                    uint32_t sl = entry_idx % cache_map->entries_per_page;
                    int64_t page = *reinterpret_cast<int64_t*>(buf_data + pg * 8);
                    auto* entry = reinterpret_cast<uint8_t*>(page + sl * 0x20);

                    if (*reinterpret_cast<uint64_t*>(entry + 0x10) == hash) {
                        node = *reinterpret_cast<void**>(entry + 0x18);
                        break;
                    }
                    entry_idx = *reinterpret_cast<uint32_t*>(entry);
                }
            } else {
                // Manual path navigation
                const char* remaining = nullptr;
                uint64_t index = 0;
                if (is_array) {
                    node = CJson_NavigateArrayPath(path, root, "<root>", 0, &remaining, &index);
                } else {
                    node = CJson_NavigateObjectPath(path, root, "<root>", 0,
                                                     reinterpret_cast<int64_t*>(&remaining), &index);
                }
            }
        }

resolve_type:
        if (node != nullptr) {
            int jtype = *reinterpret_cast<int*>(node);
            switch (jtype) {
            case 0: result_type = 6; break;  // object
            case 1: result_type = 5; break;  // array
            case 2: result_type = 1; break;  // string
            case 3: result_type = 2; break;  // integer
            case 4: result_type = 3; break;  // real
            case 5:
            case 6: result_type = 4; break;  // boolean
            default: goto not_found;
            }
            release_lock(tls);
            return result_type;
        }
    }

not_found:
    release_lock(tls);
    return 0;
}

// @0x18009f600 — CJson::PathExists
// @confidence: H
bool CJson_PathExists(CJson* json, const char* path) {
    void* alloc = GetAllocator();
    void* tls = get_tls_context();
    release_lock(alloc);

    auto* cache = reinterpret_cast<void*>(json->cache);
    auto* root = json->root;
    void* node = nullptr;

    if (path == nullptr) goto done;

    {
        const char* p = SkipPathPrefix(path);
        node = root;
        if (*p == '\0') goto done;

        if (root == nullptr) { node = nullptr; goto done; }
        int root_type = *reinterpret_cast<int*>(root);
        bool is_object = (root_type == 0);
        bool is_array = (root_type == 1);

        if (!is_object && !is_array) { node = nullptr; goto done; }

        if (cache != nullptr) {
            uint64_t hash = hash_string(path, 0xFFFFFFFFFFFFFFFFULL, 0, -1, 1);
            auto* cache_map = reinterpret_cast<CHashMap*>(cache);
            auto* bucket_array = reinterpret_cast<uint32_t*>(cache_map->bucket_array);
            uint32_t entry_idx = bucket_array[static_cast<uint32_t>(hash & 0xFFF)];
            node = nullptr;

            while (entry_idx != 0xFFFFFFFF) {
                int64_t buf_data = *reinterpret_cast<int64_t*>(&cache_map->buf_ctx[0]);
                uint32_t pg = entry_idx / cache_map->entries_per_page;
                uint32_t sl = entry_idx % cache_map->entries_per_page;
                int64_t page = *reinterpret_cast<int64_t*>(buf_data + pg * 8);
                auto* entry = reinterpret_cast<uint8_t*>(page + sl * 0x20);

                if (*reinterpret_cast<uint64_t*>(entry + 0x10) == hash) {
                    node = *reinterpret_cast<void**>(entry + 0x18);
                    break;
                }
                entry_idx = *reinterpret_cast<uint32_t*>(entry);
            }
        } else {
            const char* remaining = nullptr;
            uint64_t index = 0;
            if (is_array) {
                node = CJson_NavigateArrayPath(path, root, "<root>", 0, &remaining, &index);
            } else {
                node = CJson_NavigateObjectPath(path, root, "<root>", 0,
                                                 reinterpret_cast<int64_t*>(&remaining), &index);
            }
        }
    }

done:
    release_lock(tls);
    return node != nullptr;
}

// ============================================================================
// Allocator setup
// ============================================================================

// @0x18009af50 — CJson::SetAllocator
// @confidence: H
void CJson_SetAllocator(void* allocator) {
    if (allocator == nullptr) {
        g_json_allocator_mode = 0;
        return;
    }
    g_json_allocator_mode = 1;
    g_json_allocator = allocator;
    g_json_allocator_ctx = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(allocator) + 0x10);
    // Direct stores to jansson's internal alloc/free function pointers
    // _data.180375ab0 = &CJson_AllocCallback, _data.180375ab8 = &CJson_FreeCallback
    extern void* g_jansson_alloc_func;   // @0x180375ab0
    extern void* g_jansson_free_func;    // @0x180375ab8
    g_jansson_alloc_func = reinterpret_cast<void*>(&CJson_AllocCallback);
    g_jansson_free_func = reinterpret_cast<void*>(&CJson_FreeCallback);
}

// @0x18009b0b0 — CJson::FreeCallback
// @confidence: H
void CJson_FreeCallback(void* ptr) {
    void* alloc;
    if (g_json_allocator_mode == 1) {
        alloc = g_json_allocator;
    } else {
        alloc = get_tls_context();
    }
    // vtable[6](allocator, ptr) — free
    auto** avt = *reinterpret_cast<void***>(alloc);
    using FreeFn = void(*)(void*, void*);
    reinterpret_cast<FreeFn>(reinterpret_cast<uintptr_t*>(avt)[6])(alloc, ptr);
}

// @0x18009b100 — CJson::AllocCallback
// @confidence: H
void* CJson_AllocCallback(void* size) {
    void* alloc;
    if (g_json_allocator_mode == 1) {
        alloc = g_json_allocator;
    } else {
        alloc = get_tls_context();
    }
    // vtable[1](allocator, size) — alloc
    auto** avt = *reinterpret_cast<void***>(alloc);
    using AllocFn = void*(*)(void*, void*);
    return reinterpret_cast<AllocFn>(reinterpret_cast<uintptr_t*>(avt)[1])(alloc, size);
}

} // namespace NRadEngine
