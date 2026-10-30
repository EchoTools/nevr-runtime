// The loader opens a plugin RTLD_LOCAL (nevr-runtime plugin_platform.cpp:71). This loads the ec module as a shared object the same
// way and asks: is re33_ec_page visible to dlsym(RTLD_DEFAULT) (what the EC script libs call) before and after PublishEcPage?
// On glibc. Bionic is NOT covered by this test (docs/quest/q15t/modules/ec.md (a)).
// usage: ec_page_test <path to the shared object> [<path to another object that already exports re33_ec_page>]
//        ec_page_test <path to the shared object> sentinel <sentinel stand-in path> <script-lib stand-in path>
#include <dlfcn.h>

#include <cstdio>
#include <cstring>

// Mode "sentinel": the sentinel stand-in exports the page and is NOT global; a reader lib that depends on it stands for an EC
// script lib. PublishEcPage must adopt the sentinel's page (state 4), the reader must find that same page, and the plugin's
// own export must not be promoted (no second page in the global scope).
static int SentinelMode(const char* plugin, const char* sentinelPath, const char* readerPath) {
  void* reader = dlopen(readerPath, RTLD_NOW | RTLD_LOCAL);  // loads the sentinel stand-in as its dependency
  void* h = dlopen(plugin, RTLD_NOW | RTLD_LOCAL);
  if (reader == nullptr || h == nullptr) {
    std::printf("FAIL cannot load: %s\n", dlerror());
    return 1;
  }
  void* sentinel = dlopen(sentinelPath, RTLD_NOW | RTLD_NOLOAD);
  void* sp = sentinel ? dlsym(sentinel, "re33_ec_page") : nullptr;
  auto readerPage = reinterpret_cast<void* (*)()>(dlsym(reader, "reader_page"));
  reinterpret_cast<void (*)(const char*)>(dlsym(h, "shim_set_sentinel"))(sentinelPath);
  const int state = reinterpret_cast<int (*)()>(dlsym(h, "shim_publish"))();
  void* own = reinterpret_cast<void* (*)()>(dlsym(h, "shim_page"))();
  void* seen = readerPage();
  std::printf("ec_page_test (sentinel): state=%d sentinel=%p plugin_writes=%p reader_sees=%p global=%p\n", state, sp, own, seen,
              dlsym(RTLD_DEFAULT, "re33_ec_page"));
  int fail = 0;
  if (sp == nullptr) { std::printf("FAIL the sentinel stand-in exports no page\n"); ++fail; }
  if (state != 4) { std::printf("FAIL PublishEcPage did not adopt the sentinel's page (state %d, want 4)\n", state); ++fail; }
  if (own != sp) { std::printf("FAIL the plugin writes %p, not the sentinel's page %p\n", own, sp); ++fail; }
  if (seen != sp) { std::printf("FAIL the script-lib stand-in sees %p, not the page the plugin writes\n", seen); ++fail; }
  if (dlsym(h, "re33_ec_page") == dlsym(RTLD_DEFAULT, "re33_ec_page")) { std::printf("FAIL the plugin's own page was made global\n"); ++fail; }
  if (fail == 0) std::printf("ec_page_test: all checks passed (sentinel)\n");
  return fail == 0 ? 0 : 1;
}

int main(int argc, char** argv) {
  if (argc < 2) return 2;
  if (argc >= 5 && std::strcmp(argv[2], "sentinel") == 0) return SentinelMode(argv[1], argv[3], argv[4]);
  int fail = 0;
  void* other = nullptr;
  if (argc >= 3) {  // another provider (the source build's own libvrapi) is already global
    other = dlopen(argv[2], RTLD_NOW | RTLD_GLOBAL);
    if (other == nullptr) {
      std::printf("FAIL cannot load %s: %s\n", argv[2], dlerror());
      return 1;
    }
  }
  void* h = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
  if (h == nullptr) {
    std::printf("FAIL cannot load %s: %s\n", argv[1], dlerror());
    return 1;
  }
  void* page = dlsym(h, "re33_ec_page");
  if (page == nullptr) {
    std::printf("FAIL the plugin object does not export re33_ec_page\n");
    ++fail;
  }
  void* before = dlsym(RTLD_DEFAULT, "re33_ec_page");
  if (other != nullptr) {
    auto own = reinterpret_cast<void* (*)()>(dlsym(h, "shim_page"));
    if (own() != page) {
      std::printf("FAIL the plugin's EcPage() is not its own page while another re33_ec_page is global: it would write into %p\n", own());
      return 1;
    }
    const int state = reinterpret_cast<int (*)()>(dlsym(h, "shim_publish"))();
    void* seen = dlsym(RTLD_DEFAULT, "re33_ec_page");
    std::printf("ec_page_test (another page is global): state=%d seen=%p ours=%p other=%p\n", state, seen, page, before);
    if (state != 3 || seen == page || seen != before) {
      std::printf("FAIL another global re33_ec_page must be reported as such (state 3) and win the lookup\n");
      return 1;
    }
    std::printf("ec_page_test: all checks passed (other page)\n");
    return 0;
  }
  if (before != nullptr) {
    std::printf("FAIL a RTLD_LOCAL plugin's page is already global (%p): the premise of the test is wrong\n", before);
    ++fail;
  }
  auto publish = reinterpret_cast<int (*)()>(dlsym(h, "shim_publish"));
  auto shim_page = reinterpret_cast<void* (*)()>(dlsym(h, "shim_page"));
  if (publish == nullptr || shim_page == nullptr) {
    std::printf("FAIL shim missing\n");
    return 1;
  }
  if (shim_page() != page) {
    std::printf("FAIL EcPage() is not the exported symbol\n");
    ++fail;
  }
  const int state = publish();  // 1 = kVisible
  void* after = dlsym(RTLD_DEFAULT, "re33_ec_page");
  std::printf("ec_page_test: state=%d before=%p after=%p page=%p\n", state, before, after, page);
  if (state != 1 || after != page) {
    std::printf("FAIL PublishEcPage did not make the page visible to dlsym(RTLD_DEFAULT)\n");
    ++fail;
  }
  if (fail == 0) std::printf("ec_page_test: all checks passed\n");
  return fail == 0 ? 0 : 1;
}
