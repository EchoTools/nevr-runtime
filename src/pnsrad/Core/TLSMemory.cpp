#include "TLSMemory.h"
#include "Types.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <pthread.h>
#endif

#include <cstring>

/* @module: pnsrad.dll */
/* @purpose: Thread-local storage for memory context pointers, memory utilities,
 *           and buffer management operations. */

// ============================================================================
// Globals
// ============================================================================

/* @addr: 0x18037a2c0 (pnsrad.dll) */
/* @purpose: TLS index shared by rad_get_memory_ctx / rad_set_memory_ctx */
#ifdef _WIN32
static DWORD g_tls_index = TLS_OUT_OF_INDEXES;
#else
static pthread_key_t g_tls_key;
static bool g_tls_initialized = false;
#endif

/* @addr: 0x1803766b8 (pnsrad.dll) */
/* @purpose: Global allocator pointer set by rad_set_allocator */
extern void* g_allocator_ptr;

// ============================================================================
// TLS init/cleanup (low-VA helpers)
// ============================================================================

// @0x180001660 — tls_get_memory_ctx
void* tls_get_memory_ctx() {
#ifdef _WIN32
    if (g_tls_index == TLS_OUT_OF_INDEXES) {
        return nullptr;
    }
    return TlsGetValue(g_tls_index);
#else
    if (!g_tls_initialized) {
        return nullptr;
    }
    return pthread_getspecific(g_tls_key);
#endif
}

// @0x180001680 — tls_set_memory_ctx
void tls_set_memory_ctx(void* ctx) {
#ifdef _WIN32
    if (g_tls_index == TLS_OUT_OF_INDEXES) {
        g_tls_index = TlsAlloc();
        if (g_tls_index == TLS_OUT_OF_INDEXES) {
            return;
        }
    }
    TlsSetValue(g_tls_index, ctx);
#else
    if (!g_tls_initialized) {
        pthread_key_create(&g_tls_key, nullptr);
        g_tls_initialized = true;
    }
    pthread_setspecific(g_tls_key, ctx);
#endif
}

// @0x180001740 — tls_cleanup
void tls_cleanup() {
#ifdef _WIN32
    if (g_tls_index != TLS_OUT_OF_INDEXES) {
        TlsFree(g_tls_index);
        g_tls_index = TLS_OUT_OF_INDEXES;
    }
#else
    if (g_tls_initialized) {
        pthread_key_delete(g_tls_key);
        g_tls_initialized = false;
    }
#endif
}

// ============================================================================
// Engine runtime TLS
// ============================================================================

// @0x180089700 — rad_get_memory_ctx
// Decompilation: TlsGetValue(DAT_18037a2c0); return;
// The return value is in RAX from TlsGetValue — Ghidra shows void but it returns the pointer.
void* rad_get_memory_ctx() {
#ifdef _WIN32
    return TlsGetValue(g_tls_index);
#else
    if (!g_tls_initialized) {
        return nullptr;
    }
    return pthread_getspecific(g_tls_key);
#endif
}

// @0x180089a00 — rad_set_memory_ctx
// Decompilation: TlsSetValue(DAT_18037a2c0, this); return;
void rad_set_memory_ctx(void* ctx) {
#ifdef _WIN32
    TlsSetValue(g_tls_index, ctx);
#else
    if (!g_tls_initialized) {
        pthread_key_create(&g_tls_key, nullptr);
        g_tls_initialized = true;
    }
    pthread_setspecific(g_tls_key, ctx);
#endif
}

// ============================================================================
// Global allocator
// ============================================================================

// @0x180089870 — rad_set_allocator
// Decompilation: DAT_1803766b8 = this; return;
void rad_set_allocator(void* allocator) {
    g_allocator_ptr = allocator;
}

// ============================================================================
// Memory utilities
// ============================================================================

// @0x1800897f0 — rad_memset
// Decompilation: if (param_2 != 0) { FUN_1802154a0(this,param_1,param_2); return this; }
//                return (void *)0x0;
// FUN_1802154a0 is an optimized memset with SSE fast paths. We delegate to memset.
void* rad_memset(void* dst, uint8_t value, size_t count) {
    if (count == 0) {
        return nullptr;
    }
    memset(dst, value, count);
    return dst;
}

// @0x1800899f0 — rad_memmove
// The decompilation is a heavily unrolled, SSE-optimized memmove with small-size
// switch (1-16 bytes) and large-block copy loops. The logic handles overlapping
// regions: if src < dst < src+count, it copies backwards. Otherwise forwards.
// We delegate to memmove which provides identical semantics.
void* rad_memmove(void* dst, const void* src, size_t count) {
    if (count == 0) {
        return nullptr;
    }
    memmove(dst, src, count);
    return dst;
}

// ============================================================================
// Buffer operations
// ============================================================================

