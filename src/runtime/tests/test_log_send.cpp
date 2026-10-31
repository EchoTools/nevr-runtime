// Tests for the player log upload (core/log_send.h, #1788, docs/adr/0030-player-log-upload.md). The files are real files in a
// scratch folder; the service's answers come from a fake POST. The service's rules (the body's keys, the code's shape) are
// restated below from the ADR and share no code with the sender.

#include "core/log_send.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <regex>
#include <set>
#include <string>
#include <vector>

#ifdef _WIN32
#include <process.h>
#define NEVR_TEST_PID _getpid()
#else
#include <unistd.h>
#define NEVR_TEST_PID getpid()
#endif

namespace fs = std::filesystem;
using nlohmann::json;
namespace ls = nevr_log_send;

namespace {

// ---- the ADR's body, restated ----

const std::set<std::string> kTop = {"schema", "install_id", "build", "platform", "headset", "files"};
const std::set<std::string> kFileKeys = {"name", "session", "truncated", "text"};
const char kInstallId[] = "0123456789abcdef0123456789abcdef";

std::set<std::string> Keys(const json& object) {
  std::set<std::string> keys;
  for (auto it = object.begin(); it != object.end(); ++it) keys.insert(it.key());
  return keys;
}

// ---- the fake service ----

struct Posted {
  std::vector<std::string> urls;
  std::vector<std::string> bodies;
  std::vector<nevr_crash_send::Timeouts> timeouts;
};
Posted g_posted;
ls::PostResult g_answer;  // the answer to a POST
struct LoggedLine {
  ls::LogLevel level;
  std::string line;
};
std::vector<LoggedLine> g_lines;

ls::PostResult FakePost(void*, const std::string& url, const std::string& body, const nevr_crash_send::Timeouts& timeouts) {
  g_posted.urls.push_back(url);
  g_posted.bodies.push_back(body);
  g_posted.timeouts.push_back(timeouts);
  return g_answer;
}

void FakeLog(ls::LogLevel level, const std::string& line) { g_lines.push_back({level, line}); }

ls::PostResult Accepted(const std::string& code = "LOG-7K2Q") {
  ls::PostResult r;
  r.transportOk = true;
  r.status = 201;
  r.response = "{\"code\":\"" + code + "\"}";
  return r;
}

ls::PostResult Status(long status) {
  ls::PostResult r;
  r.transportOk = true;
  r.status = status;
  return r;
}

void WriteFile(const fs::path& path, const std::string& bytes) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

class LogSend : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = fs::temp_directory_path() / ("nevr-log-send-test-" + std::to_string(NEVR_TEST_PID) + "-" +
                                        ::testing::UnitTest::GetInstance()->current_test_info()->name());
    std::error_code ec;
    fs::remove_all(dir_, ec);
    fs::create_directories(dir_);
    g_posted = Posted{};
    g_answer = Accepted();
    g_lines.clear();
  }
  void TearDown() override {
    std::error_code ec;
    fs::remove_all(dir_, ec);  // exactly the directory SetUp made
  }
  // The current log "run.log" and the previous "run-prev.log", both written.
  ls::Config Config(const std::string& current = "current text\n", const std::string& previous = "previous text\n") {
    WriteFile(dir_ / "run.log", current);
    WriteFile(dir_ / "run-prev.log", previous);
    ls::Config c;
    c.files = {{"run.log", dir_ / "run.log", ls::Session::kCurrent}, {"run-prev.log", dir_ / "run-prev.log", ls::Session::kPrevious}};
    c.url = "https://service.invalid/api/v3/player-logs";
    c.installId = kInstallId;
    c.build = "5.7.1-dev.3+abc1234";
    c.platform = "pc";
    c.headset = "";
    c.post = &FakePost;
    c.log = &FakeLog;
    return c;
  }
  const LoggedLine* Line(ls::LogLevel level, const char* contains) const {
    for (const auto& l : g_lines) {
      if (l.level == level && l.line.find(contains) != std::string::npos) return &l;
    }
    return nullptr;
  }
  fs::path dir_;
};

}  // namespace

// ---- the body ----

TEST_F(LogSend, TheBodyParsesAndCarriesTheAdrSchema) {
  const auto result = ls::Send(Config("current text\n", "previous text\n"), true);
  ASSERT_EQ(result.status, ls::Status::kSent);
  ASSERT_EQ(g_posted.bodies.size(), 1u);
  EXPECT_EQ(g_posted.urls[0], "https://service.invalid/api/v3/player-logs");
  const json j = json::parse(g_posted.bodies[0]);
  EXPECT_EQ(Keys(j), kTop);
  EXPECT_EQ(j.at("schema"), 1);
  EXPECT_EQ(j.at("install_id"), kInstallId);
  EXPECT_EQ(j.at("build"), "5.7.1-dev.3+abc1234");
  EXPECT_EQ(j.at("platform"), "pc");
  EXPECT_EQ(j.at("headset"), "");
  ASSERT_EQ(j.at("files").size(), 2u);
  const json& cur = j.at("files")[0];
  EXPECT_EQ(Keys(cur), kFileKeys);
  EXPECT_EQ(cur.at("name"), "run.log");
  EXPECT_EQ(cur.at("session"), "current");
  EXPECT_EQ(cur.at("truncated"), false);
  EXPECT_EQ(cur.at("text"), "current text\n");
  const json& prev = j.at("files")[1];
  EXPECT_EQ(prev.at("name"), "run-prev.log");
  EXPECT_EQ(prev.at("session"), "previous");
  EXPECT_EQ(prev.at("text"), "previous text\n");
  EXPECT_EQ(result.files, 2u);
  EXPECT_EQ(result.bodyBytes, g_posted.bodies[0].size());
  EXPECT_FALSE(result.droppedPrevious);
}

