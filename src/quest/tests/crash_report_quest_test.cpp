// Host test for the Quest crash report's address resolver (src/quest/sentinel/crash_report_quest.cpp, ResolveAddress): an address
// dladdr cannot name is left unresolved, with nothing written into the caller's Location. The resolver is in the file's
// anonymous namespace and the file is built only for the device (it includes android/log.h, which the host stub supplies), so
// this translation unit includes crash_report_quest.cpp itself; the platform table it feeds is not registered here.
#include <dlfcn.h>
#include <csignal>

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "core/crash_report.h"

// BuildFrames is used only by the device-only (aarch64) Describe, so the host compile leaves it unused; this translation unit
// compiles the file as the device does and does not change the file to silence it.
#pragma GCC diagnostic ignored "-Wunused-function"
#include "quest/sentinel/crash_report_quest.cpp"  // NOLINT: ResolveAddress is in its anonymous namespace
#pragma GCC diagnostic warning "-Wunused-function"

// The file's report path calls DumpDir (defined in sentinel.cpp, which this test does not link); the resolver does not call it.
// This answers the call so the file links. Nothing here is a seam into the resolver.
namespace sentinel {
const char* DumpDir() { return "/nonexistent/crash"; }
}  // namespace sentinel

extern "C" int __android_log_write(int, const char*, const char*) { return 1; }
extern "C" int __android_log_print(int, const char*, const char*, ...) { return 1; }

// dladdr, as ResolveAddress asks it. g_dladdrMode 0 is libc's answer. 1 returns 0 and names a file and a base, so only the
// return code refuses it. 2 returns 1 and names no file. 3 returns 1 and names a file with no base. This definition takes the
// place of libc's for the calls this executable makes.
static int g_dladdrMode = 0;
extern "C" int dladdr(const void* address, Dl_info* info) noexcept {
  using RealDladdr = int (*)(const void*, Dl_info*);
  static const RealDladdr real = reinterpret_cast<RealDladdr>(dlsym(RTLD_NEXT, "dladdr"));
  if (g_dladdrMode == 0) return real(address, info);
  info->dli_fname = g_dladdrMode == 2 ? nullptr : "/nonexistent/libnevr.so";
  info->dli_fbase = g_dladdrMode == 3 ? nullptr : reinterpret_cast<void*>(std::uintptr_t{0x400000});
  return g_dladdrMode == 1 ? 0 : 1;
}

static int g_fail = 0;
#define CHECK(cond, ...)                                  \
  do {                                                    \
    if (!(cond)) {                                        \
      ++g_fail;                                           \
      std::printf("FAIL %s:%d: ", __FILE__, __LINE__);   \
      std::printf(__VA_ARGS__);                           \
      std::printf("\n");                                  \
    }                                                     \
  } while (0)

int main() {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  // dladdr fails in each way it can: the resolver refuses (false) and writes nothing into the Location
  for (int mode : {1, 2, 3}) {
    nevr_crash::Location loc;
    std::memset(&loc, 0, sizeof loc);
    g_dladdrMode = mode;
    const bool resolved = sentinel::ResolveAddress(0x1234, &loc);  // any address: the fake does not look it up
    g_dladdrMode = 0;
    CHECK(!resolved, "mode %d: ResolveAddress must refuse when dladdr cannot name the address", mode);
    CHECK(loc.module[0] == '\0' && loc.module_base == 0 && loc.symbol[0] == '\0',
          "mode %d: a refused address must leave the Location empty (module=\"%s\" base=%llu)", mode, loc.module,
          static_cast<unsigned long long>(loc.module_base));
  }
  // positive control, real dladdr: this executable's own code resolves to a named module with a base
  {
    nevr_crash::Location loc;
    std::memset(&loc, 0, sizeof loc);
    const bool resolved = sentinel::ResolveAddress(reinterpret_cast<std::uint64_t>(&sentinel::ResolveAddress), &loc);
    CHECK(resolved && loc.module[0] != '\0' && loc.module_base != 0,
          "real dladdr: ResolveAddress must resolve this executable's code (resolved=%d module=\"%s\")", resolved ? 1 : 0, loc.module);
  }
  if (g_fail == 0) std::printf("crash_report_quest_test: all checks pass\n");
  else std::printf("crash_report_quest_test: %d failure(s)\n", g_fail);
  return g_fail == 0 ? 0 : 1;
}
