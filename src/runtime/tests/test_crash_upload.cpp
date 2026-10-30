// Tests for the crash report upload (compat/crash_upload.h, #1605). The frames it sends are read back by the small
// decoder below, which re-implements the game service's reader (nakama server/evr/login_remotelogset.go) and shares
// no code with the codec, as test_self_check.cpp does.

#include "runtime/compat/crash_upload.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <random>
#include <string>
#include <vector>

#include "runtime/compat/evr_codec.h"
#include "runtime/compat/self_check.h"

namespace fs = std::filesystem;

namespace {

struct Decoded {
  bool ok = false;
  uint64_t platform = 0;
  uint64_t account = 0;
  uint64_t level = 0;
  std::vector<std::string> logs;
};

uint64_t U64(const std::string& s, std::size_t at) {
  uint64_t v = 0;
  for (std::size_t i = 0; i < 8; ++i) v |= static_cast<uint64_t>(static_cast<unsigned char>(s.at(at + i))) << (8 * i);
  return v;
}
uint32_t U32(const std::string& s, std::size_t at) {
  uint32_t v = 0;
  for (std::size_t i = 0; i < 4; ++i) v |= static_cast<uint32_t>(static_cast<unsigned char>(s.at(at + i))) << (8 * i);
  return v;
}

// Mirrors nakama's RemoteLogSet.Stream (see test_self_check.cpp).
Decoded Decode(const std::string& frame) {
  Decoded d;
  if (frame.size() < 24) return d;
  const uint64_t length = U64(frame, 16);
  if (frame.size() != 24 + length) return d;
  const std::size_t base = 24;
  d.platform = U64(frame, base + 0);
  d.account = U64(frame, base + 8);
  d.level = U64(frame, base + 8 * 6);
  std::size_t at = base + 8 * 7;
  const uint64_t count = U64(frame, at);
  at += 8;
  if (count > 1024) return d;
  std::vector<uint32_t> offsets(static_cast<std::size_t>(count), 0);
  for (std::size_t i = 1; i < count; ++i) {
    offsets[i] = U32(frame, at);
    at += 4;
  }
  const std::size_t start = at;
  for (std::size_t i = 0; i < count; ++i) {
    std::size_t p = start + offsets[i];
    std::string s;
    while (p < frame.size() && frame[p] != '\0') s.push_back(frame[p++]);
    if (p >= frame.size()) return d;
    d.logs.push_back(std::move(s));
  }
  d.ok = true;
  return d;
}

std::string Unbase64(const std::string& text) {
  std::string out;
  uint32_t acc = 0;
  int bits = 0;
  for (char c : text) {
    int v;
    if (c >= 'A' && c <= 'Z') v = c - 'A';
    else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
    else if (c >= '0' && c <= '9') v = c - '0' + 52;
    else if (c == '+') v = 62;
    else if (c == '/') v = 63;
    else continue;  // '=' padding
    acc = (acc << 6) | static_cast<uint32_t>(v);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out.push_back(static_cast<char>((acc >> bits) & 0xff));
    }
  }
  return out;
}

struct Sent {
  std::vector<std::string> frames;
  bool accept = true;
};
Sent g_sent;
std::vector<nevr_crash_upload::LogEvent> g_events;

bool Sender(const std::string& frame) {
  if (!g_sent.accept) return false;
  g_sent.frames.push_back(frame);
  return true;
}
void Logger(const nevr_crash_upload::LogEvent& e) { g_events.push_back(e); }

std::size_t CountEvents(const std::string& name) {
  std::size_t n = 0;
  for (const auto& e : g_events) {
    if (name == e.event) ++n;
  }
  return n;
}

