#include "pnsrad/Core/containers.h"
#include "pnsrad/Core/linked_list.h"
#include "pnsrad/Core/Types.h"

/* @module: pnsrad.dll */
/* @purpose: CArray/CJsonNodePair container operations — element init, destroy,
 *           array reserve, clear, and destroy-reinit. */

namespace NRadEngine {

// Allocator helpers (from pnsrad runtime)
extern void* CSysThread_GetTLS();                       // @0x180089700
extern void  CSysThread_ReleaseLock(void* lock_ctx);    // @0x180089a00
extern void  buffer_realloc(void* buf, uint64_t new_byte_cap, int flag); // @0x18008c3b0

// JSON DB internals
extern void  CHashTable_Destroy(void* table);           // @0x180097510
extern int*  CJsonPath_Set(const char* path, void* node, uint32_t create_flag, uint64_t unused); // @0x1800987b0

// Allocator vtable free: index 6 = offset 0x30
static void allocator_free(void* allocator, void* ptr) {
    auto** vtable = *reinterpret_cast<void***>(allocator);
    auto fn = reinterpret_cast<void(*)(void*, void*)>(
        reinterpret_cast<uintptr_t*>(vtable)[6]);
    fn(allocator, ptr);
}

// Allocator vtable alloc: index 1 = offset 0x08
static void* allocator_alloc(void* allocator, uint64_t size) {
    auto** vtable = *reinterpret_cast<void***>(allocator);
    auto fn = reinterpret_cast<void*(*)(void*, uint64_t)>(
        reinterpret_cast<uintptr_t*>(vtable)[1]);
    return fn(allocator, size);
}

// @0x1401d5170 — CLinkedList::Init [Quest: confirmed]
// @confidence: H
void CLinkedListBase_Init(CLinkedListBase* self) {
    void* old_alloc = self->allocator;
    void* old_data = self->data;

    self->allocator = nullptr;
    self->data = nullptr;
    self->capacity = 0;
    self->count = 0;

    // Free existing allocation if the allocator is valid and data was present
    if (old_alloc != nullptr &&
        old_alloc != reinterpret_cast<void*>(-1) &&
        old_data != nullptr) {
        allocator_free(old_alloc, old_data);
    }

    self->data = nullptr;
    self->capacity = 0;
    self->count = 0;
    self->allocator = CSysThread_GetTLS();
}

// @0x180097500 — CJsonNodePair::Init
// @confidence: H
void CJsonNodePair_Init(CJsonNodePair* self) {
    self->json_root = nullptr;
    self->json_db = nullptr;
}

// @0x18009ddf0 — CJsonNodePair_FreeDB
// Frees the cached JSON database at self->json_db.
// The decompilation acquires two nested locks (default allocator + inner),
// destroys the hash table, frees via allocator, then releases both locks.
// @confidence: H
void CJsonNodePair_FreeDB(CJsonNodePair* self) {
    if (self->json_db == nullptr) {
        return;
    }

    // DAT_180384d00 is a global default allocator; falls back to TLS if not initialized
    void* outer_lock = CSysThread_GetTLS();
    CSysThread_ReleaseLock(outer_lock);

    void* inner_lock = CSysThread_GetTLS();
    CSysThread_ReleaseLock(inner_lock);

    void* allocator = CSysThread_GetTLS();

    void* db = self->json_db;
    if (db != nullptr) {
        CHashTable_Destroy(db);
    }
    allocator_free(allocator, db);

    CSysThread_ReleaseLock(inner_lock);
    self->json_db = nullptr;
    CSysThread_ReleaseLock(outer_lock);
}

// @0x1800980d0 — CJsonNodePair_SetPath
// @confidence: H
uint64_t CJsonNodePair_SetPath(CJsonNodePair* self, const char* path, uint32_t create_flag) {
    if (self->json_db != nullptr) {
        // Cached/read-only — error path (formats error string and aborts)
        // "$ json path: %s: ERROR, json db is cached, read only."
        return 0;
    }

    void* lock_ctx = CSysThread_GetTLS();
    CSysThread_ReleaseLock(lock_ctx);

    int* result = CJsonPath_Set(path, self, create_flag, 0);

    CSysThread_ReleaseLock(lock_ctx);
    return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(result)) & 0xFFFFFFFF;
}

// @0x180097650 — CJsonNodePair::Destroy
// @confidence: H
void CJsonNodePair_Destroy(CJsonNodePair* self) {
    CJsonNodePair_FreeDB(self);
    CJsonNodePair_SetPath(self, "", 1);
}

