#pragma once

#include <string>
#include <string_view>

namespace nevr_log_diagnostics {

// The URL as a log shows it: whole, userinfo, query and fragment included (#2244). Nothing is masked. The one change is
// that control characters are written as %XX, so one URL is one log line.
std::string UrlForDiagnostics(std::string_view url);

// Format complete log messages from fixed labels and endpoint values.
// Callers emit the result with Log(level, "%s", message.c_str()).
std::string FormatUrlDiagnostic(std::string_view prefix, std::string_view url, std::string_view suffix = {});
std::string FormatUrlPairDiagnostic(std::string_view prefix, std::string_view firstUrl,
                                    std::string_view separator, std::string_view secondUrl,
                                    std::string_view suffix = {});

}  // namespace nevr_log_diagnostics
