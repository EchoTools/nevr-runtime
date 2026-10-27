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
#include "inline_hook.h"
#include "pinned_targets.h"
#include "runtime/patch/splash_skip_rules.h"

namespace nevr_quest::splash_skip {
namespace {

using sentinel::InlinePrologue;
using sentinel::InlineStatus;
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

// Proves a pinned libr15 function is the pinned build's (build ID, then its first four words) and returns its address.
// nullptr with `reason` set when it is not.
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

bool Install() noexcept {
  sentinel::ElfImage image;
  if (!sentinel::FindLoadedImage(sentinel::pinned::kLibR15, &image)) {
    sentinel::LogFields(sentinel::LogLevel::kWarn, "splash_skip", {{"result", "refused"}, {"reason", "module_not_loaded"}});
    return false;
  }
  char buildId[64] = {};
  if (!sentinel::ReadBuildId(image, buildId, sizeof(buildId)) ||
      std::strcmp(buildId, sentinel::pinned::kLibR15BuildId) != 0) {
    sentinel::LogFields(sentinel::LogLevel::kWarn, "splash_skip",
                        {{"result", "refused"}, {"reason", "build_id_mismatch"}, {"build_id", buildId}});
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

  // The four words the inline patch displaces, from the pinned build (InstallInlineThunk checks them again before a write).
  InlinePrologue prologue{};
  std::memcpy(prologue.words, sentinel::pinned::kWidgetUpdateCode, sizeof(prologue.words));
  WidgetUpdateThunk::Arm(kWidgetUpdateRecord);
  const void* const target = reinterpret_cast<const void*>(image.base + sentinel::pinned::kWidgetUpdateVaddr);
  char targetText[19];
  const InlineStatus st = sentinel::InstallInlineThunk<WidgetUpdateThunk>(target, prologue);
  sentinel::LogFields(st == InlineStatus::kOk ? sentinel::LogLevel::kInfo : sentinel::LogLevel::kError, "splash_skip",
                      {{"result", st == InlineStatus::kOk ? "installed" : sentinel::InlineStatusName(st)},
                       {"target", sentinel::HexString(targetText, reinterpret_cast<std::uintptr_t>(target))},
                       {"min_live_ms", static_cast<long long>(nevr_splash_skip::kMinPresentMs)}});
  return st == InlineStatus::kOk;
}

}  // namespace nevr_quest::splash_skip
