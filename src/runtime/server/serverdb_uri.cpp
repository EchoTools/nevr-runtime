#include "runtime/server/serverdb_uri.h"

#include <curl/curl.h>

#include <initializer_list>
#include <limits>
#include <memory>

namespace ServerDbUri {
namespace {

struct QueryParam {
  std::string_view key;  // literal, RFC 3986 unreserved; appended as-is
  std::string_view value;
  bool isList;  // comma-separated: encode each element, keep the commas
};

std::optional<std::string> EncodeList(std::string_view csv) {
  std::string encoded;
  size_t start = 0;
  while (true) {
    const size_t comma = csv.find(',', start);
    const std::string_view element =
        csv.substr(start, comma == std::string_view::npos ? std::string_view::npos : comma - start);
    const std::optional<std::string> encodedElement = EncodeQueryValue(element);
    if (!encodedElement) return std::nullopt;
    encoded += *encodedElement;
    if (comma == std::string_view::npos) break;
    encoded += ',';
    start = comma + 1;
  }
  return encoded;
}

std::optional<std::string> AppendQuery(std::string_view base, std::initializer_list<QueryParam> params) {
  std::string uri(base);
  for (const QueryParam& param : params) {
    if (param.value.empty()) continue;
    const std::optional<std::string> encoded = param.isList ? EncodeList(param.value) : EncodeQueryValue(param.value);
    if (!encoded) return std::nullopt;
    if (uri.find('?') == std::string::npos) {
      uri += '?';
    } else if (uri.back() != '?' && uri.back() != '&') {
      uri += '&';
    }
    uri.append(param.key);
    uri += '=';
    uri += *encoded;
  }
  return uri;
}

}  // namespace

std::optional<std::string> EncodeQueryValue(std::string_view value) {
  // curl_easy_escape treats length 0 as "call strlen", which would read past a
  // non-terminated string_view — so the empty case never reaches it.
  if (value.empty()) return std::string();
  if (value.size() > static_cast<size_t>((std::numeric_limits<int>::max)())) return std::nullopt;
  // A null handle is accepted since libcurl 7.82.0 (vcpkg ships 8.18.0).
  std::unique_ptr<char, decltype(&curl_free)> escaped(
      curl_easy_escape(nullptr, value.data(), static_cast<int>(value.size())), &curl_free);
  if (!escaped) return std::nullopt;
  return std::string(escaped.get());
}

std::optional<std::string> BuildLegacyUri(std::string_view socketUri, std::string_view discordId,
                                          std::string_view password, std::string_view guilds,
                                          std::string_view regions) {
  return AppendQuery(socketUri, {{"discord_id", discordId, false},
                                 {"password", password, false},
                                 {"guilds", guilds, true},
                                 {"regions", regions, true}});
}

std::optional<std::string> BuildTokenRouteUri(std::string_view tokenUri, std::string_view guilds,
                                              std::string_view regions) {
  return AppendQuery(tokenUri, {{"guilds", guilds, true}, {"regions", regions, true}});
}

std::optional<std::string> BuildBridgeCredentialUri(std::string_view remoteUri, std::string_view discordId,
                                                    std::string_view password) {
  if (discordId.empty() || password.empty()) return std::string(remoteUri);
  return AppendQuery(remoteUri, {{"discordid", discordId, false}, {"password", password, false}});
}

}  // namespace ServerDbUri
