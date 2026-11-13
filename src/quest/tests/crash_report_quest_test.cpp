// Host test for the Quest crash report's address resolver (src/quest/sentinel/crash_report_quest.cpp, ResolveAddress): an address
// dladdr cannot name is left unresolved, with nothing written into the caller's Location. The resolver is in the file's
// anonymous namespace and the file is built only for the device (it includes android/log.h, which the host stub supplies), so
// this translation unit includes crash_report_quest.cpp itself; the platform table it feeds is not registered here.
// It also drives NameReportInSummary (#2129), which FinishCrashReport calls in the breakpad dump callback: the summary is
// rewritten to name the final file, and nothing is allocated while it is.
#include <dlfcn.h>
#include <csignal>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>

#include "core/crash_report.h"

// BuildFrames is used only by the device-only (aarch64) Describe, so the host compile leaves it unused; this translation unit
// compiles the file as the device does and does not change the file to silence it.
#pragma GCC diagnostic ignored "-Wunused-function"
#include "quest/sentinel/crash_report_quest.cpp"  // NOLINT: ResolveAddress is in its anonymous namespace
#pragma GCC diagnostic warning "-Wunused-function"

// The file's report path calls DumpDir (defined in sentinel.cpp, which this test does not link); the resolver does not call it.
// This answers the call so the file links, and names the folder the FinishCrashReport test writes its report in. Nothing
// here is a seam into the resolver.
static const char* g_dumpDir = "/nonexistent/crash";
namespace sentinel {
const char* DumpDir() { return g_dumpDir; }
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

// Every allocation path, counted while g_countAllocations is set: malloc, calloc and realloc (fopen's FILE and its buffer,
// and std::string's heap through operator new, go through them) and operator new itself. glibc's own entry points do the work.
extern "C" {
void* __libc_malloc(std::size_t size);
void* __libc_calloc(std::size_t count, std::size_t size);
void* __libc_realloc(void* pointer, std::size_t size);
}
constexpr std::size_t kReadChunkBytes = 256;
constexpr mode_t kTestFileMode = 0644;
constexpr mode_t kTestDirectoryMode = 0755;
constexpr mode_t kPermissionBits = 0777;
constexpr std::size_t kLongNameBytes = 200;  // a file or folder name under NAME_MAX (255)
static bool g_countAllocations = false;
static int g_allocations = 0;
extern "C" void* malloc(std::size_t size) noexcept {
  if (g_countAllocations) ++g_allocations;
  return __libc_malloc(size);
}
extern "C" void* calloc(std::size_t count, std::size_t size) noexcept {
  if (g_countAllocations) ++g_allocations;
  return __libc_calloc(count, size);
}
extern "C" void* realloc(void* pointer, std::size_t size) noexcept {
  if (g_countAllocations) ++g_allocations;
  return __libc_realloc(pointer, size);
}
void* operator new(std::size_t size) {
  if (g_countAllocations) ++g_allocations;
  void* pointer = __libc_malloc(size);
  if (pointer == nullptr) throw std::bad_alloc();
  return pointer;
}
void operator delete(void* pointer) noexcept { std::free(pointer); }
void operator delete(void* pointer, std::size_t) noexcept { std::free(pointer); }

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

// ---- NameReportInSummary (#2129) ----

void WriteText(const std::string& path, const std::string& text) {
  const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, kTestFileMode);
  CHECK(fd >= 0 && ::write(fd, text.data(), text.size()) == static_cast<ssize_t>(text.size()), "test file %s not written",
        path.c_str());
  if (fd >= 0) ::close(fd);
}

std::string ReadText(const std::string& path) {
  std::string text;
  const int fd = ::open(path.c_str(), O_RDONLY);
  if (fd < 0) return text;
  char chunk[kReadChunkBytes];
  ssize_t got = 0;
  while ((got = ::read(fd, chunk, sizeof(chunk))) > 0) text.append(chunk, static_cast<std::size_t>(got));
  ::close(fd);
  return text;
}

bool Exists(const std::string& path) {
  struct stat status {};
  return ::stat(path.c_str(), &status) == 0;
}

std::string Report(const std::string& named) {
  return "nevr crash report v1\nsummary: crash: SIGSEGV libr15.so+0x1a2b3c report=" + named + "\nbuild: b\n";
}

