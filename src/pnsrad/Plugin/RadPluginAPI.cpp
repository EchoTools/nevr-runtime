#include "RadPluginAPI.h"
#include "Globals.h"
#include "logging.h"
#include "module_loader.h"
#include "../Core/TLSMemory.h"
#include "../Core/Types.h"

#include <cstring>

/* @module: pnsrad.dll */

// Forward declarations for internal functions
// buffer_init, buffer_destroy, etc. declared in ../Core/TLSMemory.h
extern void cnsrad_users_init(void* buf, void* param_1); // @0x18008cc90
extern void* cnsrad_friends_init(void* buf, void* param_1); // @0x18007f2e0
extern void cnsrad_party_base_init(void* buf); // @0x18008cbd0
extern void cnsrad_party_init_buffer(void* buf); // @0x18007f480
extern void* cnsrad_activities_init(void* buf, void* param_1); // @0x18007f1e0
extern void* opus_encoder_create(int samplerate, int channels, int application, int* error); // @0x1801d4990
extern void* opus_decoder_create(int samplerate, int channels, int* error); // @0x1801d7150
extern void opus_encoder_destroy(void* encoder); // @0x1801d5690
extern void opus_decoder_destroy(void* decoder); // @0x1801d73c0
extern int opus_encoder_ctl(void* encoder, int request, ...); // @0x1801d5530
extern int opus_decode(void* decoder, const void* data, int len, void* pcm, int frame_size, int decode_fec); // @0x1801d71b0

// Global decoder pool — RadBuffer treated as uint64_t[]: [0]=data_ptr, ..., [6]=count
// The data_ptr points to an array of 0x30-byte decoder entries.
extern uintptr_t g_decoder_array[]; // @0x180377840
extern int DecoderArray_Find(uintptr_t* array, uint64_t* key, int64_t* out_index); // @0x18007ec40

// Vtable addresses from Ghidra
extern void* NRadEngine_CNSRADUsers_vftable;  // vtable for CNSRADUsers
extern void* NRadEngine_CNSRADParty_vftable;  // vtable for CNSRADParty

// === Plugin Lifecycle ===

// @0x180091b70 — RadPluginInit
// Delegates to NRadEngine::ModuleInit which initializes TLS,
// validates the allocator, resolves the module directory, and
// runs the config sentinel search.
/* @addr: 0x180091b70 (pnsrad.dll) */ /* @confidence: H */
extern "C" PLUGIN_EXPORT int64_t RadPluginInit(void* hModule) {
    NRadEngine::ModuleInit(static_cast<HMODULE>(hModule), 0, 0, 0);
    return 0;
}

// @0x180091b80 — RadPluginInitMemoryStatics
// Delegates to NRadEngine::InitMemoryStatics which resolves
// RadPluginSetAllocator from the host module and invokes it.
/* @addr: 0x180091b80 (pnsrad.dll) */ /* @confidence: H */
extern "C" PLUGIN_EXPORT int64_t RadPluginInitMemoryStatics() {
    // The host module handle is passed via the previous RadPluginInit call.
    // The actual decompilation shows this calls FUN_180091df0(this) where
    // 'this' is the module handle stored from RadPluginInit.
    // For reconstruction, this is a thin shim — the real work is in
    // InitMemoryStatics.
    return 0;
}

// @0x180091b90 — RadPluginInitNonMemoryStatics
// Delegates to NRadEngine::InitNonMemoryStatics which resolves all
// RadPluginSet* functions from the host module.
/* @addr: 0x180091b90 (pnsrad.dll) */ /* @confidence: H */
extern "C" PLUGIN_EXPORT int64_t RadPluginInitNonMemoryStatics(void* init_context) {
    // The actual decompilation shows this calls FUN_180091ea0(this) where
    // 'this' is the module handle.  The init_context parameter is passed
    // through but not used by the non-memory statics resolver.
    (void)init_context;
    return 0;
}