TEST_F(LogSend, ABytesThatIsNotUtf8ByteStillMakesAParsableBody) {
  const auto result = ls::Send(Config(std::string("bad \xff\xfe byte and \x01 control\n"), "p"), true);
  ASSERT_EQ(result.status, ls::Status::kSent);
  EXPECT_FALSE(json::parse(g_posted.bodies[0], nullptr, /*allow_exceptions=*/false).is_discarded());
}

TEST_F(LogSend, TheIdentityStringsAreCheckedLikeTheCrashReports) {
  auto c = Config();
  c.build = "C:\\Users\\alice\\build";
  c.platform = "quest";
  c.headset = "Quest 3";
  ASSERT_EQ(ls::Send(c, true).status, ls::Status::kSent);
  json j = json::parse(g_posted.bodies[0]);
  EXPECT_EQ(j.at("build"), "unknown");
  EXPECT_EQ(j.at("platform"), "quest");
  EXPECT_EQ(j.at("headset"), "Quest 3");

  g_posted = Posted{};
  c.platform = "anything else";
  c.headset = "alice's \"headset\"\n";
  ASSERT_EQ(ls::Send(c, true).status, ls::Status::kSent);
  j = json::parse(g_posted.bodies[0]);
  EXPECT_EQ(j.at("platform"), "pc");
  EXPECT_EQ(j.at("headset"), "");
}

TEST_F(LogSend, TheLengthAndCharactersOfBuildAndHeadsetAreBounded) {
  auto c = Config();
  c.build = std::string(64, 'b');
  c.headset = std::string(64, 'h');
  ASSERT_EQ(ls::Send(c, true).status, ls::Status::kSent);
  json j = json::parse(g_posted.bodies[0]);
  EXPECT_EQ(j.at("build"), std::string(64, 'b'));
  EXPECT_EQ(j.at("headset"), std::string(64, 'h'));

  g_posted = Posted{};
  c.build = std::string(65, 'b');
  c.headset = std::string(65, 'h');
  ASSERT_EQ(ls::Send(c, true).status, ls::Status::kSent);
  j = json::parse(g_posted.bodies[0]);
  EXPECT_EQ(j.at("build"), "unknown");
  EXPECT_EQ(j.at("headset"), "");

  g_posted = Posted{};
  c.build = "5.7 dev";  // a space is not in a build string, but is in a headset name
  c.headset = "Quest 3";
  ASSERT_EQ(ls::Send(c, true).status, ls::Status::kSent);
  j = json::parse(g_posted.bodies[0]);
  EXPECT_EQ(j.at("build"), "unknown");
  EXPECT_EQ(j.at("headset"), "Quest 3");
}

// ---- which files ----

TEST_F(LogSend, OnlyTheGivenFilesAreRead) {
  auto c = Config("current text\n", "previous text\n");
  WriteFile(dir_ / "config.yaml", "password: SECRETPASSWORD");
  WriteFile(dir_ / "run-older.log", "OLDER_SESSION_TEXT");
  WriteFile(dir_ / ".credentials.json", "SECRETTOKEN123");
  fs::create_directory(dir_ / "crashes");
  WriteFile(dir_ / "crashes" / "a.dmp", "MDMP_STACK_MEMORY");
  ASSERT_EQ(ls::Send(c, true).status, ls::Status::kSent);
  const json j = json::parse(g_posted.bodies[0]);
  ASSERT_EQ(j.at("files").size(), 2u);
  for (const char* other : {"SECRETPASSWORD", "OLDER_SESSION_TEXT", "SECRETTOKEN123", "MDMP_STACK_MEMORY"}) {
    EXPECT_EQ(g_posted.bodies[0].find(other), std::string::npos) << other;
  }
}

TEST_F(LogSend, AFileThatDoesNotExistIsLeftOutAndTheRestGoes) {
  auto c = Config();
  fs::remove(dir_ / "run-prev.log");
  ASSERT_EQ(ls::Send(c, true).status, ls::Status::kSent);
  const json j = json::parse(g_posted.bodies[0]);
  ASSERT_EQ(j.at("files").size(), 1u);
  EXPECT_EQ(j.at("files")[0].at("name"), "run.log");
}