void WriteFile(const fs::path& path, const std::string& bytes) {
  std::ofstream out(path, std::ios::binary);
  out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

nevr_evr_codec::UserId User() {
  nevr_evr_codec::UserId user;
  user.platformCode = 4;  // OVR-ORG
  user.accountId = 4242;
  return user;
}

class CrashUpload : public ::testing::Test {
 protected:
  void SetUp() override {
    std::random_device gen;
    dir_ = fs::temp_directory_path() / ("nevr-crash-upload-" + std::to_string(gen()));
    ASSERT_TRUE(fs::create_directories(dir_));
    nevr_self_check::ResetForTest();
    nevr_crash_upload::ResetForTest();
    g_sent = Sent{};
    g_events.clear();
    nevr_self_check::SetEnabled(true);
    nevr_self_check::SetSender(&Sender);
    nevr_self_check::SetBuild("5.7.0");
    nevr_crash_upload::SetRetryIntervalForTest(std::chrono::milliseconds(0));
  }
  void TearDown() override {
    nevr_crash_upload::ResetForTest();
    nevr_self_check::ResetForTest();
    std::error_code ec;
    fs::remove_all(dir_, ec);  // exactly the directory SetUp made
  }
  void Install() {
    nevr_crash_upload::Config config;
    config.dir = dir_;
    config.build = "5.7.0";
    config.log = &Logger;
    nevr_crash_upload::Install(config);
  }
  void Report(const std::string& stem, const std::string& text) { WriteFile(dir_ / (stem + ".report.txt"), text); }
  void Dump(const std::string& stem, const std::string& bytes) { WriteFile(dir_ / (stem + ".dmp"), bytes); }
  // The strings of every frame sent, parsed. Every one must be a JSON object.
  std::vector<nlohmann::json> Strings() const {
    std::vector<nlohmann::json> out;
    for (const std::string& frame : g_sent.frames) {
      const Decoded d = Decode(frame);
      EXPECT_TRUE(d.ok);
      for (const std::string& log : d.logs) {
        const nlohmann::json j = nlohmann::json::parse(log, nullptr, false);
        EXPECT_TRUE(j.is_object()) << log.substr(0, 80);
        out.push_back(j);
      }
    }
    return out;
  }
  fs::path dir_;
};

}  // namespace

TEST_F(CrashUpload, FindsReportsOldestFirstAndPairsTheMinidump) {
  Report("a", "report a");
  Dump("a", "dump a");
  Report("b", "report b");  // no minidump
  Report("c.dmp", "report c");  // named after its dump
  Dump("c", "dump c");
  WriteFile(dir_ / "d.report.txt.sent", "already sent");
  WriteFile(dir_ / "e.dmp", "a dump with no report");
  const auto now = fs::file_time_type::clock::now();
  fs::last_write_time(dir_ / "a.report.txt", now - std::chrono::hours(3));
  fs::last_write_time(dir_ / "c.dmp.report.txt", now - std::chrono::hours(2));
  fs::last_write_time(dir_ / "b.report.txt", now - std::chrono::hours(1));

  const auto pending = nevr_crash_upload::FindPending(dir_);
  ASSERT_EQ(pending.size(), 3u);
  EXPECT_EQ(pending[0].stem, "a");
  EXPECT_EQ(pending[0].minidump.filename().string(), "a.dmp");
  EXPECT_EQ(pending[1].stem, "c.dmp");
  EXPECT_EQ(pending[1].minidump.filename().string(), "c.dmp");
  EXPECT_EQ(pending[2].stem, "b");
  EXPECT_TRUE(pending[2].minidump.empty());
}

