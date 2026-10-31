// Tests for the crash report send (core/crash_send.h, #1689). The reports it reads are made by the real writer
// (core/crash_report.cpp), so a change to the report's format breaks here and not on a player's machine; the answers it
// acts on come from a fake POST. The service's own rules (strict decode, value patterns) are restated below from the
// contract agreed with the nevr-api side, and share no code with the sender.

#include "core/crash_send.h"

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

namespace {

// ---- a report made by the real writer ----

constexpr std::uint64_t kBase = 0x140000000ull;

bool PcResolve(std::uint64_t address, nevr_crash::Location* out) {
  if (address < kBase || address >= kBase + 0x2000000) return false;
  nevr_crash::CopyText(out->module, sizeof(out->module), "echovr.exe");
  nevr_crash::CopyText(out->build_id, sizeof(out->build_id), "pe-6452dff6-1d3a000");
  out->module_base = kBase;
  if (address >= kBase + 0x1000) {
    nevr_crash::CopyText(out->symbol, sizeof(out->symbol), "CSendEventNode::Enter");
    out->symbol_offset = address - (kBase + 0x1000);
  }
  return true;
}

// A PC report: an exception code in hex, no link register, a register that holds a token, a thread named for a person and
// a log tail that holds an account id, a path and a line shaped like a backtrace frame.
std::string MakeReport(bool withLr = false) {
  nevr_crash::ResetForTest();
  const char* secrets[] = {"login token=SECRETTOKEN123 for user=alice account=OVR-ORG-4242",
                           "C:\\Users\\alice\\AppData\\Local\\nevr\\echovr\\config.yaml loaded",
                           "#9 evil.dll+0x1 FORGED_FRAME_FROM_THE_LOG"};
  for (const char* line : secrets) nevr_crash::RecordLogLine(line, std::strlen(line));
  nevr_crash::Report report;
  std::memset(&report, 0, sizeof(report));
  report.signal_name = "EXCEPTION_ACCESS_VIOLATION";
  report.signal_number = 0xC0000005;
  report.signal_number_hex = true;
  report.fault_code = 0;
  report.fault_address = 8;
  report.has_fault_address = true;
  report.pc = kBase + 0x1234;
  report.lr = kBase + 0x2468;
  report.has_lr = withLr;
  report.registers[0].name = "rax";
  report.registers[0].value = 0xDEADBEEFCAFEF00Dull;
  report.register_count = 1;
  report.frames[0] = report.pc;
  report.frames[1] = kBase + 0x2468;
  report.frames[2] = 0x5555;  // in no module
  report.frame_count = 3;
  nevr_crash::CopyText(report.thread_name, sizeof(report.thread_name), "alice-private-thread");
  report.thread_id = 4242;
  static char buffer[nevr_crash::kReportCapacity];
  nevr_crash::FormatReport(report, &PcResolve, "C:\\Users\\alice\\crashes\\nevr-crash-1.report.txt", buffer, sizeof(buffer));
  return std::string(buffer);
}

// ---- the contract's allowlist, restated ----

const std::set<std::string> kTop = {"schema", "install_id", "report_id", "build", "platform", "headset",
                                    "signal", "pc", "lr", "backtrace"};
const std::set<std::string> kSignalKeys = {"name", "number", "code", "fault_address"};
const std::set<std::string> kAddressKeys = {"module", "offset", "build_id", "symbol", "symbol_offset"};
const std::set<std::string> kFrameKeys = {"module", "offset", "symbol"};

std::set<std::string> Keys(const json& object) {
  std::set<std::string> keys;
  for (auto it = object.begin(); it != object.end(); ++it) keys.insert(it.key());
  return keys;
}

std::set<std::string> Minus(const std::set<std::string>& a, const std::set<std::string>& b) {
  std::set<std::string> out;
  for (const std::string& key : a) {
    if (b.count(key) == 0) out.insert(key);
  }
  return out;
}

// The service's value rules: a body that breaks one is a 400, and a 400 is final.
void ExpectServiceAccepts(const json& body) {
  EXPECT_EQ(Minus(Keys(body), kTop), std::set<std::string>()) << body.dump();
  EXPECT_EQ(body.at("schema"), 1);
  EXPECT_TRUE(std::regex_match(body.at("install_id").get<std::string>(), std::regex("[0-9a-f]{32}")));
  EXPECT_TRUE(std::regex_match(body.at("report_id").get<std::string>(), std::regex("[A-Za-z0-9._-]{1,96}")));
  EXPECT_TRUE(std::regex_match(body.at("build").get<std::string>(), std::regex("[A-Za-z0-9._+-]{1,64}")));
  const std::string platform = body.at("platform");
  EXPECT_TRUE(platform == "pc" || platform == "quest");
  EXPECT_LE(body.at("headset").get<std::string>().size(), 64u);
  EXPECT_EQ(Keys(body.at("signal")), kSignalKeys);
  EXPECT_TRUE(body.at("signal").at("number").is_number_integer());
  EXPECT_TRUE(body.at("signal").at("code").is_number_integer());
  const std::regex hex("0x[0-9a-fA-F]{1,16}");
  const std::string fault = body.at("signal").at("fault_address");
  EXPECT_TRUE(fault.empty() || std::regex_match(fault, hex));
  auto address = [&](const json& a, const std::set<std::string>& keys) {
    EXPECT_EQ(Keys(a), keys) << a.dump();
    EXPECT_TRUE(std::regex_match(a.at("module").get<std::string>(), std::regex("[A-Za-z0-9_.+-]{1,128}"))) << a.dump();
    EXPECT_TRUE(std::regex_match(a.at("offset").get<std::string>(), hex)) << a.dump();
    const std::string symbol = a.at("symbol");
    EXPECT_LE(symbol.size(), 256u);
    EXPECT_EQ(symbol.find('/'), std::string::npos);
    EXPECT_EQ(symbol.find('\\'), std::string::npos);
  };
  address(body.at("pc"), kAddressKeys);
  EXPECT_TRUE(std::regex_match(body.at("pc").at("build_id").get<std::string>(), std::regex("[A-Za-z0-9._-]{1,128}")));
  if (body.contains("lr")) address(body.at("lr"), kAddressKeys);
  EXPECT_LE(body.at("backtrace").size(), 32u);
  for (const json& frame : body.at("backtrace")) address(frame, kFrameKeys);
}

// ---- the fake service and the crash folder ----

struct Posted {
  std::vector<std::string> urls;
  std::vector<std::string> bodies;
  std::vector<nevr_crash_send::Timeouts> timeouts;
};
Posted g_posted;
std::vector<nevr_crash_send::PostResult> g_answers;  // the answer to each POST in turn; the last repeats
std::vector<nevr_crash_send::LogEvent> g_events;
bool g_randomWorks = true;
int g_randomCalls = 0;

nevr_crash_send::PostResult FakePost(void*, const std::string& url, const std::string& body,
                                     const nevr_crash_send::Timeouts& timeouts) {
  g_posted.urls.push_back(url);
  g_posted.bodies.push_back(body);
  g_posted.timeouts.push_back(timeouts);
  const std::size_t n = g_posted.bodies.size() - 1;
  return g_answers.empty() ? nevr_crash_send::PostResult{true, 201, 0} : g_answers[std::min(n, g_answers.size() - 1)];
}

bool FakeRandom(std::uint8_t* bytes, std::size_t count) {
  ++g_randomCalls;
  if (!g_randomWorks) return false;
  for (std::size_t i = 0; i < count; ++i) bytes[i] = static_cast<std::uint8_t>(0xA0 + i);
  return true;
}

void FakeLog(const nevr_crash_send::LogEvent& e) { g_events.push_back(e); }

void WriteFile(const fs::path& path, const std::string& bytes) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

std::string ReadAll(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

class CrashSend : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = fs::temp_directory_path() / ("nevr-crash-send-test-" + std::to_string(NEVR_TEST_PID) + "-" +
                                        ::testing::UnitTest::GetInstance()->current_test_info()->name());
    std::error_code ec;
    fs::remove_all(dir_, ec);
    fs::create_directories(dir_);
    g_posted = Posted{};
    g_answers.clear();
    g_events.clear();
    g_randomWorks = true;
    g_randomCalls = 0;
  }
  void TearDown() override {
    std::error_code ec;
    fs::remove_all(dir_, ec);  // exactly the directory SetUp made
  }
  nevr_crash_send::Config Config() const {
    nevr_crash_send::Config c;
    c.dir = dir_;
    c.url = "https://service.invalid/api/v3/crash-reports";
    c.build = "5.7.1-dev.3+abc1234";
    c.platform = "pc";
    c.headset = "";
    c.post = &FakePost;
    c.random = &FakeRandom;
    c.log = &FakeLog;
    return c;
  }
  void Report(const std::string& stem, const std::string& text = MakeReport()) {
    WriteFile(dir_ / (stem + nevr_crash::kReportSuffix), text);
  }
  bool Exists(const std::string& name) const { return fs::exists(dir_ / name); }
  const nevr_crash_send::LogEvent* Event(const char* name) const {
    for (const auto& e : g_events) {
      if (std::strcmp(e.event, name) == 0) return &e;
    }
    return nullptr;
  }
  fs::path dir_;
};

}  // namespace

