#include "runtime/patch/ui_texture_mips.h"

#include <windows.h>

#include <array>
#include <atomic>
#include <cstring>

#include "core/logging.h"
#include "nevr/pc/nevr_common.h"
#include "runtime/hook/hook_guard.h"
#include "runtime/hook/patching.h"
#include "runtime/log/boot_log_tee.h"
#include "runtime/log/symcache.h"
#include "runtime/patch/ui_texture_mips_rules.h"

namespace evr {
extern const SymCacheEntry kSymCacheData[];  // defined in the generated symcache_data.cpp, sorted by hash
extern const int kSymCacheCount;
}  // namespace evr

namespace nevr_ui_texture_mips {
namespace {

// CGTextureResource::Attach (vtable slot 7; Quest 0x19e75c4 in the disassembly): (this, stream*) -> 0 on success.
// MOV [RSP+0x10],RDX; MOV [RSP+8],RCX; PUSH RBP; PUSH RBX; PUSH RSI; PUSH RDI; PUSH R14; LEA RBP,[RSP-0x40];
// SUB RSP,0x140; CMP QWORD [RCX+0x78],0.
constexpr std::uint64_t kAttachVA = 0x1405491C0;
constexpr std::array<std::uint8_t, 32> kAttachPrologue = {0x48, 0x89, 0x54, 0x24, 0x10, 0x48, 0x89, 0x4C, 0x24, 0x08, 0x55,
                                                          0x53, 0x56, 0x57, 0x41, 0x56, 0x48, 0x8D, 0x6C, 0x24, 0xC0, 0x48,
                                                          0x81, 0xEC, 0x40, 0x01, 0x00, 0x00, 0x48, 0x83, 0x79, 0x78};
using AttachFn = std::uint64_t (*)(void* texture, void* stream);

// CGTextureResource::ForceLoadAllMips (Quest 0x19e674c): (this) -> 0 on success. It returns 0 at once when the
// resident level equals the mip count or the texture owns its full data stream, and otherwise reads every missing
// mip and replaces the device texture, polling the streaming IO until it finishes.
// PUSH RBP; LEA RBP,[RSP-0x30]; SUB RSP,0x130; MOV RDX,[RCX+0x70]; MOV EAX,[RDX+0xCC]; CMP [RCX+0xC0],EAX; JZ.
constexpr std::uint64_t kForceLoadAllMipsVA = 0x14056C420;
constexpr std::array<std::uint8_t, 32> kForceLoadAllMipsPrologue = {
    0x40, 0x55, 0x48, 0x8D, 0x6C, 0x24, 0xD0, 0x48, 0x81, 0xEC, 0x30, 0x01, 0x00, 0x00, 0x48, 0x8B,
    0x51, 0x70, 0x8B, 0x82, 0xCC, 0x00, 0x00, 0x00, 0x39, 0x81, 0xC0, 0x00, 0x00, 0x00, 0x0F, 0x84};
using ForceLoadAllMipsFn = std::uint64_t (*)(void* texture);

// CGTextureResource fields (PC; the Quest build's differ, see ui_texture_mips_rules.h).
constexpr std::size_t kNameOffset = 0x38;      // CSymbol64 name (-1 until set); the name is inferred from Attach copying it to the device-texture description
constexpr std::size_t kInfoOffset = 0x70;      // pointer to the texture info block
constexpr std::size_t kResidentOffset = 0xC0;  // u32 resident level; equals the mip count when complete
constexpr std::size_t kInfoOwnsStreamOffset = 0xC0;  // info: u32, non-zero for a texture that owns its full data stream
constexpr std::size_t kInfoMipCountOffset = 0xCC;    // info: u32 mip count

std::uintptr_t g_gameBase = 0;
AttachFn g_originalAttach = nullptr;
ForceLoadAllMipsFn g_forceLoadAllMips = nullptr;
std::atomic<unsigned> g_uiSeen{0};
std::atomic<unsigned> g_forced{0};
std::atomic<unsigned> g_failed{0};

template <typename T>
bool ReadField(const void* base, std::size_t offset, T* out) {
  if (base == nullptr) return false;
  std::memcpy(out, static_cast<const std::uint8_t*>(base) + offset, sizeof(T));
  return true;
}

void ForceIfUi(void* texture) {
  std::uint64_t nameHash = 0;
  if (!ReadField(texture, kNameOffset, &nameHash)) return;
  const char* name = NameForHash(evr::kSymCacheData, static_cast<std::size_t>(evr::kSymCacheCount), nameHash);
  if (name == nullptr || !IsUiTextureName(name)) return;
  g_uiSeen.fetch_add(1, std::memory_order_relaxed);

  const void* info = nullptr;
  TextureState state{};
  if (!ReadField(texture, kInfoOffset, &info) || !ReadField(texture, kResidentOffset, &state.resident) ||
      !ReadField(info, kInfoMipCountOffset, &state.mipCount) || !ReadField(info, kInfoOwnsStreamOffset, &state.ownsStream)) {
    Log(EchoVR::LogLevel::Warning, "[NEVR.UITEXTURE] top mip not forced name=%s hash=0x%016llx reason=texture_fields_unreadable", name,
        static_cast<unsigned long long>(nameHash));
    return;
  }
  if (!NeedsTopMip(state)) {
    Log(EchoVR::LogLevel::Debug, "[NEVR.UITEXTURE] already top mip name=%s hash=0x%016llx resident=%u mips=%u owns_stream=%u", name,
        static_cast<unsigned long long>(nameHash), state.resident, state.mipCount, state.ownsStream);
    return;
  }
  const std::uint64_t result = g_forceLoadAllMips(texture);
  if (static_cast<std::uint32_t>(result) == 0) {
    g_forced.fetch_add(1, std::memory_order_relaxed);
    Log(EchoVR::LogLevel::Info, "[NEVR.UITEXTURE] top mip forced name=%s hash=0x%016llx resident_before=%u requested_mip=%u", name,
        static_cast<unsigned long long>(nameHash), state.resident, state.mipCount);
  } else {
    g_failed.fetch_add(1, std::memory_order_relaxed);
    Log(EchoVR::LogLevel::Warning,
        "[NEVR.UITEXTURE] top mip force failed name=%s hash=0x%016llx resident=%u requested_mip=%u result=0x%llx forced=%u failed=%u", name,
        static_cast<unsigned long long>(nameHash), state.resident, state.mipCount, static_cast<unsigned long long>(result),
        g_forced.load(), g_failed.load());
  }
}

std::uint64_t AttachHook(void* texture, void* stream) {
  const std::uint64_t result = g_originalAttach(texture, stream);
  if (static_cast<std::uint32_t>(result) == 0) ForceIfUi(texture);
  return result;
}

bool Validated(std::uint64_t va, const std::uint8_t* bytes, std::size_t size, const char* name, void** target) {
  *target = nevr::ResolveVA_Checked(g_gameBase, va);
  if (nevr::ValidatePrologue(*target, bytes, size)) return true;
  nevr_boot_log_tee::TeeFprintf("[NEVR.UITEXTURE] %s hook skipped va=0x%llx reason=prologue_mismatch\n", name,
                                static_cast<unsigned long long>(va));
  return false;
}

}  // namespace

void Install(std::uintptr_t gameBase) {
  g_gameBase = gameBase;
  void* force = nullptr;
  void* attach = nullptr;
  if (!Validated(kForceLoadAllMipsVA, kForceLoadAllMipsPrologue.data(), kForceLoadAllMipsPrologue.size(), "ForceLoadAllMips", &force) ||
      !Validated(kAttachVA, kAttachPrologue.data(), kAttachPrologue.size(), "CGTextureResource::Attach", &attach)) {
    return;
  }
  g_forceLoadAllMips = reinterpret_cast<ForceLoadAllMipsFn>(force);
  g_originalAttach = reinterpret_cast<AttachFn>(attach);
  if (PatchDetour(&g_originalAttach, reinterpret_cast<PVOID>(&AttachHook), "CGTextureResource::Attach")) {
    nevr_boot_log_tee::TeeFprintf("[NEVR.UITEXTURE] CGTextureResource::Attach hook installed va=0x%llx ui_prefixes=%zu\n",
                                  static_cast<unsigned long long>(kAttachVA), sizeof(kUiPrefixes) / sizeof(kUiPrefixes[0]));
  }
}

}  // namespace nevr_ui_texture_mips
