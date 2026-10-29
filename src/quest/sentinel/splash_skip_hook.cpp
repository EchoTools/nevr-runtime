#include "splash_skip_hook.h"

#include <time.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "callback_thunk.h"
#include "got_hook.h"
#include "hook_install.h"
#include "hook_log.h"
#include "libr15_attach.h"
#include "pinned_targets.h"
#include "runtime/patch/splash_skip_rules.h"

namespace nevr_quest::splash_skip {
namespace {

using nevr_quest::libr15_patch::ActivateStatus;
using sentinel::pinned::InputPressedSig;
using sentinel::pinned::SendComponentEventSig;

// CR15VRMenuWidgetCS fields, from the update's own reads (offsets in bytes from the widget). The same offsets as the PC
// hook (src/runtime/patch/splash_skip.cpp): the pinned Update reads each of them in its click path.
constexpr std::size_t kCtxOffset = 0x80;         // the component context: the game space the click is sent to
constexpr std::size_t kDefsPtrOffset = 0xB8;     // pointer to a pointer to the 0x60-byte element definitions
constexpr std::size_t kDenseTableOffset = 0xF8;  // per dense element, 0x18 bytes; u16 at +0x10 = definition index
constexpr std::size_t kLinkTableOffset = 0x100;  // per dense element, 0x10 bytes: link0, link1
constexpr std::size_t kClickBoundOffset = 0xF2;  // u16: the dense bound the click paths use
constexpr std::size_t kHitBoundOffset = 0xF4;    // u16: the dense bound of the hit-test loop
constexpr std::size_t kButtonStateOffset = 0x138;  // the input listener the Update polls (its button table)
constexpr std::size_t kDenseStride = 0x18;
constexpr std::size_t kDenseDefIndexOffset = 0x10;
constexpr std::size_t kLinkStride = 0x10;
constexpr std::size_t kDefStride = 0x60;
constexpr std::size_t kDefClickSymbolOffset = 0x58;  // -1 when the element has none
constexpr std::size_t kMaxDenseElements = 4096;      // a bound larger than this is not a bound

// CR15VRMenuWidgetCS::Update is the row `splash_skip.widget_update` of the build-time patch table
// (quest/patch/libr15_patch_table.h; docs/adr/0021): the engine registers its address, so no GOT slot names it.
constexpr const char* kWidgetUpdateRow = "splash_skip.widget_update";

struct WidgetUpdateTag {};
using WidgetUpdateThunk = sentinel::CallbackThunk<WidgetUpdateTag, void(void*)>;

// Resolved in Install, before the hook is armed, and read only on the game's update thread.
SendComponentEventSig* g_sendEvent = nullptr;
InputPressedSig* g_pressed = nullptr;
nevr_splash_skip::Skipper g_skipper;
const void* g_tracked = nullptr;
bool g_loggedLive = false;

std::uint64_t NowMs() noexcept {
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<std::uint64_t>(ts.tv_sec) * 1000u + static_cast<std::uint64_t>(ts.tv_nsec) / 1000000u;
}

template <typename T>
T ReadAt(const void* base, std::size_t offset) noexcept {
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

// Finds the Continue element among the widget's elements, reading them the way the update's own click paths do. The
// game dereferences these tables in the same frame whenever the bound is non-zero, so reading them here is no new
// access; the bound is checked against kMaxDenseElements before any table is read.
ContinueElement FindContinue(const void* widget) noexcept {
  ContinueElement out;
  const std::size_t bound =
      std::min<std::size_t>(ReadAt<std::uint16_t>(widget, kClickBoundOffset), ReadAt<std::uint16_t>(widget, kHitBoundOffset));
  if (bound == 0 || bound > kMaxDenseElements) return out;
  const auto* defsPtr = ReadAt<const std::uint8_t* const*>(widget, kDefsPtrOffset);
  if (defsPtr == nullptr || *defsPtr == nullptr) return out;
  const std::uint8_t* defs = *defsPtr;
  const auto* dense = ReadAt<const std::uint8_t*>(widget, kDenseTableOffset);
  const auto* links = ReadAt<const std::uint8_t*>(widget, kLinkTableOffset);
  if (dense == nullptr || links == nullptr) return out;
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

void Observe(void* widget) noexcept {
  const std::uint64_t now = NowMs();
  const ContinueElement element = FindContinue(widget);
  if (!element.found) {
    if (widget == g_tracked) {
      g_skipper.Step(false, false, now);
      g_tracked = nullptr;
      g_loggedLive = false;
    }
    return;
  }
  g_tracked = widget;
  if (!g_loggedLive) {
    g_loggedLive = true;
    sentinel::LogFields(sentinel::LogLevel::kInfo, "splash_skip",
                        {{"result", "live"},
                         {"splash", "ui_mnu_splash_screen"},
                         {"dense_index", static_cast<long long>(element.denseIndex)},
                         {"skip_after_ms", static_cast<long long>(nevr_splash_skip::kMinPresentMs)}});
  }
  void* const state = static_cast<std::uint8_t*>(widget) + kButtonStateOffset;
  const long long button =
      nevr_splash_skip::PressedButton([&](std::uint64_t id) { return (g_pressed(state, id) & 0xFFu) != 0; });
  if (!g_skipper.Step(true, button >= 0, now) || element.ctx == nullptr) return;
  g_sendEvent(element.ctx, nevr_splash_skip::kContinueClickedSymbol, element.link0, element.link1, 1);
  sentinel::LogFields(sentinel::LogLevel::kInfo, "splash_skip",
                      {{"result", "skipped"},
                       {"splash", "ui_mnu_splash_screen"},
                       {"click_symbol", "continue_clicked"},
                       {"button_id", button},
                       {"live_ms", static_cast<long long>(g_skipper.PresentForMs(now))}});
}

void WidgetUpdateHook(WidgetUpdateThunk::Fn original, void* widget) noexcept {
  original(widget);
  if (widget != nullptr) Observe(widget);
}

NEVR_HOOK_RECORD(kWidgetUpdateRecord, WidgetUpdateThunk, &WidgetUpdateHook);

// Proves a function the hook CALLS is the pinned build's (its first four words) and returns its address. nullptr with
// `reason` set when it is not. The patched function itself is not checked here: it is a table row.
void* ResolvePinned(const sentinel::ElfImage& image, std::uint64_t vaddr, const std::uint32_t (&code)[4],
                    const char*& reason) noexcept {
  const unsigned char* fn = reinterpret_cast<const unsigned char*>(image.base) + vaddr;
  if (std::memcmp(fn, code, sizeof(code)) != 0) {
    reason = "prologue_mismatch";
    return nullptr;
  }
  return const_cast<unsigned char*>(fn);
}

}  // namespace

bool Install(sentinel::ImageLookup lookup) noexcept {
  sentinel::ElfImage image;
  if (lookup == nullptr || !lookup(sentinel::pinned::kLibR15, &image)) {
    sentinel::LogFields(sentinel::LogLevel::kWarn, "splash_skip", {{"result", "refused"}, {"reason", "module_not_loaded"}});
    return false;
  }
  // Nothing is read from libr15 before Attach has verified it (the live ID is then the patched file's).
  if (!nevr_quest::libr15_patch::Verified()) {
    sentinel::LogFields(sentinel::LogLevel::kWarn, "splash_skip",
                        {{"result", "refused"}, {"reason", nevr_quest::libr15_patch::ActivateStatusName(ActivateStatus::kNotVerified)}});
    return false;
  }
  const char* reason = "";
  void* const send = ResolvePinned(image, sentinel::pinned::kSendComponentEventVaddr, sentinel::pinned::kSendComponentEventCode,
                                   reason);
  void* const pressed = ResolvePinned(image, sentinel::pinned::kInputPressedVaddr, sentinel::pinned::kInputPressedCode, reason);
  if (send == nullptr || pressed == nullptr) {
    sentinel::LogFields(sentinel::LogLevel::kError, "splash_skip", {{"result", "refused"}, {"reason", reason}});
    return false;
  }
  std::memcpy(&g_sendEvent, &send, sizeof(g_sendEvent));
  std::memcpy(&g_pressed, &pressed, sizeof(g_pressed));

  WidgetUpdateThunk::Arm(kWidgetUpdateRecord);
  const ActivateStatus st = sentinel::InstallRowThunk<WidgetUpdateThunk>(kWidgetUpdateRow);
  const bool ok = st == ActivateStatus::kOk;
  char targetText[19];
  sentinel::LogFields(ok ? sentinel::LogLevel::kInfo : sentinel::LogLevel::kError, "splash_skip",
                      {{"result", ok ? "installed" : nevr_quest::libr15_patch::ActivateStatusName(st)},
                       {"row", kWidgetUpdateRow},
                       {"target", sentinel::HexString(targetText, nevr_quest::libr15_patch::RowSiteAddress(kWidgetUpdateRow))},
                       {"min_live_ms", static_cast<long long>(nevr_splash_skip::kMinPresentMs)}});
  return ok;
}

}  // namespace nevr_quest::splash_skip
