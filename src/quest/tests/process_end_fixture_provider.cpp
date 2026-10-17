// Host-test fixture: stands in for libr15's android_main, the function the glue calls through a PLT slot.
#include <atomic>

namespace {
std::atomic<int> g_calls{0};
void* g_lastApp = nullptr;
}  // namespace

extern "C" {
void android_main(void* app) {
  g_lastApp = app;
  g_calls.fetch_add(1);
}
int pefx_main_calls() { return g_calls.load(); }
void* pefx_last_app() { return g_lastApp; }
}
