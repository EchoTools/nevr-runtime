// Host test for the script redirect (#1737): the name rules (src/quest/plugin/script_redirect.h), the all-or-nothing registry
// against a real temporary directory, the empty-registry pass-through, and the log lines a registration writes. The dlopen
// handler's wiring is not driven here (dlopen_hook.cpp needs the whole integration build); the pure path it calls is.
//
// Built and run by `just test-quest-hooks`.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

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

std::string g_lines;  // every line the host logs during the test

void Capture(sentinel::LogLevel, const char* line) {
  g_lines += line;
  g_lines += '\n';
}

bool Has(const char* fragment) { return g_lines.find(fragment) != std::string::npos; }

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
}

void PassThroughWithNoRegistration() {
  sr::ResetForTest();
  char buf[sr::kNameCapacity];
  QCHECK(sr::ArmedCount() == 0);
  QCHECK(sr::RewriteLoadName(kNameA, buf, sizeof buf) == kNameA);
  QCHECK(sr::RewriteLoadName(nullptr, buf, sizeof buf) == nullptr);
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
  QCHECK(std::strcmp(sr::RewriteLoadName("libpnsovr.so", buf, sizeof buf), "libpnsovr.so") == 0);
  QCHECK(Has("\"event\":\"script_redirected\"") && Has("\"to\":\"lib37f88adb0d8066f7_q15t.so\""));
  QCHECK(Has("\"result\":\"ok\"") && Has("\"status\":\"armed\""));

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
  // A failed registration leaves the way open for a correct one.
  QCHECK(sr::RegisterIn(dir.c_str(), "q15t", pair, 2) == NEVR_SCRIPT_REDIRECT_OK);
  QCHECK(sr::ArmedCount() == 2);

  // Argument checks: refused before anything is looked at.
  sr::ResetForTest();
  const std::uint64_t dup[] = {kA, kA};
  QCHECK(sr::RegisterIn(dir.c_str(), "q15t", dup, 2) == NEVR_SCRIPT_REDIRECT_BAD_ARGUMENT);
  QCHECK(sr::RegisterIn(dir.c_str(), "q15t", pair, 0) == NEVR_SCRIPT_REDIRECT_BAD_ARGUMENT);
  QCHECK(sr::RegisterIn(dir.c_str(), nullptr, pair, 2) == NEVR_SCRIPT_REDIRECT_BAD_ARGUMENT);
  QCHECK(sr::RegisterIn(dir.c_str(), "q15t", nullptr, 2) == NEVR_SCRIPT_REDIRECT_BAD_ARGUMENT);
  std::uint64_t seventeen[sr::kMaxEntries + 1] = {};
  for (std::uint64_t i = 0; i < sr::kMaxEntries + 1; ++i) seventeen[i] = i + 1;
  QCHECK(sr::RegisterIn(dir.c_str(), "q15t", seventeen, sr::kMaxEntries + 1) == NEVR_SCRIPT_REDIRECT_BAD_ARGUMENT);
  QCHECK(sr::ArmedCount() == 0);
  QCHECK(sr::RegisterIn(nullptr, "q15t", pair, 2) == NEVR_SCRIPT_REDIRECT_NO_LIBRARY_DIR);
  QCHECK(sr::ArmedCount() == 0);

  RemoveDir(dir, all, 2);
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
  PassThroughWithNoRegistration();
  AllOrNothingRegistry();
  StateLine();
  ResultNames();
  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "script_redirect_test: %d check(s) failed\n", quest_test::Failures());
    return 1;
  }
  std::printf("script_redirect_test: all checks pass\n");
  return 0;
}