TEST_F(CrashUpload, SendsTheReportAndTheMinidumpOnceAndMarksIt) {
  const std::string text = "signal 11 (SIGSEGV) fault_addr=0x0\npc libr15.so+0x1234\n";
  std::string dump(5000, '\0');
  for (std::size_t i = 0; i < dump.size(); ++i) dump[i] = static_cast<char>(i * 7);
  Report("crash1", text);
  Dump("crash1", dump);

  Install();
  EXPECT_EQ(CountEvents("armed"), 1u);
  nevr_self_check::Flush();  // not logged in: nothing is sent, the report stays
  EXPECT_TRUE(g_sent.frames.empty());
  EXPECT_TRUE(fs::exists(dir_ / "crash1.report.txt"));

  nevr_self_check::SetLoggedIn(true, User());
  nevr_self_check::Flush();
  ASSERT_EQ(g_sent.frames.size(), 1u);
  const Decoded d = Decode(g_sent.frames[0]);
  ASSERT_TRUE(d.ok);
  EXPECT_EQ(d.platform, 4u);
  EXPECT_EQ(d.account, 4242u);
  EXPECT_EQ(d.level, nevr_self_check::kRemoteLogLevelError);

  std::string gotText;
  std::string gotDump;
  for (const nlohmann::json& j : Strings()) {
    EXPECT_EQ(j.at("message").get<std::string>(), "nevr_crash_report");
    EXPECT_EQ(j.at("message_type").get<std::string>(), "NEVR_CRASH_REPORT");
    EXPECT_EQ(j.at("userid").get<std::string>(), "OVR-ORG-4242");
    EXPECT_EQ(j.at("report_id").get<std::string>(), "crash1");
    EXPECT_EQ(j.at("build").get<std::string>(), "5.7.0");
    if (j.at("part").get<std::string>() == "report") {
      gotText += j.at("data").get<std::string>();
    } else {
      EXPECT_EQ(j.at("part").get<std::string>(), "minidump");
      EXPECT_EQ(j.at("encoding").get<std::string>(), "base64");
      EXPECT_EQ(j.at("bytes").get<std::size_t>(), dump.size());
      gotDump += j.at("data").get<std::string>();
    }
  }
  EXPECT_EQ(gotText, text);
  EXPECT_EQ(Unbase64(gotDump), dump);

  // The mark: the report is renamed, the minidump stays, and the next Flush and the next session send nothing.
  EXPECT_FALSE(fs::exists(dir_ / "crash1.report.txt"));
  EXPECT_TRUE(fs::exists(dir_ / "crash1.report.txt.sent"));
  EXPECT_TRUE(fs::exists(dir_ / "crash1.dmp"));
  nevr_self_check::Flush();
  nevr_self_check::SetLoggedIn(false);
  nevr_self_check::SetLoggedIn(true, User());
  nevr_self_check::Flush();
  EXPECT_EQ(g_sent.frames.size(), 1u);

  ASSERT_EQ(CountEvents("sent"), 1u);
  for (const auto& e : g_events) {
    if (std::string(e.event) != "sent") continue;
    EXPECT_EQ(e.report, "crash1");
    EXPECT_EQ(e.reportBytes, text.size());
    EXPECT_EQ(e.minidump, "included");
    EXPECT_EQ(e.minidumpBytes, dump.size());
  }
}

TEST_F(CrashUpload, ARefusedFrameKeepsTheReportAndTheNextPassSendsIt) {
  Report("r1", "report");
  Install();
  nevr_self_check::SetLoggedIn(true, User());
  g_sent.accept = false;
  nevr_self_check::Flush();
  EXPECT_TRUE(g_sent.frames.empty());
  EXPECT_TRUE(fs::exists(dir_ / "r1.report.txt"));
  EXPECT_EQ(CountEvents("not_sent"), 1u);
  EXPECT_EQ(CountEvents("sent"), 0u);

  g_sent.accept = true;
  nevr_self_check::Flush();
  EXPECT_EQ(g_sent.frames.size(), 1u);
  EXPECT_TRUE(fs::exists(dir_ / "r1.report.txt.sent"));
  EXPECT_EQ(CountEvents("sent"), 1u);
}

TEST_F(CrashUpload, TheRetryWaitsForItsInterval) {
  Report("r1", "report");
  Install();
  nevr_crash_upload::SetRetryIntervalForTest(std::chrono::hours(1));
  nevr_self_check::SetLoggedIn(true, User());
  g_sent.accept = false;
  nevr_self_check::Flush();
  g_sent.accept = true;
  nevr_self_check::Flush();  // inside the interval: no second attempt
  EXPECT_TRUE(g_sent.frames.empty());
  EXPECT_EQ(CountEvents("not_sent"), 1u);
}

TEST_F(CrashUpload, AMinidumpOverTheLimitStaysLocalAndTheReportStillGoes) {
  Report("big", "report");
  Dump("big", std::string(nevr_crash_upload::kMaxMinidumpBytes + 1, 'x'));
  Install();
  nevr_self_check::SetLoggedIn(true, User());
  nevr_self_check::Flush();
  ASSERT_EQ(g_sent.frames.size(), 1u);
  for (const nlohmann::json& j : Strings()) EXPECT_EQ(j.at("part").get<std::string>(), "report");
  for (const auto& e : g_events) {
    if (std::string(e.event) == "sent") {
      EXPECT_EQ(e.minidump, "too_large");
      EXPECT_EQ(e.minidumpBytes, 0u);
    }
  }
}