// ---- parse ----

TEST(CrashSendParse, ReadsTheRealWritersReport) {
  const auto parsed = nevr_crash_send::ParseReport(MakeReport(/*withLr=*/true));
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(parsed->signalName, "EXCEPTION_ACCESS_VIOLATION");
  EXPECT_EQ(parsed->signalNumber, 0xC0000005);
  EXPECT_EQ(parsed->signalCode, 0);
  EXPECT_EQ(parsed->faultAddress, "0x0000000000000008");
  EXPECT_EQ(parsed->pc.module, "echovr.exe");
  EXPECT_EQ(parsed->pc.offset, "0x1234");
  EXPECT_EQ(parsed->pc.buildId, "pe-6452dff6-1d3a000");
  EXPECT_EQ(parsed->pc.symbol, "CSendEventNode::Enter");
  EXPECT_EQ(parsed->pc.symbolOffset, "0x234");
  ASSERT_TRUE(parsed->lr.has_value());
  EXPECT_EQ(parsed->lr->offset, "0x2468");
  ASSERT_EQ(parsed->backtrace.size(), 3u);
  EXPECT_EQ(parsed->backtrace[0].module, "echovr.exe");
  EXPECT_EQ(parsed->backtrace[0].symbol, "CSendEventNode::Enter");
  EXPECT_EQ(parsed->backtrace[2].module, "unknown");  // the writer's `?` is not a module name the service takes
  EXPECT_EQ(parsed->backtrace[2].offset, "0x5555");
  EXPECT_EQ(parsed->backtrace[2].symbol, "");
}