// The renamed report's summary names the file it now is, and nothing is allocated on the way.
void TheSummaryNamesTheFinalFileWithNoAllocation(const std::string& directory) {
  const std::string path = directory + "/final.report.txt";
  const std::string provisional = directory + "/nevr-crash-1-2.report.txt";
  WriteText(path, Report(provisional));
  g_allocations = 0;
  g_countAllocations = true;
  const bool named = sentinel::NameReportInSummary(path.c_str(), provisional.c_str(), path.c_str());
  g_countAllocations = false;
  CHECK(g_allocations == 0, "NameReportInSummary allocated %d time(s); the breakpad callback may not allocate", g_allocations);
  CHECK(named, "NameReportInSummary must succeed on a whole report");
  const std::string after = ReadText(path);
  CHECK(after == Report(path), "the summary must name the final file; got \"%s\"", after.c_str());
  CHECK(!Exists(path + ".naming"), "the sibling file must be renamed over the report, not left beside it");
  struct stat status {};
  const mode_t mask = ::umask(0);
  ::umask(mask);
  CHECK(::stat(path.c_str(), &status) == 0 && (status.st_mode & kPermissionBits) == (kTestFileMode & ~mask),
        "the rewritten report has the report's file mode (mode=%o)", static_cast<unsigned>(status.st_mode & kPermissionBits));
  ::unlink(path.c_str());
}

// A report as large as a report can be, renamed to a final path longer than the provisional one: the rewrite fits.
void AFullReportIsRenamedToALongerName(const std::string& directory) {
  const std::string provisional = directory + "/p.report.txt";
  const std::string path = directory + "/" + std::string(kLongNameBytes, 'f') + ".report.txt";
  const std::string head = Report(provisional);
  const std::string full = head + std::string(nevr_crash::kReportCapacity - head.size(), 'x');
  WriteText(path, full);
  CHECK(sentinel::NameReportInSummary(path.c_str(), provisional.c_str(), path.c_str()), "a full report is renamed");
  CHECK(ReadText(path) == Report(path) + std::string(nevr_crash::kReportCapacity - head.size(), 'x'),
        "a full report's summary names its longer final name");
  ::unlink(path.c_str());
}

// A summary that does not name the provisional path is not rewritten: the call succeeds and the file is as it was.
void AReportWithNothingToRenameIsLeftAlone(const std::string& directory) {
  const std::string path = directory + "/same.report.txt";
  WriteText(path, Report("none"));
  CHECK(sentinel::NameReportInSummary(path.c_str(), (directory + "/nevr-crash-3-4.report.txt").c_str(), path.c_str()),
        "a report with nothing to rename succeeds");
  CHECK(ReadText(path) == Report("none"), "a report with nothing to rename is unchanged");
  CHECK(!Exists(path + ".naming"), "a report with nothing to rename writes no sibling file");
  ::unlink(path.c_str());
}

// Each refusal returns false and leaves the report as it was (FinishCrashReport then logs that the summary still names the
// provisional file).
void ARefusedNamingLeavesTheReportWhole(const std::string& directory) {
  const std::string provisional = directory + "/nevr-crash-5-6.report.txt";
  const std::string path = directory + "/refused.report.txt";

  // A path the sibling name cannot hold, of a report that is there to read: folders nested until the path is longer than
  // any report path the callback forms (each name stays under the file system's limit).
  std::string deep = directory;
  while (deep.size() < sentinel::kReportPathBytes) {
    deep += "/" + std::string(kLongNameBytes, 'd');
    CHECK(::mkdir(deep.c_str(), kTestDirectoryMode) == 0, "test directory %s not made", deep.c_str());
  }
  const std::string longPath = deep + "/r.report.txt";
  WriteText(longPath, Report(provisional));
  CHECK(!sentinel::NameReportInSummary(longPath.c_str(), provisional.c_str(), longPath.c_str()), "a path too long is refused");
  CHECK(ReadText(longPath) == Report(provisional), "a path too long leaves the report unchanged");
  ::unlink(longPath.c_str());
  while (deep.size() > directory.size()) {
    ::rmdir(deep.c_str());
    deep.resize(deep.rfind('/'));
  }

  // A report that is not there, and one that cannot be read (a directory opens, and its read fails).
  CHECK(!sentinel::NameReportInSummary(path.c_str(), provisional.c_str(), path.c_str()), "a missing report is refused");
  CHECK(::mkdir(path.c_str(), kTestDirectoryMode) == 0, "test directory not made");
  CHECK(!sentinel::NameReportInSummary(path.c_str(), provisional.c_str(), path.c_str()), "an unreadable report is refused");
  ::rmdir(path.c_str());

  // A file larger than a report can be: left alone, whole.
  const std::string large = Report(provisional) + std::string(nevr_crash::kReportCapacity, 'x');
  WriteText(path, large);
  CHECK(!sentinel::NameReportInSummary(path.c_str(), provisional.c_str(), path.c_str()), "a file larger than a report is refused");
  CHECK(ReadText(path) == large, "a refused file larger than a report is unchanged");

  // A report whose rewrite would not fit the output buffer: the final name is longer than any path the callback forms.
  const std::string full = Report(provisional) + std::string(nevr_crash::kReportCapacity - Report(provisional).size(), 'x');
  WriteText(path, full);
  const std::string longFinal = directory + "/" + std::string(sentinel::kReportPathBytes * 2, 'f');
  CHECK(!sentinel::NameReportInSummary(path.c_str(), provisional.c_str(), longFinal.c_str()), "a rewrite too large is refused");
  CHECK(ReadText(path) == full, "a refused rewrite leaves the report unchanged");

  // The sibling file cannot be opened (a directory holds its name): refused, the report unchanged.
  WriteText(path, Report(provisional));
  CHECK(::mkdir((path + ".naming").c_str(), kTestDirectoryMode) == 0, "test directory not made");
  CHECK(!sentinel::NameReportInSummary(path.c_str(), provisional.c_str(), path.c_str()), "an unwritable sibling is refused");
  CHECK(ReadText(path) == Report(provisional), "a refused sibling leaves the report unchanged");
  ::rmdir((path + ".naming").c_str());
  ::unlink(path.c_str());
}

