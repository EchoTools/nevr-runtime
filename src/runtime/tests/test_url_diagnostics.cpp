#include "runtime/log/url_diagnostics.h"

#include <gtest/gtest.h>

#include <string>

// A logged URL is whole (#2244): userinfo, password, query, fragment and every repeated parameter stay. Nothing in a
// log is masked.

TEST(UrlDiagnostics, KeepsUserinfoPasswordQueryAndFragment) {
  const std::string input =
      "wss://alice%40example:pa%3Ass@db.example:7443/serverdb?http_key=first&HTTP_KEY=second&"
      "%68%74%74%70%5F%6B%65%79=third&x=1&x=2&password=in-query#frag-detail";
  EXPECT_EQ(nevr_log_diagnostics::UrlForDiagnostics(input), input);
}

TEST(UrlDiagnostics, KeepsAnEmptyQueryAndFragment) {
  EXPECT_EQ(nevr_log_diagnostics::UrlForDiagnostics("https://service.example/path?#"),
            "https://service.example/path?#");
}

TEST(UrlDiagnostics, EveryShapeOfUserinfoIsKept) {
  using nevr_log_diagnostics::UrlForDiagnostics;
  for (const char* url : {"https://user:pw@h.example/p", "https://user@h.example/p", "https://user:@h.example/p",
                          "https://:pw@h.example/p", "https://u:p@ss@h.example/p", "wss://user:pass@",
                          "https://h.example/a@b:c?e=x@y:z#f@g:h", "https://service.example:8443/v2/match"}) {
    EXPECT_EQ(UrlForDiagnostics(url), url);
  }
}

TEST(UrlDiagnostics, FormattedDiagnosticIsTheWholeUrl) {
  const std::string url =
      "wss://user-detail:password-detail@db.example:7443/serverdb?http_key=query-detail&x=2#fragment-detail";
  EXPECT_EQ(nevr_log_diagnostics::FormatUrlDiagnostic("[NEVR.SERVERDB] Connecting to ServerDB at ", url),
            "[NEVR.SERVERDB] Connecting to ServerDB at " + url);
}

TEST(UrlDiagnostics, FormattedRedirectDiagnosticKeepsBothUrlsWhole) {
  const std::string from = "https://user:pass@source.example:8443/auth?http_key=source-detail#source-fragment";
  const std::string to = "https://relay.example:9443/auth?ticket=relay-detail#relay-fragment";
  EXPECT_EQ(nevr_log_diagnostics::FormatUrlPairDiagnostic("[NEVR.PATCH] service redirect key=auth from=", from,
                                                                  " to=", to),
            "[NEVR.PATCH] service redirect key=auth from=" + from + " to=" + to);
}

TEST(UrlDiagnostics, AnUnusualUrlIsLoggedAsGivenNotHidden) {
  using nevr_log_diagnostics::UrlForDiagnostics;
  EXPECT_EQ(UrlForDiagnostics("wss://host.example/path?key=%Q1"), "wss://host.example/path?key=%Q1");
  EXPECT_EQ(UrlForDiagnostics("https://host/%"), "https://host/%");
  EXPECT_EQ(UrlForDiagnostics("file://host/path"), "file://host/path");
  EXPECT_EQ(UrlForDiagnostics("not a url"), "not a url");
  EXPECT_EQ(UrlForDiagnostics(""), "");
}

TEST(UrlDiagnostics, ControlCharactersAreEscapedSoOneUrlIsOneLine) {
  using nevr_log_diagnostics::UrlForDiagnostics;
  EXPECT_EQ(UrlForDiagnostics("https://host/\nsecond line"), "https://host/%0Asecond line");
  EXPECT_EQ(UrlForDiagnostics(std::string("https://host/a\0b", 16)), "https://host/a%00b");
  EXPECT_EQ(UrlForDiagnostics("https://host/\x7f"), "https://host/%7F");
}

TEST(UrlDiagnostics, ALongUrlIsLoggedWhole) {
  const std::string longUrl = "https://host.example/" + std::string(9000, 'x');
  EXPECT_EQ(nevr_log_diagnostics::UrlForDiagnostics(longUrl), longUrl);
}
