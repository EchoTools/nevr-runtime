/*
 * safe_memory.h -- VEH-based crash-safe memory access for MinGW.
 *
 * Replaces MSVC __try/__except blocks that guard against access violations
 * during runtime memory inspection (e.g., reading unknown pointers from
 * the game engine, verifying prologue bytes before patching).
 *
 * Approach: Vectored Exception Handler + setjmp/longjmp. On ACCESS_VIOLATION,
 * the VEH fires before any frame-based SEH and longjmps back to the caller.
 * Thread-safe via thread-local jmp_buf.
 *
 * 64-bit Windows only (table-based unwinding makes longjmp safe across
 * exception frames on x86_64).
 */
#pragma once

#include <windows.h>
#include <setjmp.h>
#include <cstdint>
#include <cstring>

namespace nevr {
namespace detail {

/* Thread-local state for the VEH handler. */
inline thread_local jmp_buf g_safeJmpBuf;
inline thread_local bool g_safeActive = false;

inline LONG CALLBACK SafeMemoryVEH(EXCEPTION_POINTERS* ep) {
    if (!g_safeActive) return EXCEPTION_CONTINUE_SEARCH;
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION)
        return EXCEPTION_CONTINUE_SEARCH;

    g_safeActive = false;
    longjmp(g_safeJmpBuf, 1);
    /* unreachable */
    return EXCEPTION_CONTINUE_EXECUTION;
}

/*
 * RAII guard that installs VEH on construction and removes on destruction.
 * The handler is per-call, not persistent, to avoid priority conflicts
 * with the crash recovery VEH in gamepatches core.
 */
struct SafeMemoryGuard {
    PVOID handle;

    SafeMemoryGuard() {
        handle = AddVectoredExceptionHandler(1, SafeMemoryVEH);
        detail::g_safeActive = true;
    }

    ~SafeMemoryGuard() {
        detail::g_safeActive = false;
        if (handle) RemoveVectoredExceptionHandler(handle);
    }

    SafeMemoryGuard(const SafeMemoryGuard&) = delete;
    SafeMemoryGuard& operator=(const SafeMemoryGuard&) = delete;
};

} // namespace detail

/*
 * Compare memory at addr against expected bytes. Returns true if they match,
 * false if they don't match OR if addr is inaccessible.
 */
inline bool SafeMemcmp(const void* addr, const void* expected, size_t len) {
    detail::SafeMemoryGuard guard;
    if (setjmp(detail::g_safeJmpBuf) != 0) {
        return false;  /* Access violation — addr is bad */
    }
    return memcmp(addr, expected, len) == 0;
}

/*
 * Copy len bytes from src to dst. Returns true on success, false if
 * src is inaccessible (dst is not written in that case).
 */
inline bool SafeMemcpy(void* dst, const void* src, size_t len) {
    detail::SafeMemoryGuard guard;
    if (setjmp(detail::g_safeJmpBuf) != 0) {
        return false;
    }
    memcpy(dst, src, len);
    return true;
}

/*
 * Read a uint64_t from addr. Returns true and writes to *out on success,
 * false if addr is inaccessible (*out is not modified).
 */
inline bool SafeReadU64(uintptr_t addr, uint64_t* out) {
    detail::SafeMemoryGuard guard;
    if (setjmp(detail::g_safeJmpBuf) != 0) {
        return false;
    }
    *out = *reinterpret_cast<const uint64_t*>(addr);
    return true;
}

/*
 * Read a uint16_t from addr. Returns true and writes to *out on success.
 */
inline bool SafeReadU16(uintptr_t addr, uint16_t* out) {
    detail::SafeMemoryGuard guard;
    if (setjmp(detail::g_safeJmpBuf) != 0) {
        return false;
    }
    *out = *reinterpret_cast<const uint16_t*>(addr);
    return true;
}

} // namespace nevr