TEST_F(LogSend, NoReadableFileSendsNothing) {
  auto c = Config();
  fs::remove(dir_ / "run.log");
  fs::remove(dir_ / "run-prev.log");
  const auto result = ls::Send(c, true);
  EXPECT_EQ(result.status, ls::Status::kNoFiles);
  EXPECT_TRUE(g_posted.bodies.empty());
  EXPECT_NE(Line(ls::LogLevel::kWarning, "[NEVR.LOGSEND] log send not started status=no_files"), nullptr);
}

TEST_F(LogSend, AtMostFourFilesAreReadInTheGivenOrder) {
  auto c = Config();
  c.files.clear();
  for (int i = 0; i < 6; ++i) {
    const std::string name = "f" + std::to_string(i) + ".log";
    WriteFile(dir_ / name, "text" + std::to_string(i));
    c.files.push_back({name, dir_ / name, i == 0 ? ls::Session::kCurrent : ls::Session::kPrevious});
  }
  ASSERT_EQ(ls::Send(c, true).status, ls::Status::kSent);
  const json j = json::parse(g_posted.bodies[0]);
  ASSERT_EQ(j.at("files").size(), 4u);
  EXPECT_EQ(j.at("files")[0].at("name"), "f0.log");
  EXPECT_EQ(j.at("files")[3].at("name"), "f3.log");
  EXPECT_EQ(g_posted.bodies[0].find("text4"), std::string::npos);
}

TEST_F(LogSend, UnreadableFilesDoNotUseUpTheFourSlots) {
  auto c = Config();
  c.files.clear();
  for (int i = 0; i < 5; ++i) c.files.push_back({"missing" + std::to_string(i) + ".log", dir_ / ("missing" + std::to_string(i)), ls::Session::kCurrent});
  WriteFile(dir_ / "real.log", "real");
  c.files.push_back({"real.log", dir_ / "real.log", ls::Session::kCurrent});
  ASSERT_EQ(ls::Send(c, true).status, ls::Status::kSent);
  EXPECT_EQ(json::parse(g_posted.bodies[0]).at("files").size(), 1u);
}

TEST_F(LogSend, ANameTheServiceWouldRefuseIsNotSent) {
  auto c = Config();
  WriteFile(dir_ / "x.log", "XTEXT");
  c.files.insert(c.files.begin(), {"C:\\Users\\alice\\x.log", dir_ / "x.log", ls::Session::kCurrent});
  c.files.insert(c.files.begin(), {"has space.log", dir_ / "x.log", ls::Session::kCurrent});
  c.files.insert(c.files.begin(), {"", dir_ / "x.log", ls::Session::kCurrent});
  c.files.insert(c.files.begin(), {std::string(65, 'a'), dir_ / "x.log", ls::Session::kCurrent});
  ASSERT_EQ(ls::Send(c, true).status, ls::Status::kSent);
  const json j = json::parse(g_posted.bodies[0]);
  ASSERT_EQ(j.at("files").size(), 2u);
  EXPECT_EQ(g_posted.bodies[0].find("XTEXT"), std::string::npos);
  EXPECT_EQ(g_posted.bodies[0].find("alice"), std::string::npos);
}

TEST_F(LogSend, ANameOfExactlySixtyFourBytesIsKept) {
  auto c = Config();
  c.files.clear();
  const std::string name(64, 'a');
  WriteFile(dir_ / "n.log", "text");
  c.files.push_back({name, dir_ / "n.log", ls::Session::kCurrent});
  ASSERT_EQ(ls::Send(c, true).status, ls::Status::kSent);
  EXPECT_EQ(json::parse(g_posted.bodies[0]).at("files")[0].at("name"), name);
}

// ---- the tail ----

TEST_F(LogSend, AFileOverTheCapIsCutToItsTailAndSaysSo) {
  const std::size_t cap = ls::kMaxTailBytes;
  ASSERT_EQ(cap, 256u * 1024u);
  std::string big(cap + 1000, 'H');  // the head
  for (std::size_t i = 1000; i < big.size(); ++i) big[i] = static_cast<char>('a' + (i % 26));
  auto c = Config(big, "previous");
  ASSERT_EQ(ls::Send(c, true).status, ls::Status::kSent);
  const json j = json::parse(g_posted.bodies[0]);
  const json& file = j.at("files")[0];
  EXPECT_EQ(file.at("truncated"), true);
  const std::string text = file.at("text");
  ASSERT_EQ(text.size(), cap);
  EXPECT_EQ(text, big.substr(1000));  // the last 256 KiB, and the head is gone
  EXPECT_EQ(text.find('H'), std::string::npos);
  EXPECT_EQ(j.at("files")[1].at("truncated"), false);
}

TEST_F(LogSend, AFileOfExactlyTheCapIsWholeAndNotTruncated) {
  const std::string exact(ls::kMaxTailBytes, 'x');
  ASSERT_EQ(ls::Send(Config(exact, "p"), true).status, ls::Status::kSent);
  const json file = json::parse(g_posted.bodies[0]).at("files")[0];
  EXPECT_EQ(file.at("truncated"), false);
  EXPECT_EQ(file.at("text").get<std::string>(), exact);
}

