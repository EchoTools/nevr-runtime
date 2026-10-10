#pragma once
// The export tracer's pure decisions (#20): which DLLs a `-traceexports` list selects, and which loaded
// file is which. Header-only and free of windows.h, so the unit test needs neither Wine nor the game.

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace ExportTracePolicy {

enum : std::uint32_t {
  kPnsrad = 1,
  kPnsovr = 2,
  kPnsdemo = 4,
  kAllModules = kPnsrad | kPnsovr | kPnsdemo,
};

/// Calls of one export logged line by line before only its summary is.
constexpr std::uint64_t kFullLogCalls = 64;

/// Seconds between summary lines.
constexpr std::uint64_t kSummarySeconds = 10;

inline char Lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

inline bool EqualsNoCase(const char* a, std::size_t aLen, const char* b) {
  if (std::strlen(b) != aLen) return false;
  for (std::size_t i = 0; i < aLen; ++i) {
    if (Lower(a[i]) != Lower(b[i])) return false;
  }
  return true;
}

/// "pnsrad,pnsovr", "all", "pnsrad pnsdemo": the modules named, case-insensitive, separated by commas,
/// semicolons or spaces, with or without ".dll". An empty or null list selects nothing (the tracer is off).
/// `*unknown` (optional) is set when a token names no module.
inline std::uint32_t ParseModules(const char* list, bool* unknown = nullptr) {
  std::uint32_t mask = 0;
  if (unknown != nullptr) *unknown = false;
  if (list == nullptr) return 0;
  const char* p = list;
  while (*p != '\0') {
    while (*p == ',' || *p == ';' || *p == ' ') ++p;
    const char* const start = p;
    while (*p != '\0' && *p != ',' && *p != ';' && *p != ' ') ++p;
    std::size_t len = static_cast<std::size_t>(p - start);
    if (len == 0) continue;
    if (len > 4 && EqualsNoCase(start + len - 4, 4, ".dll")) len -= 4;
    std::uint32_t bit = 0;
    if (EqualsNoCase(start, len, "pnsrad")) bit = kPnsrad;
    else if (EqualsNoCase(start, len, "pnsovr")) bit = kPnsovr;
    else if (EqualsNoCase(start, len, "pnsdemo")) bit = kPnsdemo;
    else if (EqualsNoCase(start, len, "all")) bit = kAllModules;
    if (bit == 0 && unknown != nullptr) *unknown = true;
    mask |= bit;
  }
  return mask;
}

/// The module bit for a loaded file's path, by its file name: "...\\pnsrad.dll" -> kPnsrad; 0 for any
/// other file (pnsradmatchmaking.dll and pnsradgameserver.dll are not the platform interface).
inline std::uint32_t ModuleOfPath(const char* path) {
  if (path == nullptr) return 0;
  const char* name = path;
  for (const char* p = path; *p != '\0'; ++p) {
    if (*p == '\\' || *p == '/') name = p + 1;
  }
  const std::size_t len = std::strlen(name);
  if (EqualsNoCase(name, len, "pnsrad.dll")) return kPnsrad;
  if (EqualsNoCase(name, len, "pnsovr.dll")) return kPnsovr;
  if (EqualsNoCase(name, len, "pnsdemo.dll")) return kPnsdemo;
  return 0;
}

/// The value after `flag` in a Windows command line ("echovr.exe -windowed -traceexports pnsrad,pnsovr"):
/// tokens split on spaces and tabs outside double quotes, the flag matched case-insensitively. Writes the
/// value (ASCII only) into `out` and returns true; false when the flag is absent, has no value, or the
/// value does not fit or is not ASCII. The tracer reads the process's own command line with this instead of a
/// global the game's argument parser fills later, so no ordering between the two can latch it off.
inline bool ExtractFlagValue(const wchar_t* commandLine, const wchar_t* flag, char* out, std::size_t capacity) {
  if (commandLine == nullptr || flag == nullptr || out == nullptr || capacity == 0) return false;
  out[0] = '\0';
  const wchar_t* p = commandLine;
  bool wantValue = false;
  for (;;) {
    while (*p == L' ' || *p == L'\t') ++p;
    if (*p == L'\0') return false;
    wchar_t token[256];
    std::size_t n = 0;
    bool quoted = false;
    while (*p != L'\0' && (quoted || (*p != L' ' && *p != L'\t'))) {
      if (*p == L'"') {
        quoted = !quoted;
      } else if (n + 1 < sizeof(token) / sizeof(token[0])) {
        token[n++] = *p;
      }
      ++p;
    }
    token[n] = L'\0';
    if (wantValue) {
      if (n == 0 || n + 1 > capacity) return false;
      for (std::size_t i = 0; i < n; ++i) {
        if (token[i] > 0x7F) return false;
        out[i] = static_cast<char>(token[i]);
      }
      out[n] = '\0';
      return true;
    }
    std::size_t flagLen = 0;
    while (flag[flagLen] != L'\0') ++flagLen;
    if (n == flagLen) {
      bool same = true;
      for (std::size_t i = 0; i < n && same; ++i) {
        const wchar_t a = token[i] >= L'A' && token[i] <= L'Z' ? static_cast<wchar_t>(token[i] - L'A' + L'a') : token[i];
        const wchar_t b = flag[i] >= L'A' && flag[i] <= L'Z' ? static_cast<wchar_t>(flag[i] - L'A' + L'a') : flag[i];
        same = a == b;
      }
      wantValue = same;
    }
  }
}

inline const char* ModuleName(std::uint32_t bit) {
  switch (bit) {
    case kPnsrad: return "pnsrad";
    case kPnsovr: return "pnsovr";
    case kPnsdemo: return "pnsdemo";
    default: return "other";
  }
}

}  // namespace ExportTracePolicy
