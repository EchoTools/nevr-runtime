#ifndef PNSRAD_CORE_JSON_CJSON_H
#define PNSRAD_CORE_JSON_CJSON_H

/* @module: pnsrad.dll */
/* @purpose: CJson — JSON path-based data store for engine configuration */
/* @source: d:\projects\rad\dev\src\engine\libs\json\cjson.cpp */
/* @note: This is the pnsrad.dll instantiation of the shared NRadEngine JSON system.
 *        The same code exists in echovr.exe at different addresses.
 *        Uses jansson-compatible json_t* under the hood (json type tags:
 *        0=object, 1=array, 2=string, 3=integer, 4=real, 5=true, 6=false, 7=null) */

#include <cstdint>
#include <cstddef>

namespace NRadEngine {

// Forward declarations
struct CHashMap;

// ============================================================================
// External helpers used by CJson
// ============================================================================

// Allocator / TLS
extern void* get_tls_context();              // @0x180089700
extern void  release_lock(void* lock);       // @0x180089a00
extern void  memset_wrapper(void* ptr, int val, uint64_t size);  // @0x1800897f0
extern void  LogAndAbort(int level, int ctx, const char* fmt, ...);  // @0x1800929a0
extern void* format_string(void* out_buf, const char* fmt, ...);  // @0x18007ee20
extern int   snprintf_wrapper(char* buf, const char* fmt, ...);   // @0x180093ff0
extern uint64_t hash_string(const char* str, uint64_t max_len, int flags, int64_t p3, uint64_t p4);  // @0x1800895d0
extern int   is_whitespace(char c);          // @0x180093060
extern int   is_valid_number(const char* str, int base);  // @0x180092fc0
extern int64_t strtoll_wrapper(const char* str, int base);  // @0x1800933b0
extern uint64_t strcmp_custom(const char* a, const char* b, int flags, int64_t max, ...);  // @0x180092c30
extern int64_t strlen_wrapper(const char* str);  // @0x180093100

// jansson-like JSON primitives
extern void* json_object_get(void* obj, const char* key);  // @0x1801c3180
extern void  json_object_del(void* obj, const char* key);  // @0x1801c3160
extern int   json_object_set(void* obj, const char* key, void* val);  // @0x1801c3200
extern void* json_array_get(void* arr, uint64_t index);    // @0x1801c2bc0
extern int   json_array_set(void* arr, uint64_t index, void* val);   // @0x1801c2c90
extern void* json_array_append(void* arr, void* val);      // @0x1801c2a90
extern int64_t json_array_size(void* arr);   // @0x1801c2d40
extern void  json_decref(void* val);         // @0x1801c2dc0
extern void* json_incref(void* val);         // @0x1801c2d60
extern void* json_object_new();              // @0x1801c2f50
extern void* json_null_new();               // @0x1801c2f40
extern void* json_array_new();              // @0x1801c2a20
extern void* json_integer_new(int64_t val);  // @0x1801c2e90
extern void* json_true_new();               // @0x1801c3540
extern void* json_false_new();              // @0x1801c2e80
extern void* json_real_new(double val);      // @0x1801c3330
extern void* json_string_new(void* str);     // @0x1801c3430
extern int64_t json_integer_value(void* val); // @0x1801c2f20
extern double  json_real_value(void* val);    // @0x1801c3410
extern const char* json_string_value(void* val);  // @0x1801c34b0
extern int64_t json_object_iter_count(void* obj);  // @0x1801c31c0
extern void* json_object_iter_key(void* iter);     // @0x1801c31f0
extern void* json_loads(void* str, int64_t len, int flags, void* error_buf);  // @0x1801c37a0

// ============================================================================
// CJson — JSON document with optional path cache
// ============================================================================
// Layout: { void* root_json; int64_t cache_ptr; }
// root_json (+0x00): json_t* root of the JSON tree
// cache_ptr (+0x08): CHashMap* for path→value caching (0 = uncached/writable)
//
// When cache_ptr != 0, the document is read-only (cached mode).
// All set operations check cache_ptr first and abort if cached.

/* @addr: 0x180097650+ */
/* @size: 0x10 */
/* @confidence: H */
struct CJson {
    void*    root;       // +0x00: json_t* root node
    int64_t  cache;      // +0x08: CHashMap* path cache (0 = writable)
};
static_assert(sizeof(CJson) == 0x10);
static_assert(offsetof(CJson, root) == 0x00);
static_assert(offsetof(CJson, cache) == 0x08);

// ============================================================================
// CJson path syntax:
//   "|key"        — object key lookup (separator)
//   "[N]"         — array index lookup
//   "|key1|key2"  — nested object path
//   "[0]|key"     — array element then object key
//   ""            — root node
// ============================================================================

// --- Lifecycle ---

// @0x180097650 — CJson::Clear
// Releases old root, sets root to empty string literal.
// @confidence: H
void CJson_Clear(CJson* json);

// @0x180097680 — CJson::MoveAssign
// Move assignment: clears this, steals src's root, zeroes src.
// @confidence: H
void* CJson_MoveAssign(CJson* dst, CJson* src);

// @0x18009ddc0 — CJson::Reset
// Same as CJson_Clear. Releases root, sets to empty string.
// @confidence: H
void CJson_Reset(CJson* json);

// @0x18009ddf0 — CJson::ReleaseCache
// Destroys the path cache hash map and frees it. Sets cache to 0.
// @confidence: H
void CJson_ReleaseCache(CJson* json);

// @0x180099f40 — CJson::BuildCache
// Allocates CHashMap (0x68 bytes) and populates it by traversing the JSON tree.
// Only builds if root != null and cache == 0.
// @confidence: H
void CJson_BuildCache(CJson* json);

// --- Parsing ---

// @0x18009eaa0 — CJson::StripComments
// In-place removal of // and /* */ comments from JSON text buffer.
// Also handles escape sequences and trailing comma cleanup.
// param_1: [in/out] pointer to buffer length
// @confidence: H
void CJson_StripComments(char* buffer, uint64_t* length);

// @0x180099db0 — CJson::ParseBuffer
// Strips comments, then parses JSON from buffer using json_loads.
// Reports format errors via CBaseErr.
// param_3: error context for logging
// Returns: 0 on success, error code on failure
// @confidence: H
uint64_t CJson_ParseBuffer(CJson* json, const char* buffer, int64_t length, void* error_ctx);

// @0x180099d10 — CJson::ParseFromMemBlock
// Parses JSON from a CMemBlock-like buffer. Optionally strips trailing whitespace
// before passing to CJson_ParseBuffer.
// Returns: 0 on success, error code on failure (same as ParseBuffer)
// @confidence: H
uint32_t CJson_ParseFromMemBlock(CJson* json, int64_t* mem_block, int strip_trailing, void* error_ctx);

// --- Read: Path Navigation ---

// @0x18009dec0 — CJson::NavigatePath
// Core path resolution: walks the JSON tree following the path syntax.
// If cache is available, does hash lookup first (O(1)).
// Otherwise falls through to CJson_NavigateArray/CJson_NavigateObject.
// param_3: required flag (0=return null, 1=abort on not found)
// param_4: cache hash map (or null)
// Returns: json_t* at the resolved path, or null.
// @confidence: H
void* CJson_NavigatePath(const char* path, void* root, uint32_t required, void* cache);

// @0x18009bff0 — CJson::ParseArrayIndex
// Parses "[N]" from the path string.
// Sets *out_index to the numeric index, *out_type to 5 (more array) or 6 (object next).
// Returns: remaining path after "]", or null on parse error.
// @confidence: H
const char* CJson_ParseArrayIndex(const char* path, int64_t* out_index, int32_t* out_type);

// --- Read: Value Getters ---

// @0x180097fb0 — CJson::GetBoolean
// Returns boolean value (type 5=true, 6=false) at path.
// @confidence: H
uint32_t CJson_GetBoolean(CJson* json, const char* path, uint32_t default_val, uint32_t required);

// @0x18009afa0 — CJson::GetInt
// Returns integer value (type 3) at path.
// @confidence: H
int64_t CJson_GetInt(CJson* json, const char* path, int64_t default_val, uint32_t required);

// @0x18009b140 — CJson::GetReal
// Returns real/double value (type 3 or 4) at path.
// Falls back to integer→double conversion for type 3.
// @confidence: H
double CJson_GetReal(CJson* json, const char* path, double default_val, uint32_t required);

// @0x18009ecd0 — CJson::GetString
// Returns string value (type 2) at path.
// @confidence: H
const char* CJson_GetString(CJson* json, const char* path, const char* default_val, uint32_t required);

// @0x18009dd20 — CJson::GetFloatDirect
// Reads float from a json_t* key in an object. No path navigation.
// @confidence: H
float CJson_GetFloatDirect(CJson* json, void* key, float default_val);

// @0x18009dda0 — CJson::GetFloatAtPath (thin wrapper)
// Calls CJson_GetReal, casts result to float.
// @confidence: H
float CJson_GetFloatAtPath(CJson* json, const char* path, float default_val, uint32_t required);

// @0x180099f20 — CJson::IsEmpty
// Returns true if navigating root ("") yields null.
// @confidence: H
bool CJson_IsEmpty(CJson* json);

// --- Read: Iteration ---

// @0x180098f60 — CJson::FindFirstArrayElement
// Navigates to the array at `path`, returns pointer to first element.
// If cache is available, uses hash-based lookup.
// @confidence: H
void* CJson_FindFirstArrayElement(CJson* json, const char* path);

// @0x18009b9a0 — CJson::FindNextArrayElement
// Like FindFirstArrayElement but starts searching after `after_element`.
// Used for iteration: call with 0 for first, then pass previous result.
// @confidence: H
void* CJson_FindNextArrayElement(CJson* json, const char* path, int64_t after_element);

// @0x18009b920 — CJson::GetObjectKeyCount
// Returns number of keys in the object at `path`.
// @confidence: H
int64_t CJson_GetObjectKeyCount(CJson* json, const char* path);

// --- Read: Path resolution helpers ---

// @0x18009a880 — CJson::NavigateArrayPath
// Recursively navigates array indices in a path.
// @confidence: H
void* CJson_NavigateArrayPath(const char* path, void* node, const char* ctx_name,
                               uint64_t required, const char** out_remaining, uint64_t* out_index);

// @0x18009aad0 — CJson::NavigateObjectPath
// Recursively navigates object keys in a path.
// @confidence: H
void* CJson_NavigateObjectPath(const char* path, void* node, const char* ctx_name,
                                uint64_t required, int64_t* out_remaining, uint64_t* out_index);

// @0x18009ada0 — CJson::GetArrayElementChecked
// Gets array element at index with optional abort on failure.
// @confidence: H
void* CJson_GetArrayElementChecked(void* arr, const char* ctx_name, uint64_t index, int required);

// @0x18009ae70 — CJson::GetObjectFieldChecked
// Gets object field by key with optional abort on failure.
// @confidence: H
void* CJson_GetObjectFieldChecked(void* obj, const char* ctx_name, const char* key, int required);

// --- Write: Value Setters ---

// @0x1800980d0 — CJson::SetValue
// Core set operation. Checks if cached (aborts if so).
// Delegates to CJson_DeleteAtPath if writable.
// @confidence: H
uint64_t CJson_SetValue(CJson* json, const char* path, uint32_t required, void* error_ctx);

// @0x18009e150 — CJson::SetBoolean
// Sets boolean value at path (creates path if needed).
// @confidence: H
void CJson_SetBoolean(CJson* json, const char* path, int value, void* error_ctx);

// @0x18009e3d0 — CJson::SetInt
// Sets integer value at path.
// @confidence: H
void CJson_SetInt(CJson* json, const char* path, int64_t value, void* error_ctx);

// @0x18009e610 — CJson::SetReal
// Sets real/double value at path.
// @confidence: H
void CJson_SetReal(CJson* json, const char* path, double value, void* error_ctx);

// @0x18009e860 — CJson::SetString
// Sets string value at path.
// @confidence: H
void CJson_SetString(CJson* json, const char* path, const char* value, void* error_ctx);

// --- Write: Path creation ---

// @0x1800995a0 — CJson::NavigateForWrite
// Ensures parent nodes exist for a write operation.
// Creates intermediate objects/arrays as needed.
// Returns: json_t* of the parent node, or null on failure.
// @confidence: H
void* CJson_NavigateForWrite(const char* path, CJson* json, int64_t* out_key, uint64_t* out_index);

// @0x180099700 — CJson::CreateArrayPath
// Creates array elements along a path during write operations.
// @confidence: H
void* CJson_CreateArrayPath(const char* path, void* arr, const char* ctx_name,
                             int64_t* out_remaining, uint64_t* out_index);

// @0x180099900 — CJson::CreateObjectPath
// Creates object keys along a path during write operations.
// @confidence: H
void* CJson_CreateObjectPath(const char* path, void* obj, const char* ctx_name,
                              int64_t* out_remaining, uint64_t* out_index);

// @0x1800976e0 — CJson::EnsureArrayElement
// Ensures array has at least (index+1) elements, padding with null if needed.
// Returns: pointer to the array element at index, or null on error.
// @confidence: H
void* CJson_EnsureArrayElement(void* arr, const char* ctx_name, uint64_t index);

// @0x180097850 — CJson::EnsureObjectAtArrayIndex
// Ensures the array element at index is an object, creating it if needed.
// Returns: pointer to the object at index, or null on error.
// @confidence: H
void* CJson_EnsureObjectAtArrayIndex(void* arr, const char* ctx_name, uint64_t index);

// --- Copy/Traverse ---

// @0x18009b290 — CJson::CopyPath
// Deep-copies a subtree from one path to another within the same or different CJson.
// @confidence: H
void CJson_CopyPath(CJson* dst, CJson* src, const char* dst_path, const char* src_path);

// @0x18009c100 — CJson::PopulateCache
// Recursively walks the JSON tree and populates the CHashMap cache.
// @confidence: H
void CJson_PopulateCache(CJson* json, const char* path, CHashMap* cache);

// @0x18009a010 — CJson::SetNodeAtPath (internal)
// Core recursive setter that handles object/array/cache dispatch.
// @confidence: H
void CJson_SetNodeAtPath(CJson* json, const char* path, int required, int create_parents,
                          int with_cache, const char* key);

// @0x18009a7b0 — CJson::SetNodeSimple (thin wrapper)
// Calls CJson_SetNodeAtPath with create_parents=0, with_cache=0.
// @confidence: H
void CJson_SetNodeSimple(CJson* json, const char* path, int required, const char* key);

// @0x18009a7d0 — CJson::SetNodeWithBuffer (wrapper that initializes a buffer)
// Creates a temporary BufferContext, then calls CJson_SetNodeAtPath.
// @confidence: H
void* CJson_SetNodeWithBuffer(CJson* json, void* buf, int required, const char* key);

// --- Delete ---

// @0x180098220 — CJson::DeleteArrayPath
// Recursively navigates array path and deletes the leaf element.
// @confidence: H
uint64_t CJson_DeleteArrayPath(const char* path, void* arr, const char* ctx_name, uint64_t required);

// @0x180098480 — CJson::DeleteObjectPath
// Recursively navigates object path and deletes the leaf field.
// @confidence: H
uint64_t CJson_DeleteObjectPath(const char* path, void* obj, const char* ctx_name, uint64_t required);

// @0x1800987b0 — CJson::DeleteAtPath
// Top-level delete: resolves path type (array vs object) and delegates.
// @confidence: H
void* CJson_DeleteAtPath(const char* path, CJson* json, uint32_t required, void* error_ctx);

// --- Complex path set ---

// @0x1800979c0 — CJson::SetDeep
// Sets a value deep in the tree, creating intermediate objects/arrays as needed.
// Handles the full path syntax with | and [] segments.
// @confidence: H
void CJson_SetDeep(const char* path, int64_t* root, void* value);

// --- Type Query ---

// @0x18009ef60 — CJson::TypeOf
// Returns the public type ID of the node at path.
// Maps internal jansson types to engine types:
//   0(object)→6, 1(array)→5, 2(string)→1, 3(integer)→2, 4(real)→3, 5/6(bool)→4
// Returns 1 (null) if path not found.
// @confidence: H
int CJson_TypeOf(CJson* json, const char* path);

// @0x18009f600 — CJson::PathExists
// Returns true if the path resolves to a non-null node.
// @confidence: H
bool CJson_PathExists(CJson* json, const char* path);

// --- Allocator setup ---

// @0x18009af50 — CJson::SetAllocator
// Installs a custom allocator for JSON operations.
// Sets globals DAT_180384d00/DAT_180384d08 and registers alloc/free callbacks.
// @confidence: H
void CJson_SetAllocator(void* allocator);

// @0x18009b0b0 — CJson::FreeCallback
// Allocator free callback (delegates to allocator vtable +0x30).
// @confidence: H
void CJson_FreeCallback(void* ptr);

// @0x18009b100 — CJson::AllocCallback
// Allocator alloc callback (delegates to allocator vtable +0x08).
// @confidence: H
void* CJson_AllocCallback(void* size);

} // namespace NRadEngine

#endif // PNSRAD_CORE_JSON_CJSON_H