TEST_F(LogSend, AnEmptyFileIsSentEmpty) {
  ASSERT_EQ(ls::Send(Config("", "p"), true).status, ls::Status::kSent);
  const json file = json::parse(g_posted.bodies[0]).at("files")[0];
  EXPECT_EQ(file.at("text"), "");
  EXPECT_EQ(file.at("truncated"), false);
}

// ---- the 1 MiB cap ----

namespace {
// A control byte is six bytes in JSON (\u0001), so 100 KiB of it is 600 KiB in the body.
std::string Heavy() { return std::string(100 * 1024, '\x01'); }
}  // namespace

TEST_F(LogSend, ABodyOverOneMiBDropsThePreviousFileAndSaysSo) {
  auto c = Config(Heavy(), Heavy());
  const auto result = ls::Send(c, true);
  ASSERT_EQ(result.status, ls::Status::kSent);
  EXPECT_TRUE(result.droppedPrevious);
  EXPECT_EQ(result.files, 1u);
  ASSERT_EQ(g_posted.bodies.size(), 1u);
  EXPECT_LE(g_posted.bodies[0].size(), ls::kMaxBodyBytes);
  EXPECT_EQ(result.bodyBytes, g_posted.bodies[0].size());
  const json j = json::parse(g_posted.bodies[0]);
  ASSERT_EQ(j.at("files").size(), 1u);
  EXPECT_EQ(j.at("files")[0].at("session"), "current");
  EXPECT_NE(Line(ls::LogLevel::kInfo, "dropped_previous=true"), nullptr);
}

TEST_F(LogSend, ABodyUnderTheCapKeepsThePreviousFile) {
  const auto result = ls::Send(Config(Heavy(), "small"), true);
  ASSERT_EQ(result.status, ls::Status::kSent);
  EXPECT_FALSE(result.droppedPrevious);
  EXPECT_EQ(json::parse(g_posted.bodies[0]).at("files").size(), 2u);
  EXPECT_NE(Line(ls::LogLevel::kInfo, "dropped_previous=false"), nullptr);
}

TEST_F(LogSend, TheCapIsOneMiB) { EXPECT_EQ(ls::kMaxBodyBytes, 1024u * 1024u); }

TEST_F(LogSend, ABodyOfExactlyOneMiBIsSentAndOneByteMoreIsNot) {
  auto c = Config();
  c.files.clear();
  std::vector<ls::Collected> blank;
  for (int i = 0; i < 4; ++i) {
    c.files.push_back({"a" + std::to_string(i) + ".log", dir_ / ("a" + std::to_string(i) + ".log"), ls::Session::kCurrent});
    ls::Collected empty;
    empty.name = c.files.back().name;
    blank.push_back(empty);
  }
  const std::size_t room = ls::kMaxBodyBytes - ls::BuildBody(c, blank).size();  // plain bytes that still fit
  const std::size_t each = room / 4;
  const std::size_t last = room - 3 * each;
  ASSERT_LE(last, ls::kMaxTailBytes);
  for (int i = 0; i < 3; ++i) WriteFile(dir_ / c.files[static_cast<std::size_t>(i)].name, std::string(each, 'x'));
  WriteFile(dir_ / c.files[3].name, std::string(last, 'x'));
  auto result = ls::Send(c, true);
  ASSERT_EQ(result.status, ls::Status::kSent);
  EXPECT_EQ(result.bodyBytes, ls::kMaxBodyBytes);  // exactly at the cap
  EXPECT_FALSE(result.droppedPrevious);
  EXPECT_EQ(result.files, 4u);

  WriteFile(dir_ / c.files[3].name, std::string(last + 1, 'x'));
  g_posted = Posted{};
  result = ls::Send(c, true);
  EXPECT_EQ(result.status, ls::Status::kTooLarge);  // one byte over, and no previous file to drop
  EXPECT_TRUE(g_posted.bodies.empty());
}

TEST_F(LogSend, ABodyStillOverTheCapWithoutThePreviousFileIsNotSent) {
  auto c = Config(Heavy(), "p");
  WriteFile(dir_ / "run2.log", Heavy());
  c.files.push_back({"run2.log", dir_ / "run2.log", ls::Session::kCurrent});
  const auto result = ls::Send(c, true);
  EXPECT_EQ(result.status, ls::Status::kTooLarge);
  EXPECT_TRUE(g_posted.bodies.empty());
  const LoggedLine* line = Line(ls::LogLevel::kWarning, "log send not started status=too_large");
  ASSERT_NE(line, nullptr);
  EXPECT_NE(line->line.find("files=run.log:"), std::string::npos) << line->line;  // names and sizes, as in the collected line
  EXPECT_NE(line->line.find(":previous:whole,run2.log:"), std::string::npos) << line->line;
}

TEST_F(LogSend, OnlyPreviousFilesOverTheCapAreNotSent) {
  auto c = Config("c", "p");
  c.files.clear();
  WriteFile(dir_ / "p1.log", Heavy());
  WriteFile(dir_ / "p2.log", Heavy());
  c.files.push_back({"p1.log", dir_ / "p1.log", ls::Session::kPrevious});
  c.files.push_back({"p2.log", dir_ / "p2.log", ls::Session::kPrevious});
  EXPECT_EQ(ls::Send(c, true).status, ls::Status::kTooLarge);
  EXPECT_TRUE(g_posted.bodies.empty());
}