// FinishCrashReport after a handler wrote the report (#2129): the report is renamed beside the minidump and its summary names
// that file, not the provisional one the handler wrote. The shared writer runs with this file's platform, as the signal
// handler runs it; the crash state it keeps is once per process, so this is the only test here that writes a report.
void TheRenamedReportsSummaryNamesItsFinalFile(const std::string& directory) {
  g_dumpDir = directory.c_str();
  sentinel::RegisterCrashReportPlatform();
  nevr_crash::CrashInfo info{};
  info.code = SIGSEGV;
  info.thread_id = static_cast<std::uint64_t>(::syscall(SYS_gettid));
  CHECK(nevr_crash::WriteReport(info), "the handler's report must be written");
  const std::string provisional = nevr_crash::LastReportPath();
  CHECK(provisional.find(directory + "/nevr-crash-") == 0, "the handler writes under the provisional stem (%s)",
        provisional.c_str());
  CHECK(ReadText(provisional).find("report=" + provisional) != std::string::npos,
        "the handler's summary names the provisional file");

  const std::string dump = directory + "/0a1b2c3d-minidump.dmp";
  const std::string final = directory + "/0a1b2c3d-minidump.report.txt";
  char reportPath[sentinel::kReportPathBytes] = {};
  CHECK(sentinel::FinishCrashReport(dump.c_str(), reportPath, sizeof(reportPath)), "FinishCrashReport must find the report");
  CHECK(final == reportPath, "the report is beside the minidump; got %s", reportPath);
  CHECK(!Exists(provisional), "the provisional file is renamed away");
  const std::string text = ReadText(final);
  CHECK(text.find("summary: ") != std::string::npos && text.find("report=" + final) != std::string::npos,
        "the renamed report's summary names its final file");
  CHECK(text.find(provisional) == std::string::npos, "the renamed report no longer names the provisional file");
  ::unlink(final.c_str());
  g_dumpDir = "/nonexistent/crash";
}

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
  // #2129: NameReportInSummary, in the breakpad callback's conditions.
  // Under the build tree (gitignored; the test runs from the repository root), never the source tree or /tmp.
  char directoryTemplate[] = "build/quest-host-shared/crash-report-quest-XXXXXX";
  const char* directory = ::mkdtemp(directoryTemplate);
  CHECK(directory != nullptr, "mkdtemp failed");
  if (directory != nullptr) {
    TheSummaryNamesTheFinalFileWithNoAllocation(directory);
    AReportWithNothingToRenameIsLeftAlone(directory);
    AFullReportIsRenamedToALongerName(directory);
    TheRenamedReportsSummaryNamesItsFinalFile(directory);
    ARefusedNamingLeavesTheReportWhole(directory);
    ::rmdir(directory);
  }
  if (g_fail == 0) std::printf("crash_report_quest_test: all checks pass\n");
  else std::printf("crash_report_quest_test: %d failure(s)\n", g_fail);
  return g_fail == 0 ? 0 : 1;
}
