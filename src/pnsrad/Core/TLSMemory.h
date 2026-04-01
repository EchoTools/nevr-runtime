#ifndef PNSRAD_CORE_TLS_MEMORY_H
#define PNSRAD_CORE_TLS_MEMORY_H

/* @module: pnsrad.dll */
/* @purpose: Thread-local storage memory context management and memory utilities */

#include <cstdint>
#include <cstddef>

// ---- TLS init/cleanup (low-VA helpers) ----

/* @addr: 0x180001660 (pnsrad.dll) */
/* @confidence: H */
[[nodiscard]] void* tls_get_memory_ctx();

/* @addr: 0x180001680 (pnsrad.dll) */
/* @confidence: H */
void tls_set_memory_ctx(void* ctx);

/* @addr: 0x180001740 (pnsrad.dll) */
/* @confidence: H */
void tls_cleanup();

// ---- Engine runtime TLS (uses shared TLS index DAT_18037a2c0) ----

/* @addr: 0x180089700 (pnsrad.dll) */
/* @confidence: H */
/* @purpose: Returns per-thread memory context via TlsGetValue */
[[nodiscard]] void* rad_get_memory_ctx();

/* @addr: 0x180089a00 (pnsrad.dll) */
/* @confidence: H */
/* @purpose: Sets per-thread memory context via TlsSetValue */
void rad_set_memory_ctx(void* ctx);

// ---- Global allocator ----

/* @addr: 0x180089870 (pnsrad.dll) */
/* @confidence: H */
/* @purpose: Sets the global allocator pointer (DAT_1803766b8) */
void rad_set_allocator(void* allocator);

// ---- Memory utilities ----

/* @addr: 0x1800897f0 (pnsrad.dll) */
/* @confidence: H */
/* @purpose: memset wrapper — returns nullptr for zero-length */
void* rad_memset(void* dst, uint8_t value, size_t count);

/* @addr: 0x1800899f0 (pnsrad.dll) */
/* @confidence: H */
/* @purpose: Optimized memmove — handles overlapping regions, small-size fast paths */
void* rad_memmove(void* dst, const void* src, size_t count);

// ---- Buffer operations ----

struct BufferContext;
struct RadBuffer;

/* @addr: 0x18008b630 (pnsrad.dll) */
/* @confidence: H */
/* @purpose: Initialize a BufferContext — allocates initial buffer via allocator vtable */
void buffer_init(BufferContext* ctx, uint64_t size, int32_t alignment, void** allocator);

/* @addr: 0x18008b730 (pnsrad.dll) */
/* @confidence: H */
/* @purpose: Destroy a BufferContext — frees owned allocation via allocator vtable */
void buffer_destroy(BufferContext* ctx);

/* @addr: 0x18008bb90 (pnsrad.dll) */
/* @confidence: H */
/* @purpose: Release buffer data without freeing — clears ownership flag and pointers */
void buffer_free_data(BufferContext* ctx);

/* @addr: 0x18008c3b0 (pnsrad.dll) */
/* @confidence: H */
/* @purpose: Resize buffer allocation — realloc or alloc+copy depending on alignment */
void buffer_realloc(BufferContext* ctx, uint64_t new_byte_size, int copy_old);

/* @addr: 0x18008a3f0 (pnsrad.dll) */
/* @confidence: H */
/* @purpose: Grow a RadBuffer by additional_count elements (1 byte per element).
 *           Returns the old count (index where new elements start). */
uint64_t buffer_grow(RadBuffer* buf, uint64_t additional_count);

/* @addr: 0x1800821e0 (pnsrad.dll) */
/* @confidence: H */
/* @purpose: Grow a RadBuffer by additional_count elements (0x30 bytes per element).
 *           Returns the old count. Typed variant for 0x30-byte element containers. */
uint64_t buffer_grow_0x30(RadBuffer* buf, uint64_t additional_count);

#endif // PNSRAD_CORE_TLS_MEMORY_H
