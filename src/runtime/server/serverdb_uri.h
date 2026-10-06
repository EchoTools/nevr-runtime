#pragma once

// Builds the ServerDB WebSocket URI that GameServerLib::RequestRegistration
// connects to. Every query value is percent-encoded (issue #41): the legacy
// url-param auth path carries the operator's password in the query, and a value
// containing '&', '=', '#', '%', '+' or whitespace must not rewrite the query.
//
// Pure functions: no logging, no global state. The caller logs (and must never
// log the returned legacy URI — it contains the password).

#include <optional>
#include <string>
#include <string_view>

namespace ServerDbUri {

// RFC 3986 percent-encoding via libcurl's curl_easy_escape: every byte except
// ALPHA / DIGIT / '-' / '.' / '_' / '~' becomes %XX (uppercase hex). Space is
// %20, '+' is %2B, so Go's url.ParseQuery decodes the original bytes exactly.
// nullopt only if libcurl cannot allocate or the value exceeds INT_MAX bytes.
std::optional<std::string> EncodeQueryValue(std::string_view value);

// Legacy url-param auth: base?discord_id=..&password=..&guilds=..&regions=..
// guilds/regions are comma-separated lists; each element is encoded and the
// commas stay literal, because the server splits the decoded value on ','.
// Empty password/guilds/regions are omitted. If base already has a query, the
// parameters are appended with '&'.
std::optional<std::string> BuildLegacyUri(std::string_view socketUri, std::string_view discordId,
                                          std::string_view password, std::string_view guilds,
                                          std::string_view regions);

// Token route (nevr_serverdb_uri): base?guilds=..&regions=.., same encoding rules.
std::optional<std::string> BuildTokenRouteUri(std::string_view tokenUri, std::string_view guilds,
                                              std::string_view regions);

}  // namespace ServerDbUri
