#include "logging.h"

#include <cstring>
#include <cstdio>

/* @module: pnsrad.dll */

#ifndef _WIN32
// Stubs for non-Windows builds
typedef void* HANDLE;
#define INVALID_HANDLE_VALUE ((HANDLE)(long long)-1)
static HANDLE GetStdHandle(int) { return INVALID_HANDLE_VALUE; }
static int GetConsoleScreenBufferInfo(HANDLE, void*) { return 0; }
static void OutputDebugStringA(const char*) {}
static void SetConsoleTextAttribute(HANDLE, int) {}
#else
#include <windows.h>
#endif

// Forward declarations for internal helpers
extern "C" void memset_wrapper(void* ptr, int val, uint64_t size);  // @0x1800897f0
extern int64_t strnlen_wrapper(const char* str, int64_t max_len);   // @0x180093120
extern int64_t str_compare(const char* a, const char* b, int32_t case_sensitive, int64_t max_len); // @0x180092c30
extern int64_t str_find_char_reverse(const char* str, int32_t ch, int32_t case_sensitive); // @0x180092f00
extern int64_t vsnprintf_wrapper(char* buf, int64_t size, const char* fmt, va_list* args); // @0x180093500
extern void memset_zero(void* ptr, int val, uint64_t size);         // @0x1802154a0
extern void write_to_output(const char* buf, int64_t len, va_list* args); // @0x180092be0
extern void* get_output_handle();                                     // @0x180089b80

// Global: cached console width
/* @addr: 0x1803808e4 (pnsrad.dll) */
static int16_t s_console_width = 0;

// Global: empty string sentinel
/* @addr: 0x180222db4 (pnsrad.dll) */
extern const char g_empty_string[];

// Global: log file format prefix
/* @addr: 0x180228f58 (pnsrad.dll) */
extern const char g_log_prefix_format[];  // e.g. "%s" or a timestamp prefix

// Global: log mutex / critical section state
/* @addr: 0x1800a0ff0 (pnsrad.dll) */
extern void critical_section_enter(void* out_lock, void* cs_ptr);
/* @addr: 0x1800a1020 (pnsrad.dll) */
extern void critical_section_leave(void* lock);

