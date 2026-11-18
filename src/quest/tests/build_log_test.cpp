// Host tests for the store build's structured log (src/quest/obb/build_log.cpp, docs/standards/logging.md tag NEVR.QUESTBUILD): the
// header every record carries, the level of each call, the fields merged into the record, a timestamp that is UTC with three
// millisecond digits whatever the zone, a value that is not UTF-8 written as a record that still parses, and the stdout sink that
// writes one flushed line per record.

#include <fcntl.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "quest/obb/build_log.h"
#include "quest/tests/mini_test.h"

using nevr_quest::obb::BuildLog;

namespace {

constexpr int kSchema = 1;
constexpr std::size_t kTsChars = 24;  // "YYYY-MM-DDTHH:MM:SS.mmmZ"
constexpr std::size_t kDatePosition = 4, kTPosition = 10, kDotPosition = 19, kZPosition = 23, kMsPosition = 20;
constexpr std::int64_t kClockToleranceS = 5;
constexpr std::size_t kInfoRecord = 0, kWarningRecord = 1, kErrorRecord = 2;  // the order EachCallWritesItsOwnLevelAndOneLine writes them
constexpr std::int64_t kMsPerSecond = 1000;
constexpr std::int64_t kLowMs = 10;  // a clock reading under this has leading zeros in its three digits
constexpr std::int64_t kMidFromMs = 100, kMidToMs = 900;  // a millisecond reading of three real digits, away from either wrap
constexpr auto kPollGap = std::chrono::milliseconds(1);
constexpr mode_t kFileMode = 0600;
constexpr std::size_t kHeaderKeys = 5;     // schema, ts, level, tag, event
constexpr std::size_t kRecordsWritten = 3;  // one per level in EachCallWritesItsOwnLevelAndOneLine
constexpr int kBytesFieldValue = 4096, kCountFieldValue = 2;
constexpr std::size_t kReadChunkBytes = 256;
constexpr auto kSampleDeadline = std::chrono::seconds(10);  // the sampling retries until a sample fell inside its window
const char kReplacementCharacter[] = "\xEF\xBF\xBD";  // U+FFFD

std::vector<std::string> g_lines;
BuildLog Capture() {
  g_lines.clear();
  return BuildLog([](const std::string& line) { g_lines.push_back(line); });
}

nlohmann::json OnlyRecord() {
  CHECK_EQ(g_lines.size(), std::size_t{1});
  return g_lines.empty() ? nlohmann::json::object() : nlohmann::json::parse(g_lines[0]);
}

bool ValidTimestamp(const std::string& ts) {
  if (ts.size() != kTsChars || ts[kDatePosition] != '-' || ts[kTPosition] != 'T' || ts[kDotPosition] != '.' || ts[kZPosition] != 'Z') {
    return false;
  }
  for (std::size_t i = kMsPosition; i < kZPosition; ++i) {
    if (ts[i] < '0' || ts[i] > '9') return false;
  }
  return true;
}

}  // namespace

TEST(ARecordCarriesTheHeaderTheLoggingStandardNames) {
  BuildLog log = Capture();
  log.Info("step_done");
  const nlohmann::json record = OnlyRecord();
  CHECK_EQ(record.at("schema").get<int>(), kSchema);
  CHECK_EQ(record.at("tag").get<std::string>(), std::string("NEVR.QUESTBUILD"));
  CHECK_EQ(record.at("event").get<std::string>(), std::string("step_done"));
  CHECK_EQ(record.at("level").get<std::string>(), std::string("info"));
  CHECK(ValidTimestamp(record.at("ts").get<std::string>()));
  CHECK_EQ(record.size(), kHeaderKeys);  // no field was given, none is made up
}

TEST(EachCallWritesItsOwnLevelAndOneLine) {
  BuildLog log = Capture();
  log.Info("a");
  log.Warning("b");
  log.Error("c");
  CHECK_EQ(g_lines.size(), kRecordsWritten);
  CHECK_EQ(nlohmann::json::parse(g_lines[kInfoRecord]).at("level").get<std::string>(), std::string("info"));
  CHECK_EQ(nlohmann::json::parse(g_lines[kWarningRecord]).at("level").get<std::string>(), std::string("warning"));
  CHECK_EQ(nlohmann::json::parse(g_lines[kErrorRecord]).at("level").get<std::string>(), std::string("error"));
  for (const std::string& line : g_lines) CHECK(line.find('\n') == std::string::npos);  // one record, one line
}

TEST(TheStepsFieldsAreMergedIntoTheRecordWithTheirTypes) {
  BuildLog log = Capture();
  log.Error("refused", {{"code", "obb_bad_tag"}, {"detail", "tag 0x1"}, {"bytes", kBytesFieldValue}, {"ok", false}});
  const nlohmann::json record = OnlyRecord();
  CHECK_EQ(record.at("code").get<std::string>(), std::string("obb_bad_tag"));
  CHECK_EQ(record.at("detail").get<std::string>(), std::string("tag 0x1"));
  CHECK_EQ(record.at("bytes").get<int>(), kBytesFieldValue);
  CHECK(record.at("ok").is_boolean() && !record.at("ok").get<bool>());
  CHECK_EQ(record.at("event").get<std::string>(), std::string("refused"));
}

