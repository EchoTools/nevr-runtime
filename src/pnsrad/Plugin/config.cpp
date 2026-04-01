#include "config.h"
#include "Globals.h"

#include <cstdint>

/* @module: pnsrad.dll */

// Forward declarations for internal functions
extern void critical_section_enter(void* out_lock, void* cs_ptr);   // @0x1800a0ff0
extern void critical_section_leave(void* lock);                      // @0x1800a1020
extern int32_t hash_map_find(void* map, void* key, int64_t* out_idx); // @0x1800a1120
extern void config_register(void* env, uint64_t key_lo, uint64_t key_hi, void* value); // @0x1800a1e10

#ifndef _WIN32
typedef void* HMODULE;
static uint32_t TlsAlloc() { return 0; }
static void TlsSetValue(uint32_t slot, void* val) { (void)slot; (void)val; }
#else
#include <windows.h>
#endif

// Global: TLS slot index for the plugin module
/* @addr: 0x18037a2c0 (pnsrad.dll) */
static uint32_t s_tls_slot_index = 0;

// Global: TLS reference count
/* @addr: 0x18037a290 (pnsrad.dll) */
static int64_t s_tls_ref_count = 0;

namespace NRadEngine {

// @0x1800a1aa0 — config_lookup
// Acquires the critical section at *(env_handle)+0x28, searches the hash
// map at *(env_handle)+0x90 for key {key_lo, key_hi}, returns the value
// pointer or nullptr.
/* @addr: 0x1800a1aa0 (pnsrad.dll) */ /* @confidence: H */
void* config_lookup(void* env_handle, uint64_t key_lo, uint64_t key_hi) {
    // env_handle is a pointer-to-pointer: deref once to get the env struct
    uintptr_t env = *reinterpret_cast<uintptr_t*>(env_handle);

    // Acquire critical section at env+0x28
    uint8_t lock_buf[16];
    critical_section_enter(lock_buf, reinterpret_cast<void*>(env + 0x28));

    void* result = nullptr;
    int64_t found_idx = -1;

    // Pack the 128-bit key on the stack
    uint64_t key_pair[2] = { key_lo, key_hi };

    if (env != 0) {
        // Search the hash map at env+0x90
        void* hash_map = reinterpret_cast<void*>(env + 0x90);
        int32_t found = hash_map_find(hash_map, key_pair, &found_idx);

        if (found != 0) {
            // Entry found — value pointer is at map_base + found_idx * 0x18 + 0x10
            uintptr_t map_base = *reinterpret_cast<uintptr_t*>(env + 0x90);
            result = *reinterpret_cast<void**>(map_base + 0x10 + found_idx * 0x18);
        }
    }

    // Release critical section
    critical_section_leave(lock_buf);

    return result;
}

// @0x180092250 — TLS config initialization
// Sets up the TLS slot used by the plugin module system.
// If the environment is available, queries it for the TLS config key;
// otherwise allocates a fresh TLS slot.
/* @addr: 0x180092250 (pnsrad.dll) */ /* @confidence: H */
void config_init_tls(void* arg) {
    void* env = g_environment_ptr;

    if (env == nullptr) {
        // No environment — allocate a fresh TLS slot
        s_tls_slot_index = TlsAlloc();
        s_tls_ref_count = 0;
    } else {
        // Query the environment hash map for TLS config key
        // Key: 0xCC4A79E275EBEE3A (split into lo/hi)
        int64_t* config_data = reinterpret_cast<int64_t*>(
            config_lookup(env, 0xCC4A79E275EBEE3AULL, 0));

        if (config_data == nullptr) {
            // Not found — allocate and register
            s_tls_slot_index = TlsAlloc();
            s_tls_ref_count = 0;

            // Register the TLS config with the environment for sharing
            config_register(env, 0xCC4A79E275EBEE3AULL, 0,
                           &s_tls_ref_count);
        } else {
            // Found existing config — copy slot index and ref count
            s_tls_ref_count = *config_data;
            s_tls_slot_index = *reinterpret_cast<uint32_t*>(config_data + 6);
        }
    }

    // Increment reference count
    s_tls_ref_count = s_tls_ref_count + 1;

    // Store the argument into the TLS slot
    TlsSetValue(s_tls_slot_index, arg);
}

} // namespace NRadEngine