namespace NRadEngine {

// @0x1800929a0 — log_message (variadic entry point)
// Packs variadic args and delegates to log_message_impl.
/* @addr: 0x1800929a0 (pnsrad.dll) */ /* @confidence: H */
void log_message(uint32_t level, uint32_t context, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    log_message_impl(level, context, fmt, &args);
    va_end(args);
}

// @0x1800929c0 — log_message_impl
// Formats the message via vsnprintf into a 0x2000-byte stack buffer,
// then dispatches to the ring buffer writer and console output.
/* @addr: 0x1800929c0 (pnsrad.dll) */ /* @confidence: H */
void log_message_impl(uint32_t level, uint32_t context, const char* fmt, va_list* args) {
    // __chkstk for ~0x2000 stack bytes
    char message_buf[0x2000];

    memset_wrapper(message_buf, 0, 0x2000);

    // Check if fmt contains a '%' — if so, we need to format it
    int64_t has_format = str_compare(fmt, g_empty_string, 1, static_cast<uint64_t>(-1));
    if (has_format == 0) {
        // No format specifiers or empty string — use raw args as the message
        fmt = reinterpret_cast<const char*>(*args);
    } else {
        int64_t pct_pos = str_find_char_reverse(
            fmt, '%', 1);
        if (pct_pos != -1) {
            // Has format specifiers — format into the stack buffer
            vsnprintf_wrapper(message_buf, 0x2000, fmt, args);

            int64_t msg_len = strnlen_wrapper(message_buf, 0x2000);
            uint64_t pad_start = msg_len + 1;
            if (pad_start < 0x2000) {
                memset_zero(&message_buf[pad_start], 0, 0x2000 - pad_start);
            }
            fmt = message_buf;
        }
    }

    // Dispatch to the environment-based log buffer system
    extern int64_t get_error_buffer();     // @0x18008a740
    int64_t env = get_error_buffer();
    if (env != 0) {
        // Write to the ring buffer at env+0x... structure
        log_buffer_write(reinterpret_cast<void*>(env), fmt);
    }

    // Console output
    log_output(level, context, fmt);
}

// @0x1800925b0 — get_console_width
// Queries the console screen buffer width.  Caches result in s_console_width.
/* @addr: 0x1800925b0 (pnsrad.dll) */ /* @confidence: H */
int16_t get_console_width() {
    if (s_console_width == 0) {
        HANDLE hStdOut = GetStdHandle(-11);  // STD_OUTPUT_HANDLE = 0xFFFFFFF5

        HANDLE handle = INVALID_HANDLE_VALUE;
        if (hStdOut != nullptr) {
            handle = hStdOut;
        }

        if (handle != INVALID_HANDLE_VALUE) {
#ifdef _WIN32
            CONSOLE_SCREEN_BUFFER_INFO csbi;
            int ok = GetConsoleScreenBufferInfo(handle, &csbi);
            if (ok != 0) {
                s_console_width = csbi.dwSize.X;
            }
#endif
            return s_console_width;
        }
    }
    return s_console_width;
}

// @0x180092620 — log_output (console output with coloring)
// Selects prefix tag and color attribute, then writes to console.
/* @addr: 0x180092620 (pnsrad.dll) */ /* @confidence: H */
void log_output(uint32_t level, uint32_t context, const char* message) {
    int32_t int_level = static_cast<int32_t>(level);

    // Select prefix tag based on severity
    const char* prefix;
    if (context == 1000) {
        prefix = "";   // kLogRaw: no prefix
    } else if (int_level == 1) {
        prefix = "[DEBUG] ";
    } else if (int_level == 4) {
        prefix = "[WARNING] ";
    } else {
        prefix = "";
        if (int_level == 8) {
            prefix = "[ERROR] ";
        }
    }

    // Select log file prefix (timestamp format or empty)
    const char* file_prefix = g_log_prefix_format;
    if (context == 1000) {
        file_prefix = "";
    }

    // Print prefix to console
    if (*prefix != '\0') {
        OutputDebugStringA(prefix);
    }

    // Query console width for line wrapping
    int16_t width = get_console_width();

    // Apply color attribute based on severity
#ifdef _WIN32
    HANDLE hConsole = GetStdHandle(-11);
    if (hConsole != INVALID_HANDLE_VALUE) {
        int color_attr;
        if (int_level == 8) {
            color_attr = 0x0C;  // Red for errors
        } else if (int_level == 4) {
            color_attr = 0x0E;  // Yellow for warnings
        } else if (int_level == 1) {
            color_attr = 0x08;  // Dark gray for debug
        } else {
            color_attr = 0x07;  // Default
        }
        SetConsoleTextAttribute(hConsole, color_attr);
    }
#endif
    (void)width;

    // Output the message
    OutputDebugStringA(message);
    OutputDebugStringA("\n");

    // Write to log file through the output handle
    void* output_handle = get_output_handle();
    if (output_handle != nullptr) {
        write_to_output(file_prefix, 0, nullptr);
        write_to_output(prefix, 0, nullptr);
        write_to_output(message, 0, nullptr);
        write_to_output("\n", 0, nullptr);
    }

#ifdef _WIN32
    // Reset console color
    if (hConsole != INVALID_HANDLE_VALUE) {
        SetConsoleTextAttribute(hConsole, 0x07);
    }
#endif
}

// Format string "%s" used by log_format_entry
/* @addr: 0x180228e5c (pnsrad.dll) */
extern const char g_fmt_string_s[];  // "%s"

// @0x180092430 — log_buffer_write (ring buffer insertion)
// Writes a formatted entry into the circular log buffer under a critical section.
/* @addr: 0x180092430 (pnsrad.dll) */ /* @confidence: H */
void log_buffer_write(void* log_state, const char* message) {
    uint8_t lock_buf[16];
    uintptr_t state = reinterpret_cast<uintptr_t>(log_state);

    // Acquire critical section at state+0xa0
    critical_section_enter(lock_buf, reinterpret_cast<void*>(state + 0xa0));

    // Check ring buffer capacity
    uint64_t write_count = *reinterpret_cast<uint64_t*>(state + 0x88);
    uint64_t capacity = *reinterpret_cast<uint64_t*>(state + 0x90);

    if (write_count == capacity && write_count != 0) {
        // Buffer full — advance the read index (overwrite oldest)
        uint32_t read_idx = *reinterpret_cast<uint32_t*>(state + 0x98) + 1;
        if (read_idx == static_cast<uint32_t>(capacity)) {
            read_idx = 0;
        }
        *reinterpret_cast<uint32_t*>(state + 0x98) = read_idx;
        *reinterpret_cast<uint64_t*>(state + 0x88) = write_count - 1;
    }

    // Determine the destination entry (0x100 bytes per entry)
    char entry_buf[0x100];
    memset_wrapper(entry_buf, 0, 0x100);

    const char* src;
    if (*reinterpret_cast<uint64_t*>(state + 0x88) == 0) {
        src = entry_buf;
    } else {
        uint64_t prev_idx;
        if (*reinterpret_cast<int32_t*>(state + 0x9c) == 0) {
            prev_idx = *reinterpret_cast<uint64_t*>(state + 0x90) - 1;
        } else {
            prev_idx = static_cast<uint64_t>(
                *reinterpret_cast<int32_t*>(state + 0x9c) - 1);
        }
        src = reinterpret_cast<const char*>(
            prev_idx * 0x100 + *reinterpret_cast<uintptr_t*>(state + 0x80));
    }

    // Format the entry via vsnprintf with "%s" format
    log_format_entry(const_cast<char*>(src), g_fmt_string_s,
                     reinterpret_cast<uint64_t>(message), 0);

    // Copy into the ring buffer at the write index
    uint32_t write_idx = *reinterpret_cast<uint32_t*>(state + 0x9c);
    char* dest = reinterpret_cast<char*>(
        static_cast<uint64_t>(write_idx) * 0x100 +
        *reinterpret_cast<uintptr_t*>(state + 0x80));

    // Block copy (8 x 16 bytes = 0x100)
    for (int i = 0; i < 8; i++) {
        memcpy(dest + i * 0x20, src + i * 0x20, 0x20);
    }

    // Advance write index and count
    *reinterpret_cast<uint32_t*>(state + 0x9c) = write_idx + 1;
    if (*reinterpret_cast<uint32_t*>(state + 0x9c) >=
        static_cast<uint32_t>(*reinterpret_cast<uint64_t*>(state + 0x90))) {
        *reinterpret_cast<uint32_t*>(state + 0x9c) = 0;
    }
    *reinterpret_cast<uint64_t*>(state + 0x88) =
        *reinterpret_cast<uint64_t*>(state + 0x88) + 1;

    // Release critical section
    critical_section_leave(lock_buf);
}

// @0x180092910 — log_format_entry
// Formats a single log entry into out_buf (max 0x100 bytes) using vsnprintf.
/* @addr: 0x180092910 (pnsrad.dll) */ /* @confidence: H */
uint64_t log_format_entry(char* out_buf, const char* fmt, uint64_t arg2, uint64_t arg3) {
    // Pack args as a variadic block on the stack
    va_list va_block;
    // The original passes &local_res18 which holds {arg2, arg3}
    // We replicate via direct vsnprintf call
    uint64_t va_data[2] = { arg2, arg3 };

    uint64_t result = vsnprintf_wrapper(out_buf, 0x100, fmt,
                                         reinterpret_cast<va_list*>(&va_data));

    int64_t len = strnlen_wrapper(out_buf, 0x100);
    uint64_t pad_start = len + 1;
    if (pad_start < 0x100) {
        memset_zero(&out_buf[pad_start], 0, 0x100 - pad_start);
    }
    return result;
}

} // namespace NRadEngine
