// Host test for the script redirect (#1737): the name rules (src/quest/plugin/script_redirect.h), the all-or-nothing registry
// against a real temporary directory, the library-directory rules (an APK-mapped library is refused by name), the
// empty-registry pass-through, the redirected open (OpenRedirected, which the dlopen handler calls) against a fake loader,
// and the log lines each path writes. The handler in dlopen_hook.cpp itself is not linked here (it needs the whole
// integration build); its one call is the function this test drives.
//
// Built and run by `just test-quest-hooks`.
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <dlfcn.h>
#include <sys/stat.h>
#include <unistd.h>

#include "hook_log.h"
#include "quest/plugin/script_redirect.h"
#include "quest/tests/test_check.h"

namespace {

namespace sr = nevr_quest::script_redirect;

constexpr std::uint64_t kA = 0x37f88adb0d8066f7ULL;
constexpr std::uint64_t kB = 0xac4cd8e0a786d5c5ULL;
constexpr std::uint64_t kC = 0x0123456789abcdefULL;
const char kNameA[] = "lib37f88adb0d8066f7.so";
const char kNameB[] = "libac4cd8e0a786d5c5.so";
const char kNameC[] = "lib0123456789abcdef.so";

// Every line the host logs during the test, one per line, each as its level letter, '|', then the line.
std::string g_lines;

char LevelLetter(sentinel::LogLevel level) {
  switch (level) {
    case sentinel::LogLevel::kInfo: return 'I';
    case sentinel::LogLevel::kWarn: return 'W';
    case sentinel::LogLevel::kError: return 'E';
  }
  return '?';
}

void Capture(sentinel::LogLevel level, const char* line) {
  g_lines += LevelLetter(level);
  g_lines += '|';
  g_lines += line;
  g_lines += '\n';
}

// Captures like Capture, and clobbers errno the way a sink that writes a file can: the failed redirected open must still
// hand the game the errno the loader set.
void CaptureAndClobberErrno(sentinel::LogLevel level, const char* line) {
  Capture(level, line);
  errno = EIO;
}

bool Has(const char* fragment) { return g_lines.find(fragment) != std::string::npos; }

// The first captured line that carries `"event":"<event>"`, with its level letter; empty when none does.
std::string LineOf(const char* event) {
  const std::string key = std::string("\"event\":\"") + event + "\"";
  const std::size_t at = g_lines.find(key);
  if (at == std::string::npos) return std::string();
  const std::size_t previousEnd = at == 0 ? std::string::npos : g_lines.rfind('\n', at - 1);
  const std::size_t begin = previousEnd == std::string::npos ? 0 : previousEnd + 1;
  return g_lines.substr(begin, g_lines.find('\n', at) - begin);
}

// The level letter of the first captured line that carries `"event":"<event>"`, or '?' when none does.
char LevelOf(const char* event) {
  const std::string key = std::string("\"event\":\"") + event + "\"";
  const std::size_t at = g_lines.find(key);
  if (at == std::string::npos) return '?';
  const std::size_t previousEnd = at == 0 ? std::string::npos : g_lines.rfind('\n', at - 1);
  const std::size_t begin = previousEnd == std::string::npos ? 0 : previousEnd + 1;
  return g_lines[begin];
}

void NameRules() {
  std::uint64_t h = 0;
  QCHECK(sr::ParseScriptName(kNameC, &h) && h == kC);
  QCHECK(sr::ParseScriptName("/data/app/x/lib/arm64/lib0123456789abcdef.so", &h) && h == kC);
  QCHECK(sr::ParseScriptName("dir/lib0123456789abcdef.so", &h) && h == kC);
  QCHECK(sr::ParseScriptName("0123456789abcdef.so", &h) && h == kC);  // the bare form without lib
  QCHECK(!sr::ParseScriptName("lib0123456789ABCDEF.so", &h));          // lowercase hex only
  QCHECK(!sr::ParseScriptName("libfoo.so", &h));
  QCHECK(!sr::ParseScriptName("lib0123456789abcdef.so.bak", &h));
  QCHECK(!sr::ParseScriptName("lib0123456789abcdef", &h));
  QCHECK(!sr::ParseScriptName("lib0123456789abcdefxyz", &h));          // the suffix is .so exactly
  QCHECK(!sr::ParseScriptName("0123456789abcdefxyz", &h));             // the bare form's suffix too
  QCHECK(!sr::ParseScriptName("xyz0123456789abcdef.so", &h));          // the lib prefix is required with the long form
  QCHECK(!sr::ParseScriptName("lib0123456789abcde.so", &h));           // 15 digits
  QCHECK(!sr::ParseScriptName("lib0123456789abcdef_q15t.so", &h));     // the redirected name is not a stock name
  QCHECK(!sr::ParseScriptName("/", &h));
  QCHECK(!sr::ParseScriptName("", &h));
  QCHECK(!sr::ParseScriptName(nullptr, &h));
  QCHECK(!sr::ParseScriptName(kNameC, nullptr));

  char buf[sr::kNameCapacity];
  QCHECK(sr::RedirectName(kA, buf, sizeof buf));
  QCHECK(std::strcmp(buf, "lib37f88adb0d8066f7_q15t.so") == 0);
  QCHECK(!sr::RedirectName(kA, buf, 10));  // too small: the caller keeps the stock name
  QCHECK(!sr::RedirectName(kA, buf, 27));  // 27 characters and no room for the NUL: refused
  QCHECK(sr::RedirectName(kA, buf, 28));   // 27 characters and the NUL
}

void PassThroughWithNoRegistration() {
  sr::ResetForTest();
  g_lines.clear();
  sentinel::SetLogSink(&Capture);
  char buf[sr::kNameCapacity];
  QCHECK(sr::ArmedCount() == 0);
  QCHECK(sr::RewriteLoadName(kNameA, buf, sizeof buf) == kNameA);
  QCHECK(sr::RewriteLoadName(nullptr, buf, sizeof buf) == nullptr);
  // Every script-library load says whether a registry was set, so a run without one shows it.
  QCHECK(Has("\"event\":\"script_load\"") && Has("\"name\":\"lib37f88adb0d8066f7.so\""));
  QCHECK(Has("\"registry_set\":\"false\"") && Has("\"redirected\":\"false\""));
  QCHECK(LevelOf("script_load") == 'I');
  QCHECK(!Has("script_redirected"));
  // A name that is not a script library logs nothing.
  g_lines.clear();
  QCHECK(std::strcmp(sr::RewriteLoadName("libpnsovr.so", buf, sizeof buf), "libpnsovr.so") == 0);
  QCHECK(!Has("script_load"));
  sentinel::SetLogSink(nullptr);
}

void LibraryDirectories() {
  char dir[512];
  QCHECK(sr::LibraryDirectoryOf("/data/app/x/lib/arm64/libnevr.so", dir, sizeof dir) == nullptr);
  QCHECK(std::strcmp(dir, "/data/app/x/lib/arm64") == 0);
  // A library mapped out of the APK: named, not checked as a directory.
  const char* const inApk = sr::LibraryDirectoryOf("/data/app/x/base.apk!/lib/arm64-v8a/libnevr.so", dir, sizeof dir);
  QCHECK(inApk != nullptr && std::strstr(inApk, "library_inside_apk") != nullptr);
  QCHECK(sr::LibraryDirectoryOf("libnevr.so", dir, sizeof dir) != nullptr);  // no directory part
  QCHECK(sr::LibraryDirectoryOf(nullptr, dir, sizeof dir) != nullptr);
  QCHECK(sr::LibraryDirectoryOf("/a/b/libnevr.so", nullptr, sizeof dir) != nullptr);  // no buffer: refused, not written
  // The buffer: "/a" and its NUL need three bytes; two refuses.
  QCHECK(sr::LibraryDirectoryOf("/a/libnevr.so", dir, 2) != nullptr);
  QCHECK(sr::LibraryDirectoryOf("/a/libnevr.so", dir, 3) == nullptr && std::strcmp(dir, "/a") == 0);
}

// A directory of files named as given, created under $TMPDIR. Returns the path, or "" on failure.
std::string MakeDir(const char* const* files, std::size_t count) {
  const char* const base = std::getenv("TMPDIR");
  std::string tmpl = std::string(base != nullptr && base[0] != '\0' ? base : "/tmp") + "/q15t_redirect_XXXXXX";
  std::vector<char> pathBuf(tmpl.begin(), tmpl.end());
  pathBuf.push_back('\0');
  if (mkdtemp(pathBuf.data()) == nullptr) return std::string();
  const std::string dir = pathBuf.data();
  for (std::size_t i = 0; i < count; ++i) {
    const std::string path = dir + "/" + files[i];
    if (std::FILE* f = std::fopen(path.c_str(), "wb")) {
      std::fputs("aarch64", f);
      std::fclose(f);
    }
  }
  return dir;
}

void RemoveDir(const std::string& dir, const char* const* files, std::size_t count) {
  for (std::size_t i = 0; i < count; ++i) std::remove((dir + "/" + files[i]).c_str());
  rmdir(dir.c_str());
}

void AllOrNothingRegistry() {
  sr::ResetForTest();
  g_lines.clear();
  sentinel::SetLogSink(&Capture);

  const char* const all[] = {"lib37f88adb0d8066f7_q15t.so", "libac4cd8e0a786d5c5_q15t.so"};
  const std::string dir = MakeDir(all, 2);
  QCHECK(!dir.empty());
  const std::uint64_t pair[] = {kA, kB};

  // Both present: registered, and the loads of the two stock names open the redirected files.
  QCHECK(sr::RegisterIn(dir.c_str(), "q15t", pair, 2) == NEVR_SCRIPT_REDIRECT_OK);
  QCHECK(sr::ArmedCount() == 2);
  char buf[sr::kNameCapacity];
  QCHECK(std::strcmp(sr::RewriteLoadName(kNameA, buf, sizeof buf), "lib37f88adb0d8066f7_q15t.so") == 0);
  QCHECK(std::strcmp(sr::RewriteLoadName("/data/x/libac4cd8e0a786d5c5.so", buf, sizeof buf),
                     "libac4cd8e0a786d5c5_q15t.so") == 0);
  QCHECK(sr::RewriteLoadName(kNameC, buf, sizeof buf) == kNameC);           // not registered: passes through
  char tiny[8];
  QCHECK(sr::RewriteLoadName(kNameA, tiny, sizeof tiny) == kNameA);         // registered, but no room: stock name kept
  QCHECK(std::strcmp(sr::RewriteLoadName("libpnsovr.so", buf, sizeof buf), "libpnsovr.so") == 0);
  QCHECK(Has("\"event\":\"script_redirected\"") && Has("\"to\":\"lib37f88adb0d8066f7_q15t.so\""));
  QCHECK(LevelOf("script_redirected") == 'I');
  QCHECK(Has("\"name\":\"/data/x/libac4cd8e0a786d5c5.so\"") && Has("\"registry_set\":\"true\"") && Has("\"redirected\":\"true\""));
  QCHECK(Has("\"result\":\"ok\"") && Has("\"status\":\"armed\""));
  QCHECK(LevelOf("script_redirect") == 'I');

  // A second registration in the same process is refused, and the first one stands.
  QCHECK(sr::RegisterIn(dir.c_str(), "q15t", pair, 2) == NEVR_SCRIPT_REDIRECT_ALREADY_ARMED);
  QCHECK(sr::ArmedCount() == 2);

  // One file missing: nothing is registered, the missing name is named, and the stock name passes through.
  sr::ResetForTest();
  g_lines.clear();
  const std::uint64_t three[] = {kA, kB, kC};
  QCHECK(sr::RegisterIn(dir.c_str(), "q15t", three, 3) == NEVR_SCRIPT_REDIRECT_MISSING_LIBRARY);
  QCHECK(sr::ArmedCount() == 0);
  QCHECK(sr::RewriteLoadName(kNameA, buf, sizeof buf) == kNameA);
  QCHECK(Has("\"result\":\"missing_library\"") && Has("\"missing_count\":1"));
  QCHECK(Has("\"missing\":\"lib0123456789abcdef_q15t.so\""));
  QCHECK(LevelOf("script_redirect") == 'E');
  // A failed registration leaves the way open for a correct one.
  QCHECK(sr::RegisterIn(dir.c_str(), "q15t", pair, 2) == NEVR_SCRIPT_REDIRECT_OK);
  QCHECK(sr::ArmedCount() == 2);

  // Argument checks: refused before anything is looked at, each with its reason.
  sr::ResetForTest();
  g_lines.clear();
  const std::uint64_t dup[] = {kA, kA};
  QCHECK(sr::RegisterIn(dir.c_str(), "q15t", dup, 2) == NEVR_SCRIPT_REDIRECT_BAD_ARGUMENT);
  QCHECK(Has("\"reason\":\"duplicate_hash\"") && LevelOf("script_redirect") == 'E');
  QCHECK(sr::RegisterIn(dir.c_str(), "q15t", pair, 0) == NEVR_SCRIPT_REDIRECT_BAD_ARGUMENT);
  QCHECK(sr::RegisterIn(dir.c_str(), nullptr, pair, 2) == NEVR_SCRIPT_REDIRECT_BAD_ARGUMENT);
  QCHECK(sr::RegisterIn(dir.c_str(), "q15t", nullptr, 2) == NEVR_SCRIPT_REDIRECT_BAD_ARGUMENT);
  std::uint64_t seventeen[sr::kMaxEntries + 1] = {};
  for (std::uint64_t i = 0; i < sr::kMaxEntries + 1; ++i) seventeen[i] = i + 1;
  QCHECK(sr::RegisterIn(dir.c_str(), "q15t", seventeen, sr::kMaxEntries + 1) == NEVR_SCRIPT_REDIRECT_BAD_ARGUMENT);
  QCHECK(Has("\"reason\":\"argument_out_of_range\""));
  QCHECK(sr::ArmedCount() == 0);
  QCHECK(sr::RegisterIn(nullptr, "q15t", pair, 2) == NEVR_SCRIPT_REDIRECT_NO_LIBRARY_DIR);
  QCHECK(Has("\"reason\":\"no_library_directory\"") && Has("\"result\":\"no_library_dir\""));
  QCHECK(sr::ArmedCount() == 0);

  RemoveDir(dir, all, 2);
  sentinel::SetLogSink(nullptr);
  sr::ResetForTest();
}

// A directory named like a combat library is not one: the name is missing, and nothing is registered.
void DirectoryIsNotALibrary() {
  sr::ResetForTest();
  g_lines.clear();
  sentinel::SetLogSink(&Capture);
  const char* const present[] = {"libac4cd8e0a786d5c5_q15t.so"};
  const std::string dir = MakeDir(present, 1);
  QCHECK(!dir.empty());
  const std::string asDirectory = dir + "/lib37f88adb0d8066f7_q15t.so";
  QCHECK(mkdir(asDirectory.c_str(), 0700) == 0);
  const std::uint64_t pair[] = {kA, kB};
  QCHECK(sr::RegisterIn(dir.c_str(), "q15t", pair, 2) == NEVR_SCRIPT_REDIRECT_MISSING_LIBRARY);
  QCHECK(sr::ArmedCount() == 0);
  QCHECK(Has("\"missing\":\"lib37f88adb0d8066f7_q15t.so\"") && Has("\"missing_count\":1"));
  rmdir(asDirectory.c_str());
  RemoveDir(dir, present, 1);
  sentinel::SetLogSink(nullptr);
  sr::ResetForTest();
}

std::string g_openedName;  // the name the fake loader was asked to open
int g_openCalls = 0;

// A loader that opens: the handle is a non-null address, the name is recorded.
void* RecordingOpen(const char* name, int) {
  ++g_openCalls;
  g_openedName = name;
  return &g_openedName;
}

// A loader that fails the way dlopen does: dlerror() holds the loader's text and errno says why.
void* FailingOpen(const char* name, int) {
  ++g_openCalls;
  g_openedName = name;
  (void)dlopen("/nonexistent/q15t_redirect_test.so", RTLD_NOW);
  errno = EACCES;
  return nullptr;
}

// The redirected open (dlopen_hook.cpp's handler goes through it): the name opened, the failure line, and errno.
void OpenRedirectedPaths() {
  sr::ResetForTest();
  g_lines.clear();
  sentinel::SetLogSink(&CaptureAndClobberErrno);
  char buf[sr::kNameCapacity];

  // No registration: the stock name goes to the loader, and its failure is not a redirect failure.
  g_openCalls = 0;
  QCHECK(sr::OpenRedirected(&FailingOpen, kNameC, RTLD_NOW, buf, sizeof buf) == nullptr);
  QCHECK(g_openCalls == 1 && g_openedName == kNameC);
  QCHECK(!Has("script_redirect_failed"));

  const char* const both[] = {"lib37f88adb0d8066f7_q15t.so", "libac4cd8e0a786d5c5_q15t.so"};
  const std::string dir = MakeDir(both, 2);
  QCHECK(!dir.empty());
  const std::uint64_t pair[] = {kA, kB};
  QCHECK(sr::RegisterIn(dir.c_str(), "q15t", pair, 2) == NEVR_SCRIPT_REDIRECT_OK);

  // A registered name: the loader is asked for the combat name, and its handle comes back.
  g_lines.clear();
  g_openCalls = 0;
  QCHECK(sr::OpenRedirected(&RecordingOpen, kNameA, RTLD_NOW, buf, sizeof buf) == &g_openedName);
  QCHECK(g_openCalls == 1 && g_openedName == "lib37f88adb0d8066f7_q15t.so");
  QCHECK(LevelOf("script_redirected") == 'I' && Has("\"registry_set\":\"true\""));

  // A registered name whose redirected open fails: null, a failure line naming both names and the loader's text, and
  // errno as the loader left it (the sink above clobbers errno when it logs).
  g_lines.clear();
  errno = 0;
  QCHECK(sr::OpenRedirected(&FailingOpen, kNameA, RTLD_NOW, buf, sizeof buf) == nullptr);
  QCHECK(g_openedName == "lib37f88adb0d8066f7_q15t.so");
  const std::string failure = LineOf("script_redirect_failed");
  QCHECK(failure.find("\"from\":\"lib37f88adb0d8066f7.so\"") != std::string::npos);
  QCHECK(failure.find("\"to\":\"lib37f88adb0d8066f7_q15t.so\"") != std::string::npos);
  QCHECK(failure.find("cannot open shared object file") != std::string::npos);
  QCHECK(LevelOf("script_redirect_failed") == 'E');
  QCHECK(errno == EACCES);

  // An unregistered name in a registered process: passes through, and its failure is not a redirect failure.
  g_lines.clear();
  QCHECK(sr::OpenRedirected(&FailingOpen, kNameC, RTLD_NOW, buf, sizeof buf) == nullptr);
  QCHECK(g_openedName == kNameC && !Has("script_redirect_failed"));

  RemoveDir(dir, both, 2);
  sentinel::SetLogSink(nullptr);
  sr::ResetForTest();
}

// Register() looks in the directory the code was loaded from, which here is the test binary's own. That directory holds no
// combat library, so the registration is refused as missing, naming both, and nothing is armed. (The binary is run by a
// path with a directory part, as the recipe runs it.)
void RegisterFromOwnDirectory() {
  sr::ResetForTest();
  g_lines.clear();
  sentinel::SetLogSink(&Capture);
  const std::uint64_t pair[] = {kA, kB};
  const NevrScriptRedirectResult result = sr::Register("q15t", pair, 2);
  QCHECK(result == NEVR_SCRIPT_REDIRECT_MISSING_LIBRARY);
  QCHECK(sr::ArmedCount() == 0);
  QCHECK(Has("\"missing_count\":2") && Has("\"plugin\":\"q15t\""));

  // A library mapped out of the APK: refused by name, and nothing is armed.
  sr::ResetForTest();
  g_lines.clear();
  const NevrScriptRedirectResult inApk =
      sr::RegisterFromLibraryPath("q15t", "/data/app/x/base.apk!/lib/arm64-v8a/libnevr.so", pair, 2);
  QCHECK(inApk == NEVR_SCRIPT_REDIRECT_NO_LIBRARY_DIR);
  QCHECK(Has("library_inside_apk") && LevelOf("script_redirect") == 'E');
  QCHECK(sr::ArmedCount() == 0);

  // A library on disk: its directory is the one checked, and both names are there, so the registration is armed.
  const char* const both[] = {"lib37f88adb0d8066f7_q15t.so", "libac4cd8e0a786d5c5_q15t.so"};
  const std::string dir = MakeDir(both, 2);
  QCHECK(!dir.empty());
  sr::ResetForTest();
  QCHECK(sr::RegisterFromLibraryPath("q15t", (dir + "/libnevr.so").c_str(), pair, 2) == NEVR_SCRIPT_REDIRECT_OK);
  QCHECK(sr::ArmedCount() == 2);
  sr::ResetForTest();
  RemoveDir(dir, both, 2);

  sentinel::SetLogSink(nullptr);
  sr::ResetForTest();
}

void StateLine() {
  sr::ResetForTest();
  g_lines.clear();
  sentinel::SetLogSink(&Capture);
  sr::LogState();
  QCHECK(Has("\"event\":\"script_redirect\"") && Has("\"status\":\"pass-through\"") && Has("\"registry\":0"));
  sentinel::SetLogSink(nullptr);
}

void ResultNames() {
  QCHECK(std::strcmp(sr::ResultName(NEVR_SCRIPT_REDIRECT_OK), "ok") == 0);
  QCHECK(std::strcmp(sr::ResultName(NEVR_SCRIPT_REDIRECT_MISSING_LIBRARY), "missing_library") == 0);
  QCHECK(std::strcmp(sr::ResultName(NEVR_SCRIPT_REDIRECT_NO_LIBRARY_DIR), "no_library_dir") == 0);
}

}  // namespace

int main() {
  NameRules();
  LibraryDirectories();
  PassThroughWithNoRegistration();
  AllOrNothingRegistry();
  DirectoryIsNotALibrary();
  OpenRedirectedPaths();
  RegisterFromOwnDirectory();
  StateLine();
  ResultNames();
  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "script_redirect_test: %d check(s) failed\n", quest_test::Failures());
    return 1;
  }
  std::printf("script_redirect_test: all checks pass\n");
  return 0;
}
