// Log() knows the boot phase (#593). While the runtime boots under the DllMain loader lock, the
// game's CLog is not usable (#495), so Log() reports through the boot tee; once the tee closes, it
// reports through CLog. These tests link the real Log() (src/core/logging.cpp) and the real tee
// (src/runtime/log/boot_log_tee.cpp) and stand a recording function in for CLog.

#include <gtest/gtest.h>
#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>

#include "core/logging.h"
#include "runtime/log/boot_log_tee.h"

namespace {

int g_clogCalls = 0;
std::string g_clogLast;

VOID RecordingClog(EchoVR::LogLevel, UINT64, const CHAR* format, va_list args) {
  char buf[512];
  vsnprintf(buf, sizeof(buf), format, args);
  ++g_clogCalls;
  g_clogLast = buf;
}

// Points EchoVR::WriteLog (what InitializeFunctionPointers resolves to the game's CLog) at the recorder
// for one test, as it is during boot after the function pointers are resolved.
struct RecordingClogScope {
  EchoVR::WriteLogFunc* saved;
  RecordingClogScope() : saved(EchoVR::WriteLog) {
    EchoVR::WriteLog = &RecordingClog;
    g_clogCalls = 0;
    g_clogLast.clear();
  }
  ~RecordingClogScope() { EchoVR::WriteLog = saved; }
};

std::streamoff FileSize(const std::string& path) {
  std::ifstream in(path, std::ios::binary | std::ios::ate);
  return in ? static_cast<std::streamoff>(in.tellg()) : 0;
}

std::string ReadFrom(const std::string& path, std::streamoff start) {
  std::ifstream in(path, std::ios::binary);
  in.seekg(start);
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

}  // namespace

TEST(BootLogRouting, ALogCallDuringBootReachesTheTeeNotClog) {
  RecordingClogScope clog;
  nevr_boot_log_tee::Init();
  const std::string path = nevr_boot_log_tee::Path();
  ASSERT_FALSE(path.empty()) << "the boot tee could not resolve nevr-boot.jsonl";
  const std::streamoff start = FileSize(path);

  Log(EchoVR::LogLevel::Warning, "[NEVR.HEADLESS] boot routing probe id=%d", 593);

  const std::string written = ReadFrom(path, start);
  nevr_boot_log_tee::Close();
  EXPECT_EQ(g_clogCalls, 0) << "Log() during boot entered the game's CLog with: " << g_clogLast;
  // Rule 12: a boot line carries its level after the subsystem tag.
  EXPECT_NE(written.find("[NEVR.HEADLESS] warn; boot routing probe id=593"), std::string::npos)
      << "boot log was:\n" << written;
}

TEST(BootLogRouting, ALogCallAfterBootReachesClogNotTheTee) {
  RecordingClogScope clog;
  nevr_boot_log_tee::Init();
  const std::string path = nevr_boot_log_tee::Path();
  ASSERT_FALSE(path.empty()) << "the boot tee could not resolve nevr-boot.jsonl";
  nevr_boot_log_tee::Close();
  const std::streamoff start = FileSize(path);

  Log(EchoVR::LogLevel::Info, "[NEVR.HEADLESS] post-boot routing probe id=%d", 593);

  EXPECT_EQ(g_clogCalls, 1);
  EXPECT_EQ(g_clogLast, "[NEVR.HEADLESS] post-boot routing probe id=593");
  EXPECT_EQ(ReadFrom(path, start), "") << "Log() after boot still wrote to the boot file";
}
