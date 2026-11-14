/* Structured, allocation-free log lines for the Quest hook backend.
 *
 * Every line is one JSON object built in a fixed stack buffer: no heap allocation,
 * so it can run from an ELF constructor. It is NOT async-signal-safe (it uses
 * snprintf and __android_log_write) and must not be called from a hooked function
 * that a signal handler may reach. Format:
 *
 *   {"ts_ms":1760000000000,"level":"error","event":"got_hook","op":"install",...}
 *
 * Keys are caller-supplied literals; string values are escaped (quote, backslash,
 * control bytes) and non-ASCII bytes become '?', so a line always parses. No field
 * carries a credential, token, URL or key value: callers pass module names, symbol
 * names, link-time addresses and status tokens only.
 *
 * The line is built by hand, not with a JSON library, so that logging needs no heap: it can be
 * called from the constructor, the reporter thread and the install path alike, and a failure to
 * allocate cannot swallow the line that reports it. (The constructor that calls it is not
 * allocation-free as a whole; sentinel::Arm allocates.) That is a deliberate deviation from the
 * repository's "no hand-built serialization" rule; the host test parses every captured line with
 * nlohmann::json (when the host has it) and with a strict validator, so malformed output fails
 * the build.
 *
 * The sink is logcat on Android and stderr elsewhere. A logcat line lasts only as long as logcat
 * keeps it, which does NOT meet the durable-log rule in AGENTS.md. The intended sink is the
 * sentinel's on-disk log (sentinel_log); SetLogSink is where it plugs in. A test replaces the
 * sink to assert on lines.
 *
 * The sources are built into two shared objects, the shim (libovrplatformloader.so) and the runtime (libnevr-runtime.so), and each
 * object has its own copy of this file's state. The shim owns the process's log file. The runtime's copy writes no file: the
 * runtime registers a host sink with SetHostRecordSink, and LogFieldList hands every record to it (ForwardRecordToHost).
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <type_traits>

namespace sentinel {

enum class LogLevel { kInfo, kWarn, kError, kDebug };

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

// The same line from an array the caller built (a count that is only known at run time). `fields` must hold `count`
// entries; the strings they point at must stay valid for the call.
void LogFieldList(LogLevel level, const char* event, const LogField* fields, std::size_t count);

// The host's record sink (the Quest shim's log, for the runtime copy): a function that writes one record to the process's one
// log and returns true when it did. Set by the runtime's Initialize; nothing sets it in the shim, which owns the log.
using HostRecordSink = bool (*)(LogLevel level, const char* event, const LogField* fields, std::size_t count);
void SetHostRecordSink(HostRecordSink sink) noexcept;

// Passes one record to the host sink. False when no sink is set or it did not take the record, so the caller writes it
// where it would have written it without a host.
bool ForwardRecordToHost(LogLevel level, const char* event, const LogField* fields, std::size_t count) noexcept;

// The runtime copy's Emit (sentinel_log.cpp) forwards one already-formatted line as a record of this event with the line in this one
// field; the shim (sentinel/entry.cpp, RuntimeLog) writes such a record through its own Emit, so the line reaches the log as it does
// when the shim writes it itself.
inline constexpr char kEmitHostEvent[] = "emit";
inline constexpr char kEmitHostField[] = "msg";

// Free-text compatibility form: {"event":"message","msg":"..."} (printf-style).
void LogEvent(LogLevel level, const char* format, ...) __attribute__((format(printf, 2, 3)));

}  // namespace sentinel