TEST(CrashSendParse, AReportWithoutLrHasNone) {
  const auto parsed = nevr_crash_send::ParseReport(MakeReport(/*withLr=*/false));
  ASSERT_TRUE(parsed.has_value());
  EXPECT_FALSE(parsed->lr.has_value());
}

TEST(CrashSendParse, ALogLineShapedLikeAFrameIsNotAFrame) {
  const auto parsed = nevr_crash_send::ParseReport(MakeReport());
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(parsed->backtrace.size(), 3u);
  for (const auto& frame : parsed->backtrace) EXPECT_NE(frame.module, "evil.dll");
}

TEST(CrashSendParse, ASymbolWithAPathOrControlByteIsLeftOut) {
  const std::string text =
      "nevr crash report v1\nsignal: SIGSEGV (11) code=1 fault_addr=none\n"
      "pc: libr15.so+0x10 build_id=abcd symbol=C:\\Users\\alice\\x.dll+0x4\n"
      "backtrace:\n  #0 libr15.so+0x10 a/b+0x2\n  #1 /data/app/libr15.so+0x11 fn\nend of report\n";
  const auto parsed = nevr_crash_send::ParseReport(text);
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(parsed->pc.symbol, "");
  EXPECT_EQ(parsed->faultAddress, "");
  ASSERT_EQ(parsed->backtrace.size(), 2u);
  EXPECT_EQ(parsed->backtrace[0].symbol, "");
  EXPECT_EQ(parsed->backtrace[1].module, "unknown");  // a path is not a module name
}

