// The page as an export of the sentinel (#2187, src/quest/sentinel/ec_page.h). A process loads a stand-in for libr15 RTLD_LOCAL, which pulls in the
// sentinel's page object (ec_page.cpp built as a shared object with the sentinel's flags) as its DT_NEEDED, the way libr15 pulls in
// libovrplatformloader.so; then a stand-in for an EC script library, loaded RTLD_LOCAL on its own, asks dlsym(RTLD_DEFAULT, "re33_ec_page") as
// the EC script libraries do.
//
// Shown, on glibc: the sentinel object exports re33_ec_page; before PublishEcPage the script's lookup does not find it (the sentinel is in
// libr15's local group, not the global one); after it, the lookup finds the sentinel's own storage. With another object that already exports a
// re33_ec_page in the global scope, PublishEcPage reports it (state 2, kOther), the lookup finds that one, and the sentinel's EcPage() is still
// its own storage, so it never writes the other page. Not shown: bionic. Its RTLD_DEFAULT lookup also searches the caller's local group, and
// whether dlopen(RTLD_NOLOAD | RTLD_GLOBAL) promotes an object already loaded is not measured here (docs/quest/combat/ec.md (a)); the
// `combat_ec_page` line on a headset is the answer there.
//
// usage: ec_page_test <libr15 stand-in> <script stand-in> <sentinel page object> [<an object that already exports re33_ec_page>]
#include <dlfcn.h>

#include <cstdio>

int main(int argc, char** argv) {
  if (argc < 4) return 2;
  if (argc >= 5 && dlopen(argv[4], RTLD_NOW | RTLD_GLOBAL) == nullptr) {  // another provider is already global
    std::printf("FAIL cannot load %s: %s\n", argv[4], dlerror());
    return 1;
  }
  void* const other = argc >= 5 ? dlsym(RTLD_DEFAULT, "re33_ec_page") : nullptr;
  void* const libr15 = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
  void* const script = dlopen(argv[2], RTLD_NOW | RTLD_LOCAL);
  void* const sentinel = dlopen(argv[3], RTLD_NOW | RTLD_NOLOAD);  // already loaded, as libr15's dependency
  if (libr15 == nullptr || script == nullptr || sentinel == nullptr) {
    std::printf("FAIL cannot load the stand-ins: %s\n", dlerror());
    return 1;
  }
  int fail = 0;
  void* const page = dlsym(sentinel, "re33_ec_page");
  auto const own = reinterpret_cast<void* (*)()>(dlsym(sentinel, "shim_page"));
  auto const publish = reinterpret_cast<int (*)()>(dlsym(sentinel, "shim_publish"));
  auto const find = reinterpret_cast<void* (*)()>(dlsym(script, "script_find_page"));
  if (page == nullptr || own == nullptr || publish == nullptr || find == nullptr) {
    std::printf("FAIL the sentinel object does not export re33_ec_page, or a shim is missing\n");
    return 1;
  }
  if (own() != page) {
    std::printf("FAIL the sentinel's EcPage() is not its own exported page: it would write into %p\n", own());
    ++fail;
  }
  void* const before = find();
  if (other == nullptr && before != nullptr) {
    std::printf("FAIL the page is global before PublishEcPage (%p): the premise of the test is wrong\n", before);
    ++fail;
  }
  const int state = publish();
  void* const after = find();
  std::printf("ec_page_test: state=%d before=%p after=%p page=%p other=%p\n", state, before, after, page, other);
  if (other == nullptr && (state != 0 || after != page)) {
    std::printf("FAIL PublishEcPage did not make the page visible to a script library's dlsym(RTLD_DEFAULT)\n");
    ++fail;
  }
  if (other != nullptr && (state != 2 || after != other || own() == other)) {
    std::printf("FAIL another global re33_ec_page must be reported (state 2) and win the lookup, and the sentinel must keep its own storage\n");
    ++fail;
  }
  if (fail == 0) std::printf("ec_page_test: all checks passed%s\n", other != nullptr ? " (other page)" : "");
  return fail == 0 ? 0 : 1;
}
