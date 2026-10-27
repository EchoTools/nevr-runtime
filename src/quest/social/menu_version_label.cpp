#include "quest/social/menu_version_label.h"

#include <cstring>
#include <string>

#include "callback_thunk.h"
#include "generated/nevr_build_info.h"
#include "hook_install.h"
#include "hook_log.h"
#include "inline_hook.h"
#include "pinned_targets.h"
#include "runtime/patch/menu_version_label_rules.h"

namespace quest_social::menu_version {
namespace {

using sentinel::InlinePrologue;
using sentinel::InlineStatus;

// The first four words of CCanvasUICS::SetText in the pinned libr15 (ReVault 0x149111c; the bytes at the link-time
// address). Word 0 is `cbz x3, ...`, PC-relative: CheckInlineTarget refuses the hook on this build. The words are the
// check, not a guess: a build that differs refuses too.
constexpr InlinePrologue kSetTextPrologue{{0xb4000243u, 0xd10083ffu, 0xa9017bf3u, 0xaa0303f3u}};

struct SetTextTag {};
using SetTextThunk = sentinel::CallbackThunk<SetTextTag, std::uint64_t(void*, std::uint32_t, std::uint64_t, const char*)>;

// Written once, at install, before the hook is live; read-only afterwards. A fixed buffer, so the game's call path
// allocates nothing and carries no static destructor.
char g_label[32] = {};

// Every SetText call passes through here. The common case is one compare, and nothing is logged on this path
// (callback_thunk.h rule 1). An empty text is the script clearing the item and stays empty.
std::uint64_t OnSetText(SetTextThunk::Fn original, void* canvas, std::uint32_t handle, std::uint64_t item,
                        const char* text) noexcept {
  if (item != nevr_menu_version_label::kLobbyVersionItem || text == nullptr || text[0] == '\0' || g_label[0] == '\0') {
    return original(canvas, handle, item, text);
  }
  return original(canvas, handle, item, g_label);
}

NEVR_HOOK_RECORD(kSetTextRecord, SetTextThunk, &OnSetText);

}  // namespace

std::size_t InstallMenuVersionLabelHook() noexcept {
  sentinel::ElfImage image;
  if (!sentinel::FindLoadedImage(sentinel::pinned::kLibR15, &image)) {
    sentinel::LogFields(sentinel::LogLevel::kWarn, "menu_version", {{"status", "libr15_not_loaded"}});
    return 0;
  }
  char buildId[41] = {};
  if (!sentinel::ReadBuildId(image, buildId, sizeof buildId) ||
      std::strcmp(buildId, sentinel::pinned::kLibR15BuildId) != 0) {
    sentinel::LogFields(sentinel::LogLevel::kWarn, "menu_version",
                        {{"status", "build_id_mismatch"}, {"build_id", buildId}});
    return 0;
  }
  const std::string full = std::string("nEVR ") + nevr_build::kVersion;
  const std::string fitted = nevr_menu_version_label::FitLabel(full);
  if (fitted.empty() || fitted.size() >= sizeof g_label) {
    sentinel::LogFields(sentinel::LogLevel::kWarn, "menu_version", {{"status", "label_empty"}});
    return 0;
  }
  std::memcpy(g_label, fitted.c_str(), fitted.size() + 1);

  SetTextThunk::Arm(kSetTextRecord);
  char targetText[19];
  const void* target = reinterpret_cast<const void*>(image.base + kSetTextVaddr);
  const InlineStatus st = sentinel::InstallInlineThunk<SetTextThunk>(target, kSetTextPrologue);
  sentinel::LogFields(st == InlineStatus::kOk ? sentinel::LogLevel::kInfo : sentinel::LogLevel::kError, "menu_version",
                      {{"status", st == InlineStatus::kOk ? "installed" : sentinel::InlineStatusName(st)},
                       {"hook", "set_text"},
                       {"label", g_label},
                       {"target", sentinel::HexString(targetText, reinterpret_cast<std::uintptr_t>(target))}});
  return st == InlineStatus::kOk ? 1 : 0;
}

}  // namespace quest_social::menu_version
