#include <gtest/gtest.h>

#include <optional>
#include <string>

#include "runtime/lifecycle/clock_skew.h"

using nevr_clock_skew::CompareClocks;
using nevr_clock_skew::FormatUtc;
using nevr_clock_skew::ParseHttpDate;
using nevr_clock_skew::RefusalLine;
using nevr_clock_skew::Verdict;

namespace {

// Service time for the cases below: a Date header from a response.
constexpr const char* kServiceDate = "Fri, 30 Oct 2026 15:04:00 GMT";

std::int64_t Service() { return *ParseHttpDate(kServiceDate); }

}  // namespace

TEST(ClockSkew, ParsesTheRfcExampleToItsUnixTime) {
  // RFC 9110 section 5.6.7's example, "Sun, 06 Nov 1994 08:49:37 GMT", is Unix time 784111777.
  EXPECT_EQ(ParseHttpDate("Sun, 06 Nov 1994 08:49:37 GMT"), std::optional<std::int64_t>(784111777));
}

TEST(ClockSkew, ParsedDateFormatsBackToTheSameInstant) {
  EXPECT_EQ(FormatUtc(Service()), "2026-10-30T15:04:00Z");
  EXPECT_EQ(FormatUtc(0), "1970-01-01T00:00:00Z");
}

TEST(ClockSkew, RejectsMalformedDates) {
  EXPECT_FALSE(ParseHttpDate("").has_value());                                // absent header
  EXPECT_FALSE(ParseHttpDate("Sun, 06 Nov 1994 08:49:37 PST").has_value());  // not GMT
  EXPECT_FALSE(ParseHttpDate("Sun, 06 Foo 1994 08:49:37 GMT").has_value());  // unknown month
  EXPECT_FALSE(ParseHttpDate("Sun, 06 Nov 1994 08:49:3x GMT").has_value());  // not a digit
  EXPECT_FALSE(ParseHttpDate("Sun, 06 Nov 1994 24:00:00 GMT").has_value());  // hour out of range
  EXPECT_FALSE(ParseHttpDate("Sun, 06 Nov 1994 08:49:37 GMT extra").has_value());
}

TEST(ClockSkew, WithinTheDriftThresholdIsOk) {
  const std::int64_t service = Service();
  EXPECT_EQ(CompareClocks(service, service).verdict, Verdict::kOk);
  EXPECT_EQ(CompareClocks(service + 30, service).verdict, Verdict::kOk);
  EXPECT_EQ(CompareClocks(service - 30, service).verdict, Verdict::kOk);
}

TEST(ClockSkew, PastTheDriftThresholdButWithinToleranceWarns) {
  const std::int64_t service = Service();
  const auto check = CompareClocks(service + 45, service);
  EXPECT_EQ(check.verdict, Verdict::kDrift);
  EXPECT_EQ(check.skew_seconds, 45);
  EXPECT_EQ(CompareClocks(service + 300, service).verdict, Verdict::kDrift);  // the tolerance itself does not refuse
}

TEST(ClockSkew, BehindByMoreThanTheToleranceRefusesWithBothTimesAndTheFix) {
  const std::int64_t service = Service();
  const std::int64_t local = service - (5 * 3600 + 2 * 60);  // the 5h02m case from the issue
  const auto check = CompareClocks(local, service);
  EXPECT_EQ(check.verdict, Verdict::kRefuse);
  EXPECT_EQ(check.skew_seconds, -18120);

  const std::string line = RefusalLine(local, service);
  EXPECT_NE(line.find("system clock is 5h02m behind service time"), std::string::npos) << line;
  EXPECT_NE(line.find("system 2026-10-30T10:02:00Z"), std::string::npos) << line;
  EXPECT_NE(line.find("service 2026-10-30T15:04:00Z"), std::string::npos) << line;
  EXPECT_NE(line.find("sync the clock (Windows: w32tm /resync) and restart"), std::string::npos) << line;
}

TEST(ClockSkew, AheadByMoreThanTheToleranceRefuses) {
  const std::int64_t service = Service();
  const auto check = CompareClocks(service + 301, service);
  EXPECT_EQ(check.verdict, Verdict::kRefuse);
  EXPECT_NE(RefusalLine(service + 301, service).find("5m01s ahead service time"), std::string::npos);
}
