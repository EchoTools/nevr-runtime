// Issue #41: the ServerDB URI used to be built with snprintf, so a password (or any
// config value) containing '&', '=', '#', '%', '+', or whitespace rewrote the query.
// These tests pin the percent-encoded output and parse it back the way the server
// does (split on '&', split on the first '=', percent-decode the value).

#include "runtime/server/serverdb_uri.h"

#include <curl/curl.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace {

using QueryMap = std::multimap<std::string, std::string>;

std::string PercentDecode(const std::string& encoded) {
  int decodedLength = 0;
  std::unique_ptr<char, decltype(&curl_free)> decoded(
      curl_easy_unescape(nullptr, encoded.c_str(), static_cast<int>(encoded.size()), &decodedLength), &curl_free);
  if (!decoded) {
    ADD_FAILURE() << "curl_easy_unescape failed for " << encoded;
    return {};
  }
  return std::string(decoded.get(), static_cast<size_t>(decodedLength));
}

// Parses the URI with libcurl's URL parser (not the builder), asserts there is no
// fragment, and decodes every query pair.
QueryMap ParseQuery(const std::string& uri) {
  QueryMap pairs;
  std::unique_ptr<CURLU, decltype(&curl_url_cleanup)> parsed(curl_url(), &curl_url_cleanup);
  if (!parsed || curl_url_set(parsed.get(), CURLUPART_URL, uri.c_str(), CURLU_NON_SUPPORT_SCHEME) != CURLUE_OK) {
    ADD_FAILURE() << "URI did not parse: " << uri;
    return pairs;
  }
  char* fragment = nullptr;
  EXPECT_EQ(curl_url_get(parsed.get(), CURLUPART_FRAGMENT, &fragment, 0), CURLUE_NO_FRAGMENT) << uri;
  curl_free(fragment);

  char* rawQuery = nullptr;
  if (curl_url_get(parsed.get(), CURLUPART_QUERY, &rawQuery, 0) != CURLUE_OK || rawQuery == nullptr) {
    ADD_FAILURE() << "URI has no query: " << uri;
    return pairs;
  }
  const std::string query(rawQuery);
  curl_free(rawQuery);

  size_t start = 0;
  while (start <= query.size()) {
    const size_t end = std::min(query.find('&', start), query.size());
    const std::string pair = query.substr(start, end - start);
    const size_t equals = pair.find('=');
    if (equals == std::string::npos) {
      pairs.emplace(pair, "");
    } else {
      pairs.emplace(pair.substr(0, equals), PercentDecode(pair.substr(equals + 1)));
    }
    start = end + 1;
  }
  return pairs;
}

constexpr std::string_view kHostileValue = "p&ss=w#rd%25+ ?/@&guilds=999";

}  // namespace

TEST(ServerDbUri, EncodeQueryValueEncodesEverythingButUnreserved) {
  EXPECT_EQ(ServerDbUri::EncodeQueryValue("AZaz09-._~"), "AZaz09-._~");
  EXPECT_EQ(ServerDbUri::EncodeQueryValue("&=#%+ ?/@,"), "%26%3D%23%25%2B%20%3F%2F%40%2C");
  EXPECT_EQ(ServerDbUri::EncodeQueryValue("\xC3\xA9"), "%C3%A9");
  EXPECT_EQ(ServerDbUri::EncodeQueryValue(""), "");
}

TEST(ServerDbUri, EncodeQueryValueHonoursStringViewLengthNotTerminator) {
  const std::string backing = "ab&cd";
  EXPECT_EQ(ServerDbUri::EncodeQueryValue(std::string_view(backing).substr(0, 3)), "ab%26");
}

TEST(ServerDbUri, LegacyUriPercentEncodesPasswordExactly) {
  const std::optional<std::string> uri = ServerDbUri::BuildLegacyUri(
      "ws://db.example:777/serverdb", "123456789", kHostileValue, "111,222", "us-east");
  ASSERT_TRUE(uri.has_value());
  EXPECT_EQ(*uri,
            "ws://db.example:777/serverdb?discord_id=123456789"
            "&password=p%26ss%3Dw%23rd%2525%2B%20%3F%2F%40%26guilds%3D999"
            "&guilds=111,222&regions=us-east");
}

