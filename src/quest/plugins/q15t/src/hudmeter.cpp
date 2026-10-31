#include "hudmeter.h"

#include <atomic>
#include <cstring>

#include "pbhud_log.h"
#include "plugin_log.h"
#include "safe_read.h"

namespace q15t {
namespace {

// ELF vaddrs of the pinned build (disassembly VA = ELF + 0x100000)
constexpr std::uintptr_t kSlot = 0x36c6ce8, kFn = 0x2726eac;  // R57: JUMP_SLOT / CSetCanvasElementAttributesNode::Enter
constexpr char kSym[] = "_ZN10NRadEngine31CSetCanvasElementAttributesNode5EnterERKNS_35SSetCanvasElementAttributesNodeDataE";
constexpr std::uint64_t kComp = 0xe55af932c88ec864ull;  // the lobby helmet's canvas component
constexpr std::uint64_t kItem = 0x9fd2e8cc0e46df3cull;  // sym("weapon_meter")
// SSetCanvasElementAttributesNodeData: +0x18 component, +0x20 item, +0x34 size flag, +0x38 size (x, y). The node reads
// the size at data+0x38 itself (ELF 0x2727134 add x3, x19, #0x38 -> CCanvasUICS::SetElementSize).
constexpr std::uintptr_t kDComp = 0x18, kDItem = 0x20, kDSizeOn = 0x34, kDSize = 0x38;
constexpr std::size_t kDataPrefix = 0x40;  // read once per call: everything through the size
constexpr float kFull[2] = {86.0f, 88.0f};  // weapon_meter's authored size
constexpr unsigned kFillLines = 8;

using NextFn = std::uint64_t (*)(void* node, const void* data);
std::atomic<std::uintptr_t> g_next{0};
std::atomic<unsigned> g_full{0}, g_guarded{0}, g_unreadable{0}, g_fill_lines{0};
std::atomic<bool> g_passed{false};
std::atomic<std::uint32_t> g_last_y{0};
std::atomic<std::uint64_t> g_other_comp{0};

template <typename T>
T At(const std::uint8_t* p, std::uintptr_t off) {
  T v;
  std::memcpy(&v, p + off, sizeof v);
  return v;
}

__attribute__((noinline, cold)) void LogFull(unsigned n, std::uint64_t comp, std::uint64_t item) {
  if (n & (n - 1)) return;  // 1st, 2nd, 4th, 8th ...
  Rec().U("n", n).F("from_x", 0).F("from_y", 0).F("to_x", static_cast<double>(kFull[0])).F("to_y", static_cast<double>(kFull[1]))
      .X("comp", comp).X("item", item).Emit(NEVR_LOG_INFO, "hudmeter: full size written for a 0 x 0 weapon_meter write");
}

__attribute__((noinline, cold)) void LogFill(float x, float y) {  // the first kFillLines size writes that change the height
  std::uint32_t yb;
  std::memcpy(&yb, &y, sizeof yb);
  if (g_last_y.exchange(yb, std::memory_order_relaxed) == yb) return;
  if (g_fill_lines.fetch_add(1, std::memory_order_relaxed) < kFillLines)
    Rec().F("x", static_cast<double>(x)).F("y", static_cast<double>(y)).Emit(NEVR_LOG_INFO, "hudmeter: fill weapon_meter size");
}

__attribute__((noinline, cold)) void LogOtherComp(std::uint64_t comp) {  // the first component other than the lobby helmet's, once
  std::uint64_t none = 0;
  if (!g_other_comp.compare_exchange_strong(none, comp, std::memory_order_relaxed)) return;
  Rec().X("comp", comp).X("lobby_helmet_comp", kComp).Emit(NEVR_LOG_INFO, "hudmeter: weapon_meter on another canvas component; guarded as well");
}

__attribute__((noinline, cold)) void LogPass(float x, float y) {  // the first other size after a run of full writes: the fill is live
  if (g_passed.exchange(true, std::memory_order_relaxed)) return;
  Rec().F("x", static_cast<double>(x)).F("y", static_cast<double>(y)).U("full_writes_before", g_full.load(std::memory_order_relaxed))
      .Emit(NEVR_LOG_INFO, "hudmeter: pass; full size was read, the fill is live");
}

std::uint64_t Wrapper(void* node, const void* data) {
  const auto next = reinterpret_cast<NextFn>(g_next.load(std::memory_order_acquire));
  if (next == nullptr) return 0;
  std::uint8_t d[kDataPrefix];
  if (!SafeRead(reinterpret_cast<std::uintptr_t>(data), d, sizeof d)) {  // on any doubt the real function, untouched
    g_unreadable.fetch_add(1, std::memory_order_relaxed);
    return next(node, data);
  }
  if (At<std::uint64_t>(d, kDItem) != kItem || At<std::int32_t>(d, kDSizeOn) == 0) return next(node, data);
  g_guarded.fetch_add(1, std::memory_order_relaxed);
  // any canvas component (rev 3: the lobby helmet's is not the only one a match uses); the first other one is named once
  if (At<std::uint64_t>(d, kDComp) != kComp) LogOtherComp(At<std::uint64_t>(d, kDComp));
  const float x = At<float>(d, kDSize), y = At<float>(d, kDSize + 4);
  if (x != 0.0f || y != 0.0f) {
    if (!g_passed.load(std::memory_order_relaxed)) LogPass(x, y);
    else LogFill(x, y);
    return next(node, data);
  }
  LogFull(g_full.fetch_add(1, std::memory_order_relaxed) + 1, At<std::uint64_t>(d, kDComp), At<std::uint64_t>(d, kDItem));
  g_passed.store(false, std::memory_order_relaxed);  // one pass line per helmet start
  // This call only: Enter sets the authored size, then the node's own value is put back. The bytes were just read, so the
  // range is mapped; the buffer is the calling script thread's own node-data buffer (thread + 0xb0).
  auto* size = reinterpret_cast<std::uint8_t*>(const_cast<void*>(data)) + kDSize;
  std::memcpy(size, kFull, sizeof kFull);
  const std::uint64_t r = next(node, data);
  std::memcpy(size, d + kDSize, sizeof kFull);  // what was there: 0 x 0
  return r;
}

}  // namespace

HudmeterStats GetHudmeterStats() { return {g_guarded.load(), g_full.load(), g_unreadable.load()}; }

void ResetHudmeterForTest() {
  g_next = 0;
  g_full = g_guarded = g_unreadable = g_fill_lines = 0;
  g_passed = false;
  g_last_y = 0;
  g_other_comp = 0;
}

InstallResult InstallHudmeter(std::uintptr_t base) {
  const InstallResult r = InstallImportHook(kSym, base, kSlot, reinterpret_cast<std::uintptr_t>(&Wrapper), &g_next, kFn);
  Rec rec;
  rec.X("slot_elf_vaddr", kSlot).X("fn_elf_vaddr", kFn).S("status", StatusName(r.status)).S("reason", r.reason);
  if (r.status == SwapStatus::kRestoreFailed)  // the wrapper is in, the page protection could not be put back
    rec.Emit(NEVR_LOG_ERROR, "hudmeter: on, but the slot page's old protection could not be restored");
  else if (r.status == SwapStatus::kOk)
    rec.F("full_x", static_cast<double>(kFull[0])).F("full_y", static_cast<double>(kFull[1]))
        .Emit(NEVR_LOG_INFO, "hudmeter: on; a 0 x 0 size write to weapon_meter becomes the authored full size");
  else
    rec.Emit(NEVR_LOG_ERROR, "hudmeter: OFF; nothing installed");
  return r;
}

}  // namespace q15t
