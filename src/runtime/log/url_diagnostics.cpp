#include "runtime/log/url_diagnostics.h"

#include <cstdio>

namespace nevr_log_diagnostics {
namespace {
// Text with every control character written as %XX, so a URL (which can be any bytes the config or the network gave)
// stays on one log line.
std::string OneLine(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (const char c : text) {
    const unsigned char value = static_cast<unsigned char>(c);
    if (value < 0x20 || value == 0x7f) {
      char escaped[4];
      std::snprintf(escaped, sizeof escaped, "%%%02X", value);
      out.append(escaped);
    } else {
      out.push_back(c);
    }
  }
  return out;
}
}  // namespace

std::string UrlForDiagnostics(std::string_view input) { return OneLine(input); }

std::string FormatUrlDiagnostic(std::string_view prefix, std::string_view url, std::string_view suffix) {
  std::string message;
  message.reserve(prefix.size() + url.size() + suffix.size());
  message.append(prefix);
  message.append(UrlForDiagnostics(url));
  message.append(suffix);
  return message;
}

std::string FormatUrlPairDiagnostic(std::string_view prefix, std::string_view firstUrl,
                                    std::string_view separator, std::string_view secondUrl,
                                    std::string_view suffix) {
  std::string message;
  message.reserve(prefix.size() + firstUrl.size() + separator.size() + secondUrl.size() + suffix.size());
  message.append(prefix);
  message.append(UrlForDiagnostics(firstUrl));
  message.append(separator);
  message.append(UrlForDiagnostics(secondUrl));
  message.append(suffix);
  return message;
}

}  // namespace nevr_log_diagnostics