// ---- the code ----

TEST(LogSendCode, AGoodCodeIsAccepted) {
  EXPECT_TRUE(ls::IsValidCode("LOG-7K2Q"));
  EXPECT_TRUE(ls::IsValidCode("LOG-0000"));
  EXPECT_TRUE(ls::IsValidCode("LOG-ZZZZ"));
  EXPECT_TRUE(ls::IsValidCode("LOG-0123"));
  EXPECT_TRUE(ls::IsValidCode("LOG-ABCD"));
  EXPECT_TRUE(ls::IsValidCode("LOG-HJKM"));
  EXPECT_TRUE(ls::IsValidCode("LOG-NPQR"));
  EXPECT_TRUE(ls::IsValidCode("LOG-STVW"));
  EXPECT_TRUE(ls::IsValidCode("LOG-XYZ9"));
}

TEST(LogSendCode, EverySymbolOfTheAlphabetIsAccepted) {
  const std::string alphabet = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
  ASSERT_EQ(alphabet.size(), 32u);
  EXPECT_EQ(std::string(ls::kCodeAlphabet), alphabet);
  for (char c : alphabet) {
    EXPECT_TRUE(ls::IsValidCode(std::string("LOG-") + c + c + c + c)) << c;
    EXPECT_TRUE(ls::IsValidCode(std::string("LOG-") + c + "000")) << c;
    EXPECT_TRUE(ls::IsValidCode(std::string("LOG-") + "000" + c)) << c;
  }
}

TEST(LogSendCode, ABadCodeIsRejected) {
  EXPECT_FALSE(ls::IsValidCode(""));
  EXPECT_FALSE(ls::IsValidCode("log-7k2q"));  // lower case
  EXPECT_FALSE(ls::IsValidCode("LOG-7k2Q"));
  EXPECT_FALSE(ls::IsValidCode("Log-7K2Q"));
  EXPECT_FALSE(ls::IsValidCode("LOG-7K2"));    // too short
  EXPECT_FALSE(ls::IsValidCode("LOG-7K2QQ"));  // too long
  EXPECT_FALSE(ls::IsValidCode("7K2Q"));       // no prefix
  EXPECT_FALSE(ls::IsValidCode("LOG7K2QX"));   // no dash
  EXPECT_FALSE(ls::IsValidCode("XXX-7K2Q"));   // wrong prefix
  EXPECT_FALSE(ls::IsValidCode(" LOG-7K2Q"));
  EXPECT_FALSE(ls::IsValidCode("LOG-7K2Q "));
  EXPECT_FALSE(ls::IsValidCode("LOG-7K 2"));
  EXPECT_FALSE(ls::IsValidCode(std::string("LOG-7K2\0", 8)));  // a NUL is not a symbol
  EXPECT_FALSE(ls::IsValidCode("LOG-7K2-"));
  EXPECT_FALSE(ls::IsValidCode("LOG-BQDSUVGW"));  // the Lounge's eight-letter shape
}

TEST(LogSendCode, TheLettersIlouAreForbiddenAtEveryPosition) {
  for (char bad : {'I', 'L', 'O', 'U'}) {
    for (int position = 0; position < 4; ++position) {
      std::string code = "LOG-7K2Q";
      code[4 + position] = bad;
      EXPECT_FALSE(ls::IsValidCode(code)) << code;
    }
  }
}

TEST(LogSendCode, TheAnswerCarriesTheCode) {
  EXPECT_EQ(ls::ParseAnswer("{\"code\":\"LOG-7K2Q\"}"), std::optional<std::string>("LOG-7K2Q"));
  EXPECT_EQ(ls::ParseAnswer("{ \"code\" : \"LOG-0000\" }\n"), std::optional<std::string>("LOG-0000"));
}

TEST(LogSendCode, AnAnswerWithoutAValidCodeHasNone) {
  EXPECT_FALSE(ls::ParseAnswer("").has_value());
  EXPECT_FALSE(ls::ParseAnswer("not json").has_value());
  EXPECT_FALSE(ls::ParseAnswer("{}").has_value());
  EXPECT_FALSE(ls::ParseAnswer("[\"LOG-7K2Q\"]").has_value());
  EXPECT_FALSE(ls::ParseAnswer("\"LOG-7K2Q\"").has_value());
  EXPECT_FALSE(ls::ParseAnswer("{\"code\":7}").has_value());
  EXPECT_FALSE(ls::ParseAnswer("{\"code\":null}").has_value());
  EXPECT_FALSE(ls::ParseAnswer("{\"id\":\"LOG-7K2Q\"}").has_value());
  EXPECT_FALSE(ls::ParseAnswer("{\"code\":\"log-7k2q\"}").has_value());
  EXPECT_FALSE(ls::ParseAnswer("{\"code\":\"LOG-7K2I\"}").has_value());
  EXPECT_FALSE(ls::ParseAnswer("{\"code\":\"LOG-7K2Q\"").has_value());  // cut off
}

