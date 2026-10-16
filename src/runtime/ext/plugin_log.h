// plugin_log.h — how a line a plugin logs through ctx->log becomes a host log line (N135, #598).
//
// PCH-safe on purpose: std types only, like plugin_manifest.h, so the unit test links it with no
// host code. The loader (plugin_loader.cpp, HostPluginLog) hands the result to Log().
//
// Line shape:   [NEVR.PLUGIN] plugin=<id> <message> <key>=<value> ...
// `plugin=` is the host's name for the plugin (its config.yaml entry's `name`), never a string the
// plugin picks for itself in the line. A plugin's text cannot start a second line (control characters
// become spaces) and cannot overrun the host's boot buffer (Log() formats into 2048 bytes before the
// game logger exists), so each part is capped and fields past the cap are counted, not silently lost.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "extension/plugin_interface.h"

namespace nevr_plugin_log {

inline constexpr std::size_t kMaxMessageBytes = 512;
inline constexpr std::size_t kMaxKeyBytes = 48;
inline constexpr std::size_t kMaxValueBytes = 128;
inline constexpr std::size_t kMaxLineBytes = 1500;  // under Log()'s 2048-byte boot buffer

struct Formatted {
  uint32_t level;    // an NevrLogLevel value, always in range
  std::string line;  // the whole host log line, no trailing newline
};

namespace detail {

inline bool IsControl(char c) {
  const unsigned char u = static_cast<unsigned char>(c);
  return u < 0x20 || u == 0x7f;
}

inline bool IsIdentChar(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '.' ||
         c == '-';
}

// At most `cap` bytes of `text`, control characters as spaces; "..." marks a cut.
inline std::string Clean(const char* text, std::size_t cap) {
  std::string out;
  if (text == nullptr) return out;
  for (const char* p = text; *p != '\0'; ++p) {
    if (out.size() == cap) {
      out += "...";
      break;
    }
    out += IsControl(*p) ? ' ' : *p;
  }
  return out;
}

// key=value needs no quotes unless the value is empty or holds a space, '=', '"' or '\'.
inline std::string Value(const char* text) {
  const std::string clean = Clean(text, kMaxValueBytes);
  const bool plain = !clean.empty() && clean.find_first_of(" =\"\\") == std::string::npos;
  if (plain) return clean;
  std::string quoted = "\"";
  for (const char c : clean) {
    if (c == '"' || c == '\\') quoted += '\\';
    quoted += c;
  }
  quoted += '"';
  return quoted;
}

// A key is [A-Za-z0-9_.-]; anything else becomes '_'. Empty (or null) after cleaning: no key.
inline std::string Key(const char* text) {
  std::string out;
  if (text == nullptr) return out;
  for (const char* p = text; *p != '\0' && out.size() < kMaxKeyBytes; ++p) out += IsIdentChar(*p) ? *p : '_';
  return out;
}

}  // namespace detail

// The plugin's id as it appears after `plugin=`: [A-Za-z0-9_.-], anything else '_'; "unknown" when
// the plugin passed none.
inline std::string Identity(const char* id) {
  const std::string key = detail::Key(id);
  return key.empty() ? std::string("unknown") : key;
}

inline Formatted Format(const char* id, uint32_t level, const char* message, const NevrLogField* fields,
                        uint32_t fieldCount) {
  Formatted out;
  const bool levelValid = level <= static_cast<uint32_t>(NEVR_LOG_ERROR);
  out.level = levelValid ? level : static_cast<uint32_t>(NEVR_LOG_WARNING);
  out.line = "[NEVR.PLUGIN] plugin=" + Identity(id);
  const std::string text = detail::Clean(message, kMaxMessageBytes);
  if (!text.empty()) out.line += " " + text;
  if (!levelValid) out.line += " level_invalid=" + std::to_string(level);
  if (fields == nullptr) fieldCount = 0;
  uint32_t dropped = 0;
  for (uint32_t i = 0; i < fieldCount; ++i) {
    const std::string key = detail::Key(fields[i].key);
    if (key.empty()) {
      ++dropped;
      continue;
    }
    const std::string part = " " + key + "=" + detail::Value(fields[i].value);
    if (out.line.size() + part.size() > kMaxLineBytes) {
      ++dropped;
      continue;
    }
    out.line += part;
  }
  if (dropped > 0) out.line += " fields_dropped=" + std::to_string(dropped);
  return out;
}

}  // namespace nevr_plugin_log
