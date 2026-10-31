#pragma once

// The server's system clock against a trusted time: the Date header of a service's HTTPS response (RFC 9110
// section 6.6.1, the IMF-fixdate form). A clock far from service time breaks the signature check's
// timestamp window, token expiry, cross-machine log times and time-based matchmaking, so a dedicated server
// refuses to run past the refusal tolerance and warns past the drift threshold. Pure logic: no clock read, no
// socket, no log call; the caller supplies both times and writes the result.
//
// Prior art: Kerberos (MIT krb5 clockskew, default 5 minutes) and TLS certificate validity both use a fixed
// tolerance and reject with a message that names the skew.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>

namespace nevr_clock_skew {

/// Refuse a server whose clock is further than this from service time (Kerberos' default).
inline constexpr std::int64_t kRefuseToleranceSeconds = 300;
/// Warn, and keep running, when the skew is past this (the drift a clock sync corrects).
inline constexpr std::int64_t kDriftWarnSeconds = 30;

enum class Verdict { kOk, kDrift, kRefuse };

struct Check {
  Verdict verdict;
  /// Local minus service, in seconds: negative when the system clock is behind.
  std::int64_t skew_seconds;
};

namespace detail {

// Howard Hinnant's civil-date algorithms (public domain): days since 1970-01-01 and back, proleptic Gregorian.
constexpr std::int64_t DaysFromCivil(std::int64_t y, unsigned m, unsigned d) {
  y -= m <= 2 ? 1 : 0;
  const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

struct CivilDate {
  std::int64_t year;
  unsigned month;
  unsigned day;
};

constexpr CivilDate CivilFromDays(std::int64_t z) {
  z += 719468;
  const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = static_cast<unsigned>(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const std::int64_t y = static_cast<std::int64_t>(yoe) + era * 400;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  const unsigned d = doy - (153 * mp + 2) / 5 + 1;
  const unsigned m = mp < 10 ? mp + 3 : mp - 9;
  return {y + (m <= 2 ? 1 : 0), m, d};
}

// Reads `count` ASCII digits at `pos`; nullopt when any is not a digit.
inline std::optional<unsigned> Digits(std::string_view s, std::size_t pos, std::size_t count) {
  unsigned value = 0;
  for (std::size_t i = pos; i < pos + count; i++) {
    if (s[i] < '0' || s[i] > '9') return std::nullopt;
    value = value * 10 + static_cast<unsigned>(s[i] - '0');
  }
  return value;
}

inline std::int64_t FloorDiv(std::int64_t a, std::int64_t b) {
  const std::int64_t q = a / b;
  return (a % b != 0 && (a < 0) != (b < 0)) ? q - 1 : q;
}

}  // namespace detail

/// Parses an IMF-fixdate, "Sun, 06 Nov 1994 08:49:37 GMT", to Unix seconds. The weekday name is not checked:
/// it does not change the instant. Returns nullopt for anything else, including an absent header (the empty
/// string): a caller with no Date header skips the check and logs that, it never refuses on missing data.
inline std::optional<std::int64_t> ParseHttpDate(std::string_view s) {
  static constexpr std::array<std::string_view, 12> kMonths = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                                               "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  if (s.size() != 29 || s[3] != ',' || s[4] != ' ' || s[7] != ' ' || s[11] != ' ' || s[16] != ' ' ||
      s[19] != ':' || s[22] != ':' || s[25] != ' ' || s.substr(26) != "GMT") {
    return std::nullopt;
  }
  const auto day = detail::Digits(s, 5, 2);
  const auto year = detail::Digits(s, 12, 4);
  const auto hour = detail::Digits(s, 17, 2);
  const auto minute = detail::Digits(s, 20, 2);
  const auto second = detail::Digits(s, 23, 2);
  if (!day || !year || !hour || !minute || !second) return std::nullopt;

  unsigned month = 0;
  for (std::size_t i = 0; i < kMonths.size(); i++) {
    if (s.substr(8, 3) == kMonths[i]) month = static_cast<unsigned>(i + 1);
  }
  if (month == 0 || *day < 1 || *day > 31 || *hour > 23 || *minute > 59 || *second > 60) return std::nullopt;

  const std::int64_t days = detail::DaysFromCivil(static_cast<std::int64_t>(*year), month, *day);
  return days * 86400 + static_cast<std::int64_t>(*hour) * 3600 + static_cast<std::int64_t>(*minute) * 60 +
         static_cast<std::int64_t>(*second);
}

/// Compares the system clock with service time, both Unix seconds.
inline Check CompareClocks(std::int64_t local, std::int64_t service) {
  const std::int64_t skew = local - service;
  const std::int64_t magnitude = skew < 0 ? -skew : skew;
  if (magnitude > kRefuseToleranceSeconds) return {Verdict::kRefuse, skew};
  if (magnitude > kDriftWarnSeconds) return {Verdict::kDrift, skew};
  return {Verdict::kOk, skew};
}

/// "2026-10-30T15:04:00Z" for a Unix time.
inline std::string FormatUtc(std::int64_t unixSeconds) {
  const std::int64_t days = detail::FloorDiv(unixSeconds, 86400);
  const std::int64_t secondOfDay = unixSeconds - days * 86400;
  const detail::CivilDate date = detail::CivilFromDays(days);
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%04lld-%02u-%02uT%02lld:%02lld:%02lldZ", static_cast<long long>(date.year),
                date.month, date.day, static_cast<long long>(secondOfDay / 3600),
                static_cast<long long>(secondOfDay % 3600 / 60), static_cast<long long>(secondOfDay % 60));
  return buf;
}

/// "system clock is 5h02m behind service time (system 2026-10-30T10:02:00Z, service 2026-10-30T15:04:00Z)".
inline std::string ClockLine(std::int64_t local, std::int64_t service) {
  const std::int64_t skew = local - service;
  const std::int64_t magnitude = skew < 0 ? -skew : skew;
  char amount[32];
  if (magnitude >= 3600) {
    std::snprintf(amount, sizeof(amount), "%lldh%02lldm", static_cast<long long>(magnitude / 3600),
                  static_cast<long long>(magnitude % 3600 / 60));
  } else if (magnitude >= 60) {
    std::snprintf(amount, sizeof(amount), "%lldm%02llds", static_cast<long long>(magnitude / 60),
                  static_cast<long long>(magnitude % 60));
  } else {
    std::snprintf(amount, sizeof(amount), "%llds", static_cast<long long>(magnitude));
  }
  return std::string("system clock is ") + amount + (skew < 0 ? " behind" : " ahead") + " service time (system " +
         FormatUtc(local) + ", service " + FormatUtc(service) + ")";
}

/// The refusal line: names both times and the fix. Printed to the console and logged by the caller.
inline std::string RefusalLine(std::int64_t local, std::int64_t service) {
  return ClockLine(local, service) + "; sync the clock (Windows: w32tm /resync) and restart";
}

}  // namespace nevr_clock_skew