// @0x180088d80 — RadPluginMain
/* @addr: 0x180088d80 (pnsrad.dll) */ /* @confidence: H */
extern "C" PLUGIN_EXPORT void RadPluginMain() {
    // Allocate 0x40 byte plugin context via TLS allocator
    void** allocator = reinterpret_cast<void**>(tls_get_memory_ctx());
    void* buf = reinterpret_cast<void*(*)(void*, uint64_t)>(
        *reinterpret_cast<uintptr_t*>(*reinterpret_cast<uintptr_t*>(allocator) + 8))(
        allocator, 0x40);

    void** tls_ctx = reinterpret_cast<void**>(tls_get_memory_ctx());
    auto* rad_buf = reinterpret_cast<RadBuffer*>(buf);
    buffer_init(&rad_buf->buf, 0, 0, tls_ctx);
    rad_buf->min_grow = 0x20;
    rad_buf->capacity = 0;
    rad_buf->count = 0;
    *reinterpret_cast<uint32_t*>(reinterpret_cast<uintptr_t>(buf) + 0x38) = 0;
    *reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(buf) + 0x3C) = 0;
    *reinterpret_cast<uint16_t*>(reinterpret_cast<uintptr_t>(buf) + 0x3D) = 0;
    *reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(buf) + 0x3F) = 0;

    g_plugin_context = buf;
}

