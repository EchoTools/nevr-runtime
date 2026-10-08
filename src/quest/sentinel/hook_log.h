/* Structured, allocation-free log lines for the Quest hook backend.
 *
 * Every line is `event=<name> key=value ...` in one fixed buffer, so it is safe
 * from an ELF constructor and from inside a hooked call. No line carries a
 * credential, token, URL or key value: callers pass module names, symbol names,
 * link-time addresses and status tokens only.
 *
 * The default sink is logcat on Android and stderr elsewhere. A test replaces
 * it with SetLogSink to assert on the lines.
 */
#pragma once

namespace sentinel {

enum class LogLevel { kInfo, kWarn, kError };

using LogSink = void (*)(LogLevel level, const char* line);

// Replaces the sink; nullptr restores the default. Returns the previous sink
// (nullptr when it was the default).
LogSink SetLogSink(LogSink sink);

// Formats into a 512-byte buffer (truncating) and delivers one line.
void LogEvent(LogLevel level, const char* format, ...) __attribute__((format(printf, 2, 3)));

}  // namespace sentinel
