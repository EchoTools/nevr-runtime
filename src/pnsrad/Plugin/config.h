#ifndef PNSRAD_PLUGIN_CONFIG_H
#define PNSRAD_PLUGIN_CONFIG_H

/* @module: pnsrad.dll */
/* @purpose: Environment config hash-table lookup with critical section protection.
 *   The host provides an opaque environment handle (g_environment_ptr) that
 *   contains a paged hash map at +0x90 and a CRITICAL_SECTION at +0x28.
 *   config_lookup acquires the critical section, probes the hash map for a
 *   128-bit key, and returns the value pointer (or null).
 */

#include <cstdint>

namespace NRadEngine {

// @0x1800a1aa0 — config_lookup (environment hash table lookup)
// Acquires the critical section at *(arg1)+0x28, then searches the hash
// map at *(arg1)+0x90 for a 128-bit key {arg2, arg3}.  Returns the
// value pointer from the matching entry, or nullptr if not found.
//
// The hash map is a paged structure where each entry is 0x18 bytes:
//   +0x00: uint64_t key_lo
//   +0x08: uint64_t key_hi
//   +0x10: void*    value_ptr
//
// This function is called:
//   - By InitGlobals (0x180088ae0) to check environment features
//   - By ModuleInit (0x180092310 via 0x180092250) to query TLS config
//   - By the error system (0x18008a740) to resolve error buffer handles
//   - By log_message_impl (0x1800929c0) to format log keys
/* @addr: 0x1800a1aa0 (pnsrad.dll) */ /* @confidence: H */
void* config_lookup(void* env_handle, uint64_t key_lo, uint64_t key_hi);

// @0x180092250 — TLS config init (called from ModuleInit)
// If the global environment is null, allocates a fresh TLS slot via
// TlsAlloc.  Otherwise, probes the environment hash map for the TLS
// config key (0xcc4a9e27759ebee3a) and copies the TLS slot index and
// reference count from the returned structure.  Increments the TLS
// reference count and stores the argument into the TLS slot.
/* @addr: 0x180092250 (pnsrad.dll) */ /* @confidence: H */
void config_init_tls(void* arg);

} // namespace NRadEngine

#endif // PNSRAD_PLUGIN_CONFIG_H