TEST(ServerDbUri, LegacyUriRoundTripsHostileValuesWithoutFieldBleed) {
  const std::optional<std::string> uri = ServerDbUri::BuildLegacyUri(
      "wss://db.example/serverdb", "123456789", kHostileValue, "111,222", "us-east,eu_west");
  ASSERT_TRUE(uri.has_value());
  const QueryMap pairs = ParseQuery(*uri);
  const QueryMap expected = {
      {"discord_id", "123456789"},
      {"password", std::string(kHostileValue)},
      {"guilds", "111,222"},
      {"regions", "us-east,eu_west"},
  };
  EXPECT_EQ(pairs, expected) << *uri;
}

// No whitespace, so a raw-concatenated URI would still parse — this is the case
// where an unencoded password injects a second guilds= and truncates at '#'.
TEST(ServerDbUri, PasswordCannotInjectAParameterOrAFragment) {
  const std::optional<std::string> uri =
      ServerDbUri::BuildLegacyUri("ws://h/s", "1", "a&guilds=999#tail", "111", "");
  ASSERT_TRUE(uri.has_value());
  const QueryMap pairs = ParseQuery(*uri);
  EXPECT_EQ(pairs.count("guilds"), 1u) << *uri;
  const QueryMap expected = {{"discord_id", "1"}, {"password", "a&guilds=999#tail"}, {"guilds", "111"}};
  EXPECT_EQ(pairs, expected) << *uri;
}

TEST(ServerDbUri, HostileListElementsAreEncodedButCommasStayDelimiters) {
  const std::optional<std::string> uri =
      ServerDbUri::BuildLegacyUri("ws://h/s", "1", "pw", "111,2&x=y", "a#b,c d");
  ASSERT_TRUE(uri.has_value());
  EXPECT_EQ(*uri, "ws://h/s?discord_id=1&password=pw&guilds=111,2%26x%3Dy&regions=a%23b,c%20d");
  const QueryMap pairs = ParseQuery(*uri);
  const QueryMap expected = {
      {"discord_id", "1"}, {"password", "pw"}, {"guilds", "111,2&x=y"}, {"regions", "a#b,c d"}};
  EXPECT_EQ(pairs, expected);
}

// Every value the server accepts (session_ws.go discordIDPattern ^[0-9]+$,
// regionPattern ^[-A-Za-z0-9_]+$, guildPattern ^([0-9]+|any)$) is RFC 3986
// unreserved, so the wire bytes for an already-working config do not change.
TEST(ServerDbUri, ServerAcceptedValuesProduceTheSameBytesAsBefore) {
  const std::optional<std::string> uri = ServerDbUri::BuildLegacyUri(
      "ws://db.example:777/serverdb", "123456789", "hunter2", "111,any,222", "us-east,eu_west");
  ASSERT_TRUE(uri.has_value());
  EXPECT_EQ(*uri,
            "ws://db.example:777/serverdb?discord_id=123456789&password=hunter2"
            "&guilds=111,any,222&regions=us-east,eu_west");
}

TEST(ServerDbUri, EmptyOptionalFieldsAreOmitted) {
  EXPECT_EQ(ServerDbUri::BuildLegacyUri("ws://h/s", "1", "", "", ""), "ws://h/s?discord_id=1");
  EXPECT_EQ(ServerDbUri::BuildLegacyUri("ws://h/s", "1", "", "", "r"), "ws://h/s?discord_id=1&regions=r");
  EXPECT_EQ(ServerDbUri::BuildTokenRouteUri("wss://h/nevr", "", ""), "wss://h/nevr");
}

TEST(ServerDbUri, BaseWithExistingQueryGetsAmpersandSeparator) {
  EXPECT_EQ(ServerDbUri::BuildLegacyUri("ws://h/s?format=evr", "1", "a b", "", ""),
            "ws://h/s?format=evr&discord_id=1&password=a%20b");
  EXPECT_EQ(ServerDbUri::BuildTokenRouteUri("ws://h/s?", "", "r"), "ws://h/s?regions=r");
  EXPECT_EQ(ServerDbUri::BuildTokenRouteUri("ws://h/s?x=1&", "g", ""), "ws://h/s?x=1&guilds=g");
}

