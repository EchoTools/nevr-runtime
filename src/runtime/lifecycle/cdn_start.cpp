#include "runtime/lifecycle/cdn_start.h"

#include <atomic>
#include <string>

#include "cdn/cdn_manifest.h"
#include "cdn/cdn_service.h"
#include "cdn/env_windows.h"
#include "core/logging.h"
#include "core/nevr_root.h"

namespace nevr_cdn_start {

namespace {

// The CDN's folder under the cache: <nevr root>\cache\cdn.
constexpr const char* kCdnCacheName = "cdn";

// Written once per published set, read from any thread. Sets are never freed, so a reader may keep the pointer.
std::atomic<const nevr_cdn::Mounted*> g_published{nullptr};

void PublishSet(const nevr_cdn::Mounted* set) { g_published.store(set, std::memory_order_release); }

}  // namespace

bool Start() noexcept {
  const std::string root = nevr_root::RootA();
  if (root.empty()) {
    Log(EchoVR::LogLevel::Warning,
        "[NEVR.CDN] cdn_start result=off reason=no_nevr_root consequence=\"no CDN asset is served this run\"");
    return false;
  }
  const std::string dir = nevr_root::Join(nevr_root::CacheDir(root), kCdnCacheName);
  const nevr_cdn::Settings settings{};
  const nevr_cdn::RunConfig config = nevr_cdn::MakeRunConfig(settings, dir, nevr_cdn::kWindowsX64);
  // The Env holds no state, so one object for the process lifetime (the fetch thread outlives every caller) is all it needs.
  static nevr_cdn::WindowsEnv env;
  return nevr_cdn::Start(&env, config, &PublishSet);
}

const nevr_cdn::Mounted* Published() noexcept { return g_published.load(std::memory_order_acquire); }

}  // namespace nevr_cdn_start