// @0x180082260 — CArray<CJsonNodePair>::Reserve
// Grows the array if needed, initializes new elements.
// Returns the old count (insertion point).
// @confidence: H
uint64_t CArrayJsonNodePair_Reserve(void* array, uint64_t additional) {
    auto* buf = static_cast<RadBuffer*>(array);
    uint64_t old_count = buf->count;
    uint64_t cap = buf->capacity;

    if (cap < old_count + additional) {
        uint64_t grow = buf->min_grow;
        if (grow <= cap) {
            grow = cap;
        }
        if (additional > grow) {
            grow = additional;
        }
        buffer_realloc(&buf->buf, (cap + grow) * 0x10, 1);
        buf->capacity += grow;
        old_count = buf->count;
    }

    buf->count = old_count + additional;

    void* alloc_ctx = static_cast<void*>(buf->buf.allocator);
    void* lock_ctx = CSysThread_GetTLS();
    CSysThread_ReleaseLock(alloc_ctx);

    auto* data = static_cast<CJsonNodePair*>(buf->buf.data);
    for (uint64_t i = old_count; i < buf->count; ++i) {
        CJsonNodePair_Init(&data[i]);
    }

    CSysThread_ReleaseLock(lock_ctx);
    return old_count;
}

// @0x18007f5d0 — CArray<CJsonNodePair>::DestroyReverse
// Iterates in reverse destroying each element, then frees the buffer.
// The decompilation reads fields at +0x00 (data), +0x08 (count), +0x10 (allocator)
// which is a simpler 3-field layout, not the full RadBuffer.
// @confidence: H
void CArrayJsonNodePair_DestroyReverse(void* array) {
    auto* data = *reinterpret_cast<CJsonNodePair**>(array);
    void* allocator = *reinterpret_cast<void**>(
        reinterpret_cast<uintptr_t>(array) + 0x10);
    int64_t count = *reinterpret_cast<int64_t*>(
        reinterpret_cast<uintptr_t>(array) + 0x08);

    if (count != 0 && allocator != nullptr) {
        for (int64_t i = count - 1; i >= 0; --i) {
            CJsonNodePair_Destroy(&data[i]);
        }
        allocator_free(allocator, data);
    }
}

// @0x180086920 — CArray<CJsonNodePair>::Clear
// Destroys all elements forward, sets count to 0. Retains buffer allocation.
// The decompilation checks `(*(longlong *)this + lVar1) != 0` which tests
// whether the base data pointer plus element offset is non-null.
// @confidence: H
void CArrayJsonNodePair_Clear(void* array) {
    auto* buf = static_cast<RadBuffer*>(array);
    void* alloc_ctx = static_cast<void*>(buf->buf.allocator);
    void* lock_ctx = CSysThread_GetTLS();
    CSysThread_ReleaseLock(alloc_ctx);

    auto* data = static_cast<CJsonNodePair*>(buf->buf.data);
    if (data != nullptr) {
        for (uint64_t i = 0; i < buf->count; ++i) {
            CJsonNodePair_Destroy(&data[i]);
        }
    }
    buf->count = 0;

    CSysThread_ReleaseLock(lock_ctx);
}

// @0x18008c850 — CArray<CJsonNodePair>::DestroyAndReinit
// Destroys all elements in reverse, frees old buffer, allocates new buffer
// of new_count elements and initializes them. Replaces allocator.
// @confidence: H
void CArrayJsonNodePair_DestroyAndReinit(void* array, int64_t new_count, void* new_allocator) {
    auto* data = *reinterpret_cast<CJsonNodePair**>(array);
    void* allocator = *reinterpret_cast<void**>(
        reinterpret_cast<uintptr_t>(array) + 0x10);
    int64_t old_count = *reinterpret_cast<int64_t*>(
        reinterpret_cast<uintptr_t>(array) + 0x08);

    if (old_count != 0 && allocator != nullptr) {
        for (int64_t i = old_count - 1; i >= 0; --i) {
            CJsonNodePair_Destroy(&data[i]);
        }
        allocator_free(allocator, data);
    }

    *reinterpret_cast<int64_t*>(reinterpret_cast<uintptr_t>(array) + 0x08) = new_count;
    *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(array) + 0x10) = new_allocator;

    if (new_count == 0 || new_allocator == nullptr) {
        *reinterpret_cast<void**>(array) = nullptr;
    } else {
        void* new_data = allocator_alloc(new_allocator, static_cast<uint64_t>(new_count) * 0x10);

        void* lock_ctx = CSysThread_GetTLS();
        CSysThread_ReleaseLock(new_allocator);

        auto* elements = static_cast<CJsonNodePair*>(new_data);
        for (int64_t i = 0; i < new_count; ++i) {
            CJsonNodePair_Init(&elements[i]);
        }

        CSysThread_ReleaseLock(lock_ctx);
        *reinterpret_cast<void**>(array) = new_data;
    }
}

} // namespace NRadEngine
