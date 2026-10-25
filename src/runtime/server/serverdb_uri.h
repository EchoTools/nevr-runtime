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

namespace nevr_serverdb_uri {

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

// The WebSocket bridge's URL credentials (compat/ws_bridge.cpp, config and login
// connections): remoteUri?discordid=..&password=.. against the same Nakama
// handler (session_ws.go reads "discordid", then "discord_id", and "password").
// Both credentials or neither: if either is empty, remoteUri is returned
// unchanged — an empty secret never goes on the wire (N115). The key stays
// "discordid" because ws_bridge.cpp detects URL credentials by that literal.
std::optional<std::string> BuildBridgeCredentialUri(std::string_view remoteUri, std::string_view discordId,
                                                    std::string_view password);

// Removes one literal "key=value" query parameter from `uri`, wherever it sits
// in the query string, and reinserts a correct separator (issue #116). Always
// treating the character before a match as removable would delete the URI's own '?' whenever the parameter
// was first in the query (e.g. "base?format=evr&discordid=1" ->
// "basediscordid=1", concatenating path and query with no separator).
// Handles, and only acts on, an exact boundary match (preceded by '?', '&', or
// start-of-string; followed by '&' or end-of-string) — a substring hit inside
// another key or value (e.g. "xformat=evr=1") is left alone:
//   - sole param:      "base?format=evr"                 -> "base"
//   - leading, more follow: "base?format=evr&a=1"        -> "base?a=1"
//   - trailing:        "base?a=1&format=evr"              -> "base?a=1"
//   - middle:          "base?a=1&format=evr&b=2"          -> "base?a=1&b=2"
//   - absent:          "base?a=1"                         -> "base?a=1" (unchanged)
// Removes only the first match; these query strings never repeat a key.
std::string RemoveQueryParam(std::string_view uri, std::string_view param);

}  // namespace nevr_serverdb_uri
