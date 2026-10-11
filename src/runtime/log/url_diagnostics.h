#pragma once

#include <string>
#include <string_view>

namespace nevr_log_diagnostics {

// Allocates and parses a URL for diagnostics, removing userinfo, the full query,
// and fragment. Unparseable or suspicious input is hidden entirely. Not safe to
// call from DllMain or before runtime initialization.
std::string RedactUrlForDiagnostics(std::string_view url);

// Format complete log messages from fixed labels and redacted endpoint values.
// Callers emit the result with Log(level, "%s", message.c_str()).
std::string FormatRedactedUrlDiagnostic(std::string_view prefix, std::string_view url,
                                        std::string_view suffix = {});
std::string FormatRedactedUrlPairDiagnostic(std::string_view prefix, std::string_view firstUrl,
                                            std::string_view separator, std::string_view secondUrl,
                                            std::string_view suffix = {});

}  // namespace nevr_log_diagnostics