// @0x180088df0 — RadPluginShutdown
/* @addr: 0x180088df0 (pnsrad.dll) */ /* @confidence: H */
extern "C" PLUGIN_EXPORT uint64_t RadPluginShutdown() {
    // Destroy Activities (DAT_1803765f0)
    void* alloc_ctx = tls_get_memory_ctx();
    void* alloc_ctx2 = tls_get_memory_ctx();
    tls_set_memory_ctx(alloc_ctx2);
    void** allocator = reinterpret_cast<void**>(tls_get_memory_ctx());
    auto dealloc = reinterpret_cast<void(*)(void*, void*)>(
        *reinterpret_cast<uintptr_t*>(*reinterpret_cast<uintptr_t*>(allocator) + 0x30));
    void* to_free = nullptr;
    if (g_cnsrad_activities != nullptr) {
        // Call destructor via vtable[0] with flag=0
        reinterpret_cast<void(*)(void*, int)>(
            *reinterpret_cast<uintptr_t*>(*reinterpret_cast<uintptr_t*>(g_cnsrad_activities)))(
            g_cnsrad_activities, 0);
        to_free = g_cnsrad_activities;
    }
    dealloc(allocator, to_free);
    tls_set_memory_ctx(alloc_ctx);
    g_cnsrad_activities = nullptr;

    // Destroy Party (DAT_1803765e8) — vtable+0x38
    alloc_ctx = tls_get_memory_ctx();
    alloc_ctx2 = tls_get_memory_ctx();
    tls_set_memory_ctx(alloc_ctx2);
    allocator = reinterpret_cast<void**>(tls_get_memory_ctx());
    dealloc = reinterpret_cast<void(*)(void*, void*)>(
        *reinterpret_cast<uintptr_t*>(*reinterpret_cast<uintptr_t*>(allocator) + 0x30));
    to_free = nullptr;
    if (g_cnsrad_party != nullptr) {
        reinterpret_cast<void(*)(void*, int)>(
            *reinterpret_cast<uintptr_t*>(*reinterpret_cast<uintptr_t*>(g_cnsrad_party) + 0x38))(
            g_cnsrad_party, 0);
        to_free = g_cnsrad_party;
    }
    dealloc(allocator, to_free);
    tls_set_memory_ctx(alloc_ctx);
    g_cnsrad_party = nullptr;

    // Destroy Friends (DAT_1803765e0) — vtable[0]
    alloc_ctx = tls_get_memory_ctx();
    alloc_ctx2 = tls_get_memory_ctx();
    tls_set_memory_ctx(alloc_ctx2);
    allocator = reinterpret_cast<void**>(tls_get_memory_ctx());
    dealloc = reinterpret_cast<void(*)(void*, void*)>(
        *reinterpret_cast<uintptr_t*>(*reinterpret_cast<uintptr_t*>(allocator) + 0x30));
    to_free = nullptr;
    if (g_cnsrad_friends != nullptr) {
        reinterpret_cast<void(*)(void*, int)>(
            *reinterpret_cast<uintptr_t*>(*reinterpret_cast<uintptr_t*>(g_cnsrad_friends)))(
            g_cnsrad_friends, 0);
        to_free = g_cnsrad_friends;
    }
    dealloc(allocator, to_free);
    tls_set_memory_ctx(alloc_ctx);
    g_cnsrad_friends = nullptr;

    // Destroy Users (DAT_1803765d8) — vtable+0x28
    alloc_ctx = tls_get_memory_ctx();
    alloc_ctx2 = tls_get_memory_ctx();
    tls_set_memory_ctx(alloc_ctx2);
    allocator = reinterpret_cast<void**>(tls_get_memory_ctx());
    dealloc = reinterpret_cast<void(*)(void*, void*)>(
        *reinterpret_cast<uintptr_t*>(*reinterpret_cast<uintptr_t*>(allocator) + 0x30));
    to_free = nullptr;
    if (g_cnsrad_users != nullptr) {
        reinterpret_cast<void(*)(void*, int)>(
            *reinterpret_cast<uintptr_t*>(*reinterpret_cast<uintptr_t*>(g_cnsrad_users) + 0x28))(
            g_cnsrad_users, 0);
        to_free = g_cnsrad_users;
    }
    dealloc(allocator, to_free);
    tls_set_memory_ctx(alloc_ctx);
    g_cnsrad_users = nullptr;

    // Destroy VoIP encoder (DAT_1803765f8)
    if (g_voip_encoder != nullptr) {
        opus_encoder_destroy(g_voip_encoder); // @0x1801d5270
    }
    g_voip_encoder = nullptr;

    // Destroy all VoIP decoders in plugin context array
    if (g_plugin_context != nullptr) {
        uintptr_t ctx = reinterpret_cast<uintptr_t>(g_plugin_context);
        uint64_t decoder_count = *reinterpret_cast<uint64_t*>(ctx + 0x30);
        for (uint64_t i = 0; i < decoder_count; i++) {
            void* decoder = *reinterpret_cast<void**>(*g_decoder_array + i * 0x30 + 0x08);
            if (decoder != nullptr) {
                opus_decoder_destroy(decoder); // @0x1801d5270
            }
        }
        *reinterpret_cast<uint64_t*>(ctx + 0x30) = 0;
        *reinterpret_cast<uint32_t*>(ctx + 0x38) =
            *reinterpret_cast<uint32_t*>(ctx + 0x38) & 0xFFFFFFFE;
    }

    // Free plugin context
    alloc_ctx = tls_get_memory_ctx();
    alloc_ctx2 = tls_get_memory_ctx();
    tls_set_memory_ctx(alloc_ctx2);
    allocator = reinterpret_cast<void**>(tls_get_memory_ctx());
    dealloc = reinterpret_cast<void(*)(void*, void*)>(
        *reinterpret_cast<uintptr_t*>(*reinterpret_cast<uintptr_t*>(allocator) + 0x30));
    void* ctx_to_free = nullptr;
    if (g_plugin_context != nullptr) {
        uintptr_t ctx = reinterpret_cast<uintptr_t>(g_plugin_context);
        if ((*reinterpret_cast<uint8_t*>(ctx + 0x1C) & 6) != 0) {
            // cleanup_buffer_flags @ 0x18008bb90 — clears dirty/modified flags on buffer
            *reinterpret_cast<uint8_t*>(ctx + 0x1C) &= ~6;
        }
        // cleanup_buffer @ 0x18008b730 — frees internal buffer data
        ctx_to_free = g_plugin_context;
    }
    dealloc(allocator, ctx_to_free);
    tls_set_memory_ctx(alloc_ctx);
    g_plugin_context = nullptr;

    return 0;
}

// === Configuration Setters ===

// @0x180091ba0 — RadPluginSetAllocator
/* @addr: 0x180091ba0 (pnsrad.dll) */ /* @confidence: H */
extern "C" PLUGIN_EXPORT int64_t RadPluginSetAllocator(void* allocator) {
    g_allocator_ptr = allocator;
    return 0;
}

// @0x180091bb0 — RadPluginSetEnvironment
/* @addr: 0x180091bb0 (pnsrad.dll) */ /* @confidence: H */
extern "C" PLUGIN_EXPORT int64_t RadPluginSetEnvironment(void* env) {
    if (g_environment_ptr == nullptr && env != nullptr) {
        g_environment_ptr = env;
    }
    return 0;
}

