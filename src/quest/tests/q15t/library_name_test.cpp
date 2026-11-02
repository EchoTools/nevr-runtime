// Host test for LibraryName's dladdr refusal (src/quest/plugins/q15t/src/headset_gates.cpp, the link gate's caller). The Bind
// frame that feeds LibraryName is absent on the host (HeadsetWrapper passes a zero bind sp), so gates_test never reaches the
// dladdr branch; this test calls LibraryName directly, which is why it includes headset_gates.cpp (its anonymous namespace is
// then visible here). headset_gates.cpp is not linked again: this translation unit is its only copy in the binary.
#include <dlfcn.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>

#include "headset_gates.cpp"  // NOLINT: LibraryName is in its anonymous namespace

// dladdr, as LibraryName asks it. g_dladdrMode 0 is libc's answer. 1 returns 0 and names a file, so only the return code
// refuses it. 2 returns 1 and names no file. This definition takes the place of libc's for the calls this executable makes.
static int g_dladdrMode = 0;
extern "C" int dladdr(const void* address, Dl_info* info) noexcept {
  using RealDladdr = int (*)(const void*, Dl_info*);
  static const RealDladdr real = reinterpret_cast<RealDladdr>(dlsym(RTLD_NEXT, "dladdr"));
  if (g_dladdrMode == 0) return real(address, info);
  info->dli_fname = g_dladdrMode == 1 ? "/nonexistent/libq15t_script.so" : nullptr;
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
  std::setvbuf(stdout, nullptr, _IONBF, 0);  // a failing check is printed before a crash below it
  // dladdr fails both ways: LibraryName refuses (known = false) and writes nothing into the caller's buffer.
  for (int mode : {1, 2}) {
    char out[64] = "untouched";
    g_dladdrMode = mode;
    const bool known = q15t::LibraryName(0x1234, out, sizeof out);  // any non-zero address: the fake does not look it up
    g_dladdrMode = 0;
    CHECK(!known, "mode %d: LibraryName must refuse when dladdr fails (returned true, out=\"%s\")", mode, out);
    CHECK(std::strcmp(out, "untouched") == 0, "mode %d: a refused name must not be written (out=\"%s\")", mode, out);
  }
  // positive control, real dladdr: an address in this executable gives its file name, so the refusal is not "always false"
  {
    char out[256] = "";
    const bool known = q15t::LibraryName(reinterpret_cast<std::uintptr_t>(&q15t::LibraryName), out, sizeof out);
    CHECK(known && out[0] != '\0', "real dladdr: LibraryName must name the library holding its own code (known=%d out=\"%s\")",
          known ? 1 : 0, out);
  }
  if (g_fail == 0) std::printf("library_name_test: all checks pass\n");
  else std::printf("library_name_test: %d failure(s)\n", g_fail);
  return g_fail == 0 ? 0 : 1;
}