TEST(CrashSendParse, ANegativeSiCodeIsKept) {
  const auto parsed = nevr_crash_send::ParseReport(
      "signal: SIGABRT (6) code=-6 fault_addr=none\npc: libc.so+0x1 build_id=none symbol=?\nend of report\n");
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(parsed->signalCode, -6);
}

TEST(CrashSendParse, AReportWithoutSignalOrPcIsNotSendable) {
  EXPECT_FALSE(nevr_crash_send::ParseReport("").has_value());
  EXPECT_FALSE(nevr_crash_send::ParseReport("nevr crash report v1\nsignal: SIGSEGV (11) code=1 fault_addr=none\n").has_value());
  EXPECT_FALSE(nevr_crash_send::ParseReport("pc: libr15.so+0x1 build_id=none symbol=?\n").has_value());
  EXPECT_FALSE(nevr_crash_send::ParseReport("signal: SIG SEGV (11) code=1 fault_addr=none\npc: a+0x1 build_id=x symbol=?\n").has_value());
}

TEST(CrashSendParse, AtMostThirtyTwoFrames) {
  std::string text = "signal: SIGSEGV (11) code=1 fault_addr=none\npc: a.so+0x1 build_id=none symbol=?\nbacktrace:\n";
  for (int i = 0; i < 40; ++i) text += "  #" + std::to_string(i) + " a.so+0x" + std::to_string(i + 1) + " f\n";
  const auto parsed = nevr_crash_send::ParseReport(text);
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(parsed->backtrace.size(), 32u);
}

// ---- the allowlist: the one test that fails when any other field appears ----

TEST(CrashSendBody, CarriesTheAllowlistAndNothingElse) {
  const std::string text = MakeReport(/*withLr=*/true);
  const auto parsed = nevr_crash_send::ParseReport(text);
  ASSERT_TRUE(parsed.has_value());
  const std::string body = nevr_crash_send::BuildBody(*parsed, "0123456789abcdef0123456789abcdef", "nevr-crash-run-1",
                                                       "5.7.1-dev.3+abc1234", "pc", "Quest 3");
  const json j = json::parse(body);
  ExpectServiceAccepts(j);
  EXPECT_EQ(Keys(j), kTop);  // every allowed key is present when the report has an lr
  // Nothing the report holds beyond the allowlist is in the body.
  for (const char* secret : {"SECRETTOKEN123", "alice", "OVR-ORG-4242", "AppData", "config.yaml", "FORGED_FRAME_FROM_THE_LOG",
                             "evil.dll", "DEADBEEF", "deadbeef", "private-thread", "tid", "4242", "rax", "registers", "Users"}) {
    EXPECT_EQ(body.find(secret), std::string::npos) << secret;
  }
}

TEST(CrashSendBody, AnUnknownKeyWouldBeCaught) {
  // The check the test above relies on: a body with one extra key is not the allowlist.
  json j = json::parse(nevr_crash_send::BuildBody(*nevr_crash_send::ParseReport(MakeReport()),
                                                  "0123456789abcdef0123456789abcdef", "r1", "5.7.0", "pc", ""));
  j["thread"] = "GameThread";
  EXPECT_NE(Minus(Keys(j), kTop), std::set<std::string>());
}

TEST(CrashSendBody, ABadBuildOrHeadsetIsReplacedNotSent) {
  const auto parsed = nevr_crash_send::ParseReport(MakeReport());
  const json j = json::parse(nevr_crash_send::BuildBody(*parsed, "0123456789abcdef0123456789abcdef", "r1",
                                                         "C:\\Users\\alice\\build", "quest", "alice's \"headset\"\n"));
  EXPECT_EQ(j.at("build"), "unknown");
  EXPECT_EQ(j.at("headset"), "");
  EXPECT_EQ(j.at("platform"), "quest");
}

