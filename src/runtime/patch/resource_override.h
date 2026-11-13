#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#ifdef NEVR_TEST_HOOKS
void TestHook_InitResourceOverrides(void (*original)(void*));
void TestHook_SetOriginal(void (*original)(void*));
void TestHook_RegisterInitialOverrides(const wchar_t* cache_dir);
/// The _overrides/ scan of one directory; returns the number of files that failed to register.
size_t TestHook_RegisterOverrideFilesIn(const char* override_dir);
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

/// Register an embedded data override (data pointer must outlive the hook). It serves every load of its resource; with
/// several fixed entries for one key, the first registered serves and the later ones are passed over (#2343).
/// A buffer-0 load served frees the reader's own buffer once the game has its copy (game_buffer.h, FreeReaderBuffer,
/// #2345), as a transform's answer does; a buffer-1 payload is written in place. Neither leaves a buffer allocated.
/// The type names the buffer replaced (resource_buffer_key.h): 0, the logical type or Make(<logical>,
/// "Win10") name buffer 0, which the game gets a copy of in its own heap; Make(<logical>, "Win10GPU") names buffer 1 (a
/// texture's payload), which is written over the game's own buffer when it is no larger and is otherwise not served,
/// with a Warning. The cache and `_overrides/` key the same. A texture's descriptor (buffer 0) and payload (buffer 1)
/// are two entries: a payload of another size than the game's needs its descriptor entry too. Before #1421 an entry
/// keyed by either on-disk type matched nothing. The label follows resource_label.h (1 to 64 bytes of printable ASCII,
/// no double quote or backslash); any other label, a null one included, is refused with a Warning that gives its length
/// and the rule, and nothing is registered. The same holds for RegisterResourceTransform (it returns false) and
/// RegisterResourceOverrideFromFile.
void RegisterResourceOverride(uint64_t type_hash, uint64_t name_hash,
                              const void* data, uint64_t size,
                              const char* label);

/// The label of the first fixed (non-transform) entry that would serve a load an entry keyed by the on-disk type
/// `on_disk_type` serves, for `name_hash`; empty when there is none. An entry of that same type competes, and so does
/// one keyed by a logical type whose buffer-0 on-disk type it is, and one of type 0, which names buffer 0 of every type
/// (from an on-disk type alone the buffer cannot be told, so a type-0 entry is taken to compete).
std::string FirstFixedOverrideLabelFor(uint64_t on_disk_type, uint64_t name_hash);

/// What a transform returns for a resource: `original` and `original_size` are the buffer the game delivered, and a
/// transform that wants to serve other bytes sets `*out` and `*out_size` and returns true. `original` is valid only
/// during the call: when the answer is served, the game's buffer is freed (game_buffer.h, FreeReaderBuffer), so a
/// transform keeps no pointer into it. The bytes it returns must stay valid and unchanged until the game's callback for
/// that load returns: the runtime copies them into the game's heap, and the game reads that copy, never the transform's
/// bytes. Returning false leaves the game's own buffer in place for this transform; the next transform of the key is asked
/// (RegisterResourceTransform). It runs on the game's resource thread, outside the
/// registry lock; it must not throw.
using ResourceTransformFn = bool (*)(const void* original, uint64_t original_size, const void** out,
                                     uint64_t* out_size, void* user);

/// Register a transform for a resource (type 0 = any type): unlike an override, which serves the same bytes on every
/// load, a transform sees the game's buffer on every load of that resource, so it can build its answer from the
/// player's own data. Only the primary buffer (index 0) is offered: a transform keyed by a buffer-1 on-disk type is
/// never asked. A fixed override of the same key outranks a transform whatever the registration order: the transform
/// answers only the loads no override serves (#1479). Transforms of one key are asked in registration order; one that
/// declines hands the load to the next. Returns true when the transform is in the registry. False: a null transform, or
/// no registry (the caller's summary should say so). A failed hook install is logged where it fails.
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

/// Kept for the `NEVR_ResetResourceOverrides` export (plugin ABI): a fixed override already serves every load of its
/// resource (#2343), so there is nothing to re-arm. It logs one Info line with the number of fixed overrides and
/// changes nothing; the shutdown report still counts each override that served.
void ResetResourceOverrides();

/// Remove all overrides whose data pointer falls within [data_start, data_end).
/// Call from plugin shutdown before the plugin DLL is freed to prevent
/// dangling pointers in the override registry. Do not call this from the
/// original resource callback while it is using the same replacement.
void DeregisterResourceOverrides(const void* data_start, const void* data_end);
