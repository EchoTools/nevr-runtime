#include "quest/sentinel/ui_texture_mips.h"

#include <cstdint>
#include <cstring>

#include "callback_thunk.h"
#include "got_hook.h"
#include "hook_install.h"
#include "hook_log.h"
#include "libr15_attach.h"
#include "pinned_targets.h"
#include "runtime/log/symcache.h"
#include "runtime/patch/ui_texture_mips_rules.h"

namespace evr {
extern const SymCacheEntry kSymCacheData[];  // the merged PC and Quest symbol names, sorted by hash (symcache_data.cpp)
extern const int kSymCacheCount;
}  // namespace evr

namespace nevr_quest::ui_texture_mips {
namespace {

// NRadEngine::CGTextureResource::Attach (libr15 0x18e75c4 pinned; disassembly 0x19e75c4 = pinned + 0x100000): this (x0),
// stream (x1); returns 0 on success (w0). Reached through the resource's vtable, so there is no GOT slot: it is the row
// `ui_texture.attach` of the build-time patch table (quest/patch/libr15_patch_table.h; docs/adr/0021), whose address and
// stock words the patcher checks against the store file. The hook is one atomic store into the row's slot.
constexpr const char* kAttachRow = "ui_texture.attach";

// NRadEngine::CGTextureResource::ForceLoadAllMips (libr15 0x18e674c pinned; disassembly 0x19e674c): this (x0), returns 0 on
// success. It returns at once when the resident level (this+0xe0) already equals the mip count (info+0xcc) or the
// texture owns its full data stream (info+0xc0 != 0). It is called directly, not hooked.
//   sub sp,sp,#0xd0; str x20,[sp,#0xb0]; stp x19,x30,[sp,#0xc0]; ldr x8,[x0,#0x70].
// Its address and first four words are sentinel::pinned::kForceLoadAllMipsVaddr/Code (pinned_targets.h; got_pinned_test
// reads them in the store file): the patch does not touch it, so the install proves them before calling it.
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

// The detour. The thunk's original is the row's trampoline: it runs the displaced words and continues in Attach.
std::uint64_t OnAttach(AttachThunk::Fn original, void* texture, void* stream) noexcept {
  const std::uint64_t result = original(texture, stream);
  if (static_cast<std::uint32_t>(result) == 0) ForceIfUi(texture);
  return result;
}

NEVR_HOOK_RECORD(kAttachRecord, AttachThunk, &OnAttach);

}  // namespace

std::size_t InstallUiTextureMips(sentinel::ImageLookup lookup) noexcept {
  sentinel::ElfImage image;
  if (lookup == nullptr || !lookup(sentinel::pinned::kLibR15, &image)) {
    sentinel::LogFields(sentinel::LogLevel::kWarn, "ui_texture", {{"status", "libr15_not_loaded"}});
    return 0;
  }
  char targetText[19];
  // Nothing is read from libr15 before Attach has verified it (the live ID is then the patched file's).
  if (!nevr_quest::libr15_patch::Verified()) {
    sentinel::LogFields(sentinel::LogLevel::kError, "ui_texture",
                        {{"status", nevr_quest::libr15_patch::ActivateStatusName(nevr_quest::libr15_patch::ActivateStatus::kNotVerified)},
                         {"hook", "CGTextureResource::Attach"},
                         {"row", kAttachRow},
                         {"target", sentinel::HexString(targetText, 0)}});
    return 0;
  }

  const unsigned char* const force = reinterpret_cast<const unsigned char*>(image.base) + sentinel::pinned::kForceLoadAllMipsVaddr;
  if (std::memcmp(force, sentinel::pinned::kForceLoadAllMipsCode, sizeof(sentinel::pinned::kForceLoadAllMipsCode)) != 0) {
    sentinel::LogFields(sentinel::LogLevel::kError, "ui_texture",
                        {{"status", "prologue_mismatch"},
                         {"hook", "ForceLoadAllMips"},
                         {"target", sentinel::HexString(targetText, reinterpret_cast<std::uintptr_t>(force))}});
    return 0;
  }
  g_forceLoadAllMips = reinterpret_cast<ForceLoadAllMipsFn>(const_cast<unsigned char*>(force));

  AttachThunk::Arm(kAttachRecord);
  const nevr_quest::libr15_patch::ActivateStatus st = sentinel::InstallRowThunk<AttachThunk>(kAttachRow);
  const bool ok = st == nevr_quest::libr15_patch::ActivateStatus::kOk;
  sentinel::LogFields(ok ? sentinel::LogLevel::kInfo : sentinel::LogLevel::kError, "ui_texture",
                      {{"status", ok ? "installed" : nevr_quest::libr15_patch::ActivateStatusName(st)},
                       {"hook", "CGTextureResource::Attach"},
                       {"row", kAttachRow},
                       {"target", sentinel::HexString(targetText, nevr_quest::libr15_patch::RowSiteAddress(kAttachRow))}});
  return ok ? 1 : 0;
}

}  // namespace nevr_quest::ui_texture_mips