TEST(CrashSendBody, TheBodyIsFarUnderTheServiceCap) {
  std::string text = "signal: SIGSEGV (11) code=1 fault_addr=none\npc: a.so+0x1 build_id=none symbol=?\nbacktrace:\n";
  for (int i = 0; i < 32; ++i) text += "  #" + std::to_string(i) + " module" + std::to_string(i) + ".so+0xffffffffffffffff " + std::string(250, 's') + "\n";
  const auto parsed = nevr_crash_send::ParseReport(text);
  ASSERT_TRUE(parsed.has_value());
  const std::string body = nevr_crash_send::BuildBody(*parsed, "0123456789abcdef0123456789abcdef", "r1", "5.7.0", "pc", "");
  EXPECT_LE(body.size(), nevr_crash_send::kMaxBodyBytes);
}

// ---- the send ----

TEST_F(CrashSend, SendsOneReportAndMarksItSent) {
  Report("nevr-crash-run-1");
  const auto outcome = nevr_crash_send::SendPending(Config());
  EXPECT_EQ(outcome.sent, 1u);
  ASSERT_EQ(g_posted.bodies.size(), 1u);
  EXPECT_EQ(g_posted.urls[0], "https://service.invalid/api/v3/crash-reports");
  ExpectServiceAccepts(json::parse(g_posted.bodies[0]));
  EXPECT_FALSE(Exists("nevr-crash-run-1.report.txt"));
  EXPECT_TRUE(Exists("nevr-crash-run-1.report.txt.sent"));
  ASSERT_NE(Event("sent"), nullptr);
  EXPECT_EQ(Event("sent")->report, "nevr-crash-run-1");
  EXPECT_EQ(Event("sent")->status, 201);
  ASSERT_NE(Event("pass"), nullptr);
}

TEST_F(CrashSend, ASecondStartSendsNothing) {
  Report("nevr-crash-run-1");
  nevr_crash_send::SendPending(Config());
  g_posted = Posted{};
  g_events.clear();
  nevr_crash_send::SendPending(Config());
  EXPECT_TRUE(g_posted.bodies.empty());
  EXPECT_TRUE(g_events.empty());  // a folder with nothing to send writes no record
}

TEST_F(CrashSend, AnInstallIdIsMadeOnceAndKept) {
  Report("a");
  nevr_crash_send::SendPending(Config());
  const std::string id = ReadAll(dir_ / nevr_crash_send::kInstallIdName);
  EXPECT_TRUE(std::regex_match(id, std::regex("[0-9a-f]{32}\n")));
  EXPECT_EQ(json::parse(g_posted.bodies[0]).at("install_id"), id.substr(0, 32));
  Report("b");
  nevr_crash_send::SendPending(Config());
  EXPECT_EQ(json::parse(g_posted.bodies[1]).at("install_id"), id.substr(0, 32));
  EXPECT_EQ(g_randomCalls, 1);
}

TEST_F(CrashSend, NoInstallIdFileIsMadeWhenThereIsNothingToSend) {
  nevr_crash_send::SendPending(Config());
  EXPECT_FALSE(Exists(nevr_crash_send::kInstallIdName));
  EXPECT_EQ(g_randomCalls, 0);
}

TEST_F(CrashSend, WithoutAnInstallIdNothingIsSent) {
  g_randomWorks = false;
  Report("a");
  const auto outcome = nevr_crash_send::SendPending(Config());
  EXPECT_TRUE(g_posted.bodies.empty());
  EXPECT_EQ(outcome.deferred, 1u);
  EXPECT_TRUE(Exists("a.report.txt"));
  EXPECT_NE(Event("install_id_unavailable"), nullptr);
}

TEST_F(CrashSend, TheMinidumpIsNeverReadOrSent) {
  Report("a");
  WriteFile(dir_ / "a.dmp", "MDMP-SECRET-STACK-MEMORY token=SECRETTOKEN123");
  nevr_crash_send::SendPending(Config());
  ASSERT_EQ(g_posted.bodies.size(), 1u);
  EXPECT_EQ(g_posted.bodies[0].find("MDMP"), std::string::npos);
  EXPECT_EQ(g_posted.bodies[0].find("SECRETTOKEN123"), std::string::npos);
  EXPECT_EQ(ReadAll(dir_ / "a.dmp"), "MDMP-SECRET-STACK-MEMORY token=SECRETTOKEN123");  // it stays, untouched
}

