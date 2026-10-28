// Built with -fno-exceptions (callback_thunk.h, reached through pinned_targets.h, requires it).
#include "quest/plugin/plugin_game_image.h"

#include "quest/sentinel/got_hook.h"
#include "quest/sentinel/pinned_targets.h"

namespace nevr_quest::plugin {

bool ReadGameBuildId(char* out, std::size_t capacity) noexcept {
  sentinel::ElfImage image;
  return sentinel::FindLoadedImage(sentinel::pinned::kLibR15, &image) && sentinel::ReadBuildId(image, out, capacity);
}

std::uintptr_t GameBaseAddress() noexcept {
  sentinel::ElfImage image;
  return sentinel::FindLoadedImage(sentinel::pinned::kLibR15, &image) ? image.base : 0;
}

}  // namespace nevr_quest::plugin
