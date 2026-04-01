#ifndef PNSRAD_PLUGIN_LOGGING_H
#define PNSRAD_PLUGIN_LOGGING_H

/* @module: pnsrad.dll */
/* @purpose: Internal printf-style logging with console color and ring buffer support.
 *   Messages are formatted via vsnprintf into a stack buffer, then dispatched
 *   through a ring buffer to a console output routine with ANSI-style coloring.
 */

#include <cstdint>
#include <cstdarg>

namespace NRadEngine {

// Log severity levels (arg1 to log_message / log_output)
enum LogLevel : uint32_t {
    kLogDebug   = 1,
    kLogWarning = 4,
    kLogError   = 8,
    kLogRaw     = 1000,  // No prefix, used for undecorated output
};

// @0x1800929a0 — log_message (variadic entry point)
// Packs the va_list tail onto the stack and delegates to log_message_impl.
// Never returns if level triggers abort.
//   param_1: log level (see LogLevel)
//   param_2: context id (0 = default)
//   fmt:     printf format string
/* @addr: 0x1800929a0 (pnsrad.dll) */ /* @confidence: H */
void log_message(uint32_t level, uint32_t context, const char* fmt, ...);

// @0x1800929c0 — log_message_impl (formatting + dispatch)
// Allocates ~0x2000 bytes on the stack, formats via vsnprintf (delegates
// to the CRT through fcn.180093500), then dispatches to the ring buffer
// writer and console output routines.
//   level:   log level bitmask
//   context: numeric context id
//   fmt:     format string
//   args:    pointer to variadic argument block
/* @addr: 0x1800929c0 (pnsrad.dll) */ /* @confidence: H */
void log_message_impl(uint32_t level, uint32_t context, const char* fmt, va_list* args);

// @0x180092620 — log_output (console output with coloring)
// Selects a prefix tag ("[DEBUG] ", "[WARNING] ", "[ERROR] ") and
// a console color attribute based on severity level. Writes to the
// console handle via OutputDebugStringA and optionally to a file.
// On Windows, queries console screen buffer info via GetStdHandle +
// GetConsoleScreenBufferInfo to apply color attributes.
/* @addr: 0x180092620 (pnsrad.dll) */ /* @confidence: H */
void log_output(uint32_t level, uint32_t context, const char* message);

// @0x180092430 — log_buffer_write (ring buffer insertion)
// Writes a formatted log entry into the circular log buffer,
// guarded by a critical section. Handles wrap-around via the
// ring index at +0x98/+0x9c in the log state structure.
/* @addr: 0x180092430 (pnsrad.dll) */ /* @confidence: H */
void log_buffer_write(void* log_state, const char* message);

// @0x1800925b0 — get_console_width
// Queries the console screen buffer width via GetStdHandle(-11)
// + GetConsoleScreenBufferInfo.  Caches the result in a global
// (DAT_1803808e4).  Returns 0 on failure.
/* @addr: 0x1800925b0 (pnsrad.dll) */ /* @confidence: H */
int16_t get_console_width();

// @0x180092910 — log_format_entry
// Formats a single log entry (max 0x100 bytes) using vsnprintf
// and null-pads the remainder.
/* @addr: 0x180092910 (pnsrad.dll) */ /* @confidence: H */
uint64_t log_format_entry(char* out_buf, const char* fmt, uint64_t arg2, uint64_t arg3);

} // namespace NRadEngine

#endif // PNSRAD_PLUGIN_LOGGING_H
