/* SYNTHESIS -- custom tool code, not from binary */
#pragma once

#include <cstddef>
#include <cstring>

/* N78 — game lines that are one event whatever their hex value.
 *
 * The rate limiter folds digit runs, so "call #41" and "call #42" are one line. A value printed with `%x` also holds the
 * letters a-f, and two values that differ in a letter are two lines to that fold. A script writes
 * "realdiv(%x) divide by zero" (the string is in the game's script DLLs) every time it divides by zero, with a different value at
 * each site and call, up to about 120 lines a second from the STARBURST script; none of them collapsed.
 *
 * A rule here names a format: `head`, 1 to 16 hex digits, then `tail`. The limiter gives every line of one rule one identity, so
 * the first kThreshold of a window pass and the rest are counted in one `[NEVR.LOGFILTER] repeated` line. The rule is the
 * format and never "any hex": a line that differs only in hex but is not in this table keeps its own identity.
 *
 * Platform-free, no game address. The Quest sentinel does not capture the game's log lines (no CLog hook under src/quest), so the
 * only user is the PC filter's limiter.
 */
namespace nevr_collapse_format {

struct Rule {
  const char* name; /* the identity every line of the rule shares */
  const char* head; /* text before the value */
  const char* tail; /* text after the value */
};

inline constexpr Rule kRules[] = {
    {"script_realdiv", "realdiv(", ") divide by zero"},
};

constexpr std::size_t kMaxHexDigits = 16;

/* The name of the rule `msg` (len bytes, not necessarily NUL-terminated) is a line of, or nullptr. The format may sit anywhere in
 * the line: the filter writes "[EVR] " in front of a game line and may leave text after it. */
inline const char* Match(const char* msg, int len) {
  if (msg == nullptr || len <= 0) return nullptr;
  const std::size_t n = static_cast<std::size_t>(len);
  for (const Rule& rule : kRules) {
    const std::size_t head = std::strlen(rule.head);
    const std::size_t tail = std::strlen(rule.tail);
    for (std::size_t i = 0; i + head <= n; i++) {
      if (std::memcmp(msg + i, rule.head, head) != 0) continue;
      std::size_t j = i + head;
      std::size_t digits = 0;
      while (j < n && digits <= kMaxHexDigits) {
        const char c = msg[j];
        const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        if (!hex) break;
        j++;
        digits++;
      }
      if (digits == 0 || digits > kMaxHexDigits) continue;
      if (j + tail <= n && std::memcmp(msg + j, rule.tail, tail) == 0) return rule.name;
    }
  }
  return nullptr;
}

}  // namespace nevr_collapse_format