TEST(ServerDbUri, TokenRouteUriEncodesListsAndRoundTrips) {
  const std::optional<std::string> uri = ServerDbUri::BuildTokenRouteUri("wss://h/nevr", "111,222", "a&b");
  ASSERT_TRUE(uri.has_value());
  EXPECT_EQ(*uri, "wss://h/nevr?guilds=111,222&regions=a%26b");
  const QueryMap pairs = ParseQuery(*uri);
  const QueryMap expected = {{"guilds", "111,222"}, {"regions", "a&b"}};
  EXPECT_EQ(pairs, expected);
}

// The old builder truncated at 1024 bytes; the new one has no fixed buffer.
TEST(ServerDbUri, LongValuesAreNotTruncated) {
  const std::string longRegions(2000, 'r');
  const std::optional<std::string> uri = ServerDbUri::BuildLegacyUri("ws://h/s", "1", "pw", "", longRegions);
  ASSERT_TRUE(uri.has_value());
  EXPECT_EQ(*uri, "ws://h/s?discord_id=1&password=pw&regions=" + longRegions);
}

// --- compat/ws_bridge.cpp URL credentials (config + login connections) --------
// Same Nakama handler (session_ws.go reads "discordid" then "discord_id", and
// "password"), so the same encoding contract applies.

TEST(ServerDbUri, BridgeCredentialsPercentEncodePasswordExactly) {
  const std::optional<std::string> uri =
      ServerDbUri::BuildBridgeCredentialUri("wss://g.example/ws?format=evr", "123456789", kHostileValue);
  ASSERT_TRUE(uri.has_value());
  EXPECT_EQ(*uri,
            "wss://g.example/ws?format=evr&discordid=123456789"
            "&password=p%26ss%3Dw%23rd%2525%2B%20%3F%2F%40%26guilds%3D999");
}

TEST(ServerDbUri, BridgeCredentialsRoundTripHostileValuesWithoutFieldBleed) {
  const std::optional<std::string> uri =
      ServerDbUri::BuildBridgeCredentialUri("wss://g.example/ws?format=evr", "123456789", kHostileValue);
  ASSERT_TRUE(uri.has_value());
  const QueryMap expected = {
      {"format", "evr"}, {"discordid", "123456789"}, {"password", std::string(kHostileValue)}};
  EXPECT_EQ(ParseQuery(*uri), expected) << *uri;
}

// The binary must not disagree with itself: both connection paths deliver the
// same decoded password for the same config value.
TEST(ServerDbUri, BridgeAndServerDbPathsSendTheSamePasswordBytes) {
  const std::string password = "a+b%41;c&d#e";
  const std::optional<std::string> bridge = ServerDbUri::BuildBridgeCredentialUri("ws://h/ws", "1", password);
  const std::optional<std::string> serverDb = ServerDbUri::BuildLegacyUri("ws://h/ws", "1", password, "", "");
  ASSERT_TRUE(bridge.has_value());
  ASSERT_TRUE(serverDb.has_value());
  const QueryMap bridgePairs = ParseQuery(*bridge);
  const QueryMap serverDbPairs = ParseQuery(*serverDb);
  ASSERT_EQ(bridgePairs.count("password"), 1u) << *bridge;
  ASSERT_EQ(serverDbPairs.count("password"), 1u) << *serverDb;
  EXPECT_EQ(bridgePairs.find("password")->second, password);
  EXPECT_EQ(serverDbPairs.find("password")->second, password);
}

TEST(ServerDbUri, BridgeCredentialsAreBothOrNeither) {
  EXPECT_EQ(ServerDbUri::BuildBridgeCredentialUri("ws://h/ws?format=evr", "1", ""), "ws://h/ws?format=evr");
  EXPECT_EQ(ServerDbUri::BuildBridgeCredentialUri("ws://h/ws", "", "pw"), "ws://h/ws");
  EXPECT_EQ(ServerDbUri::BuildBridgeCredentialUri("ws://h/ws", "1", "pw"), "ws://h/ws?discordid=1&password=pw");
}

