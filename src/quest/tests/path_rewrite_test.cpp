// The data-path rewrite of the pinned service-only libr15 (#1737): sentinel/path_rewrite.h.
#include "path_rewrite.h"

#include <cstring>
#include <initializer_list>

#include "test_check.h"

namespace pr = nevr_quest::path_rewrite;

namespace {

const char* Run(const char* path, const char* pkg, char* buf) { return pr::Rewrite(path, pkg, buf, pr::kBufferSize); }

}  // namespace

int main() {
  char buf[pr::kBufferSize];
  // The afk prefix is rewritten to the running package, the rest kept.
  const char* in = "/sdcard/Android/media/com.readyatdawn.afk/files/_data/5932408047/rad15/android/packages/x";
  const char* out = Run(in, "com.readyatdawn.r15", buf);
  QCHECK(out == buf);
  QCHECK(std::strcmp(out, "/sdcard/Android/media/com.readyatdawn.r15/files/_data/5932408047/rad15/android/packages/x") == 0);
  // The bare root (no trailing slash) is rewritten too.
  QCHECK(std::strcmp(Run("/sdcard/Android/media/com.readyatdawn.afk", "com.readyatdawn.r15", buf),
                     "/sdcard/Android/media/com.readyatdawn.r15") == 0);
  // Exact package segment only.
  const char* afkx = "/sdcard/Android/media/com.readyatdawn.afkx/files/a";
  QCHECK(Run(afkx, "com.readyatdawn.r15", buf) == afkx);
  // Other paths untouched (returned as the same pointer).
  for (const char* other : {"/sdcard/Android/media/com.readyatdawn.r15/files/a", "/data/data/com.readyatdawn.r15/files/afk",
                            "/storage/emulated/0/readyatdawn/_data", "relative/com.readyatdawn.afk/x", ""}) {
    QCHECK(Run(other, "com.readyatdawn.r15", buf) == other);
  }
  QCHECK(Run(nullptr, "com.readyatdawn.r15", buf) == nullptr);
  // Identity when the running package is afk.
  QCHECK(Run(in, "com.readyatdawn.afk", buf) == in);
  // A result that does not fit is the original.
  char small[24];
  QCHECK(pr::Rewrite(in, "com.readyatdawn.r15", small, sizeof small) == in);
  // The running package from /proc/self/cmdline.
  char pkg[64];
  const char cmd[] = "com.readyatdawn.r15\0--arg";
  QCHECK(pr::PackageFromCmdline(cmd, sizeof cmd, pkg, sizeof pkg) && std::strcmp(pkg, "com.readyatdawn.r15") == 0);
  const char svc[] = "com.readyatdawn.afk:svc";
  QCHECK(pr::PackageFromCmdline(svc, sizeof svc, pkg, sizeof pkg) && std::strcmp(pkg, "com.readyatdawn.afk") == 0);
  QCHECK(!pr::PackageFromCmdline("/bin/sh", 7, pkg, sizeof pkg) && pkg[0] == '\0');
  if (quest_test::Failures() != 0) return 1;
  std::puts("path_rewrite_test: OK");
  return 0;
}
