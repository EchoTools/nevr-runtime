#include "runtime/lifecycle/cdn_start.h"

#include <winsock2.h>  // before windows.h: core/logging.h pulls it in
#include <windows.h>

#include <atomic>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "cdn/cdn_demand.h"
#include "cdn/cdn_manifest.h"
#include "cdn/cdn_service.h"
#include "cdn/env_windows.h"
#include "core/logging.h"
#include "core/nevr_root.h"
#include "core/update/engine_start.h"
#include "core/update/fs.h"
#include "core/update/names.h"
#include "runtime/content/cdn_scripts_install.h"
#include "runtime/ext/plugin_trust.h"
#include "runtime/content/content_pack.h"
#include "runtime/lifecycle/update_fs_win32.h"
#include "runtime/lifecycle/update_worker.h"
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

// The self-update trial (PLAN-v7 5.4 "The self-update trial", G9): update\pending.json names a release, and this start is the last try of it.
// The first try still fetches before boot; the last one never waits, so a player who closed the first start during its wait cannot lose
// the release by closing the second. A pending file that exists and cannot be read or parsed is taken as the trial, with the reason
// logged. The runtime is hashed only once the file parses. No lock: the only runtime writer of pending.json runs 10 s after the first frame.
bool LastUpdateTrialThisStart(const std::string& root) {
  const std::string path = nevr_root::Join(nevr_root::Join(root, nevr_update_names::kUpdateDir), nevr_update_names::kPendingJson);
  std::string text;
  const nevr_update::ReadResult read = nevr_update::Win32UpdateFs().ReadSmallFile(path, &text);
  if (read == nevr_update::ReadResult::kAbsent) return nevr_update::LastUpdateTrial(std::nullopt, std::string_view());
  const char* cause = nullptr;
  nlohmann::json doc;
  nevr_update::Pending pending;
  std::string runtime_sha256;
  if (read == nevr_update::ReadResult::kUnreadable) {
    cause = "unreadable";
  } else if (read == nevr_update::ReadResult::kTooBig) {
    cause = "too_big";
  } else if (!nevr_update::ParseJsonText(text, &doc) || !nevr_update::ParsePending(doc, &pending)) {
    cause = "unparsed";
  } else {
    std::string version;
    std::string error;
    if (!nevr_update::ReadRunningRuntime(&version, &runtime_sha256, &error)) cause = "runtime_unhashed";
  }
  if (cause != nullptr) {
    Log(EchoVR::LogLevel::Info,
        "[NEVR.CDN] cdn_start role=client fetch=none reason=update_pending_unreadable cause=%s consequence=\"this start makes no boot fetch: "
        "the pending self-update is taken as its trial\"",
        cause);
    return true;
  }
  return nevr_update::LastUpdateTrial(std::string_view(text), runtime_sha256);
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
  // The script gate (cdn_script_gate.h) reads the game's script folder, and the registrar claims each label's `scripts` copies before it
  // (cdn_scripts_install.h, PLAN-v5 R10b). Both are set here: the boot mount and the fetch both run the gate.
  config.script_dir = GameScriptDir();
  config.registrar = nevr_cdn_scripts::PcRegistrar(dir, nevr_plugin_trust::ActiveMode() == nevr_plugin_trust::Mode::SignedOnly);
  // The boot-state writes (the refetch marker and the boot-fetch stamp, both beside the saved manifest, PLAN-v7 5.4) are the client's, and the
  // self-update trial (PLAN-v7 G9) decides whether this start waits for the boot fetch. Both are set here, and only the client's start sets them.
  config.boot_state = true;
  config.last_update_trial = LastUpdateTrialThisStart(root);
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
