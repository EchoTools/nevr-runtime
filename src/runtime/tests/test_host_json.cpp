// The fixed-buffer JSON line writer the BugSplat64.dll host uses where it cannot
// allocate (runtime/host/host_json.h). Every record it writes is parsed back with
// nlohmann::json: a writer that cannot be parsed is the failure "no hand-built
// serialization" in AGENTS.md exists to prevent.

#include "runtime/host/host_json.h"

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include <windows.h>

#include <cstdint>
#include <cstring>
#include <string>

namespace {

using nevr_host::JsonLine;

// The record the writer produced, parsed. Fails the test when it does not parse
// or is not exactly one newline-terminated line.
nlohmann::json Parse(const char* buffer, std::size_t length) {
  EXPECT_GT(length, 1u);
  EXPECT_EQ(buffer[length - 1], '\n');
  EXPECT_EQ(std::memchr(buffer, '\n', length - 1), nullptr) << "more than one line";
  return nlohmann::json::parse(std::string(buffer, length - 1));
}

}  // namespace

TEST(HostJson, EveryRecordCarriesTheHeader) {
  char buffer[512];
  JsonLine line(buffer, sizeof(buffer), "probe");
  const std::size_t length = line.Finish();
  const nlohmann::json record = Parse(buffer, length);
  EXPECT_EQ(record.at("tag"), "NEVR.HOST");
  EXPECT_EQ(record.at("event"), "probe");
  EXPECT_EQ(record.at("pid"), GetCurrentProcessId());
  const std::string ts = record.at("ts");
  ASSERT_EQ(ts.size(), 24u);
  EXPECT_EQ(ts[4], '-');
  EXPECT_EQ(ts[10], 'T');
  EXPECT_EQ(ts[19], '.');
  EXPECT_EQ(ts[23], 'Z');
}

TEST(HostJson, StringsAreEscapedSoTheyParseBackIdentically) {
  char buffer[512];
  JsonLine line(buffer, sizeof(buffer), "probe");
  const std::string tricky = std::string("quote\" back\\slash tab\t nl\n ctl\x01 end");
  line.Str("text", tricky.c_str());
  const nlohmann::json record = Parse(buffer, line.Finish());
  EXPECT_EQ(record.at("text"), tricky);
}

TEST(HostJson, WidePathsBecomeUtf8WithBackslashesEscaped) {
  char buffer[512];
  JsonLine line(buffer, sizeof(buffer), "probe");
  // C:\users\<e acute>\<U+1F600>\nevr.dmp
  const wchar_t path[] = {L'C', L':', L'\\', L'u', L's', L'e', L'r', L's', L'\\', 0x00E9, L'\\',
                          0xD83D, 0xDE00, L'\\', L'n', L'e', L'v', L'r', L'.', L'd', L'm', L'p', 0};
  line.Wide("path", path);
  const nlohmann::json record = Parse(buffer, line.Finish());
  EXPECT_EQ(record.at("path"), "C:\\users\\\xC3\xA9\\\xF0\x9F\x98\x80\\nevr.dmp");
}

TEST(HostJson, AnUnpairedSurrogateBecomesTheReplacementCharacter) {
  char buffer[512];
  JsonLine line(buffer, sizeof(buffer), "probe");
  const wchar_t lone_high[] = {L'a', 0xD83D, L'b', 0};
  const wchar_t lone_low[] = {L'c', 0xDE00, L'd', 0};
  line.Wide("high", lone_high).Wide("low", lone_low);
  const nlohmann::json record = Parse(buffer, line.Finish());
  EXPECT_EQ(record.at("high"), "a\xEF\xBF\xBD" "b");
  EXPECT_EQ(record.at("low"), "c\xEF\xBF\xBD" "d");
}