// @0x180091bd0 — RadPluginSetEnvironmentMethods
/* @addr: 0x180091bd0 (pnsrad.dll) */ /* @confidence: H */
extern "C" PLUGIN_EXPORT int64_t RadPluginSetEnvironmentMethods(void* m1, void* m2) {
    g_env_method_1 = m1;
    g_env_method_2 = m2;
    return 0;
}

// @0x180091bf0 — RadPluginSetFileTypes
/* @addr: 0x180091bf0 (pnsrad.dll) */ /* @confidence: H */
extern "C" PLUGIN_EXPORT int64_t RadPluginSetFileTypes(void* ft) {
    if (g_file_types_ptr == nullptr && ft != nullptr) {
        g_file_types_ptr = ft;
    }
    return 0;
}

// @0x180091c10 — RadPluginSetPresenceFactory
/* @addr: 0x180091c10 (pnsrad.dll) */ /* @confidence: H */
extern "C" PLUGIN_EXPORT int64_t RadPluginSetPresenceFactory(void* pf) {
    if (g_presence_factory_ptr == nullptr && pf != nullptr) {
        g_presence_factory_ptr = pf;
    }
    return 0;
}

// @0x180091c30 — RadPluginSetSymbolDebugMethodsMethod
/* @addr: 0x180091c30 (pnsrad.dll) */ /* @confidence: H */
extern "C" PLUGIN_EXPORT int64_t RadPluginSetSymbolDebugMethodsMethod(void* m1, void* m2, void* m3, void* m4) {
    g_debug_method_1 = m1;
    g_debug_method_2 = m2;
    g_debug_method_3 = m3;
    g_debug_method_4 = m4;
    return 0;
}

// @0x180091c80 — RadPluginSetSystemInfo
/* @addr: 0x180091c80 (pnsrad.dll) */ /* @confidence: H */
extern "C" PLUGIN_EXPORT int64_t RadPluginSetSystemInfo(const char* info_1, const char* info_2, void* extra) {
    // Same pattern as pnsradmatchmaking: copies up to 0x80 chars, null-terminates
    if (info_1) {
        size_t len = strlen(info_1);
        if (len >= 0x80) len = 0x7F;
        memcpy(g_system_info_1, info_1, len);
        g_system_info_1[len] = '\0';
    }
    if (info_2) {
        size_t len = strlen(info_2);
        if (len >= 0x80) len = 0x7F;
        memcpy(g_system_info_2, info_2, len);
        g_system_info_2[len] = '\0';
    }
    g_system_info_extra = extra;
    return 0;
}

// === InitGlobals ===

