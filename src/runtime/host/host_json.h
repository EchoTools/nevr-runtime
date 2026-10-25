#pragma once

// A JSON object line written into a caller-owned fixed buffer: no heap, no CRT
// formatting, no locks, no I/O. It exists for the two host paths that cannot
// allocate, the DllMain-adjacent code and the crash callback; everywhere else the
// host builds records with nlohmann::json. Both produce one object per line with
// the same header (`tag`, `ts`, `pid`), so a reader cannot tell them apart.
//
// A record that does not fit is never emitted truncated (a truncated JSON line is
// unparseable and worse than none): Finish() rewinds to the header and appends
// `"error":"record_too_large"` instead.

#include <windows.h>

#include <cstddef>
#include <cstdint>

namespace nevr_host {

// UTC, millisecond resolution: "2026-01-02T03:04:05.678Z" plus a terminating NUL.
inline constexpr std::size_t kTimestampSize = 25;

inline bool FormatUtcTimestamp(char (&out)[kTimestampSize]) noexcept {
  FILETIME now{};
  GetSystemTimeAsFileTime(&now);
  SYSTEMTIME time{};
  if (FileTimeToSystemTime(&now, &time) == FALSE) return false;
  const unsigned fields[7] = {time.wYear, time.wMonth, time.wDay, time.wHour,
                              time.wMinute, time.wSecond, time.wMilliseconds};
  const int widths[7] = {4, 2, 2, 2, 2, 2, 3};
  const char separators[7] = {'-', '-', 'T', ':', ':', '.', 'Z'};
  std::size_t at = 0;
  for (int field = 0; field < 7; ++field) {
    unsigned value = fields[field];
    for (int digit = widths[field] - 1; digit >= 0; --digit) {
      out[at + static_cast<std::size_t>(digit)] = static_cast<char>('0' + value % 10);
      value /= 10;
    }
    at += static_cast<std::size_t>(widths[field]);
    out[at++] = separators[field];
  }
  out[at] = '\0';
  return true;
}

class JsonLine {
 public:
  JsonLine(char* buffer, std::size_t capacity, const char* event) noexcept
      : buffer_(buffer), capacity_(capacity) {
    Raw('{');
    Key("tag");
    Quoted("NEVR.HOST");
    Key("ts");
    Timestamp();
    Key("pid");
    Decimal(GetCurrentProcessId());
    Key("event");
    Quoted(event);
    header_end_ = length_;
  }

  JsonLine& Str(const char* key, const char* value) noexcept {
    Key(key);
    Quoted(value == nullptr ? "" : value);
    return *this;
  }

  // UTF-16 to UTF-8 with JSON escaping. An unpaired surrogate becomes U+FFFD.
  JsonLine& Wide(const char* key, const wchar_t* value) noexcept {
    Key(key);
    Raw('"');
    for (std::size_t at = 0; value != nullptr && value[at] != L'\0'; ++at) {
      std::uint32_t code = value[at];
      if (code >= 0xD800 && code <= 0xDBFF) {
        const std::uint32_t low = value[at + 1];
        if (low >= 0xDC00 && low <= 0xDFFF) {
          code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
          ++at;
        } else {
          code = 0xFFFD;
        }
      } else if (code >= 0xDC00 && code <= 0xDFFF) {
        code = 0xFFFD;
      }
      Utf8(code);
    }
    Raw('"');
    return *this;
  }

  JsonLine& UInt(const char* key, std::uint64_t value) noexcept {
    Key(key);
    Decimal(value);
    return *this;
  }

  // Signed values are written as decimal with a leading minus sign.
  JsonLine& Int(const char* key, std::int64_t value) noexcept {
    Key(key);
    if (value < 0) {
      Raw('-');
      Decimal(static_cast<std::uint64_t>(0) - static_cast<std::uint64_t>(value));
    } else {
      Decimal(static_cast<std::uint64_t>(value));
    }
    return *this;
  }