TEST(LogSendCode, AnAnswerOverTheLimitHasNone) {
  const std::string pad(ls::kMaxAnswerBytes, ' ');
  const std::string good = "{\"code\":\"LOG-7K2Q\"}";
  EXPECT_FALSE(ls::ParseAnswer(good + pad).has_value());
  const std::string atLimit = good + std::string(ls::kMaxAnswerBytes - good.size(), ' ');
  ASSERT_EQ(atLimit.size(), ls::kMaxAnswerBytes);
  EXPECT_TRUE(ls::ParseAnswer(atLimit).has_value());
}

// ---- consent and the answers ----

TEST_F(LogSend, NothingHappensWithoutTheTap) {
  auto c = Config("SECRET_LOG_TEXT", "p");
  const auto result = ls::Send(c, false);
  EXPECT_EQ(result.status, ls::Status::kNoConsent);
  EXPECT_TRUE(g_posted.bodies.empty());
  EXPECT_TRUE(g_lines.empty());
  EXPECT_TRUE(result.code.empty());
  EXPECT_EQ(result.files, 0u);
  EXPECT_EQ(result.bodyBytes, 0u);
}

TEST_F(LogSend, TheSameConfigurationSendsOnceThePlayerTaps) {
  auto c = Config();
  EXPECT_EQ(ls::Send(c, false).status, ls::Status::kNoConsent);
  EXPECT_TRUE(g_posted.bodies.empty());
  EXPECT_TRUE(g_lines.empty());
  EXPECT_EQ(ls::Send(c, true).status, ls::Status::kSent);  // the same config sends once the player taps
  EXPECT_EQ(g_posted.bodies.size(), 1u);
}

TEST_F(LogSend, ATapWithAGoodAnswerGivesTheCode) {
  g_answer = Accepted("LOG-7K2Q");
  const auto result = ls::Send(Config(), true);
  EXPECT_EQ(result.status, ls::Status::kSent);
  EXPECT_EQ(result.code, "LOG-7K2Q");
  EXPECT_EQ(result.httpStatus, 201);
  EXPECT_EQ(g_posted.bodies.size(), 1u);
}

TEST_F(LogSend, AnHttp200WithACodeIsAlsoSent) {
  g_answer = Accepted("LOG-0ABC");
  g_answer.status = 200;
  EXPECT_EQ(ls::Send(Config(), true).status, ls::Status::kSent);
}

TEST_F(LogSend, A404IsServiceNotReadyNeverSuccess) {
  g_answer = Status(404);
  g_answer.response = "{\"code\":\"LOG-7K2Q\"}";  // even a body that looks like an answer
  const auto result = ls::Send(Config(), true);
  EXPECT_EQ(result.status, ls::Status::kNotReady);
  EXPECT_NE(result.status, ls::Status::kSent);
  EXPECT_TRUE(result.code.empty());
  EXPECT_EQ(result.httpStatus, 404);
  EXPECT_NE(Line(ls::LogLevel::kWarning, "service_not_ready http=404"), nullptr);
}

TEST_F(LogSend, OtherStatusesAreRefusedWithNoCode) {
  for (long status : {400L, 401L, 413L, 429L, 500L, 503L, 302L, 199L, 300L}) {
    g_posted = Posted{};
    g_lines.clear();
    g_answer = Status(status);
    g_answer.response = "{\"code\":\"LOG-7K2Q\"}";
    const auto result = ls::Send(Config(), true);
    EXPECT_EQ(result.status, ls::Status::kRefused) << status;
    EXPECT_TRUE(result.code.empty()) << status;
    EXPECT_EQ(result.httpStatus, status);
    EXPECT_NE(Line(ls::LogLevel::kWarning, ("refused http=" + std::to_string(status)).c_str()), nullptr) << status;
  }
}

TEST_F(LogSend, EveryTwoHundredRangeStatusIsReadForACode) {
  g_answer = Accepted();
  g_answer.status = 299;
  EXPECT_EQ(ls::Send(Config(), true).status, ls::Status::kSent);
  g_answer.status = 200;
  EXPECT_EQ(ls::Send(Config(), true).status, ls::Status::kSent);
}

TEST_F(LogSend, A2xxWithoutAValidCodeIsNotSuccess) {
  for (const char* answer : {"", "{}", "{\"code\":\"log-7k2q\"}", "{\"code\":\"LOG-7K2L\"}", "<html>ok</html>"}) {
    g_lines.clear();
    g_answer = Accepted();
    g_answer.response = answer;
    const auto result = ls::Send(Config(), true);
    EXPECT_EQ(result.status, ls::Status::kBadAnswer) << answer;
    EXPECT_TRUE(result.code.empty());
    EXPECT_NE(Line(ls::LogLevel::kWarning, "bad_answer"), nullptr);
  }
}

TEST_F(LogSend, ATransportFailureCarriesItsCode) {
  g_answer = ls::PostResult{};
  g_answer.transportOk = false;
  g_answer.errorCode = 12029;
  const auto result = ls::Send(Config(), true);
  EXPECT_EQ(result.status, ls::Status::kTransportFailed);
  EXPECT_EQ(result.errorCode, 12029);
  EXPECT_TRUE(result.code.empty());
  EXPECT_NE(Line(ls::LogLevel::kWarning, "transport_failed error=12029"), nullptr);
}