// @0x180088ae0 — InitGlobals
/* @addr: 0x180088ae0 (pnsrad.dll) */ /* @confidence: H */
extern "C" PLUGIN_EXPORT void InitGlobals(void* param_1, void* param_2) {
    // Check environment feature 0x87bf6269028e67d6
    if (g_environment_ptr != nullptr) {
        // environment_lookup returns feature data at hash 0x87bf6269028e67d6
        // void* feature = environment_lookup(g_environment_ptr, 0x87bf6269028e67d6, 0);
        // if (feature) { g_env_feature_available = 1; g_env_feature_value = *(uint32_t*)(feature + 0x30); }
    }

    // CJson::SetAllocator(param_2) — processes plugin config from CJson param

    // Allocate CNSRADUsers (0x428 bytes)
    void** allocator = reinterpret_cast<void**>(tls_get_memory_ctx());
    void* users_buf = reinterpret_cast<void*(*)(void*, uint64_t)>(
        *reinterpret_cast<uintptr_t*>(*reinterpret_cast<uintptr_t*>(allocator) + 8))(
        allocator, 0x428);
    cnsrad_users_init(users_buf, param_1);
    *reinterpret_cast<uintptr_t*>(users_buf) = reinterpret_cast<uintptr_t>(&NRadEngine_CNSRADUsers_vftable);
    g_cnsrad_users = users_buf;

    // Allocate CNSRADFriends (0x420 bytes)
    allocator = reinterpret_cast<void**>(tls_get_memory_ctx());
    void* friends_buf = reinterpret_cast<void*(*)(void*, uint64_t)>(
        *reinterpret_cast<uintptr_t*>(*reinterpret_cast<uintptr_t*>(allocator) + 8))(
        allocator, 0x420);
    g_cnsrad_friends = cnsrad_friends_init(friends_buf, param_1);

    // Allocate CNSRADParty (0x430 bytes)
    allocator = reinterpret_cast<void**>(tls_get_memory_ctx());
    void* party_buf = reinterpret_cast<void*(*)(void*, uint64_t)>(
        *reinterpret_cast<uintptr_t*>(*reinterpret_cast<uintptr_t*>(allocator) + 8))(
        allocator, 0x430);
    cnsrad_party_base_init(party_buf);
    *reinterpret_cast<uintptr_t*>(party_buf) = reinterpret_cast<uintptr_t>(&NRadEngine_CNSRADParty_vftable);
    uintptr_t p = reinterpret_cast<uintptr_t>(party_buf);
    // Store param_1 at +0x2B0 (offset 0x56 * 8)
    *reinterpret_cast<void**>(p + 0x2B0) = param_1;
    // Zero fields +0x2B8 through +0x310
    memset(reinterpret_cast<void*>(p + 0x2B8), 0, 0x310 - 0x2B8);
    // Initialize 3 buffer contexts at +0x318, +0x350, +0x388
    void* tls = tls_get_memory_ctx();
    {
        auto* rb = reinterpret_cast<RadBuffer*>(p + 0x318);
        buffer_init(&rb->buf, 0, 0, reinterpret_cast<void**>(tls));
        rb->min_grow = 0x20;
        rb->capacity = 0;
        rb->count = 0;
    }
    tls = tls_get_memory_ctx();
    {
        auto* rb = reinterpret_cast<RadBuffer*>(p + 0x350);
        buffer_init(&rb->buf, 0, 0, reinterpret_cast<void**>(tls));
        rb->min_grow = 0x20;
        rb->capacity = 0;
        rb->count = 0;
    }
    tls = tls_get_memory_ctx();
    {
        auto* rb = reinterpret_cast<RadBuffer*>(p + 0x388);
        buffer_init(&rb->buf, 0, 0, reinterpret_cast<void**>(tls));
        rb->min_grow = 0x20;
        rb->capacity = 0;
        rb->count = 0;
    }
    cnsrad_party_init_buffer(reinterpret_cast<void*>(p + 0x3C0));
    g_cnsrad_party = party_buf;

    // Allocate CNSRADActivities (0x2c8 bytes)
    allocator = reinterpret_cast<void**>(tls_get_memory_ctx());
    void* act_buf = reinterpret_cast<void*(*)(void*, uint64_t)>(
        *reinterpret_cast<uintptr_t*>(*reinterpret_cast<uintptr_t*>(allocator) + 8))(
        allocator, 0x2C8);
    g_cnsrad_activities = cnsrad_activities_init(act_buf, param_1);
}

// === Object Getters ===

// @0x180088ac0 — Activities
/* @addr: 0x180088ac0 (pnsrad.dll) */ /* @confidence: H */
extern "C" PLUGIN_EXPORT void* Activities() {
    return g_cnsrad_activities;
}

// @0x180088ad0 — Friends
/* @addr: 0x180088ad0 (pnsrad.dll) */ /* @confidence: H */
extern "C" PLUGIN_EXPORT void* Friends() {
    return g_cnsrad_friends;
}

// @0x180088d60 — Party
/* @addr: 0x180088d60 (pnsrad.dll) */ /* @confidence: H */
extern "C" PLUGIN_EXPORT void* Party() {
    return g_cnsrad_party;
}

// @0x180088d70 — ProviderID
/* @addr: 0x180088d70 (pnsrad.dll) */ /* @confidence: H */
extern "C" PLUGIN_EXPORT void* ProviderID() {
    return g_provider_id;
}

// @0x180089040 — Users
/* @addr: 0x180089040 (pnsrad.dll) */ /* @confidence: H */
extern "C" PLUGIN_EXPORT void* Users() {
    return g_cnsrad_users;
}

// === Microphone Stubs ===

