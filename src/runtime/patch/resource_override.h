#pragma once

#include <cstddef>
#include <cstdint>

#ifdef NEVR_TEST_HOOKS
void TestHook_InitResourceOverrides(void (*original)(void*));
void TestHook_SetOriginal(void (*original)(void*));
void TestHook_RegisterInitialOverrides(const wchar_t* cache_dir);
void TestHook_SetDeregisterWaitMs(unsigned ms);
unsigned long long TestHook_DeregisterTimeouts();
void TestHook_RunResourceCallback(void* callback_data);
void TestHook_CloseResourceOverrides();
/// The address of the 8-byte slot that holds the memory-context pointer of the game (nullptr: the game has none).
void TestHook_SetMemoryContextSlot(void* const* slot);
#endif

/// Where the fail-closed path (a callback deregistering its own replacement) leaves its one line before it
/// terminates the process. The sink must not allocate or take locks. Optional: stderr is the fallback.
void SetResourceOverrideFatalLineSink(void (*sink)(const char* line));

/// What ShutdownResourceOverride found, returned so the figures are testable as well as logged. `never_applied` counts
/// fixed overrides only; `transforms_unserved` counts transforms that answered no load this session.
struct ResourceOverrideShutdownReport {
    uint64_t registered;
    uint64_t never_applied;
    uint64_t transforms_unserved;
    bool hook_installed;
};

/// Reports the registry at normal process exit or dynamic unload: one warning per fixed override the game never
/// served (passed over by an earlier override, or never requested), and a summary. When the hook is live the module is pinned, so owned buffers stay until process
/// exit; when it never installed, owned buffers are freed and the registry released.
ResourceOverrideShutdownReport ShutdownResourceOverride();

/// Install the resource override hook on AsyncResourceIOCallback.
/// Registers the cached overrides and scans _overrides/
/// directory for additional file-based overrides.
void InstallResourceOverride();

/// Register an embedded data override (data pointer must outlive the hook). The type names the buffer replaced
/// (resource_buffer_key.h): 0, the logical type or Make(<logical>, "Win10") name buffer 0, which the game gets a copy of
/// in its own heap; Make(<logical>, "Win10GPU") names buffer 1 (a texture's payload), which is written over the game's
/// own buffer when it is no larger and is otherwise not served, with a Warning. The cache and `_overrides/` key the same.
/// A texture's descriptor (buffer 0) and payload (buffer 1) are two entries: a payload of another size than the game's
/// needs its descriptor entry too. Before #1421 an entry keyed by either on-disk type matched nothing.
void RegisterResourceOverride(uint64_t type_hash, uint64_t name_hash,
                              const void* data, uint64_t size,
                              const char* label);

/// What a transform returns for a resource: `original` and `original_size` are the buffer the game delivered, and a
/// transform that wants to serve other bytes sets `*out` and `*out_size` and returns true. The bytes it returns
/// must stay valid and unchanged for the rest of the process (the game keeps reading them after the callback).
/// Returning false leaves the game's own buffer in place for this transform; the next transform of the key is asked
/// (RegisterResourceTransform). It runs on the game's resource thread, outside the
/// registry lock; it must not throw.
using ResourceTransformFn = bool (*)(const void* original, uint64_t original_size, const void** out,
                                     uint64_t* out_size, void* user);

/// Register a transform for a resource (type 0 = any type): unlike an override, which replaces the buffer once, a
/// transform sees the game's buffer on every load of that resource, so it can build its answer from the player's
/// own data and serve it again when the resource loads again. Only the primary buffer (index 0) is offered: a transform
/// keyed by a buffer-1 on-disk type is never asked. A fixed override of the same key outranks a transform whatever the
/// registration order: the transform answers only the loads no override serves (#1479). Transforms of one key are asked
/// in registration order; one that declines hands the load to the next.
/// Returns true when the transform is in the registry. False: a null transform, or no registry (the caller's
/// summary should say so). A failed hook install is logged where it fails.
bool RegisterResourceTransform(uint64_t type_hash, uint64_t name_hash, ResourceTransformFn transform, void* user,
                               const char* label);

/// Reads an `_overrides/` file name: `0xTYPE.0xNAME` (the type names the buffer, as for RegisterResourceOverride) or
/// `0xNAME` (type 0, buffer 0). True, with both hashes set, when the name is one of those with a non-zero name hash.
bool ParseOverrideFileName(const char* file_name, uint64_t* type_hash, uint64_t* name_hash);

/// Register a file-based override (loaded from disk, memory owned by the registry).
void RegisterResourceOverrideFromFile(uint64_t type_hash, uint64_t name_hash,
                                      const char* file_path,
                                      const char* label);

/// Registers every override the asset cache (patch/asset_cache.h) names in its index.json: the blob is read and
/// hashed again, and only bytes that match their key are registered; a mismatch, a missing blob and a bad
/// index each log a Warning and register nothing. Returns how many were registered. Then applies the size cap
/// (`cap_bytes`; the production value unless a test narrows it): blobs the index names are never evicted.
size_t RegisterCachedResourceOverrides(const wchar_t* cache_dir);
size_t RegisterCachedResourceOverrides(const wchar_t* cache_dir, uint64_t cap_bytes);

/// Reset all applied flags so overrides can re-trigger on next level load.
void ResetResourceOverrides();

/// Remove all overrides whose data pointer falls within [data_start, data_end).
/// Call from plugin shutdown before the plugin DLL is freed to prevent
/// dangling pointers in the override registry. Do not call this from the
/// original resource callback while it is using the same replacement.
void DeregisterResourceOverrides(const void* data_start, const void* data_end);