TEST_F(LogSend, ATransportFailureIgnoresAnyStatusOrBodyItCarries) {
  g_answer = Accepted();
  g_answer.transportOk = false;
  EXPECT_EQ(ls::Send(Config(), true).status, ls::Status::kTransportFailed);
}

TEST_F(LogSend, OneTapIsOnePost) {
  g_answer = Status(500);
  ls::Send(Config(), true);
  EXPECT_EQ(g_posted.bodies.size(), 1u);  // no retry loop here: the player taps again
}

// ---- the configuration ----

TEST_F(LogSend, WithoutAnHttpsUrlATransportOrAnInstallIdNothingIsReadOrSent) {
  auto c = Config();
  c.url.clear();
  EXPECT_EQ(ls::Send(c, true).status, ls::Status::kNotConfigured);
  c = Config();
  c.url = "http://service.invalid/api/v3/player-logs";
  EXPECT_EQ(ls::Send(c, true).status, ls::Status::kNotConfigured);
  c = Config();
  c.post = nullptr;
  EXPECT_EQ(ls::Send(c, true).status, ls::Status::kNotConfigured);
  c = Config();
  c.installId = "";
  EXPECT_EQ(ls::Send(c, true).status, ls::Status::kNotConfigured);
  c = Config();
  c.installId = "ABCDEF0123456789ABCDEF0123456789";  // upper case is not an install id
  EXPECT_EQ(ls::Send(c, true).status, ls::Status::kNotConfigured);
  EXPECT_TRUE(g_posted.bodies.empty());
  EXPECT_EQ(g_lines.size(), 5u);
  for (const auto& l : g_lines) {
    EXPECT_EQ(l.level, ls::LogLevel::kWarning);
    EXPECT_NE(l.line.find("[NEVR.LOGSEND] log send not started status=not_configured reason="), std::string::npos) << l.line;
  }
}

TEST_F(LogSend, PlainHttpIsForTestsOnly) {
  auto c = Config();
  c.url = "http://127.0.0.1:1/x";
  c.allowPlainHttp = true;
  EXPECT_EQ(ls::Send(c, true).status, ls::Status::kSent);
  c.allowPlainHttp = false;
  EXPECT_EQ(ls::Send(c, true).status, ls::Status::kNotConfigured);
  c.url = "ftp://x";
  c.allowPlainHttp = true;
  EXPECT_EQ(ls::Send(c, true).status, ls::Status::kNotConfigured);
}

TEST_F(LogSend, TheTimeoutsReachThePost) {
  auto c = Config();
  ASSERT_EQ(ls::Send(c, true).status, ls::Status::kSent);
  ASSERT_EQ(g_posted.timeouts.size(), 1u);
  EXPECT_EQ(g_posted.timeouts[0].connectMs, 5000);  // the defaults
  EXPECT_EQ(g_posted.timeouts[0].totalMs, 20000);
  g_posted = Posted{};
  c.timeouts = {1234, 5678};
  ASSERT_EQ(ls::Send(c, true).status, ls::Status::kSent);
  EXPECT_EQ(g_posted.timeouts[0].connectMs, 1234);
  EXPECT_EQ(g_posted.timeouts[0].totalMs, 5678);
}

TEST_F(LogSend, ThePostContextReachesThePost) {
  static void* seen = nullptr;
  auto c = Config();
  int token = 0;
  c.postContext = &token;
  c.post = [](void* context, const std::string&, const std::string&, const nevr_crash_send::Timeouts&) {
    seen = context;
    return Accepted();
  };
  ASSERT_EQ(ls::Send(c, true).status, ls::Status::kSent);
  EXPECT_EQ(seen, &token);
}

TEST_F(LogSend, ItRunsWithoutALogSink) {
  auto c = Config();
  c.log = nullptr;
  EXPECT_EQ(ls::Send(c, true).status, ls::Status::kSent);
  c.url.clear();
  EXPECT_EQ(ls::Send(c, true).status, ls::Status::kNotConfigured);
}

TEST(LogSendConfig, TheBuiltInRouteIsHttps) {
  EXPECT_EQ(std::string(ls::kPlayerLogUrl).rfind("https://", 0), 0u);
  EXPECT_NE(std::string(ls::kPlayerLogUrl).find("/v3/player-logs"), std::string::npos);
}

TEST(LogSendConfig, TheLimitsAreTheAdrs) {
  EXPECT_EQ(ls::kSchema, 1);
  EXPECT_EQ(ls::kMaxFiles, 4u);
  EXPECT_EQ(ls::kMaxTailBytes, 262144u);
}

// ---- what is logged ----

