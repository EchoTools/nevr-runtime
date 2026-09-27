#include "runtime/server/url_diagnostics.h"

#include <gtest/gtest.h>

#include <string>

namespace {
constexpr char kMalformed[] = "[redacted malformed URL]";
}

TEST(UrlDiagnostics, RemovesCredentialsRepeatedQueryAndFragmentButKeepsEndpoint) {
  const std::string input =
      "wss://alice%40example:pa%3Ass@db.example:7443/serverdb?http_key=first&HTTP_KEY=second&"
      "%68%74%74%70%5F%6B%65%79=third&x=1&x=2#frag-secret";
  const std::string redacted = GameServer::RedactUrlForDiagnostics(input);
  EXPECT_EQ(redacted, "wss://db.example:7443/serverdb");
  EXPECT_EQ(redacted.find("alice"), std::string::npos);
  EXPECT_EQ(redacted.find("pa%3Ass"), std::string::npos);
  EXPECT_EQ(redacted.find("http_key"), std::string::npos);
  EXPECT_EQ(redacted.find("HTTP_KEY"), std::string::npos);
  EXPECT_EQ(redacted.find("frag-secret"), std::string::npos);
  EXPECT_EQ(redacted.find('?'), std::string::npos);
  EXPECT_EQ(redacted.find('#'), std::string::npos);
}

TEST(UrlDiagnostics, RemovesExistingQueryAndFragmentEvenWhenEmpty) {
  const std::string redacted = GameServer::RedactUrlForDiagnostics("https://service.example/path?existing=1#section");
  EXPECT_EQ(redacted, "https://service.example/path");
  EXPECT_EQ(GameServer::RedactUrlForDiagnostics("https://service.example/path?#"),
            "https://service.example/path");
}

TEST(UrlDiagnostics, SafeEndpointWithoutSecretsIsRetained) {
  EXPECT_EQ(GameServer::RedactUrlForDiagnostics("https://service.example:8443/v2/match"),
            "https://service.example:8443/v2/match");
}

TEST(UrlDiagnostics, MalformedEscapesUrlsControlsAndUnsupportedSchemesFailClosed) {
  EXPECT_EQ(GameServer::RedactUrlForDiagnostics("wss://host.example/path?key=%Q1"), kMalformed);
  EXPECT_EQ(GameServer::RedactUrlForDiagnostics("https://host/%"), kMalformed);
  EXPECT_EQ(GameServer::RedactUrlForDiagnostics("wss://user:pass@"), kMalformed);
  EXPECT_EQ(GameServer::RedactUrlForDiagnostics(std::string("https://host/\0secret", 20)), kMalformed);
  EXPECT_EQ(GameServer::RedactUrlForDiagnostics("https://host/\nsecret"), kMalformed);
  EXPECT_EQ(GameServer::RedactUrlForDiagnostics("https://host/\x7fsecret"), kMalformed);
  EXPECT_EQ(GameServer::RedactUrlForDiagnostics("file://host/path"), kMalformed);
}

TEST(UrlDiagnostics, OversizedInputFailsClosed) {
  const std::string oversized = "https://host.example/" + std::string(8192, 'x');
  EXPECT_EQ(GameServer::RedactUrlForDiagnostics(oversized), kMalformed);
}