TEST_F(CrashUpload, ALongReportIsCutAtACharacterAndStaysInsideTheServiceLimits) {
  std::string text;
  while (text.size() < nevr_crash_upload::kMaxReportBytes * 2) text += "frame \xc3\xa9\xe2\x82\xac end\n";  // 2- and 3-byte characters
  Report("long", text);
  Install();
  nevr_self_check::SetLoggedIn(true, User());
  nevr_self_check::Flush();
  ASSERT_EQ(g_sent.frames.size(), 1u);
  const Decoded d = Decode(g_sent.frames[0]);
  ASSERT_TRUE(d.ok);
  EXPECT_LE(d.logs.size(), 50u);
  std::size_t total = 0;
  for (const std::string& log : d.logs) total += log.size();
  EXPECT_LE(total, 256u * 1024u);
  std::string got;
  for (const nlohmann::json& j : Strings()) got += j.at("data").get<std::string>();
  EXPECT_LE(got.size(), nevr_crash_upload::kMaxReportBytes);
  EXPECT_EQ(got, text.substr(0, got.size()));  // a prefix, with no cut character (a cut one would not match)
  EXPECT_EQ(got.find('\xef'), std::string::npos);  // no replacement character (U+FFFD starts with 0xEF)
  for (const auto& e : g_events) {
    if (std::string(e.event) == "sent") EXPECT_EQ(e.reportBytes, text.size());
  }
}

TEST_F(CrashUpload, AReportThatEscapesToSixTimesItsSizeStillFitsTheFrame) {
  Report("ctl", std::string(nevr_crash_upload::kMaxReportBytes, '\x01'));  // each byte is six bytes of JSON
  Dump("ctl", std::string(nevr_crash_upload::kMaxMinidumpBytes, 'm'));
  Install();
  nevr_self_check::SetLoggedIn(true, User());
  nevr_self_check::Flush();
  ASSERT_EQ(g_sent.frames.size(), 1u);
  const Decoded d = Decode(g_sent.frames[0]);
  ASSERT_TRUE(d.ok);
  std::size_t total = 0;
  for (const std::string& log : d.logs) total += log.size();
  EXPECT_LE(total, nevr_crash_upload::kMaxSetPayloadBytes);
  EXPECT_LE(d.logs.size(), 50u);
}

TEST_F(CrashUpload, AtMostThreeReportsPerPassAndTheRestFollow) {
  for (int i = 0; i < 5; ++i) Report("c" + std::to_string(i), "report " + std::to_string(i));
  Install();
  nevr_self_check::SetLoggedIn(true, User());
  nevr_self_check::Flush();
  EXPECT_EQ(g_sent.frames.size(), nevr_crash_upload::kMaxReportsPerPass);
  nevr_self_check::Flush();
  EXPECT_EQ(g_sent.frames.size(), 5u);
  nevr_self_check::Flush();
  EXPECT_EQ(g_sent.frames.size(), 5u);
  EXPECT_EQ(nevr_crash_upload::FindPending(dir_).size(), 0u);
}

TEST_F(CrashUpload, AReportWrittenLaterIsFoundByTheNextSession) {
  Install();
  nevr_self_check::SetLoggedIn(true, User());
  nevr_self_check::Flush();
  EXPECT_TRUE(g_sent.frames.empty());
  Report("late", "written after the first pass");
  nevr_self_check::Flush();  // this session already looked
  EXPECT_TRUE(g_sent.frames.empty());
  nevr_self_check::SetLoggedIn(false);
  nevr_self_check::SetLoggedIn(true, User());
  nevr_self_check::Flush();
  EXPECT_EQ(g_sent.frames.size(), 1u);
}

TEST_F(CrashUpload, NoCrashDirectoryTurnsItOffAndSaysSo) {
  nevr_crash_upload::Config config;
  config.log = &Logger;
  nevr_crash_upload::Install(config);
  EXPECT_EQ(CountEvents("off"), 1u);
  EXPECT_EQ(CountEvents("armed"), 0u);
  nevr_self_check::SetLoggedIn(true, User());
  nevr_self_check::Flush();
  EXPECT_TRUE(g_sent.frames.empty());
}

TEST_F(CrashUpload, AFolderNamedLikeAReportIsNotAReport) {
  fs::create_directory(dir_ / "dir.report.txt");  // a directory named like a report is not a regular file
  nevr_self_check::SetLoggedIn(true, User());
  Install();
  nevr_self_check::Flush();
  EXPECT_TRUE(g_sent.frames.empty());
  EXPECT_EQ(CountEvents("sent"), 0u);
}
