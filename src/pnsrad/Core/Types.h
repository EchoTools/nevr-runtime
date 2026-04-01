#ifndef PNSRAD_CORE_TYPES_H
#define PNSRAD_CORE_TYPES_H

#include <cstdint>
#include <cstddef>

/* @module: pnsrad.dll */
/* @purpose: Core type definitions for RAD Engine network service provider plugin */

#pragma pack(push, 8)

/* @addr: Referenced throughout plugin */
/* @size: 0x40 */
/* @confidence: H */
struct AllocatorVTable {
    /* @offset: 0x00 */ void* unknown_0x00;
    /* @offset: 0x08 */ void* (*alloc)(void* ctx, size_t size);
    /* @offset: 0x10 */ void* (*realloc_simple)(void* ctx, void* ptr, size_t size);
    /* @offset: 0x18 */ void* unknown_0x18;
    /* @offset: 0x20 */ void* unknown_0x20;
    /* @offset: 0x28 */ void* (*alloc_aligned)(void* ctx, int32_t alignment, size_t size);
    /* @offset: 0x30 */ void (*free)(void* ctx, void* ptr);
    /* @offset: 0x38 */ void (*free_aligned)(void* ctx, void* ptr);
};
static_assert(sizeof(AllocatorVTable) == 0x40);
static_assert(offsetof(AllocatorVTable, alloc) == 0x08);
static_assert(offsetof(AllocatorVTable, realloc_simple) == 0x10);
static_assert(offsetof(AllocatorVTable, alloc_aligned) == 0x28);
static_assert(offsetof(AllocatorVTable, free) == 0x30);
static_assert(offsetof(AllocatorVTable, free_aligned) == 0x38);

/* @addr: 0x180376600 (pnsrad.dll) (global instance) */
/* @size: 0x40 */
/* @confidence: H */
/* @updated: 2026-03-30 — all offsets verified against RadPluginDestroy @0x180088d80,
 *   VoipCreateDecoder @0x180089060, shutdown loop @0x180088e60.
 *   +0x30 accessed as decoder_count (uint64_t), +0x38 accessed as flags (uint32_t & 0xFFFFFFFE),
 *   +0x1C accessed as buffer flags (uint8_t & 6). Layout matches BufferContext(0x00-0x1F) +
 *   RadBuffer tail(0x20-0x37) + flags(0x38-0x3F). */
struct PluginContext {
    /* @offset: 0x00 */ void* buffer_ptr;            // Initialized by buffer_init
    /* @offset: 0x08 */ uint64_t size;               // Initialized by buffer_init
    /* @offset: 0x10 */ void* allocator_ctx;         // TLS memory context
    /* @offset: 0x18 */ uint64_t capacity;           // Initialized by buffer_init
    /* @offset: 0x20 */ uint64_t field_0x20;         // Set to 0x20 (initial capacity)
    /* @offset: 0x28 */ uint64_t field_0x28;         // Set to 0
    /* @offset: 0x30 */ uint64_t decoder_count;      // VoIP decoder count in array
    /* @offset: 0x38 */ uint32_t flags;              // Bit 0 cleared on shutdown
    /* @offset: 0x3c */ uint32_t field_0x3c;         // Padding/extra fields
};
static_assert(sizeof(PluginContext) == 0x40);
static_assert(offsetof(PluginContext, buffer_ptr) == 0x00);
static_assert(offsetof(PluginContext, size) == 0x08);
static_assert(offsetof(PluginContext, allocator_ctx) == 0x10);
static_assert(offsetof(PluginContext, capacity) == 0x18);
static_assert(offsetof(PluginContext, field_0x20) == 0x20);
static_assert(offsetof(PluginContext, field_0x28) == 0x28);
static_assert(offsetof(PluginContext, decoder_count) == 0x30);
static_assert(offsetof(PluginContext, flags) == 0x38);
static_assert(offsetof(PluginContext, field_0x3c) == 0x3c);

/* @size: 0x20 */
/* @confidence: H */
/* @purpose: Inner allocation tracking — owns a heap buffer via an allocator vtable.
 *           Used as the base of RadBuffer. Fields verified against
 *           buffer_init @0x18008b630, buffer_destroy @0x18008b730,
 *           buffer_realloc @0x18008c3b0, and buffer_free_data @0x18008bb90. */
struct BufferContext {
    /* @offset: 0x00 */ void*    data;          // Heap pointer (-1 sentinel when size==0 after alloc)
    /* @offset: 0x08 */ uint64_t byte_capacity; // Allocated byte count (param to realloc)
    /* @offset: 0x10 */ void**   allocator;     // Points to allocator instance (deref for vtable)
    /* @offset: 0x18 */ int32_t  alignment;     // 0 = unaligned alloc, nonzero = aligned alloc
    /* @offset: 0x1c */ uint32_t flags;         // Bit 0: owns allocation. Bits 1,2: special states.
};
static_assert(sizeof(BufferContext) == 0x20);
static_assert(offsetof(BufferContext, data) == 0x00);
static_assert(offsetof(BufferContext, byte_capacity) == 0x08);
static_assert(offsetof(BufferContext, allocator) == 0x10);
static_assert(offsetof(BufferContext, alignment) == 0x18);
static_assert(offsetof(BufferContext, flags) == 0x1c);

/* @size: 0x38 */
/* @confidence: H */
/* @purpose: Growable array/buffer — wraps BufferContext with element-count tracking.
 *           Used by string and dynamic array operations throughout the engine.
 *           Layout verified against buffer_grow @0x18008a3f0, push_char @0x18008b970,
 *           and typed buffer_grow @0x1800821e0. */
struct RadBuffer {
    /* @offset: 0x00 */ BufferContext buf;       // Inner allocation
    /* @offset: 0x20 */ uint64_t min_grow;      // Minimum growth increment (elements)
    /* @offset: 0x28 */ uint64_t capacity;      // Total capacity (elements)
    /* @offset: 0x30 */ uint64_t count;         // Current element count
};
static_assert(sizeof(RadBuffer) == 0x38);
static_assert(offsetof(RadBuffer, buf) == 0x00);
static_assert(offsetof(RadBuffer, min_grow) == 0x20);
static_assert(offsetof(RadBuffer, capacity) == 0x28);
static_assert(offsetof(RadBuffer, count) == 0x30);

#pragma pack(pop)

#endif // PNSRAD_CORE_TYPES_H
