#include "runtime/server/url_diagnostics.h"

#include <gtest/gtest.h>

#include <string>

TEST(UrlDiagnostics, RemovesCredentialsQueryAndFragmentButKeepsEndpointPath) {
  const std::string input = "wss://alice%40example:pa%3Ass@db.example:7443/serverdb?http_key=a%26b&x=1#frag-secret";
  const std::string redacted = GameServer::RedactUrlForDiagnostics(input);
  EXPECT_NE(redacted, input);
  EXPECT_NE(redacted.find("wss://"), std::string::npos) << redacted;
  EXPECT_NE(redacted.find("db.example:7443/serverdb"), std::string::npos) << redacted;
  EXPECT_EQ(redacted.find("alice"), std::string::npos);
  EXPECT_EQ(redacted.find("pa%3Ass"), std::string::npos);
  EXPECT_EQ(redacted.find("http_key"), std::string::npos);
  EXPECT_EQ(redacted.find("a%26b"), std::string::npos);
  EXPECT_EQ(redacted.find("frag-secret"), std::string::npos);
  EXPECT_EQ(redacted.find('?'), std::string::npos);
  EXPECT_EQ(redacted.find('#'), std::string::npos);
}

TEST(UrlDiagnostics, RemovesExistingQueryAndFragmentEvenWhenEmpty) {
  const std::string redacted = GameServer::RedactUrlForDiagnostics("https://service.example/path?existing=1#section");
  EXPECT_EQ(redacted, "https://service.example/path");
}

TEST(UrlDiagnostics, InvalidEscapesMalformedUrlsAndEmbeddedControlsFailClosed) {
  EXPECT_EQ(GameServer::RedactUrlForDiagnostics("wss://host.example/path?key=%Q1"),
            "[redacted malformed URL]");
  EXPECT_EQ(GameServer::RedactUrlForDiagnostics("wss://user:pass@"), "[redacted malformed URL]");
  EXPECT_EQ(GameServer::RedactUrlForDiagnostics(std::string("https://host/\0secret", 20)),
            "[redacted malformed URL]");
  EXPECT_EQ(GameServer::RedactUrlForDiagnostics("https://host/\nsecret"), "[redacted malformed URL]");
}

TEST(UrlDiagnostics, OversizedInputFailsClosed) {
  const std::string oversized = "https://host.example/" + std::string(8192, 'x');
  EXPECT_EQ(GameServer::RedactUrlForDiagnostics(oversized), "[redacted malformed URL]");
}
