// Host test for "a fault the game recovers from writes no update/crashed" (ADR 0024, section 6): the real signal handler
// (src/quest/sentinel/crash_first_hook.cpp, OnSignal) with the real marker (crash_report_quest.cpp, MarkUpdateCrashed) as its
// `unrecovered` action, as sentinel.cpp wires them, over a root whose update/pending.json exists.
//
// A fault the kernel raised is delivered by calling OnSignal with a real context; the game's handler is registered through the
// fixture library that calls libc's sigaction through a hooked slot, as libr15 does (crash_first_hook_test.cpp has the other
// half: the order of the actions and each way a fault is or is not recovered). Here the question is the file: a game handler
// that fixes up the faulting context, or jumps out, leaves no update/crashed; one that leaves the fault as it was, and a signal
// with no game handler, leave it.
//
// This translation unit includes crash_report_quest.cpp itself, as update_crash_marker_test.cpp does.

#include <android/log.h>
#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <setjmp.h>
#include <signal.h>
#include <sys/stat.h>
#include <ucontext.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "crash_first_hook.h"
#include "hook_log.h"
#include "quest/tests/test_check.h"

#pragma GCC diagnostic ignored "-Wunused-function"
#include "quest/sentinel/crash_report_quest.cpp"  // NOLINT: the marker in this unit, as update_crash_marker_test does
#pragma GCC diagnostic warning "-Wunused-function"

namespace {

namespace cf = nevr_quest::crash_first;

constexpr mode_t kTestFileMode = 0600;
constexpr mode_t kTestDirMode = 0700;
constexpr char kTempTemplate[] = "nevr-crash-recovery-XXXXXX";
constexpr std::uint64_t kPerturbBit = 1;  // the bit a recovering game handler flips in one register of the faulting context

using SigactionFn = int (*)(int, const struct sigaction*, struct sigaction*);
SigactionFn g_gameSigaction = nullptr;

// The marker as sentinel.cpp's MarkAction runs it.
void MarkAction(int, siginfo_t*, void*) noexcept { sentinel::MarkUpdateCrashed(); }

void GameLeavesTheFaultAsItWas(int, siginfo_t*, void*) {}

void GameFixesUpTheContext(int, siginfo_t*, void* ucontext) {
  ucontext_t* context = static_cast<ucontext_t*>(ucontext);
#if defined(__aarch64__)
  context->uc_mcontext.regs[0] ^= kPerturbBit;
#else
  context->uc_mcontext.gregs[REG_RAX] ^= static_cast<greg_t>(kPerturbBit);
#endif
}

sigjmp_buf g_jumpOut;
void GameJumpsOut(int, siginfo_t*, void*) { siglongjmp(g_jumpOut, 1); }

struct sigaction SiginfoAction(void (*handler)(int, siginfo_t*, void*)) {
  struct sigaction action;
  std::memset(&action, 0, sizeof(action));
  sigemptyset(&action.sa_mask);
  action.sa_sigaction = handler;
  action.sa_flags = SA_SIGINFO;
  return action;
}

bool Exists(const std::string& path) {
  struct stat st;
  std::memset(&st, 0, sizeof(st));
  return ::lstat(path.c_str(), &st) == 0;
}

// A root with update/pending.json, and the sentinel log pointed below it (the marker's record goes there).
struct Root {
  std::string path;
  std::string logs;
  std::string trusted;
  sentinel::LogDirCandidate candidate;
};

bool MakeRoot(Root* root) {
  const char* base = std::getenv("TMPDIR");
  const std::string pattern = std::string(base != nullptr && base[0] != '\0' ? base : ".") + "/" + kTempTemplate;
  std::vector<char> buffer(pattern.begin(), pattern.end());
  buffer.push_back('\0');
  if (::mkdtemp(buffer.data()) == nullptr) return false;
  root->path = buffer.data();
  if (::mkdir((root->path + "/update").c_str(), kTestDirMode) != 0) return false;
  const int fd = ::open((root->path + "/update/pending.json").c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, kTestFileMode);
  if (fd < 0) return false;
  ::close(fd);
  root->trusted = root->path;
  root->logs = root->path + "/nevr/logs";
  root->candidate = sentinel::LogDirCandidate{"test", root->logs.c_str(), root->trusted.c_str()};
  sentinel::SetLogDirCandidatesForTest(&root->candidate, 1);
  sentinel::ResetSessionForTest();
  return true;
}

// Removes what the test made: the marker, the pending record, the log files and the folders, each by its exact path.
void RemoveRoot(const Root& root) {
  if (DIR* handle = ::opendir(root.logs.c_str())) {
    while (const dirent* entry = ::readdir(handle)) {
      const std::string name = entry->d_name;
      if (name != "." && name != "..") ::unlink((root.logs + "/" + name).c_str());
    }
    ::closedir(handle);
  }
  ::rmdir(root.logs.c_str());
  ::rmdir((root.path + "/nevr").c_str());
  ::unlink((root.path + "/update/crashed").c_str());
  ::unlink((root.path + "/update/pending.json").c_str());
  ::rmdir((root.path + "/update").c_str());
  ::rmdir(root.path.c_str());
}

// Arms the marker over a fresh root and the handler with the marker as its one action, registers the game's handler (null: none),
// and delivers a fault the kernel raised. `jumps`: the game's handler leaves by a jump, so the call comes back through it.
// Returns whether update/crashed exists afterwards.
bool CrashedAfter(void (*game)(int, siginfo_t*, void*), bool jumps = false) {
  Root root;
  const bool made = MakeRoot(&root);
  QCHECK(made);
  if (!made) {
    RemoveRoot(root);
    return false;
  }
  QCHECK(sentinel::ArmUpdateCrashMarker(root.path.c_str()));
  cf::ResetForTest();
  QCHECK(cf::InstallHandlers({nullptr, nullptr, nullptr, &MarkAction}) == cf::kGuardedCount);
  if (game != nullptr) {
    const struct sigaction request = SiginfoAction(game);
    QCHECK(g_gameSigaction(SIGSEGV, &request, nullptr) == 0);
  }
  siginfo_t info;
  std::memset(&info, 0, sizeof(info));
  info.si_signo = SIGSEGV;
  info.si_code = SEGV_MAPERR;
  ucontext_t context;
  QCHECK(getcontext(&context) == 0);
  if (!jumps || sigsetjmp(g_jumpOut, 1) == 0) cf::OnSignal(SIGSEGV, &info, &context);
  const bool crashed = Exists(root.path + "/update/crashed");
  RemoveRoot(root);
  return crashed;
}

void AFaultTheGamesHandlerFixesUpLeavesNoMarker() { QCHECK(!CrashedAfter(&GameFixesUpTheContext)); }

void AGameHandlerThatJumpsOutLeavesNoMarker() { QCHECK(!CrashedAfter(&GameJumpsOut, /*jumps=*/true)); }

void AFaultLeftAsItWasLeavesTheMarker() { QCHECK(CrashedAfter(&GameLeavesTheFaultAsItWas)); }

void AFaultWithNoGameHandlerLeavesTheMarker() { QCHECK(CrashedAfter(nullptr)); }

}  // namespace

