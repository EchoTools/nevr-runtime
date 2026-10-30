// Host and Android test that the shared invite service links into a Quest build (docs/adr/0017, #966). The service and its
// libcurl transport are the PC's own sources (src/runtime/patch/invite_service.cpp, invite_transport_curl.cpp); the sentinel
// starts nothing yet, so this proves the link and the off path: a Start with the feature off says why, is not active, and
// the process then shuts the service down cleanly.
#include "runtime/patch/invite_service.h"

#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

#define CHECK(cond)                                                                 \
  do {                                                                              \
    if (!(cond)) {                                                                  \
      std::fprintf(stderr, "%s:%d CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                                 \
    }                                                                               \
  } while (0)

}  // namespace

int main() {
  std::mutex m;
  std::vector<std::string> lines;
  nevr_invite_service::Settings settings;
  settings.invite_codes = nevr_invite::Switch::Off;
  settings.log = [&m, &lines](nevr_invite::LogLevel, const std::string& line) {
    const std::lock_guard<std::mutex> lock(m);
    lines.push_back(line);
  };

  nevr_invite_service::Start(settings);
  CHECK(!nevr_invite_service::Active());

  bool offLine = false;
  bool onLine = false;
  {
    const std::lock_guard<std::mutex> lock(m);
    for (const std::string& line : lines) {
      offLine = offLine || line == "[NEVR.INVITE] feature off reason=not_configured source=default";
      onLine = onLine || line == "[NEVR.INVITE] service on";
    }
  }
  CHECK(offLine);
  CHECK(!onLine);

  nevr_invite_service::Shutdown();
  CHECK(!nevr_invite_service::Active());

  if (g_failures != 0) {
    std::fprintf(stderr, "invite_service_link_test: %d check(s) failed\n", g_failures);
    return 1;
  }
  std::printf("invite_service_link_test: all checks pass\n");
  return 0;
}
