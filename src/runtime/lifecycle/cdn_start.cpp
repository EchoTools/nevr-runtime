#include "runtime/lifecycle/cdn_start.h"

#include <winsock2.h>  // before windows.h: core/logging.h pulls it in
#include <windows.h>

#include <atomic>
#include <memory>
#include <string>
#include <string_view>

#include "cdn/cdn_demand.h"
#include "cdn/cdn_manifest.h"
#include "cdn/cdn_service.h"
#include "cdn/env_windows.h"
#include "core/logging.h"
#include "core/nevr_root.h"
#include "runtime/content/content_pack.h"
#include "runtime/patch/plugin_assets.h"
#include "runtime/combat/announcer_callouts.h"

namespace nevr_cdn_start {

namespace {

// The CDN's folder under the cache: <nevr root>\cache\cdn.
constexpr const char* kCdnCacheName = "cdn";

// Where the game looks for the script DLLs a CScriptResource record names: `<exe dir>\scripts\`, with the trailing separator, in UTF-8,
// the spelling WindowsEnv::OpenBlob converts back (env_windows.cpp), so a game folder with a non-ASCII character is found; the exe path is
// read wide, as map_channel.cpp's ExeDir and map_scripts_install.cpp's SetPolicy read it. Empty when the exe's path cannot be read, in
// which case the CDN's script gate is not applied (cdn_run.h).
std::string GameScriptDir() {
  constexpr DWORD kPathChars = MAX_PATH * 2;
  wchar_t exe[kPathChars];
  const DWORD n = GetModuleFileNameW(nullptr, exe, kPathChars);
  if (n == 0 || n >= kPathChars) return std::string();
  const std::wstring path(exe, n);
  const std::size_t cut = path.find_last_of(L"\\/");
  if (cut == std::wstring::npos) return std::string();
  const std::string dir = nevr_content::Narrow(path.substr(0, cut));
  if (dir.empty()) return std::string();
  return dir + "\\scripts\\";
}

// Written once per published set, read from any thread. Sets are never freed, so a reader may keep the pointer.
std::atomic<const nevr_cdn::Mounted*> g_published{nullptr};

// True on the thread that runs the boot mount (Start sets it for the synchronous part): the sets published there are the ones
// built from the store before the game reads its resources; the fetch thread's are later.
thread_local bool t_boot_mount = false;

// A set with no shadow (nothing mounted) leaves the earlier one in place for the read hook.
void PublishSet(const nevr_cdn::Mounted* set) {
  g_published.store(set, std::memory_order_release);
  if (set != nullptr && set->shadow != nullptr) nevr::assets::PublishCdnShadow(set->shadow.get());
  // The EchoCombat announcer callouts need the sound bank record in the boot set (announcer_callouts.h).
  if (set != nullptr) nevr_combat::announcer::OnPublished(set->shadow.get(), set->labels, t_boot_mount);
}

// A server's demand set (PLAN-v5 R6-a-2b): kept for the read hook, which serves it, and never fed to the announcer (announcer_callouts.h).
// Sets are not freed: the read hook keeps the shadow for the life of the process, so the set is leaked on purpose, once per session.
void PublishServerSet(std::unique_ptr<nevr_cdn::Mounted> set) {
  if (!set || !set->shadow) return;
  const nevr_cdn::Mounted* kept = set.release();
  g_published.store(kept, std::memory_order_release);
  nevr::assets::PublishCdnShadow(kept->shadow.get());
}

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
  nevr_cdn::RunConfig config = nevr_cdn::MakeRunConfig(settings, dir, nevr_cdn::kWindowsX64);
  // The script gate (cdn_script_gate.h) reads the game's script folder. The registrar of the `scripts` copies is wired by PLAN-v5 R10b:
  // until then none is set, so no script is held and no label lists one.
  config.script_dir = GameScriptDir();
  // The Env holds no state, so one object for the process lifetime (the fetch thread outlives every caller) is all it needs.
  static nevr_cdn::WindowsEnv env;
  // The boot mount publishes on this thread inside Start; the fetch thread it spawns is another thread.
  t_boot_mount = true;
  const bool started = nevr_cdn::Start(&env, config, &PublishSet);
  t_boot_mount = false;
  return started;
}

const nevr_cdn::Mounted* Published() noexcept { return g_published.load(std::memory_order_acquire); }

SessionDemandOutcome ResolveDemandForSession(std::uint64_t level, std::string_view game_type, const std::function<bool()>& keep_going) {
  SessionDemandOutcome out;
  const std::string root = nevr_root::RootA();
  if (root.empty()) {
    out.reason = "no_nevr_root";
    return out;
  }
  const std::string dir = nevr_root::Join(nevr_root::CacheDir(root), kCdnCacheName);
  const nevr_cdn::Settings settings{};
  nevr_cdn::RunConfig config = nevr_cdn::MakeRunConfig(settings, dir, nevr_cdn::kWindowsX64);
  config.policy.role = nevr_cdn::Role::kServer;
  config.script_dir = GameScriptDir();
  // A server has no boot mount, so no label is fixed at boot (ADR 0045 decision 7).
  config.boot_only.clear();
  // The manifest's v2 to v1 fallback on a 404 is the client's too: a decision, two-way door (PLAN-v5 R6-a-2b 3.6).
  static nevr_cdn::WindowsEnv env;
  config = nevr_cdn::ResolveManifestPath(env, config, keep_going);
  const nevr_cdn::DemandInput input{level, std::string(game_type)};
  const nevr_cdn::DemandResult result =
      nevr_cdn::ResolveDemand(env, config, input, nullptr, PublishServerSet, keep_going);
  out.ok = result.ok;
  if (!result.ok) out.reason = result.reason;
  return out;
}

}  // namespace nevr_cdn_start