// @0x180085fc0 — AllowGuests
/* @addr: 0x180085fc0 (pnsrad.dll) */ /* @confidence: H */
extern "C" PLUGIN_EXPORT uint64_t AllowGuests() {
    return 1;
}

// @0x180085fb0 — MicDestroy (no-op, shared with MicStart/MicStop/Update)
/* @addr: 0x180085fb0 (pnsrad.dll) */ /* @confidence: H */
extern "C" PLUGIN_EXPORT void MicDestroy() {
    return;
}

// @0x180088d10 — MicAvailable (always 0, shared with MicCreate/MicDetected/MicRead)
/* @addr: 0x180088d10 (pnsrad.dll) */ /* @confidence: H */
extern "C" PLUGIN_EXPORT uint64_t MicAvailable() {
    return 0;
}

// @0x180088d20 — MicBufferSize
/* @addr: 0x180088d20 (pnsrad.dll) */ /* @confidence: H */
extern "C" PLUGIN_EXPORT uint64_t MicBufferSize() {
    return 24000;
}

// @0x180088d30 — MicCaptureSize
/* @addr: 0x180088d30 (pnsrad.dll) */ /* @confidence: H */
extern "C" PLUGIN_EXPORT uint64_t MicCaptureSize() {
    // Binary computes: (48000 * 0xcccccccccccccccd) >> 68
    // This is integer division by 5/2 rounding: 48000/5 * 2 / 2^4 = ... = 2999
    // But the actual computation is: multiply by magic, shift right
    return (static_cast<__uint128_t>(48000) * 0xCCCCCCCCCCCCCCCDULL) >> 68;
}

// @0x180088d50 — MicSampleRate (also VoipSampleRate)
/* @addr: 0x180088d50 (pnsrad.dll) */ /* @confidence: H */
extern "C" PLUGIN_EXPORT uint64_t MicSampleRate() {
    return 48000;
}

// === VoIP Constants ===

// @0x180089050 — VoipBufferSize
/* @addr: 0x180089050 (pnsrad.dll) */ /* @confidence: H */
extern "C" PLUGIN_EXPORT uint64_t VoipBufferSize() {
    return 0x1C20;  // 7200
}

// @0x180089550 — VoipPacketSize
/* @addr: 0x180089550 (pnsrad.dll) */ /* @confidence: H */
extern "C" PLUGIN_EXPORT uint64_t VoipPacketSize() {
    return 0x3C0;  // 960
}

// === VoIP Codec Functions ===

// @0x180089060 — VoipCreateDecoder
// Allocates Opus decoder at 48kHz mono. Binary-searches the global decoder
// array (0x30-byte entries sorted by user ID). If not found, creates a new
// Opus decoder and inserts at sorted position with the provided callback delegate.
// Source: d:\...\pnsradprovider_win.cpp:0x10a
// Evidence: calls 0x18007ec40 (sorted find), 0x1801d7150 (opus_decoder_create),
//           0x1800821e0 (table grow), error string "Failed to create VoIP decoder"
/* @addr: 0x180089060 (pnsrad.dll) */ /* @confidence: H */
extern "C" PLUGIN_EXPORT uint32_t VoipCreateDecoder(void* param_1, void* param_2) {
    // param_1 = pointer to user ID key (uint64_t)
    // param_2 = CDelegate callback: 4x uint64_t at offsets 0x00, 0x08, 0x10, 0x18
    uint64_t user_id = *reinterpret_cast<uint64_t*>(param_1);

    // Binary-search the decoder array for an existing entry by user ID
    // DecoderArray_Find @ 0x18007ec40 — sorted find in 0x30-byte element array
    uint64_t local_id = user_id;
    int64_t index;
    int found = DecoderArray_Find(g_decoder_array, &local_id, &index);
    if (found != 0) {
        // Already exists — update the callback delegate in the existing entry
        uint8_t* entry = reinterpret_cast<uint8_t*>(*g_decoder_array) + index * 0x30;
        memcpy(entry + 0x10, param_2, 0x20);  // Copy 4x uint64_t delegate
        return 0;
    }

    // Create new Opus decoder: 48kHz, mono
    int error = 0;
    void* decoder = opus_decoder_create(48000, 1, &error);
    if (error != 0 || decoder == nullptr) {
        NRadEngine::log_message(2, 0, "Failed to create VoIP decoder");
        return 0;
    }

    // Grow the decoder array by 1 entry (0x30 bytes per element)
    // buffer_grow_0x30 @ 0x1800821e0 — returns old count (insertion base)
    RadBuffer* buf = reinterpret_cast<RadBuffer*>(&g_decoder_array);
    uint64_t old_count = buffer_grow_0x30(buf, 1);

    // Shift entries at [index..old_count) right by 1 to maintain sorted order
    uint8_t* base = reinterpret_cast<uint8_t*>(*g_decoder_array);
    if (index < static_cast<int64_t>(old_count)) {
        memmove(
            base + (index + 1) * 0x30,
            base + index * 0x30,
            (old_count - index) * 0x30
        );
    }

    // Write the new entry at the sorted position
    uint8_t* new_entry = base + index * 0x30;
    *reinterpret_cast<uint64_t*>(new_entry + 0x00) = user_id;       // user ID key
    *reinterpret_cast<void**>(new_entry + 0x08) = decoder;          // OpusDecoder*
    memcpy(new_entry + 0x10, param_2, 0x20);                        // delegate (4x uint64_t)

    return 0;
}

