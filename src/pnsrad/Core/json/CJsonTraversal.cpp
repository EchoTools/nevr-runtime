#include "src/pnsrad/Core/json/CJsonTraversal.h"
#include <cstring>

namespace NRadEngine {

// Global allocator references (defined in CJson.cpp as static — need extern-visible copies)
extern void* g_json_allocator;     // @0x180384d00
extern int   g_json_allocator_mode; // @0x180384d08

// Object iteration (wraps CJson_FindNextArrayElement for object key traversal)
// @0x18009b9a0 — CJson::FindNextArrayElement reused for object iteration
static inline void* CJson_ObjectIterFirst(CJson* json, const char* path, int64_t after) {
    return CJson_FindNextArrayElement(json, path, after);
}

// Helper: copy 512-byte path buffer via 128-byte unrolled loop (matches binary pattern)
// @0x18009C4F0 (inlined) — 512-byte unrolled memcpy for path buffers
static void CopyPathBuffer(char* dst, const void* src) {
    auto* s = reinterpret_cast<const uint64_t*>(src);
    auto* d = reinterpret_cast<uint64_t*>(dst);
    for (int i = 0; i < 64; i++) {
        d[i] = s[i];
    }
}

// ============================================================================
// Dispatch
// ============================================================================

// @0x18009c3f0 — CJsonTraversal::Dispatch
// @confidence: H
void CJsonTraversal_Dispatch(CJsonTraversal* trav, const char* dst_path, const char* src_path) {
    // Normalize paths: strip leading '|'
    const char* src_p = src_path;
    if (*src_path == '|') src_p = src_path + 1;
    const char* dst_p = dst_path;
    if (*dst_path == '|') dst_p = dst_path + 1;

    // Query type of source node
    int type = CJson_TypeOf(trav->src, src_p);

    auto** vtable = reinterpret_cast<void**>(trav->vtable);
    using HandlerFn = void(*)(CJsonTraversal*, const char*, const char*);

    switch (type) {
    case 0: // object → vfn4 (handle object begin)
        reinterpret_cast<HandlerFn>(vtable[4])(trav, dst_p, src_p);
        break;
    case 1: // string → vfn3 (handle string)
        reinterpret_cast<HandlerFn>(vtable[3])(trav, dst_p, src_p);
        break;
    case 2: // integer → vfn1 (handle integer)
        reinterpret_cast<HandlerFn>(vtable[1])(trav, dst_p, src_p);
        break;
    case 3: // real → vfn2 (handle real)
        reinterpret_cast<HandlerFn>(vtable[2])(trav, dst_p, src_p);
        break;
    case 4: // boolean → vfn0 (handle boolean)
        reinterpret_cast<HandlerFn>(vtable[0])(trav, dst_p, src_p);
        break;
    case 5: // array → vfn5 (handle array)
        reinterpret_cast<HandlerFn>(vtable[5])(trav, dst_p, src_p);
        break;
    case 6: // dict → vfn6 (handle dict)
        reinterpret_cast<HandlerFn>(vtable[6])(trav, dst_p, src_p);
        break;
    }

    // Post-dispatch hook
    if ((trav->flags & 1) != 0) {
        extern void post_traversal_hook(int val);  // @0x1800a1040
        post_traversal_hook(0);
    }
}

// @0x18009ede0 — CJsonTraversal::TraverseRoot
// @confidence: H
void CJsonTraversal_TraverseRoot(CJsonTraversal* trav, CJson* dst_json, CJson* src_json) {
    trav->dst = dst_json;       // this+0x08 = param_1
    trav->src = src_json;       // this+0x10 = param_2

    bool exists = CJson_PathExists(src_json, "");
    if (!exists) return;

    int type = CJson_TypeOf(trav->src, "");
    auto** vtable = reinterpret_cast<void**>(trav->vtable);
    using HandlerFn = void(*)(CJsonTraversal*, const char*, const char*);

    switch (type) {
    case 0:
        reinterpret_cast<HandlerFn>(vtable[4])(trav, "", "");
        break;
    case 1:
        reinterpret_cast<HandlerFn>(vtable[3])(trav, "", "");
        break;
    case 2:
        reinterpret_cast<HandlerFn>(vtable[1])(trav, "", "");
        break;
    case 3:
        reinterpret_cast<HandlerFn>(vtable[2])(trav, "", "");
        break;
    case 4:
        reinterpret_cast<HandlerFn>(vtable[0])(trav, "", "");
        break;
    case 5:
        reinterpret_cast<HandlerFn>(vtable[5])(trav, "", "");
        break;
    case 6:
        reinterpret_cast<HandlerFn>(vtable[6])(trav, "", "");
        break;
    }

    if ((trav->flags & 1) != 0) {
        extern void post_traversal_hook(int val);  // @0x1800a1040
        post_traversal_hook(0);
    }
}

// @0x18009ef20 — CJsonTraversal::TraverseFromPath
// @confidence: H
void CJsonTraversal_TraverseFromPath(CJsonTraversal* trav, CJson* dst_json, CJson* src_json, const char* path) {
    trav->dst = dst_json;       // this+0x08 = param_1
    trav->src = src_json;       // this+0x10 = param_2

    bool exists = CJson_PathExists(src_json, path);
    if (exists) {
        CJsonTraversal_Dispatch(trav, path, path);
    }
}

// @0x18009fc30 — CJsonTraversal::SetCopyMode
// @confidence: H
void* CJsonTraversal_SetCopyMode(CJsonTraversal* trav) {
    trav->flags &= ~4ULL;  // Clear bit 2
    trav->flags |= 2ULL;   // Set bit 1
    return trav;
}

// ============================================================================
// Default vtable implementations
// ============================================================================

// @0x18009C6C0 — vfunction1: handle boolean
// @confidence: H
void CJsonTraversal_HandleBoolean(CJsonTraversal* trav, const char* dst_path,
                                   const char* src_path, void* error_ctx) {
    // Verify destination expects boolean type
    int type = CJson_TypeOf(trav->dst, dst_path);
    if (type != 4) {
        CJson_SetValue(trav->dst, dst_path, 0, error_ctx);
    }

    // Read boolean from source
    uint32_t value = CJson_GetBoolean(trav->src, src_path, 0, 0);
    // Write boolean to destination
    CJson_SetBoolean(trav->dst, dst_path, value, nullptr);
}

// @0x18009C740 — vfunction2: handle integer
// @confidence: H
void CJsonTraversal_HandleInteger(CJsonTraversal* trav, const char* dst_path,
                                   const char* src_path, const char* error_ctx) {
    // Verify destination expects integer type
    int type = CJson_TypeOf(trav->dst, dst_path);
    if (type != 2) {
        CJson_SetValue(trav->dst, dst_path, 0, const_cast<char*>(error_ctx));
    }

    CJson* dst_json = trav->dst;
    CJson* src_json = trav->src;

    // Get TLS context for locking
    void* alloc = g_json_allocator;
    extern int g_json_allocator_mode;
    if (g_json_allocator_mode != 1) {
        alloc = get_tls_context();
    }
    void* tls = get_tls_context();
    release_lock(alloc);

    // Navigate source to find the integer value
    auto* cache = reinterpret_cast<void*>(src_json->cache);
    auto* root = src_json->root;

    // Path navigation (follows the same complex pattern as other navigators)
    // For brevity, delegates to NavigatePath which handles the full path syntax
    void* node = CJson_NavigatePath(src_path, root, 0, cache);

    if (node != nullptr && *reinterpret_cast<int*>(node) == 3) {
        int64_t value = json_integer_value(node);
        release_lock(tls);
        CJson_SetInt(dst_json, dst_path, value, nullptr);
    } else {
        release_lock(tls);
    }
}

// @0x18009CFD0 — vfunction3: handle real/float
// @confidence: H
void CJsonTraversal_HandleReal(CJsonTraversal* trav, const char* dst_path,
                                const char* src_path, const char* error_ctx) {
    int type = CJson_TypeOf(trav->dst, dst_path);
    if (type != 3) {
        CJson_SetValue(trav->dst, dst_path, 0, const_cast<char*>(error_ctx));
    }

    CJson* src_json = trav->src;

    void* alloc = g_json_allocator;
    extern int g_json_allocator_mode;
    if (g_json_allocator_mode != 1) {
        alloc = get_tls_context();
    }
    void* tls = get_tls_context();
    release_lock(alloc);

    auto* cache = reinterpret_cast<void*>(src_json->cache);
    void* node = CJson_NavigatePath(src_path, src_json->root, 0, cache);

    if (node != nullptr) {
        double value;
        int node_type = *reinterpret_cast<int*>(node);
        if (node_type == 4) {
            value = json_real_value(node);
        } else if (node_type == 3) {
            int64_t iv = json_integer_value(node);
            value = static_cast<double>(iv);
        } else {
            release_lock(tls);
            return;
        }
        release_lock(tls);
        CJson_SetReal(trav->dst, dst_path, value, nullptr);
    } else {
        release_lock(tls);
    }
}

// @0x18009D690 — vfunction4: handle string
// @confidence: H
void CJsonTraversal_HandleString(CJsonTraversal* trav, const char* dst_path,
                                  const char* src_path, const char* error_ctx) {
    int type = CJson_TypeOf(trav->dst, dst_path);
    if (type != 1) {
        CJson_SetValue(trav->dst, dst_path, 0, const_cast<char*>(error_ctx));
    }

    CJson* src_json = trav->src;

    void* alloc = g_json_allocator;
    extern int g_json_allocator_mode;
    if (g_json_allocator_mode != 1) {
        alloc = get_tls_context();
    }
    void* tls = get_tls_context();
    release_lock(alloc);

    auto* cache = reinterpret_cast<void*>(src_json->cache);
    void* node = CJson_NavigatePath(src_path, src_json->root, 0, cache);

    if (node != nullptr && *reinterpret_cast<int*>(node) == 2) {
        const char* value = json_string_value(node);
        release_lock(tls);
        CJson_SetString(trav->dst, dst_path, value, nullptr);
    } else {
        release_lock(tls);
    }
}

// @0x18009C4F0 — vfunction6: handle array iteration
// @confidence: H
void CJsonTraversal_HandleArray(CJsonTraversal* trav, const char* dst_path,
                                 const char* src_path, void* error_ctx) {
    CJson* dst_json = trav->dst;
    CJson* src_json = trav->src;

    // Compound condition from binary: if copy-mode OR destination type != array(5),
    // reset destination value first
    uint64_t mode = trav->flags & 6;
    if (mode == 2 ||
        CJson_TypeOf(dst_json, dst_path) != 5) {
        CJson_SetValue(dst_json, dst_path, 0, error_ctx);
    }

    // Get source array element count
    void* arr_size = CJson_FindFirstArrayElement(src_json, src_path);
    void* src_idx = nullptr;
    void* dst_arr_idx = nullptr;

    if (mode == 4) {
        // Parallel iteration mode: get destination array element count
        dst_arr_idx = CJson_FindFirstArrayElement(dst_json, dst_path);
    }

    if (arr_size == nullptr) return;

    do {
        // Build source path: "%s[%llu]"
        char src_buf[512];
        char src_path_buf[512];
        format_string(src_buf, "%s[%llu]", src_path, src_idx);
        CopyPathBuffer(src_path_buf, src_buf);

        // Build destination path: "%s[%llu]"
        char dst_buf[512];
        char dst_path_buf[512];
        format_string(dst_buf, "%s[%llu]", dst_path, dst_arr_idx);
        CopyPathBuffer(dst_path_buf, dst_buf);

        CJsonTraversal_Dispatch(trav, dst_path_buf, src_path_buf);

        src_idx = reinterpret_cast<void*>(reinterpret_cast<intptr_t>(src_idx) + 1);
        dst_arr_idx = reinterpret_cast<void*>(reinterpret_cast<intptr_t>(dst_arr_idx) + 1);
    } while (src_idx < arr_size);
}

// @0x18009CDD0 — vfunction7: handle object key iteration
// @confidence: H
void CJsonTraversal_HandleObject(CJsonTraversal* trav, const char* dst_path,
                                  const char* src_path, void* error_ctx) {
    // Verify destination type is object(6)
    int type = CJson_TypeOf(trav->dst, dst_path);
    if (type != 6) {
        CJson_SetValue(trav->dst, dst_path, 0, error_ctx);
    }

    // Get first object iterator — CJson::FindNextArrayElement(src, src_path, 0)
    void* iter = CJson_ObjectIterFirst(trav->src, src_path, 0);
    void* alloc = g_json_allocator;

    while (iter != nullptr) {
        // Get key name with TLS locking
        extern int g_json_allocator_mode;
        void* a = alloc;
        if (g_json_allocator_mode != 1) {
            a = get_tls_context();
        }
        void* tls = get_tls_context();
        release_lock(a);

        int64_t key_name = 0;
        if (iter != nullptr) {
            key_name = reinterpret_cast<int64_t>(json_object_iter_key(iter));  // FUN_1801c31c0
        }

        release_lock(tls);

        // Build paths: "%s|%s"
        char src_buf[512];
        char src_path_buf[512];
        format_string(src_buf, "%s|%s", src_path, key_name);
        CopyPathBuffer(src_path_buf, src_buf);

        char dst_buf[512];
        char dst_path_buf[512];
        format_string(dst_buf, "%s|%s", dst_path, key_name);
        CopyPathBuffer(dst_path_buf, dst_buf);

        CJsonTraversal_Dispatch(trav, dst_path_buf, src_path_buf);

        // Advance iterator — CJson::FindNextArrayElement(src, src_path, iter)
        iter = CJson_ObjectIterFirst(trav->src, src_path, reinterpret_cast<int64_t>(iter));
        alloc = g_json_allocator;
    }
}

// Global allocator references declared at top of file

} // namespace NRadEngine