// FinishCrashReport's report naming calls DumpDir; this test never reaches that call. Nothing here is a seam into the marker.
namespace sentinel {
const char* DumpDir() { return ""; }
}  // namespace sentinel

extern "C" int __android_log_write(int, const char*, const char*) { return 1; }
extern "C" int __android_log_print(int, const char*, const char*, ...) { return 1; }

int main(int argc, char** argv) {
  const std::string fixtureDir = argc > 1 ? argv[1] : ".";
  void* consumer = dlopen((fixtureDir + "/libcffx_consumer.so").c_str(), RTLD_NOW | RTLD_LOCAL);
  QCHECK(consumer != nullptr);
  if (consumer == nullptr) return 1;
  g_gameSigaction = reinterpret_cast<SigactionFn>(dlsym(consumer, "cffx_sigaction"));
  QCHECK(g_gameSigaction != nullptr);
  const sentinel::GotTarget sigactionTarget{"libcffx_consumer.so", "sigaction", sentinel::RelocKind::kJumpSlot};
  const sentinel::GotTarget signalTarget{"libcffx_consumer.so", "signal", sentinel::RelocKind::kJumpSlot};
  QCHECK(cf::InstallAt(sigactionTarget, signalTarget));
  AFaultTheGamesHandlerFixesUpLeavesNoMarker();
  AGameHandlerThatJumpsOutLeavesNoMarker();
  AFaultLeftAsItWasLeavesTheMarker();
  AFaultWithNoGameHandlerLeavesTheMarker();
  cf::RestoreHandlers();
  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "update_crash_marker_recovery_test: %d check(s) failed\n", quest_test::Failures());
    return 1;
  }
  std::printf("update_crash_marker_recovery_test: all checks pass\n");
  return 0;
}
