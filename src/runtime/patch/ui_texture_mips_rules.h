#pragma once
// Which textures the UI keeps at their top mip, and when a texture still needs forcing. The decisions are apart
// from the hook that applies them (ui_texture_mips.cpp) so they are unit-tested without the game.
//
// Evidence (disassembly of the game binaries; PC VAs are echovr.exe, Quest VAs are libr15.so pinned vaddr + 0x100000):
//   - CGTextureResource::Attach (0x1405491c0) creates the device texture at its base level when texture streaming is
//     on (DAT_1420a8ab8 != 0), and calls ForceLoadAllMips (0x14056c420) only when streaming is off.
//   - Streaming raises a texture's level only for textures registered in a streaming system (CalcObject/CalcLevel
//     MipRequirementsTask -> UpdateTextureSizes 0x1405e91f0). The streaming code has no UI, splash or menu class.
//   - ForceLoadAllMips returns at once when the resident level (this+0xc0) already equals the mip count
//     (info+0xcc) or the texture owns its full data stream (info+0xc0 != 0).
// So a UI texture is forced at the one place every texture passes once: right after Attach.

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace nevr_ui_texture_mips {

// A texture name starts with one of these when the UI system owns it (symbol corpus: ui_*_clr, mnu_*, menu_*,
// loading_*, splash_*). World, avatar and prop textures use other prefixes (mpl_, cst_, prp_, ...).
constexpr std::string_view kUiPrefixes[] = {"ui_", "mnu_", "menu_", "loading_", "splash"};
constexpr std::string_view kUiExactNames[] = {"default_splash_texture"};

// A texture with more mips than this is not read as a texture: a misread field must not start a read of the file.
constexpr std::uint32_t kMaxMipCount = 16;

inline bool IsUiTextureName(std::string_view name) {
  for (const std::string_view exact : kUiExactNames) {
    if (name == exact) return true;
  }
  for (const std::string_view prefix : kUiPrefixes) {
    if (name.size() >= prefix.size() && name.substr(0, prefix.size()) == prefix) return true;
  }
  return false;
}

// What the hook reads from the CGTextureResource after Attach (PC offsets: resident this+0xc0, info = *(this+0x70),
// mip count info+0xcc, own-stream flag info+0xc0).
struct TextureState {
  std::uint32_t resident;
  std::uint32_t mipCount;
  std::uint32_t ownsStream;
};

// True when the texture is short of its top mip and ForceLoadAllMips would load it. The game's own function repeats
// the first two tests; they are here so the hook logs a forced texture only when it did something.
inline bool NeedsTopMip(const TextureState& t) {
  if (t.mipCount == 0 || t.mipCount > kMaxMipCount) return false;
  if (t.ownsStream != 0) return false;
  return t.resident != t.mipCount;
}

// A binary search over a table sorted by ascending unsigned hash, as the generated symbol cache is. Returns nullptr
// for an unknown hash.
template <typename Entry>
const char* NameForHash(const Entry* table, std::size_t count, std::uint64_t hash) {
  std::size_t lo = 0;
  std::size_t hi = count;
  while (lo < hi) {
    const std::size_t mid = lo + (hi - lo) / 2;
    if (table[mid].hash < hash) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  if (lo < count && table[lo].hash == hash) return table[lo].name;
  return nullptr;
}

}  // namespace nevr_ui_texture_mips
