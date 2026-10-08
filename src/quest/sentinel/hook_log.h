/* Structured, allocation-free log lines for the Quest hook backend.
 *
 * Every line is one JSON object built in a fixed stack buffer, so it is safe from
 * an ELF constructor and from inside a hooked call:
 *
 *   {"ts_ms":1760000000000,"level":"error","event":"got_hook","op":"install",...}
 *
 * Keys are caller-supplied literals; string values are escaped (quote, backslash,
 * control bytes) and non-ASCII bytes become '?', so a line always parses. No field
 * carries a credential, token, URL or key value: callers pass module names, symbol
 * names, link-time addresses and status tokens only.
 *
 * The sink is logcat on Android and stderr elsewhere. The durable on-device log
 * file belongs to the sentinel config/log work (separate PR); until it lands a
 * line is durable only as long as logcat keeps it. A test replaces the sink with
 * SetLogSink to assert on the lines.
 */
#pragma once

#include <cstdint>
#include <initializer_list>
#include <type_traits>

namespace sentinel {

enum class LogLevel { kInfo, kWarn, kError };

using LogSink = void (*)(LogLevel level, const char* line);

// Replaces the sink; nullptr restores the default. Returns the previous sink
// (nullptr when it was the default).
LogSink SetLogSink(LogSink sink);

// One key and a string or integer value.
struct LogField {
  const char* key;
  const char* text;      // nullptr: use `number`
  long long number;

  LogField(const char* k, const char* v) : key(k), text(v != nullptr ? v : "(null)"), number(0) {}
  template <typename T, typename = std::enable_if_t<std::is_integral_v<T>>>
  LogField(const char* k, T v) : key(k), text(nullptr), number(static_cast<long long>(v)) {}
};

// "0x" + 16 hex digits into `buf`; returns `buf` so it can be passed as a value.
const char* HexString(char (&buf)[19], std::uint64_t value);

// Emits one JSON line: ts_ms, level, event, then the fields in order.
void LogFields(LogLevel level, const char* event, std::initializer_list<LogField> fields);

// Free-text compatibility form: {"event":"message","msg":"..."} (printf-style).
void LogEvent(LogLevel level, const char* format, ...) __attribute__((format(printf, 2, 3)));

}  // namespace sentinel