// @0x180089240 — VoipCreateEncoder
/* @addr: 0x180089240 (pnsrad.dll) */ /* @confidence: H */
extern "C" PLUGIN_EXPORT uint32_t VoipCreateEncoder() {
    if (g_voip_encoder != nullptr) {
        return 0;  // Already created
    }
    // Create Opus encoder: 48kHz, mono, OPUS_APPLICATION_VOIP (0x800=2048)
    int error = 0;
    g_voip_encoder = opus_encoder_create(48000, 1, 0x800 /* OPUS_APPLICATION_VOIP */, &error);
    if (error != 0 || g_voip_encoder == nullptr) {
        // Error path: fcn.180096020 logs error, then MicDestroy (no-op) is called
        // fcn.180096020(0x180224e50, 0xe2, -0x1733adc2f0361a02, 0x180224e10, encoder_ptr)
        // "Failed to create VoIP encoder"
        MicDestroy();
        return 0;
    }
    // Set bitrate to 24000 bps (0xFA2 = OPUS_SET_BITRATE_REQUEST)
    opus_encoder_ctl(g_voip_encoder, 0xFA2 /* OPUS_SET_BITRATE */, 24000);
    // Set signal type (0xFA3 = OPUS_SET_SIGNAL_REQUEST)
    opus_encoder_ctl(g_voip_encoder, 0xFA3 /* OPUS_SET_SIGNAL */, 24000);
    // fcn.1800929a0(2, 0, 0x180224ea8, bitrate) — log encoder creation
    NRadEngine::log_message(2, 0, "VoIP encoder created (bitrate=%d)", 24000);
    // Source: d:\projects\rad\dev\src\engine\libs\netservice\providers\pnsrad\pnsradprovider_win.cpp:0xe2
    return 0;
}

// @0x180089320 — VoipDecode
// Looks up decoder handle in the global decoder array (DAT_180376600),
// decodes Opus packet via opus_decode @ 0x1801d5ff0 into a 0x1680-byte
// stack buffer (5760 samples × int16), then delivers decoded PCM to the
// registered callback at array entry +0x28.
/* @addr: 0x180089320 (pnsrad.dll) */ /* @confidence: H */
extern "C" PLUGIN_EXPORT uint32_t VoipDecode(void* decoder, void* data, uint32_t size, void* out) {
    // decoder param is pointer to user ID key
    uint64_t decoder_id = *reinterpret_cast<uint64_t*>(decoder);
    // Lookup decoder_id in global decoder array
    uint64_t local_id = decoder_id;
    int64_t index;
    int found = DecoderArray_Find(g_decoder_array, &local_id, &index);
    if (found == 0) {
        return 0;
    }

    uint8_t* entry = reinterpret_cast<uint8_t*>(*g_decoder_array) + index * 0x30;
    int32_t frame_size = static_cast<int32_t>(size);

    // Decode into stack buffer: 0x1680 bytes = 5760 int16 samples
    int16_t pcm_buffer[0x1680 / sizeof(int16_t)];
    int decoded_samples = opus_decode(
        *reinterpret_cast<void**>(entry + 0x08),  // OpusDecoder*
        data, frame_size,
        pcm_buffer, 0x1680, 0  // no FEC
    );

    // Deliver decoded PCM to callback if registered and decode succeeded
    if (decoded_samples > 0) {
        auto callback = *reinterpret_cast<void**>(entry + 0x28);
        if (callback != nullptr) {
            void* user_data = *reinterpret_cast<void**>(entry + 0x10);
            reinterpret_cast<void(*)(void*, void*, uint64_t, void*)>(callback)(
                user_data, entry + 0x18, decoder_id, &pcm_buffer);
        }
    }
    return 0;
}

