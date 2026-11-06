// Host test for LibraryName's dladdr refusal (src/quest/combat/headset_gates.cpp, the menu gate's caller). The Bind frame that feeds LibraryName
// is absent on the host (the host entry passes a zero bind sp), so headset_gates_test never reaches the dladdr branch; this test calls
// LibraryName directly. Moved from the q15t plugin's library_name_test, which included the plugin's .cpp to reach an anonymous-namespace
// function; LibraryName is a member of the module's header here, and the assertions are the same.
//
// Built and run by `just test-quest-combat`.
#include <dlfcn.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "quest/combat/headset_gates.h"
#include "quest/tests/test_check.h"

namespace hg = nevr_quest::combat::headset_gates;

// dladdr, as LibraryName asks it. g_dladdrMode 0 is libc's answer. 1 returns 0 and names a file, so only the return code refuses it. 2 returns 1
// and names no file. This definition takes the place of libc's for the calls this executable makes.
static int g_dladdrMode = 0;
extern "C" int dladdr(const void* address, Dl_info* info) noexcept {
  using RealDladdr = int (*)(const void*, Dl_info*);
  static const RealDladdr real = reinterpret_cast<RealDladdr>(dlsym(RTLD_NEXT, "dladdr"));
  if (g_dladdrMode == 0) return real(address, info);
  info->dli_fname = g_dladdrMode == 1 ? "/nonexistent/libq15t_script.so" : nullptr;
  return g_dladdrMode == 1 ? 0 : 1;
}

int main() {
  std::setvbuf(stdout, nullptr, _IONBF, 0);  // a failing check is printed before a crash below it
  // dladdr fails both ways: LibraryName refuses (known = false) and writes nothing into the caller's buffer.
  for (int mode : {1, 2}) {
    char out[64] = "untouched";
    g_dladdrMode = mode;
    const bool known = hg::LibraryName(0x1234, out, sizeof out);  // any non-zero address: the fake does not look it up
    g_dladdrMode = 0;
    QCHECK(!known);
    QCHECK(std::strcmp(out, "untouched") == 0);
  }
  // positive control, real dladdr: an address in this executable gives its file name, so the refusal is not "always false"
  {
    char out[256] = "";
    const bool known = hg::LibraryName(reinterpret_cast<std::uintptr_t>(&hg::LibraryName), out, sizeof out);
    QCHECK(known && out[0] != '\0');
    QCHECK(std::strchr(out, '/') == nullptr);  // the file name, not the path
  }
  // a null address is never named
  {
    char out[64] = "untouched";
    QCHECK(!hg::LibraryName(0, out, sizeof out) && std::strcmp(out, "untouched") == 0);
  }
  if (quest_test::Failures() == 0) std::printf("headset_gates_library_name_test: all checks pass\n");
  return quest_test::Failures() == 0 ? 0 : 1;
}
