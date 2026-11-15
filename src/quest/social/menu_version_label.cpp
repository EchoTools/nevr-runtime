#include "quest/social/menu_version_label_hook.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>

#include "callback_thunk.h"
#include "generated/nevr_build_info.h"
#include "hook_install.h"
#include "hook_log.h"
#include "runtime/patch/menu_version_label_rules.h"

namespace quest_social::menu_version {
namespace {

// Written once, at install, before the hook is armed; read-only afterwards. A fixed buffer, so the game's call path
// allocates nothing and carries no static destructor.
char g_label[32] = {};
sentinel::GotHook g_hook;
std::atomic<unsigned long long> g_replaced{0};

// Every SetText call passes through here (the credits page sets text each frame). The common case is one compare. An empty
// text is the script clearing the item when its page is disabled and stays empty. The publisher lock item is drawn as the
// service's name (PublisherLockReplacement). The first replacement writes one line (the same logger the OS-build hook uses
// from its handler); the rest write none.
std::uint64_t OnSetText(Thunk::Fn original, void* canvas, std::uint32_t handle, std::uint64_t item,
                        const char* text) noexcept {
  const char* drawn = nevr_menu_version_label::PublisherLockReplacement(item, text);
  const char* itemName = "publisher_lock";
  if (drawn == nullptr) {
    if (item != nevr_menu_version_label::kLobbyVersionItem || text == nullptr || text[0] == '\0' || g_label[0] == '\0') {
      return original(canvas, handle, item, text);
    }
    drawn = g_label;
    itemName = "lobby_version";
  }
  if (g_replaced.fetch_add(1, std::memory_order_acq_rel) == 0) {
    sentinel::LogFields(sentinel::LogLevel::kInfo, "menu_version",
                        {{"status", "label_drawn"}, {"item", itemName}, {"label", drawn}});
  }
  return original(canvas, handle, item, drawn);
}

NEVR_HOOK_RECORD(kSetTextRecord, Thunk, &OnSetText);

void Arm(const char* label) noexcept {
  std::strncpy(g_label, label, sizeof g_label - 1);
  g_label[sizeof g_label - 1] = '\0';
  g_replaced.store(0, std::memory_order_relaxed);
  Thunk::Arm(kSetTextRecord);
}

}  // namespace

void ArmForTest(const char* label) noexcept { Arm(label); }

void ResetForTest() noexcept {
  g_label[0] = '\0';
  g_replaced.store(0, std::memory_order_relaxed);
  Thunk::Reset();
}

unsigned long long Replaced() noexcept { return g_replaced.load(std::memory_order_acquire); }

bool InstallAt(const sentinel::GotTarget& target, const char* label) noexcept {
  Arm(label);
  const sentinel::GotStatus status = sentinel::InstallThunk<Thunk>(g_hook, target);
  const bool ok = status == sentinel::GotStatus::kOk;
  sentinel::LogFields(ok ? sentinel::LogLevel::kInfo : sentinel::LogLevel::kError, "menu_version",
                      {{"status", ok ? "installed" : sentinel::GotStatusName(status)},
                       {"hook", "got_slot"},
                       {"symbol", sentinel::pinned::kCanvasSetTextSymbol},
                       {"label", g_label}});
  return ok;
}

// A version without its "+<commit>" build metadata, as the slot shows it (FitLabel drops the same suffix).
static std::string WithoutBuildMetadata(const char* version) {
  const std::string text(version);
  const std::string::size_type plus = text.find('+');
  return plus == std::string::npos ? text : text.substr(0, plus);
}

std::string LabelFor(const char* runtime, const char* app) {
  const std::string whole =
      std::string("nEVR ") + WithoutBuildMetadata(runtime) + " (app " + WithoutBuildMetadata(app) + ")";
  if (whole.size() <= nevr_menu_version_label::kMaxLabelChars) return whole;
  // Too long for the slot: the runtime alone, which is the part a player checks first.
  return nevr_menu_version_label::FitLabel(std::string("nEVR ") + runtime);
}

// The runtime and the app are this build's: the sentinel runs the runtime it was built with. The update path (its loader
// maps a staged runtime) passes the loaded runtime's version here once it lands.
std::string Label() { return LabelFor(nevr_build::kVersion, nevr_build::kVersion); }

std::size_t InstallMenuVersionLabelHook() noexcept {
  const std::string fitted = Label();
  if (fitted.empty() || fitted.size() >= sizeof g_label) {
    sentinel::LogFields(sentinel::LogLevel::kWarn, "menu_version", {{"status", "label_empty"}});
    return 0;
  }
  return InstallAt(sentinel::pinned::LibR15CanvasSetText(), fitted.c_str()) ? 1 : 0;
}

}  // namespace quest_social::menu_version