// --- RemoveQueryParam (issue #116) --------------------------------------
// ws_bridge.cpp's matchmaker path (connIdx >= 2) strips "format=evr" from the
// bridge-credential URI. The previous inline version deleted the character
// before the match unconditionally, which deleted the URI's own '?' whenever
// format=evr was the first query param, concatenating path and query with no
// separator: "wss://g.example/ws?format=evr&discordid=1&password=pw" became
// "wss://g.example/wsdiscordid=1&password=pw".

// The exact shape BuildBridgeCredentialUri produces today for the matchmaker
// path: format=evr first, discordid/password appended after by AppendQuery.
// This is the shape on local dev rigs, where the configured socket_uri
// already carries "?format=evr&token=..." before credentials are appended
// (tools/scenario/run_scenario.py:353, tools/winvm/systest.py:72,
// docs/reference/local-nakama.md:55, tools/nakama-local/evr_peer.py:104).
// This is the trigger case from the issue, reproduced via the real builder
// rather than a hand-typed string.
TEST(ServerDbUri, RemoveQueryParamFixesTheActualBridgeCredentialShape) {
  const std::optional<std::string> withCredentials =
      ServerDbUri::BuildBridgeCredentialUri("wss://g.example/ws?format=evr", "123456789", "pw");
  ASSERT_TRUE(withCredentials.has_value());
  ASSERT_EQ(*withCredentials, "wss://g.example/ws?format=evr&discordid=123456789&password=pw");

  const std::string stripped = ServerDbUri::RemoveQueryParam(*withCredentials, "format=evr");
  EXPECT_EQ(stripped, "wss://g.example/ws?discordid=123456789&password=pw");
  // Must still parse as a URI with a '?' separating path from query — the bug
  // produced "wss://g.example/wsdiscordid=...", which is not.
  EXPECT_NE(stripped.find('?'), std::string::npos) << stripped;
  const QueryMap pairs = ParseQuery(stripped);
  const QueryMap expected = {{"discordid", "123456789"}, {"password", "pw"}};
  EXPECT_EQ(pairs, expected);
}

// The example config's shape (docs/reference/example-config.yaml:175), where
// the commented-out socket_uri has no format=evr param at all — the case
// that stayed latent because the bug path is simply absent there, not
// because of any favorable parameter ordering.
TEST(ServerDbUri, RemoveQueryParamTrailing) {
  EXPECT_EQ(ServerDbUri::RemoveQueryParam("wss://g.example/ws?discordid=1&password=pw&format=evr", "format=evr"),
            "wss://g.example/ws?discordid=1&password=pw");
}

TEST(ServerDbUri, RemoveQueryParamMiddle) {
  EXPECT_EQ(ServerDbUri::RemoveQueryParam("wss://g.example/ws?discordid=1&format=evr&password=pw", "format=evr"),
            "wss://g.example/ws?discordid=1&password=pw");
}

TEST(ServerDbUri, RemoveQueryParamSoleParam) {
  EXPECT_EQ(ServerDbUri::RemoveQueryParam("wss://g.example/ws?format=evr", "format=evr"), "wss://g.example/ws");
}

TEST(ServerDbUri, RemoveQueryParamAbsentIsNoOp) {
  EXPECT_EQ(ServerDbUri::RemoveQueryParam("wss://g.example/ws?discordid=1&password=pw", "format=evr"),
            "wss://g.example/ws?discordid=1&password=pw");
  EXPECT_EQ(ServerDbUri::RemoveQueryParam("wss://g.example/ws", "format=evr"), "wss://g.example/ws");
}

// A substring hit inside another key/value is not a boundary match — leave it
// alone rather than mangling an unrelated parameter.
TEST(ServerDbUri, RemoveQueryParamDoesNotMatchSubstring) {
  EXPECT_EQ(ServerDbUri::RemoveQueryParam("wss://g.example/ws?xformat=evrx=1", "format=evr"),
            "wss://g.example/ws?xformat=evrx=1");
}