// The timestamp is UTC whatever the zone of the machine that runs the build.
TEST(TheTimestampIsUtcWhateverTheZone) {
  const char* const saved = std::getenv("TZ");
  const std::string saved_zone = saved != nullptr ? saved : "";
  ::setenv("TZ", "UTC-5", 1);  // a zone five hours ahead: a local-time reading would show it
  ::tzset();
  BuildLog log = Capture();
  const std::time_t before = std::time(nullptr);
  log.Info("now");
  const std::string ts = OnlyRecord().at("ts").get<std::string>();
  struct tm parsed {};
  CHECK(::strptime(ts.c_str(), "%Y-%m-%dT%H:%M:%S", &parsed) != nullptr);
  const std::time_t read = ::timegm(&parsed);
  CHECK(read - before <= kClockToleranceS && before - read <= kClockToleranceS);
  if (saved != nullptr) {
    ::setenv("TZ", saved_zone.c_str(), 1);
  } else {
    ::unsetenv("TZ");
  }
  ::tzset();
  CHECK((std::getenv("TZ") != nullptr) == (saved != nullptr));  // the zone is the process's own again
  CHECK(saved == nullptr || saved_zone == std::getenv("TZ"));
}

std::int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

// One record written while the clock's milliseconds-in-the-second are inside [from, to]: the clock before and after the call and the
// milliseconds the record carries. Retried until a sample has both readings inside the window (a preemption or a second's wrap spoils
// a sample, not the code under test), within the deadline; `ok` is false when none did.
struct MsSample {
  bool ok = false;
  std::int64_t before = 0, after = 0, written = 0;
};
MsSample SampleInWindow(std::int64_t from, std::int64_t to) {
  const auto give_up = std::chrono::steady_clock::now() + kSampleDeadline;
  while (std::chrono::steady_clock::now() < give_up) {
    const std::int64_t now = NowMs() % kMsPerSecond;
    if (now < from || now > to) {
      std::this_thread::sleep_for(kPollGap);
      continue;
    }
    BuildLog log = Capture();
    const std::int64_t before = NowMs() % kMsPerSecond;
    log.Info("sample");
    const std::int64_t after = NowMs() % kMsPerSecond;
    if (before < from || after > to || after < before) continue;
    const std::string ts = OnlyRecord().at("ts").get<std::string>();
    if (!ValidTimestamp(ts)) return {};
    return {true, before, after, std::stoi(ts.substr(kMsPosition, kZPosition - kMsPosition))};
  }
  return {};
}

// The milliseconds are the clock's, written with three digits: a record made while the clock reads under 10 ms past the second has two
// leading zeros, and the value is the clock's within the time the call took.
TEST(TheMillisecondsAreTheClocksAndZeroPaddedToThreeDigits) {
  const MsSample sample = SampleInWindow(0, kLowMs - 1);
  CHECK(sample.ok);  // a sample at the start of a second, so the milliseconds needed their padding
  CHECK(sample.written >= sample.before && sample.written <= sample.after);
}

// A record made well inside a second reads the clock's three-digit milliseconds, not a shortened figure.
TEST(TheMillisecondsOfAMidSecondRecordAreTheClocks) {
  const MsSample sample = SampleInWindow(kMidFromMs, kMidToMs);
  CHECK(sample.ok);
  CHECK(sample.written >= sample.before && sample.written <= sample.after);
}

// A path or detail with bytes that are not UTF-8 never makes the log throw or write a line that does not parse.
TEST(AValueThatIsNotUtf8IsReplacedAndTheRecordStillParses) {
  BuildLog log = Capture();
  log.Warning("odd", {{"detail", std::string("a\xFF" "b")}});
  const nlohmann::json record = OnlyRecord();
  CHECK_EQ(record.at("detail").get<std::string>(), std::string("a") + kReplacementCharacter + "b");
}

// The standard-output sink writes each record as one line and flushes it, so a build killed after a step has that step's record.
TEST(TheStdoutSinkWritesOneFlushedLinePerRecord) {
  const char* dir = std::getenv("NEVR_TEST_TMP");
  const std::string path = std::string(dir != nullptr ? dir : "/tmp") + "/build_log_stdout_" + std::to_string(::getpid());
  std::fflush(stdout);
  const int saved = ::dup(STDOUT_FILENO);
  const int file = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, kFileMode);
  CHECK(saved >= 0 && file >= 0);
  ::dup2(file, STDOUT_FILENO);
  ::close(file);
  BuildLog log = BuildLog::Stdout();
  log.Info("first");
  log.Warning("second", {{"n", kCountFieldValue}});
  // Read what reached the file before stdout is given back and before any flush of ours: the sink flushed it itself.
  FILE* in = std::fopen(path.c_str(), "rb");
  std::string text;
  char buffer[kReadChunkBytes];
  for (std::size_t n; in != nullptr && (n = std::fread(buffer, 1, sizeof buffer, in)) > 0;) text.append(buffer, n);
  if (in != nullptr) std::fclose(in);
  ::dup2(saved, STDOUT_FILENO);
  ::close(saved);
  std::remove(path.c_str());
  const std::size_t first_end = text.find('\n');
  CHECK(first_end != std::string::npos);
  CHECK(!text.empty() && text.back() == '\n');
  if (first_end == std::string::npos) return;
  const nlohmann::json first = nlohmann::json::parse(text.substr(0, first_end));
  const nlohmann::json second = nlohmann::json::parse(text.substr(first_end + 1));  // the rest: the second record and its newline
  CHECK_EQ(first.at("event").get<std::string>(), std::string("first"));
  CHECK_EQ(second.at("event").get<std::string>(), std::string("second"));
  CHECK_EQ(second.at("n").get<int>(), kCountFieldValue);
}

int main(int argc, char** argv) { return mini_test::RunAll(argc, argv); }