TEST_F(CrashSend, A400IsFinalAnd503IsNot) {
  Report("bad");
  g_answers = {{true, 400, 0}};
  const auto rejected = nevr_crash_send::SendPending(Config());
  EXPECT_EQ(rejected.rejected, 1u);
  EXPECT_TRUE(Exists("bad.report.txt.rejected"));
  EXPECT_FALSE(Exists("bad.report.txt"));
  ASSERT_NE(Event("rejected"), nullptr);

  g_events.clear();
  Report("later");
  g_answers = {{true, 503, 0}};
  const auto deferred = nevr_crash_send::SendPending(Config());
  EXPECT_EQ(deferred.deferred, 1u);
  EXPECT_TRUE(Exists("later.report.txt"));  // stays for the next start
  ASSERT_NE(Event("deferred"), nullptr);
  EXPECT_EQ(Event("deferred")->status, 503);
}

TEST_F(CrashSend, EachAnswerMeansWhatTheContractSays) {
  using nevr_crash_send::Classify;
  using nevr_crash_send::Verdict;
  EXPECT_EQ(Classify({true, 200, 0}), Verdict::kAccepted);  // a duplicate
  EXPECT_EQ(Classify({true, 201, 0}), Verdict::kAccepted);
  EXPECT_EQ(Classify({true, 400, 0}), Verdict::kRejected);
  EXPECT_EQ(Classify({true, 413, 0}), Verdict::kRejected);
  EXPECT_EQ(Classify({true, 422, 0}), Verdict::kRejected);
  EXPECT_EQ(Classify({true, 429, 0}), Verdict::kRetryLater);
  EXPECT_EQ(Classify({true, 404, 0}), Verdict::kRetryLater);  // the route is not deployed yet
  EXPECT_EQ(Classify({true, 500, 0}), Verdict::kRetryLater);
  EXPECT_EQ(Classify({true, 302, 0}), Verdict::kRetryLater);  // no redirect is followed; a page is not an answer
  EXPECT_EQ(Classify({false, 0, 12007}), Verdict::kRetryLater);
}

TEST_F(CrashSend, ADeadNetworkEndsThePassAtTheFirstReport) {
  Report("a");
  Report("b");
  Report("c");
  g_answers = {{false, 0, 12029}};
  const auto outcome = nevr_crash_send::SendPending(Config());
  EXPECT_EQ(g_posted.bodies.size(), 1u);  // not three timeouts in a row
  EXPECT_EQ(outcome.deferred, 1u);
  EXPECT_TRUE(Exists("a.report.txt") && Exists("b.report.txt") && Exists("c.report.txt"));
  ASSERT_NE(Event("deferred"), nullptr);
  EXPECT_EQ(Event("deferred")->errorCode, 12029);
}

TEST_F(CrashSend, AtMostThreeReportsAPass) {
  for (const char* stem : {"a", "b", "c", "d", "e"}) Report(stem);
  const auto outcome = nevr_crash_send::SendPending(Config());
  EXPECT_EQ(outcome.sent, 3u);
  EXPECT_EQ(g_posted.bodies.size(), 3u);
  ASSERT_NE(Event("pass"), nullptr);
  EXPECT_EQ(Event("pass")->bytes, 2u);  // two left for the next start
}

TEST_F(CrashSend, ReportsGoOldestFirst) {
  Report("new");
  Report("old");
  const auto now = fs::file_time_type::clock::now();
  fs::last_write_time(dir_ / "old.report.txt", now - std::chrono::hours(3));
  fs::last_write_time(dir_ / "new.report.txt", now - std::chrono::hours(1));
  nevr_crash_send::SendPending(Config());
  ASSERT_EQ(g_posted.bodies.size(), 2u);
  EXPECT_EQ(json::parse(g_posted.bodies[0]).at("report_id"), "old");
  EXPECT_EQ(json::parse(g_posted.bodies[1]).at("report_id"), "new");
}