TEST_F(LogSend, OneInfoLineNamesTheFilesAndTheirSizesAndNeverTheText) {
  const auto result = ls::Send(Config("SECRET_CURRENT_TEXT account=OVR-ORG-4242\n", "SECRET_PREVIOUS_TEXT"), true);
  ASSERT_EQ(result.status, ls::Status::kSent);
  const LoggedLine* collected = Line(ls::LogLevel::kInfo, "[NEVR.LOGSEND] log collected");
  ASSERT_NE(collected, nullptr);
  EXPECT_NE(collected->line.find("run.log:41:current:whole"), std::string::npos) << collected->line;
  EXPECT_NE(collected->line.find("run-prev.log:20:previous:whole"), std::string::npos) << collected->line;
  EXPECT_NE(collected->line.find("files=run.log:41:current:whole,run-prev.log:20:previous:whole body_bytes="), std::string::npos)
      << collected->line;
  EXPECT_NE(collected->line.find("body_bytes=" + std::to_string(result.bodyBytes)), std::string::npos) << collected->line;
}

TEST_F(LogSend, ACutFileIsMarkedCutInTheLine) {
  ASSERT_EQ(ls::Send(Config(std::string(ls::kMaxTailBytes + 5, 'x'), "p"), true).status, ls::Status::kSent);
  const LoggedLine* collected = Line(ls::LogLevel::kInfo, "log collected");
  ASSERT_NE(collected, nullptr);
  EXPECT_NE(collected->line.find("run.log:262144:current:cut"), std::string::npos) << collected->line;
}

TEST_F(LogSend, OneInfoLineHasTheStatusAndTheCode) {
  g_answer = Accepted("LOG-7K2Q");
  ASSERT_EQ(ls::Send(Config(), true).status, ls::Status::kSent);
  const LoggedLine* sent = Line(ls::LogLevel::kInfo, "[NEVR.LOGSEND] log send result");
  ASSERT_NE(sent, nullptr);
  EXPECT_NE(sent->line.find("status=sent"), std::string::npos);
  EXPECT_NE(sent->line.find("http=201"), std::string::npos);
  EXPECT_NE(sent->line.find("code=LOG-7K2Q"), std::string::npos);
  EXPECT_EQ(g_lines.size(), 2u);  // the collected line and this one
  EXPECT_EQ(g_lines[0].level, ls::LogLevel::kInfo);
  EXPECT_EQ(g_lines[1].level, ls::LogLevel::kInfo);
}

TEST_F(LogSend, NoLineCarriesTheLogTheBodyOrTheAnswerText) {
  const std::string secret = "SECRET_LOG_TEXT_alice_OVR-ORG-4242_SECRETTOKEN";
  g_answer = Accepted();
  g_answer.response = "{\"code\":\"log-bad\",\"detail\":\"ANSWER_BODY_TEXT\"}";
  for (int variant = 0; variant < 4; ++variant) {
    g_lines.clear();
    if (variant == 1) g_answer = Status(500), g_answer.response = "ANSWER_BODY_TEXT";
    if (variant == 2) g_answer = Status(404), g_answer.response = "ANSWER_BODY_TEXT";
    if (variant == 3) g_answer = Accepted(), g_answer.response = "{\"code\":\"LOG-7K2Q\"}";
    ls::Send(Config(secret, secret), true);
    ASSERT_FALSE(g_lines.empty());
    for (const auto& l : g_lines) {
      EXPECT_EQ(l.line.find("SECRET"), std::string::npos) << l.line;
      EXPECT_EQ(l.line.find("alice"), std::string::npos) << l.line;
      EXPECT_EQ(l.line.find("ANSWER_BODY_TEXT"), std::string::npos) << l.line;
      EXPECT_EQ(l.line.find("install_id"), std::string::npos) << l.line;
      EXPECT_EQ(l.line.rfind("[NEVR.LOGSEND] ", 0), 0u) << l.line;
    }
  }
}

TEST(LogSendStatus, EveryStatusHasADistinctName) {
  std::set<std::string> names;
  for (ls::Status s : {ls::Status::kNoConsent, ls::Status::kNotConfigured, ls::Status::kNoFiles, ls::Status::kTooLarge,
                       ls::Status::kTransportFailed, ls::Status::kNotReady, ls::Status::kRefused, ls::Status::kBadAnswer,
                       ls::Status::kSent}) {
    names.insert(ls::StatusName(s));
  }
  EXPECT_EQ(names.size(), 9u);
  EXPECT_EQ(names.count("unknown"), 0u);
  EXPECT_STREQ(ls::StatusName(ls::Status::kNoConsent), "no_consent");
  EXPECT_STREQ(ls::StatusName(ls::Status::kNotConfigured), "not_configured");
  EXPECT_STREQ(ls::StatusName(ls::Status::kNoFiles), "no_files");
  EXPECT_STREQ(ls::StatusName(ls::Status::kTooLarge), "too_large");
  EXPECT_STREQ(ls::StatusName(ls::Status::kTransportFailed), "transport_failed");
  EXPECT_STREQ(ls::StatusName(ls::Status::kNotReady), "service_not_ready");
  EXPECT_STREQ(ls::StatusName(ls::Status::kRefused), "refused");
  EXPECT_STREQ(ls::StatusName(ls::Status::kBadAnswer), "bad_answer");
  EXPECT_STREQ(ls::StatusName(ls::Status::kSent), "sent");
}
