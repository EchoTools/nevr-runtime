#include "quest/integration/self_check_wiring.h"

#include <atomic>
#include <exception>
#include <functional>
#include <string>
#include <utility>

#include "quest/integration/post_load.h"

namespace nevr_quest::integration {

void ApplySelfCheck(quest_net::FrameTapSinks* tap, const SelfCheckHooks& hooks) {
  std::function<void(std::uint64_t, std::uint64_t)> previous = std::move(tap->onLoginUser);
  tap->onLoginUser = [previous](std::uint64_t platform, std::uint64_t account) {
    if (previous) previous(platform, account);
    nevr_evr_codec::UserId user;
    user.platformCode = platform;
    user.accountId = account;
    nevr_self_check::SetLoggedIn(true, user);
  };
  nevr_self_check::SetEnabled(true);
  nevr_self_check::SetSender(hooks.sender);
  nevr_self_check::SetLogSink(hooks.log);
  nevr_self_check::SetBuild(hooks.build != nullptr ? hooks.build : "");
}

void ReportAndroidMainReturned() noexcept {
  try {
    const nevr_self_check::CheckId id = nevr_self_check::Register(
        {"android_main_returned", "the process ends when the game's android_main returns", nullptr});
    (void)id;  // placeholder, filled in by the next commit
  } catch (const std::exception&) {
    // The unit builds strings; a failure to allocate one loses the result, never the process end.
  }
}

namespace {
std::atomic<std::uint64_t> g_installs{0};
std::atomic<std::uint64_t> g_imagesReported{0};
}  // namespace

void NoteMatchmakingRedirectInstalled() { g_installs.fetch_add(1, std::memory_order_release); }

bool MatchmakingReloadProbe(nevr_self_check::Observation* out) {
  const std::uint64_t images = MatchmakingImages();
  const std::uint64_t installs = g_installs.load(std::memory_order_acquire);
  const bool settled = PostLoadStatsView().matchmakingSettled != 0;
  if (images == 0 || (installs < images && !settled)) return false;
  if (g_imagesReported.exchange(images) == images) return false;
  out->observed = "images=" + std::to_string(images) + " installs=" + std::to_string(installs);
  out->pass = installs >= images;
  return true;
}

void ResetMatchmakingReloadCheckForTest() {
  g_installs.store(0);
  g_imagesReported.store(0);
}

}  // namespace nevr_quest::integration