TEST_F(CrashSend, AReportThatCannotBeReadIsSetAsideAndNeverSent) {
  Report("junk", "this is not a crash report\n");
  const auto outcome = nevr_crash_send::SendPending(Config());
  EXPECT_TRUE(g_posted.bodies.empty());
  EXPECT_EQ(outcome.rejected, 1u);
  EXPECT_TRUE(Exists("junk.report.txt.rejected"));
  ASSERT_NE(Event("unreadable"), nullptr);
}

TEST_F(CrashSend, AReportNameTheServiceWouldRefuseIsSetAside) {
  Report("has space");
  Report("plus+sign");
  nevr_crash_send::SendPending(Config());
  EXPECT_TRUE(g_posted.bodies.empty());
  EXPECT_TRUE(Exists("has space.report.txt.rejected"));
  EXPECT_TRUE(Exists("plus+sign.report.txt.rejected"));
}

TEST_F(CrashSend, OnlyReportFilesAreLookedAt) {
  WriteFile(dir_ / "x.dmp", "dump");
  WriteFile(dir_ / "x.dmp.maps", "maps");
  WriteFile(dir_ / "old.report.txt.sent", "sent");
  WriteFile(dir_ / "old2.report.txt.rejected", "rejected");
  WriteFile(dir_ / "notes.txt", "notes");
  fs::create_directory(dir_ / "dir.report.txt");  // a directory named like a report is not a regular file
  nevr_crash_send::SendPending(Config());
  EXPECT_TRUE(g_posted.bodies.empty());
  EXPECT_TRUE(g_events.empty());
}

TEST_F(CrashSend, OffWithoutAUrlAnHttpsUrlAFolderOrATransport) {
  Report("a");
  auto c = Config();
  c.url.clear();
  nevr_crash_send::SendPending(c);
  c = Config();
  c.url = "http://service.invalid/api/v3/crash-reports";
  nevr_crash_send::SendPending(c);
  c = Config();
  c.dir.clear();
  nevr_crash_send::SendPending(c);
  c = Config();
  c.post = nullptr;
  nevr_crash_send::SendPending(c);
  EXPECT_TRUE(g_posted.bodies.empty());
  EXPECT_EQ(g_events.size(), 4u);
  for (const auto& e : g_events) EXPECT_STREQ(e.event, "off");
  EXPECT_TRUE(Exists("a.report.txt"));
}

TEST_F(CrashSend, PlainHttpIsForTestsOnly) {
  Report("a");
  auto c = Config();
  c.url = "http://127.0.0.1:1/x";
  c.allowPlainHttp = true;
  nevr_crash_send::SendPending(c);
  EXPECT_EQ(g_posted.bodies.size(), 1u);
}

TEST_F(CrashSend, TheTimeoutsAreBoundedAndShrinkWithTheBudget) {
  Report("a");
  auto c = Config();
  c.timeouts = {3000, 6000};
  nevr_crash_send::SendPending(c);
  ASSERT_EQ(g_posted.timeouts.size(), 1u);
  EXPECT_LE(g_posted.timeouts[0].connectMs, 3000);
  EXPECT_LE(g_posted.timeouts[0].totalMs, 6000);
  EXPECT_GE(g_posted.timeouts[0].totalMs, 1000);
}

TEST_F(CrashSend, NoRequestStartsAfterTheBudget) {
  Report("a");
  auto c = Config();
  c.budget = std::chrono::milliseconds(0);
  nevr_crash_send::SendPending(c);
  EXPECT_TRUE(g_posted.bodies.empty());
  EXPECT_TRUE(Exists("a.report.txt"));
}

TEST_F(CrashSend, NoLogEventCarriesTheReportOrTheBody) {
  Report("a");
  nevr_crash_send::SendPending(Config());
  for (const auto& e : g_events) {
    const std::string all = std::string(e.event) + e.report + e.detail;
    EXPECT_EQ(all.find("SECRETTOKEN123"), std::string::npos);
    EXPECT_EQ(all.find("alice"), std::string::npos);
    EXPECT_EQ(all.find("install_id"), std::string::npos);
  }
}

TEST(CrashSendConfig, TheBuiltInRouteIsHttps) {
  EXPECT_EQ(std::string(nevr_crash_send::kCrashReportUrl).rfind("https://", 0), 0u);
}