// @0x18008b630 — buffer_init
// Decompilation:
//   this+0x10 = param_3 (allocator)
//   this+0x00 = 0 (initially, then set to alloc result)
//   this+0x1c = 0 (flags cleared)
//   this+0x08 = param_1 (size)
//   If param_1 == 0: data = -1 sentinel
//   Else if param_2 == 0: data = allocator->vtable->alloc(allocator, param_1)
//   Else: data = allocator->vtable->alloc_aligned(allocator, param_2, param_1)
//   flags |= 1 (owns allocation)
//   alignment = param_2
void buffer_init(BufferContext* ctx, uint64_t size, int32_t alignment, void** allocator) {
    ctx->allocator = allocator;
    ctx->data = nullptr;
    ctx->flags = 0;
    ctx->byte_capacity = size;

    void* result;
    if (size == 0) {
        result = reinterpret_cast<void*>(static_cast<uintptr_t>(-1));
    } else {
        auto* vtable = reinterpret_cast<AllocatorVTable*>(*allocator);
        if (alignment == 0) {
            result = vtable->alloc(allocator, size);
        } else {
            result = vtable->alloc_aligned(allocator, alignment, size);
        }
    }

    ctx->data = result;
    ctx->flags |= 1;
    ctx->alignment = alignment;
}

// @0x18008b730 — buffer_destroy
// Decompilation:
//   if (size != 0 && data != 0) {
//     if (flags & 1) {
//       if (alignment != 0) { allocator->vtable->free_aligned(...); }
//       else { allocator->vtable->free(...); }
//     }
//     data = 0; size = 0;
//   }
void buffer_destroy(BufferContext* ctx) {
    if (ctx->byte_capacity == 0 || ctx->data == nullptr) {
        return;
    }

    if (ctx->flags & 1) {
        auto* vtable = reinterpret_cast<AllocatorVTable*>(*ctx->allocator);
        if (ctx->alignment != 0) {
            vtable->free_aligned(ctx->allocator, ctx->data);
        } else {
            vtable->free(ctx->allocator, ctx->data);
        }
    }

    ctx->data = nullptr;
    ctx->byte_capacity = 0;
}

// @0x18008bb90 — buffer_free_data
// Decompilation:
//   flags &= ~1 (clear ownership)
//   data = 0
//   size = 0
//   alignment = 0
void buffer_free_data(BufferContext* ctx) {
    ctx->flags &= ~static_cast<uint32_t>(1);
    ctx->data = nullptr;
    ctx->byte_capacity = 0;
    ctx->alignment = 0;
}

// @0x18008c3b0 — buffer_realloc
// Resizes the allocation owned by a BufferContext. Two paths:
//   alignment == 0: use realloc_simple (vtable+0x10), then zero-fill new region
//   alignment != 0: alloc_aligned new block, optionally copy old data, free old block
void buffer_realloc(BufferContext* ctx, uint64_t new_byte_size, int copy_old) {
    uint64_t old_size = ctx->byte_capacity;
    if (new_byte_size == old_size) {
        return;
    }

    auto* vtable = reinterpret_cast<AllocatorVTable*>(*ctx->allocator);

    if (ctx->alignment == 0) {
        // Unaligned path: realloc in place
        void* old_ptr = (old_size == 0) ? nullptr : ctx->data;
        void* new_ptr = vtable->realloc_simple(ctx->allocator, old_ptr, new_byte_size);
        ctx->data = new_ptr;

        if (new_byte_size == 0) {
            ctx->data = reinterpret_cast<void*>(static_cast<uintptr_t>(-1));
        } else if (old_size < new_byte_size) {
            rad_memset(static_cast<uint8_t*>(new_ptr) + old_size, 0,
                       new_byte_size - old_size);
        }
    } else {
        // Aligned path: alloc new, copy, free old
        void* new_ptr;
        if (new_byte_size == 0) {
            new_ptr = reinterpret_cast<void*>(static_cast<uintptr_t>(-1));
        } else {
            new_ptr = vtable->alloc_aligned(ctx->allocator, ctx->alignment, new_byte_size);
        }

        if (old_size != 0) {
            if (new_byte_size != 0 && copy_old) {
                uint64_t copy_size = (old_size < new_byte_size) ? old_size : new_byte_size;
                memcpy(new_ptr, ctx->data, copy_size);
            }
            vtable->free_aligned(ctx->allocator, ctx->data);
        }

        if (old_size < new_byte_size) {
            rad_memset(static_cast<uint8_t*>(new_ptr) + old_size, 0,
                       new_byte_size - old_size);
        }

        ctx->data = new_ptr;
    }

    ctx->byte_capacity = new_byte_size;
}

// Shared growth logic for RadBuffer — element_size controls the byte multiplier
// passed to buffer_realloc. Both buffer_grow and buffer_grow_0x30 are thin
// wrappers over this.
static uint64_t buffer_grow_impl(RadBuffer* buf, uint64_t additional_count, uint64_t element_size) {
    uint64_t old_count = buf->count;
    uint64_t total_capacity = buf->capacity;

    if (total_capacity < old_count + additional_count) {
        uint64_t grow = buf->min_grow;
        if (buf->min_grow <= total_capacity) {
            grow = total_capacity;
        }
        if (additional_count > grow) {
            grow = additional_count;
        }

        buffer_realloc(&buf->buf, (total_capacity + grow) * element_size, 1);
        buf->capacity += grow;
        old_count = buf->count;
    }

    buf->count = old_count + additional_count;
    return old_count;
}

// @0x18008a3f0 — buffer_grow (byte-element variant)
uint64_t buffer_grow(RadBuffer* buf, uint64_t additional_count) {
    return buffer_grow_impl(buf, additional_count, 1);
}

// @0x1800821e0 — buffer_grow_0x30 (0x30-byte element variant)
uint64_t buffer_grow_0x30(RadBuffer* buf, uint64_t additional_count) {
    return buffer_grow_impl(buf, additional_count, 0x30);
}
