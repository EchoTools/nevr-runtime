#include "quest/sentinel/ui_texture_mips.h"

#include <cstdint>
#include <cstring>

#include "callback_thunk.h"
#include "got_hook.h"
#include "hook_install.h"
#include "hook_log.h"
#include "inline_hook.h"
#include "pinned_targets.h"
#include "runtime/log/symcache.h"
#include "runtime/patch/ui_texture_mips_rules.h"

namespace evr {
extern const SymCacheEntry kSymCacheData[];  // the merged PC and Quest symbol names, sorted by hash (symcache_data.cpp)
extern const int kSymCacheCount;
}  // namespace evr

namespace nevr_quest::ui_texture_mips {
namespace {

// NRadEngine::CGTextureResource::Attach (libr15 0x18e75c4 pinned; ReVault 0x19e75c4 = pinned + 0x100000): this (x0),
// stream (x1); returns 0 on success (w0). Reached through the resource's vtable, so there is no GOT slot: an inline
// detour (inline_hook.h). Its first four words are the prologue the backend compares before it writes a byte.
//   sub sp,sp,#0xf0; stp x21,x20,[sp,#0xd0]; stp x19,x30,[sp,#0xe0]; ldr x8,[x0,#0x78].
constexpr std::uint64_t kAttachVaddr = 0x18e75c4;
constexpr sentinel::InlinePrologue kAttachPrologue{{0xD103C3FFu, 0xA90D53F5u, 0xA90E7BF3u, 0xF9403C08u}};

// NRadEngine::CGTextureResource::ForceLoadAllMips (libr15 0x18e674c pinned; ReVault 0x19e674c): this (x0), returns 0 on
// success. It returns at once when the resident level (this+0xe0) already equals the mip count (info+0xcc) or the
// texture owns its full data stream (info+0xc0 != 0). It is called directly, not hooked.
//   sub sp,sp,#0xd0; str x20,[sp,#0xb0]; stp x19,x30,[sp,#0xc0]; ldr x8,[x0,#0x70].
constexpr std::uint64_t kForceLoadAllMipsVaddr = 0x18e674c;
constexpr sentinel::InlinePrologue kForceLoadAllMipsPrologue{{0xD10343FFu, 0xF9005BF4u, 0xA90C7BF3u, 0xF9403808u}};
using ForceLoadAllMipsFn = std::uint64_t (*)(void* texture);

// CGTextureResource fields on Quest, measured from the two functions above (the PC offsets differ: resident +0xc0).
constexpr std::size_t kNameOffset = 0x38;            // CSymbol64 name; Attach copies it into the device-texture request
constexpr std::size_t kInfoOffset = 0x70;            // pointer to the texture info block (set by Attach on success)
constexpr std::size_t kResidentOffset = 0xE0;        // u32 resident level; equals the mip count when complete
constexpr std::size_t kInfoOwnsStreamOffset = 0xC0;  // info: u32, non-zero for a texture that owns its full data stream
constexpr std::size_t kInfoMipCountOffset = 0xCC;    // info: u32 mip count

struct AttachTag {};
using AttachFn = std::uint64_t(void* texture, void* stream);
using AttachThunk = sentinel::CallbackThunk<AttachTag, AttachFn>;

ForceLoadAllMipsFn g_forceLoadAllMips = nullptr;  // set by the install, once its prologue is checked

template <typename T>
bool ReadField(const void* base, std::size_t offset, T* out) noexcept {
  if (base == nullptr) return false;
  std::memcpy(out, static_cast<const std::uint8_t*>(base) + offset, sizeof(T));
  return true;
}

// Runs after every successful Attach. A texture whose name is the UI system's is forced to its top mip; the outcome is
// logged once per UI texture with the mip it asked for. Called from a thunk entry: no allocation, no throw.
void ForceIfUi(void* texture) noexcept {
  std::uint64_t nameHash = 0;
  if (!ReadField(texture, kNameOffset, &nameHash)) return;
  const char* name = nevr_ui_texture_mips::NameForHash(evr::kSymCacheData, static_cast<std::size_t>(evr::kSymCacheCount), nameHash);
  if (name == nullptr || !nevr_ui_texture_mips::IsUiTextureName(name)) return;

  char hashText[19];
  sentinel::HexString(hashText, static_cast<std::uintptr_t>(nameHash));
  const void* info = nullptr;
  nevr_ui_texture_mips::TextureState state{};
  if (!ReadField(texture, kInfoOffset, &info) || !ReadField(texture, kResidentOffset, &state.resident) ||
      !ReadField(info, kInfoMipCountOffset, &state.mipCount) || !ReadField(info, kInfoOwnsStreamOffset, &state.ownsStream)) {
    sentinel::LogFields(sentinel::LogLevel::kWarn, "ui_texture",
                        {{"status", "unreadable"}, {"name", name}, {"hash", hashText}});
    return;
  }
  if (!nevr_ui_texture_mips::NeedsTopMip(state)) {
    sentinel::LogFields(sentinel::LogLevel::kInfo, "ui_texture",
                        {{"status", "already_top"},
                         {"name", name},
                         {"hash", hashText},
                         {"resident", state.resident},
                         {"requested_mip", state.mipCount}});
    return;
  }
  const std::uint64_t result = g_forceLoadAllMips(texture);
  const bool ok = static_cast<std::uint32_t>(result) == 0;
  sentinel::LogFields(ok ? sentinel::LogLevel::kInfo : sentinel::LogLevel::kWarn, "ui_texture",
                      {{"status", ok ? "forced" : "force_failed"},
                       {"name", name},
                       {"hash", hashText},
                       {"resident_before", state.resident},
                       {"requested_mip", state.mipCount}});
}

// The detour. The thunk's original is the trampoline: it runs the displaced prologue and continues in Attach.
std::uint64_t OnAttach(AttachThunk::Fn original, void* texture, void* stream) noexcept {
  const std::uint64_t result = original(texture, stream);
  if (static_cast<std::uint32_t>(result) == 0) ForceIfUi(texture);
  return result;
}

NEVR_HOOK_RECORD(kAttachRecord, AttachThunk, &OnAttach);

}  // namespace

std::size_t InstallUiTextureMips() noexcept {
  sentinel::ElfImage image;
  if (!sentinel::FindLoadedImage(sentinel::pinned::kLibR15, &image)) {
    sentinel::LogFields(sentinel::LogLevel::kWarn, "ui_texture", {{"status", "libr15_not_loaded"}});
    return 0;
  }
  char buildId[41] = {};
  if (!sentinel::ReadBuildId(image, buildId, sizeof buildId) ||
      std::strcmp(buildId, sentinel::pinned::kLibR15BuildId) != 0) {
    sentinel::LogFields(sentinel::LogLevel::kWarn, "ui_texture",
                        {{"status", "build_id_mismatch"}, {"build_id", buildId}});
    return 0;
  }

  char targetText[19];
  const void* force = reinterpret_cast<const void*>(image.base + kForceLoadAllMipsVaddr);
  const sentinel::InlineStatus forceStatus = sentinel::CheckInlineTarget(force, kForceLoadAllMipsPrologue);
  if (forceStatus != sentinel::InlineStatus::kOk) {
    sentinel::LogFields(sentinel::LogLevel::kError, "ui_texture",
                        {{"status", sentinel::InlineStatusName(forceStatus)},
                         {"hook", "ForceLoadAllMips"},
                         {"target", sentinel::HexString(targetText, reinterpret_cast<std::uintptr_t>(force))}});
    return 0;
  }
  g_forceLoadAllMips = reinterpret_cast<ForceLoadAllMipsFn>(const_cast<void*>(force));

  const void* attach = reinterpret_cast<const void*>(image.base + kAttachVaddr);
  AttachThunk::Arm(kAttachRecord);
  const sentinel::InlineStatus st = sentinel::InstallInlineThunk<AttachThunk>(attach, kAttachPrologue);
  sentinel::LogFields(st == sentinel::InlineStatus::kOk ? sentinel::LogLevel::kInfo : sentinel::LogLevel::kError,
                      "ui_texture",
                      {{"status", st == sentinel::InlineStatus::kOk ? "installed" : sentinel::InlineStatusName(st)},
                       {"hook", "CGTextureResource::Attach"},
                       {"target", sentinel::HexString(targetText, reinterpret_cast<std::uintptr_t>(attach))}});
  return st == sentinel::InlineStatus::kOk ? 1 : 0;
}

}  // namespace nevr_quest::ui_texture_mips