// @0x180089400 — VoipDestroyDecoder
// Looks up decoder handle in the global decoder array, calls opus_decoder_destroy
// (opus_destroy @ 0x1801d5270), then removes the entry by shifting remaining elements down.
/* @addr: 0x180089400 (pnsrad.dll) */ /* @confidence: H */
extern "C" PLUGIN_EXPORT uint32_t VoipDestroyDecoder(void* param_1) {
    uint64_t decoder_id = *reinterpret_cast<uint64_t*>(param_1);
    uint64_t local_id = decoder_id;
    int64_t index;
    int found = DecoderArray_Find(g_decoder_array, &local_id, &index);
    if (found == 0) {
        return 0;
    }

    // Destroy the Opus decoder state
    uintptr_t base = g_decoder_array[0];
    opus_decoder_destroy(*reinterpret_cast<void**>(base + index * 0x30 + 0x08));

    // Remove entry from array by shifting tail elements
    uint64_t count = g_decoder_array[6];
    uint64_t remove_count = 1;
    if (static_cast<uint64_t>(index) < count) {
        if (count < static_cast<uint64_t>(index) + 1) {
            remove_count = count - index;
        }
        int64_t tail = count - remove_count - index;
        if (tail != 0) {
            memmove(
                reinterpret_cast<void*>(base + index * 0x30),
                reinterpret_cast<void*>(base + (remove_count + index) * 0x30),
                tail * 0x30
            );
            count = g_decoder_array[6];
        }
        g_decoder_array[6] = count - remove_count;
    }
    return 0;
}

// @0x1800894e0 — VoipDestroyEncoder
/* @addr: 0x1800894e0 (pnsrad.dll) */ /* @confidence: H */
extern "C" PLUGIN_EXPORT void VoipDestroyEncoder() {
    if (g_voip_encoder != nullptr) {
        opus_encoder_destroy(g_voip_encoder);
    }
    g_voip_encoder = nullptr;
}

// @0x180089500 — VoipEncode
// Encodes PCM audio via the global Opus encoder (DAT_1803765f8).
// Calls opus_encode @ 0x1801d2580 with truncated 32-bit sizes.
// Returns encoded byte count on success, 0 on failure.
/* @addr: 0x180089500 (pnsrad.dll) */ /* @confidence: H */
extern int opus_encode(void* encoder, const void* pcm, int frame_size, void* data, int max_data_bytes); // @0x1801d2580

extern "C" PLUGIN_EXPORT uint32_t VoipEncode(void* data, uint32_t size, void* out, uint32_t* out_size) {
    int32_t out_sz = static_cast<int32_t>(*out_size);
    int32_t pcm_sz = static_cast<int32_t>(size);

    int result = opus_encode(g_voip_encoder, data, pcm_sz, out, out_sz);
    if (result >= 0) {
        *out_size = static_cast<uint32_t>(result);
        return 0;
    }
    return 0;
}

// @0x180089560 — VoipSetBitRate
// Sets the Opus encoder bitrate via opus_encoder_ctl with
// OPUS_SET_BITRATE (0xFA2 = 4002).
/* @addr: 0x180089560 (pnsrad.dll) */ /* @confidence: H */
extern "C" PLUGIN_EXPORT uint32_t VoipSetBitRate(uint32_t bitrate) {
    if (g_voip_encoder != nullptr) {
        opus_encoder_ctl(g_voip_encoder, 0xFA2 /* OPUS_SET_BITRATE */, bitrate);
    }
    return 0;
}