  // A hexadecimal value as a "0x..." string (JSON has no hex numbers).
  JsonLine& Hex(const char* key, std::uint64_t value) noexcept {
    Key(key);
    Raw('"');
    Raw('0');
    Raw('x');
    bool started = false;
    for (int shift = 60; shift >= 0; shift -= 4) {
      const unsigned digit = static_cast<unsigned>((value >> shift) & 0xF);
      if (digit == 0 && !started && shift != 0) continue;
      started = true;
      Raw(static_cast<char>(digit < 10 ? '0' + digit : 'a' + (digit - 10)));
    }
    Raw('"');
    return *this;
  }

  JsonLine& Bool(const char* key, bool value) noexcept {
    Key(key);
    Text(value ? "true" : "false");
    return *this;
  }

  // Lower-case hex string of `count` bytes.
  JsonLine& Bytes(const char* key, const unsigned char* bytes, std::size_t count) noexcept {
    Key(key);
    Raw('"');
    for (std::size_t at = 0; at < count; ++at) {
      static constexpr char kDigits[] = "0123456789abcdef";
      Raw(kDigits[bytes[at] >> 4]);
      Raw(kDigits[bytes[at] & 0xF]);
    }
    Raw('"');
    return *this;
  }

  // Close the object and append the newline. Returns the line length, never 0
  // for a buffer that holds at least the header and the overflow marker.
  std::size_t Finish() noexcept {
    if (overflow_) {
      length_ = header_end_;
      overflow_ = false;
      Key("error");
      Quoted("record_too_large");
    }
    Raw('}');
    Raw('\n');
    if (overflow_ || capacity_ == 0) return 0;
    return length_;
  }

  bool Overflowed() const noexcept { return overflow_; }

 private:
  void Raw(char c) noexcept {
    if (overflow_) return;
    if (length_ + 1 > capacity_) {
      overflow_ = true;
      return;
    }
    buffer_[length_++] = c;
  }

  void Text(const char* text) noexcept {
    for (; *text != '\0'; ++text) Raw(*text);
  }

  void Key(const char* key) noexcept {
    if (length_ > 1) Raw(',');
    Quoted(key);
    Raw(':');
  }

  void Quoted(const char* text) noexcept {
    Raw('"');
    for (; *text != '\0'; ++text) {
      Escaped(static_cast<unsigned char>(*text));
    }
    Raw('"');
  }

  void Escaped(unsigned char c) noexcept {
    if (c == '"' || c == '\\') {
      Raw('\\');
      Raw(static_cast<char>(c));
    } else if (c < 0x20) {
      static constexpr char kDigits[] = "0123456789abcdef";
      Text("\\u00");
      Raw(kDigits[c >> 4]);
      Raw(kDigits[c & 0xF]);
    } else {
      Raw(static_cast<char>(c));
    }
  }

  void Utf8(std::uint32_t code) noexcept {
    if (code < 0x80) {
      Escaped(static_cast<unsigned char>(code));
    } else if (code < 0x800) {
      Raw(static_cast<char>(0xC0 | (code >> 6)));
      Raw(static_cast<char>(0x80 | (code & 0x3F)));
    } else if (code < 0x10000) {
      Raw(static_cast<char>(0xE0 | (code >> 12)));
      Raw(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
      Raw(static_cast<char>(0x80 | (code & 0x3F)));
    } else {
      Raw(static_cast<char>(0xF0 | (code >> 18)));
      Raw(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
      Raw(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
      Raw(static_cast<char>(0x80 | (code & 0x3F)));
    }
  }

  void Decimal(std::uint64_t value) noexcept {
    char digits[20];
    std::size_t count = 0;
    do {
      digits[count++] = static_cast<char>('0' + value % 10);
      value /= 10;
    } while (value != 0);
    while (count > 0) Raw(digits[--count]);
  }

  void Timestamp() noexcept {
    char text[kTimestampSize] = {};
    if (!FormatUtcTimestamp(text)) {
      Quoted("unknown");
      return;
    }
    Quoted(text);
  }

  char* buffer_;
  std::size_t capacity_;
  std::size_t length_ = 0;
  std::size_t header_end_ = 0;
  bool overflow_ = false;
};

}  // namespace nevr_host
