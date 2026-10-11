#include "runtime/log/url_diagnostics.h"

#include <gtest/gtest.h>

#include <string>

namespace {
constexpr char kMalformed[] = "[redacted malformed URL]";
}

TEST(UrlDiagnostics, RemovesCredentialsRepeatedQueryAndFragmentButKeepsEndpoint) {
  const std::string input =
      "wss://alice%40example:pa%3Ass@db.example:7443/serverdb?http_key=first&HTTP_KEY=second&"
      "%68%74%74%70%5F%6B%65%79=third&x=1&x=2#frag-secret";
  const std::string redacted = nevr_log_diagnostics::RedactUrlForDiagnostics(input);
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
  const std::string redacted = nevr_log_diagnostics::RedactUrlForDiagnostics("https://service.example/path?existing=1#section");
  EXPECT_EQ(redacted, "https://service.example/path");
  EXPECT_EQ(nevr_log_diagnostics::RedactUrlForDiagnostics("https://service.example/path?#"),
            "https://service.example/path");
}

TEST(UrlDiagnostics, SafeEndpointWithoutSecretsIsRetained) {
  EXPECT_EQ(nevr_log_diagnostics::RedactUrlForDiagnostics("https://service.example:8443/v2/match"),
            "https://service.example:8443/v2/match");
}

TEST(UrlDiagnostics, FormattedDiagnosticRedactsSecretsAndKeepsUsefulEndpoint) {
  const std::string message = nevr_log_diagnostics::FormatRedactedUrlDiagnostic(
      "[NEVR.SERVERDB] Connecting to ServerDB at ",
      "wss://user-sentinel:password-sentinel@db.example:7443/serverdb?http_key=query-sentinel&x=2#fragment-sentinel");
  EXPECT_EQ(message, "[NEVR.SERVERDB] Connecting to ServerDB at wss://db.example:7443/serverdb");
  EXPECT_EQ(message.find("user-sentinel"), std::string::npos);
  EXPECT_EQ(message.find("password-sentinel"), std::string::npos);
  EXPECT_EQ(message.find("query-sentinel"), std::string::npos);
  EXPECT_EQ(message.find("fragment-sentinel"), std::string::npos);
}

TEST(UrlDiagnostics, FormattedRedirectDiagnosticRedactsBothUrlsAndKeepsKey) {
  const std::string message = nevr_log_diagnostics::FormatRedactedUrlPairDiagnostic(
      "[NEVR.PATCH] service redirect key=auth from=",
      "https://user:pass@source.example:8443/auth?http_key=source-secret#source-fragment",
      " to=",
      "https://relay.example:9443/auth?ticket=relay-secret#relay-fragment");
  EXPECT_EQ(message,
            "[NEVR.PATCH] service redirect key=auth from=https://source.example:8443/auth "
            "to=https://relay.example:9443/auth");
  EXPECT_EQ(message.find("source-secret"), std::string::npos);
  EXPECT_EQ(message.find("relay-secret"), std::string::npos);
  EXPECT_EQ(message.find("source-fragment"), std::string::npos);
  EXPECT_EQ(message.find("relay-fragment"), std::string::npos);
  EXPECT_NE(message.find("key=auth"), std::string::npos);
  EXPECT_NE(message.find("source.example:8443/auth"), std::string::npos);
  EXPECT_NE(message.find("relay.example:9443/auth"), std::string::npos);
}

TEST(UrlDiagnostics, MalformedEscapesUrlsControlsAndUnsupportedSchemesFailClosed) {
  EXPECT_EQ(nevr_log_diagnostics::RedactUrlForDiagnostics("wss://host.example/path?key=%Q1"), kMalformed);
  EXPECT_EQ(nevr_log_diagnostics::RedactUrlForDiagnostics("https://host/%"), kMalformed);
  EXPECT_EQ(nevr_log_diagnostics::RedactUrlForDiagnostics("wss://user:pass@"), kMalformed);
  EXPECT_EQ(nevr_log_diagnostics::RedactUrlForDiagnostics(std::string("https://host/\0secret", 20)), kMalformed);
  EXPECT_EQ(nevr_log_diagnostics::RedactUrlForDiagnostics("https://host/\nsecret"), kMalformed);
  EXPECT_EQ(nevr_log_diagnostics::RedactUrlForDiagnostics("https://host/\x7fsecret"), kMalformed);
  EXPECT_EQ(nevr_log_diagnostics::RedactUrlForDiagnostics("file://host/path"), kMalformed);
}

TEST(UrlDiagnostics, OversizedInputFailsClosed) {
  const std::string oversized = "https://host.example/" + std::string(8192, 'x');
  EXPECT_EQ(nevr_log_diagnostics::RedactUrlForDiagnostics(oversized), kMalformed);
}