TEST(HostJson, NumbersBooleansAndBytesRoundTrip) {
  char buffer[512];
  JsonLine line(buffer, sizeof(buffer), "probe");
  const unsigned char bytes[] = {0x00, 0x0f, 0xa5, 0xff};
  line.UInt("max", 0xFFFFFFFFFFFFFFFFull)
      .UInt("zero", 0)
      .Int("negative", -42)
      .Int("min", INT64_MIN)
      .Hex("hex_zero", 0)
      .Hex("hex", 0xC0000005)
      .Hex("hex_wide", 0x1400116720ull)
      .Bool("yes", true)
      .Bool("no", false)
      .Bytes("bytes", bytes, sizeof(bytes));
  const nlohmann::json record = Parse(buffer, line.Finish());
  EXPECT_EQ(record.at("max").get<std::uint64_t>(), 0xFFFFFFFFFFFFFFFFull);
  EXPECT_EQ(record.at("zero"), 0);
  EXPECT_EQ(record.at("negative"), -42);
  EXPECT_EQ(record.at("min").get<std::int64_t>(), INT64_MIN);
  EXPECT_EQ(record.at("hex_zero"), "0x0");
  EXPECT_EQ(record.at("hex"), "0xc0000005");
  EXPECT_EQ(record.at("hex_wide"), "0x1400116720");
  EXPECT_EQ(record.at("yes"), true);
  EXPECT_EQ(record.at("no"), false);
  EXPECT_EQ(record.at("bytes"), "000fa5ff");
}

TEST(HostJson, ARecordThatDoesNotFitIsNeverTruncated) {
  // Room for the header and the overflow marker, not for the long value.
  char buffer[200];
  JsonLine line(buffer, sizeof(buffer), "crash_dump");
  const std::string long_value(400, 'x');
  line.Str("path", long_value.c_str());
  EXPECT_TRUE(line.Overflowed());
  const std::size_t length = line.Finish();
  ASSERT_NE(length, 0u);
  const nlohmann::json record = Parse(buffer, length);  // still valid JSON
  EXPECT_EQ(record.at("event"), "crash_dump");
  EXPECT_EQ(record.at("error"), "record_too_large");
  EXPECT_FALSE(record.contains("path"));
}

TEST(HostJson, ABufferTooSmallForTheHeaderYieldsNothingRatherThanBrokenJson) {
  char buffer[16];
  JsonLine line(buffer, sizeof(buffer), "probe");
  EXPECT_EQ(line.Finish(), 0u);
}

TEST(HostJson, KeysKeepTheirOrderAndSeparators) {
  char buffer[512];
  JsonLine line(buffer, sizeof(buffer), "probe");
  line.UInt("a", 1).UInt("b", 2).UInt("c", 3);
  const std::size_t length = line.Finish();
  const std::string text(buffer, length);
  EXPECT_NE(text.find("\"a\":1,\"b\":2,\"c\":3}"), std::string::npos) << text;
}

// The stop line (runtime/host/stop_line.h) is the host's one record of another tag: same four header fields, in the
// same order, under the tag the caller names.
TEST(HostJson, ALineOfAnotherTagCarriesTheSameHeaderUnderThatTag) {
  char buffer[512];
  JsonLine line(buffer, sizeof(buffer), "NEVR.STOP", "stop");
  line.Bool("stopped", true);
  const std::size_t length = line.Finish();
  const std::string text(buffer, length);
  EXPECT_EQ(text.rfind("{\"tag\":\"NEVR.STOP\",\"ts\":\"", 0), 0u) << text;
  const nlohmann::json record = Parse(buffer, length);
  EXPECT_EQ(record.at("tag"), "NEVR.STOP");
  EXPECT_EQ(record.at("event"), "stop");
  EXPECT_EQ(record.at("pid"), GetCurrentProcessId());
  EXPECT_EQ(record.at("stopped"), true);
  const std::size_t event_at = text.find("\"event\":\"stop\"");
  const std::size_t pid_at = text.find("\"pid\":");
  const std::size_t ts_at = text.find("\"ts\":");
  EXPECT_LT(ts_at, pid_at);
  EXPECT_LT(pid_at, event_at);
}

TEST(HostJson, ARecordTooLargeForItsBufferKeepsTheTagItWasGiven) {
  char buffer[200];
  JsonLine line(buffer, sizeof(buffer), "NEVR.STOP", "stop");
  const std::string long_value(400, 'x');
  line.Str("detail", long_value.c_str());
  const nlohmann::json record = Parse(buffer, line.Finish());
  EXPECT_EQ(record.at("tag"), "NEVR.STOP");
  EXPECT_EQ(record.at("error"), "record_too_large");
}
