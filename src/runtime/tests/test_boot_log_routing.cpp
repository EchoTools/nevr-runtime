// Log() knows the boot phase (#593). While the runtime boots under the DllMain loader lock, the
// game's CLog is not usable (#495), so Log() reports through the boot tee; once the tee closes, it
// reports through CLog. These tests link the real Log() (src/core/logging.cpp) and the real tee
// (src/runtime/log/boot_log_tee.cpp) and stand a recording function in for CLog.

#include <gtest/gtest.h>
#include <winsock2.h>
#include <windows.h>
#include <fcntl.h>
#include <io.h>

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

// Runs `fn` with the process's stderr (fd 2) pointed at a scratch file and returns what it wrote.
template <typename Fn>
std::string CaptureStderr(Fn fn) {
  char dir[MAX_PATH];
  char path[MAX_PATH];
  GetTempPathA(sizeof(dir), dir);
  GetTempFileNameA(dir, "tee", 0, path);
  std::fflush(stderr);
  const int saved = _dup(2);
  const int fd = _open(path, _O_WRONLY | _O_TRUNC | _O_BINARY);
  _dup2(fd, 2);
  _close(fd);
  fn();
  std::fflush(stderr);
  _dup2(saved, 2);
  _close(saved);
  const std::string out = ReadFrom(path, 0);
  DeleteFileA(path);
  return out;
}

}  // namespace

// #653: the first stderr line the tee writes ends in a newline, so the next stderr line starts on its own.
TEST(BootLogRouting, TheOpeningLineEndsInANewlineOnStderr) {
  const std::string err = CaptureStderr([] { nevr_boot_log_tee::Init(); });
  nevr_boot_log_tee::Close();
  const std::string::size_type at = err.find("[NEVR.PATCH] boot log opened run=");
  ASSERT_NE(at, std::string::npos) << "stderr was:\n" << err;
  const std::string::size_type end = err.find('\n', at);
  ASSERT_NE(end, std::string::npos) << "the opening line has no newline; stderr was:\n" << err;
  const std::string::size_type next = err.find("[NEVR", at + 1);
  if (next != std::string::npos) {
    EXPECT_EQ(err[next - 1], '\n') << "the next line runs onto the opening line:\n" << err;
  }
}

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
