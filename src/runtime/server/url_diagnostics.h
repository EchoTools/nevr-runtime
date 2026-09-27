#pragma once

#include <string>
#include <string_view>

namespace GameServer {

// Returns a URL suitable for diagnostics with credentials and all query and
// fragment data removed. Unparseable or suspicious input is hidden entirely.
std::string RedactUrlForDiagnostics(std::string_view url);

}  // namespace GameServer
