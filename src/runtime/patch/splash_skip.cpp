#include "runtime/patch/splash_skip.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cstring>

#include "core/logging.h"
#include "nevr_common.h"
#include "runtime/hook/hook_guard.h"
#include "runtime/hook/patching.h"
#include "runtime/log/boot_log_tee.h"
#include "runtime/patch/splash_skip_rules.h"

namespace nevr_splash_skip {
namespace {

// CR15VRMenuWidgetCS per-frame "Update" (registered by fcn.1408e2150 with the name "CR15VRMenuWidgetCS"): (this).
// MOV RAX,RSP; MOV [RAX+8],RBX; PUSH RBP,RSI,RDI,R12-R15; LEA RBP,[RAX-0xE8]; SUB RSP,0x1B0.
constexpr std::uint64_t kWidgetUpdateVA = 0x140A7BDD0;
constexpr std::array<std::uint8_t, 32> kWidgetUpdatePrologue = {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x08, 0x55, 0x56, 0x57, 0x41,
                                                                0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8D, 0xA8, 0x18,
                                                                0xFF, 0xFF, 0xFF, 0x48, 0x81, 0xEC, 0xB0, 0x01, 0x00, 0x00};
using WidgetUpdateFn = void (*)(void* widget);

// DispatchComponentEvent twin without the self-dispatch gate, the call the widget makes for a click:
// (ctx, event symbol, link0, link1, flags). SUB RSP,0x58; MOV RAX,[RCX+0x58]; MOV [RSP+0x20],RDX; LEA RDX,[RSP+0x20];
// MOV [RSP+0x30],R8; MOV R8D,[RSP+0x80].
constexpr std::uint64_t kDispatchClickVA = 0x14050FC90;
constexpr std::array<std::uint8_t, 32> kDispatchClickPrologue = {0x48, 0x83, 0xEC, 0x58, 0x48, 0x8B, 0x41, 0x58, 0x48, 0x89, 0x54,
                                                                 0x24, 0x20, 0x48, 0x8D, 0x54, 0x24, 0x20, 0x4C, 0x89, 0x44, 0x24,
                                                                 0x30, 0x44, 0x8B, 0x84, 0x24, 0x80, 0x00, 0x00, 0x00, 0x48};
using DispatchClickFn = void (*)(void* ctx, std::uint64_t symbol, std::uint64_t link0, std::uint64_t link1, std::uint32_t flags);

// The widget's button table lookup: (state = widget+0x138, id) -> bit 2 (set for one input step when the input became
// active) of the entry's flags at +0x230, 0 when the id is not in the table. Its twin at 0x140F9BF40 returns bit 4 and
// is byte-identical but for the shift at +0x5F; the shift is checked so the two cannot be taken for each other.
// MOV R8,[RCX+0x80]; TEST R8,R8; JZ; MOV R11,[RCX+0x50]; XOR R9D,R9D; ...
constexpr std::uint64_t kButtonEdgeVA = 0x140F9B770;
constexpr std::array<std::uint8_t, 32> kButtonPrologue = {0x4C, 0x8B, 0x81, 0x80, 0x00, 0x00, 0x00, 0x4D, 0x85, 0xC0, 0x74,
                                                          0x3E, 0x4C, 0x8B, 0x59, 0x50, 0x45, 0x33, 0xC9, 0x4D, 0x85, 0xC0,
                                                          0x74, 0x32, 0x0F, 0x1F, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00};
constexpr std::size_t kButtonShiftOffset = 0x5D;  // SHR EAX,imm8: C1 E8 02 (edge); the held twin has C1 E8 04
constexpr std::array<std::uint8_t, 3> kEdgeShift = {0xC1, 0xE8, 0x02};
using ButtonFn = std::uint32_t (*)(void* state, std::uint64_t id);

// CR15VRMenuWidgetCS fields, from the update's own reads (offsets in bytes from the widget).
constexpr std::size_t kCtxOffset = 0x80;            // param_1[0x10]: the component context passed to the dispatch
constexpr std::size_t kDefsPtrOffset = 0xB8;        // param_1[0x17]: pointer to a pointer to the 0x60-byte element definitions
constexpr std::size_t kDenseTableOffset = 0xF8;     // param_1[0x1f]: per dense element, 0x18 bytes; u16 at +0x10 = definition index
constexpr std::size_t kLinkTableOffset = 0x100;     // param_1[0x20]: per dense element, 0x10 bytes: link0, link1
constexpr std::size_t kClickBoundOffset = 0xF2;     // u16: the dense bound the click paths use
constexpr std::size_t kHitBoundOffset = 0xF4;       // u16: the dense bound of the hit-test loop
constexpr std::size_t kButtonStateOffset = 0x138;   // param_1 + 0x27: the button table
constexpr std::size_t kDenseStride = 0x18;
constexpr std::size_t kDenseDefIndexOffset = 0x10;
constexpr std::size_t kLinkStride = 0x10;
constexpr std::size_t kDefStride = 0x60;
constexpr std::size_t kDefClickSymbolOffset = 0x58;  // -1 when the element has none
constexpr std::size_t kMaxDenseElements = 4096;       // a bound read from the game that is larger is not a bound

WidgetUpdateFn g_originalUpdate = nullptr;
DispatchClickFn g_dispatchClick = nullptr;
ButtonFn g_buttonEdge = nullptr;
Skipper g_skipper;
const void* g_trackedWidget = nullptr;
bool g_wasPresentLogged = false;

bool Readable(const void* address, std::size_t length) {
  if (address == nullptr || length == 0) return false;
  MEMORY_BASIC_INFORMATION info{};
  if (VirtualQuery(address, &info, sizeof(info)) == 0) return false;
  if (info.State != MEM_COMMIT || (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0) return false;
  const auto begin = reinterpret_cast<std::uintptr_t>(address);
  const auto regionEnd = reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize;
  return begin + length <= regionEnd;
}

template <typename T>
T ReadAt(const void* base, std::size_t offset) {
  T value{};
  std::memcpy(&value, static_cast<const std::uint8_t*>(base) + offset, sizeof(T));
  return value;
}

struct ContinueElement {
  bool found = false;
  void* ctx = nullptr;
  std::uint64_t link0 = 0;
  std::uint64_t link1 = 0;
  std::size_t denseIndex = 0;
};

// Finds the Continue element among the widget's elements, reading them the way the update's click paths do.
ContinueElement FindContinue(const void* widget) {
  ContinueElement out;
  const auto bound = std::min<std::size_t>(ReadAt<std::uint16_t>(widget, kClickBoundOffset), ReadAt<std::uint16_t>(widget, kHitBoundOffset));
  if (bound == 0 || bound > kMaxDenseElements) return out;
  const auto defsPtr = ReadAt<const std::uint8_t* const*>(widget, kDefsPtrOffset);
  const auto dense = ReadAt<const std::uint8_t*>(widget, kDenseTableOffset);
  const auto links = ReadAt<const std::uint8_t*>(widget, kLinkTableOffset);
  if (!Readable(defsPtr, sizeof(void*)) || !Readable(dense, bound * kDenseStride) || !Readable(links, bound * kLinkStride)) return out;
  const std::uint8_t* defs = *defsPtr;
  if (defs == nullptr) return out;
  const long long index = nevr_splash_skip::FindContinueElement(bound, [&](std::size_t i) {
    const auto defIndex = ReadAt<std::uint16_t>(dense, i * kDenseStride + kDenseDefIndexOffset);
    return ReadAt<std::uint64_t>(defs, static_cast<std::size_t>(defIndex) * kDefStride + kDefClickSymbolOffset);
  });
  if (index < 0) return out;
  out.found = true;
  out.denseIndex = static_cast<std::size_t>(index);
  out.link0 = ReadAt<std::uint64_t>(links, out.denseIndex * kLinkStride);
  out.link1 = ReadAt<std::uint64_t>(links, out.denseIndex * kLinkStride + 8);
  out.ctx = ReadAt<void*>(widget, kCtxOffset);
  return out;
}

void Observe(void* widget) {
  const std::uint64_t now = GetTickCount64();
  const ContinueElement element = FindContinue(widget);
  if (!element.found) {
    if (widget == g_trackedWidget) {
      g_skipper.Step(false, false, now);
      g_trackedWidget = nullptr;
      g_wasPresentLogged = false;
    }
    return;
  }
  g_trackedWidget = widget;
  if (!g_wasPresentLogged) {
    g_wasPresentLogged = true;
    Log(EchoVR::LogLevel::Info,
        "[NEVR.SPLASH] splash continue element live widget=%p dense_index=%zu link0=0x%016llx link1=0x%016llx skip_after_ms=%llu",
        widget, element.denseIndex, static_cast<unsigned long long>(element.link0), static_cast<unsigned long long>(element.link1),
        static_cast<unsigned long long>(kMinPresentMs));
  }
  void* const state = static_cast<std::uint8_t*>(widget) + kButtonStateOffset;
  const long long button = PressedButton([&](std::uint64_t id) { return (g_buttonEdge(state, id) & 0xFF) != 0; });
  if (!g_skipper.Step(true, button >= 0, now) || element.ctx == nullptr) return;
  g_dispatchClick(element.ctx, kContinueClickedSymbol, element.link0, element.link1, 1);
  Log(EchoVR::LogLevel::Info,
      "[NEVR.SPLASH] splash skipped by button press splash=ui_mnu_splash_screen click_symbol=continue_clicked button_id=%lld "
      "live_ms=%llu link0=0x%016llx link1=0x%016llx",
      button, static_cast<unsigned long long>(g_skipper.PresentForMs(now)), static_cast<unsigned long long>(element.link0),
      static_cast<unsigned long long>(element.link1));
}

void WidgetUpdateHook(void* widget) {
  g_originalUpdate(widget);
  if (widget != nullptr) Observe(widget);
}

std::uintptr_t g_gameBase = 0;

// Resolves `va` and checks `size` bytes of `expected` at it (and, when `extra` is non-null, `extraSize` bytes at
// `extraOffset`). Logs the mismatch and returns nullptr; the caller installs nothing.
void* ResolveValidated(std::uint64_t va, const std::uint8_t* expected, std::size_t size, const char* name,
                       const std::uint8_t* extra = nullptr, std::size_t extraOffset = 0, std::size_t extraSize = 0) {
  void* target = nevr::ResolveVA_Checked(g_gameBase, va);
  const bool ok = nevr::ValidatePrologue(target, expected, size) &&
                  (extra == nullptr || nevr::ValidatePrologue(static_cast<std::uint8_t*>(target) + extraOffset, extra, extraSize));
  if (ok) return target;
  nevr_boot_log_tee::TeeFprintf("[NEVR.SPLASH] splash skip not installed: %s va=0x%llx reason=bytes_mismatch\n", name,
                                static_cast<unsigned long long>(va));
  return nullptr;
}

}  // namespace

void Install(std::uintptr_t gameBase) {
  g_gameBase = gameBase;
  void* update = ResolveValidated(kWidgetUpdateVA, kWidgetUpdatePrologue.data(), kWidgetUpdatePrologue.size(), "CR15VRMenuWidgetCS::Update");
  void* dispatch = ResolveValidated(kDispatchClickVA, kDispatchClickPrologue.data(), kDispatchClickPrologue.size(), "DispatchComponentEvent");
  void* edge = ResolveValidated(kButtonEdgeVA, kButtonPrologue.data(), kButtonPrologue.size(), "button edge predicate", kEdgeShift.data(),
                                kButtonShiftOffset, kEdgeShift.size());
  if (update == nullptr || dispatch == nullptr || edge == nullptr) return;
  g_dispatchClick = reinterpret_cast<DispatchClickFn>(dispatch);
  g_buttonEdge = reinterpret_cast<ButtonFn>(edge);
  g_originalUpdate = reinterpret_cast<WidgetUpdateFn>(update);
  if (PatchDetour(&g_originalUpdate, reinterpret_cast<PVOID>(&WidgetUpdateHook), "CR15VRMenuWidgetCS::Update")) {
    nevr_boot_log_tee::TeeFprintf("[NEVR.SPLASH] splash skip hook installed va=0x%llx min_live_ms=%llu\n",
                                  static_cast<unsigned long long>(kWidgetUpdateVA), static_cast<unsigned long long>(kMinPresentMs));
  }
}

}  // namespace nevr_splash_skip
