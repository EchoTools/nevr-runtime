#include "shieldgamma.h"

#include <atomic>
#include <cmath>
#include <cstring>

#include "pbhud_log.h"
#include "plugin_log.h"
#include "safe_read.h"

namespace q15t {
namespace {

// ELF vaddrs of the pinned build (disassembly VA = ELF + 0x100000)
constexpr std::uintptr_t kSlot = 0x36d2020, kFn = 0x272d55c;  // R58: JUMP_SLOT / CSetMaterialPropertyNode::Update
constexpr char kSym[] = "_ZN10NRadEngine24CSetMaterialPropertyNode6UpdateERKNS_28SSetMaterialPropertyNodeDataE";
constexpr std::uint64_t kMat = 0xdbde1b05fa8f6200ull;      // the spawn-shield / barrier material
constexpr std::uint64_t kTubeMat = 0xb28e940eb21a5798ull;  // M, the tubefork window material (TU-1)
constexpr std::uint64_t kRingProp = 0xdd35e419672ee0baull;  // the script's ring colour
// SSetMaterialPropertyNodeData: +0x20 material, +0x28 property, +0x30 type (1 = vec4), +0x38..+0x44 value (r, g, b, a),
// +0x4c colourspace (0 = linear). Update reads up to +0x4c; the source copies 0x58 bytes.
constexpr std::size_t kDataSize = 0x58;
constexpr std::uintptr_t kDMat = 0x20, kDProp = 0x28, kDType = 0x30, kDVal = 0x38, kDSpace = 0x4c;
constexpr std::int32_t kVec4 = 1, kLinear = 0;
constexpr float kGamma = 2.2f;  // the PC pixel shader's own decode: pow(saturate(x), 2.2)

using NextFn = std::uint64_t (*)(void* node, const void* data);
std::atomic<std::uintptr_t> g_next{0};
std::atomic<unsigned> g_decoded{0}, g_mirrored{0}, g_unreadable{0};
std::atomic<bool> g_gamma{false}, g_mirror{false};

template <typename T>
T At(const std::uint8_t* p, std::uintptr_t off) {
  T v;
  std::memcpy(&v, p + off, sizeof v);
  return v;
}

float Decode(float x) { return std::pow(x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x), kGamma); }

__attribute__((noinline, cold)) void LogDecode(unsigned n, const char* what, std::uint64_t prop, const float* from, const float* to) {
  if (n & (n - 1)) return;  // 1st, 2nd, 4th, 8th ...
  Rec().U("n", n).X("prop", prop).F("r", static_cast<double>(from[0])).F("g", static_cast<double>(from[1])).F("b", static_cast<double>(from[2]))
      .F("to_r", static_cast<double>(to[0])).F("to_g", static_cast<double>(to[1])).F("to_b", static_cast<double>(to[2]))
      .Emit(NEVR_LOG_INFO, what);
}

// per update while the duration runs; returns the stock w0
std::uint64_t Wrapper(void* node, const void* data) {
  const auto next = reinterpret_cast<NextFn>(g_next.load(std::memory_order_acquire));
  if (next == nullptr) return 0;
  std::uint8_t d[kDataSize];
  if (!SafeRead(reinterpret_cast<std::uintptr_t>(data), d, sizeof d)) {  // on any doubt the real function, untouched
    g_unreadable.fetch_add(1, std::memory_order_relaxed);
    return next(node, data);
  }
  if (At<std::uint64_t>(d, kDMat) != kMat || At<std::int32_t>(d, kDType) != kVec4 || At<std::int32_t>(d, kDSpace) != kLinear)
    return next(node, data);
  const std::uint64_t prop = At<std::uint64_t>(d, kDProp);
  float was[3], now[3];
  std::memcpy(was, d + kDVal, sizeof was);
  for (int i = 0; i < 3; ++i) now[i] = Decode(was[i]);
  if (g_mirror.load(std::memory_order_relaxed) && prop == kRingProp) {
    // Update only resolves the actor's model, calls CModelCS::SetMaterialProperty and reads the node's start time (ELF
    // 0x272d55c..0x272d7e0), so a second call on a copy with material := M is the write alone. The real call's result is kept.
    std::uint8_t copy[kDataSize];
    std::memcpy(copy, d, sizeof copy);
    std::memcpy(copy + kDMat, &kTubeMat, sizeof kTubeMat);
    std::memcpy(copy + kDVal, now, sizeof now);
    LogDecode(g_mirrored.fetch_add(1, std::memory_order_relaxed) + 1, "shieldgamma: tube mirror write", prop, was, now);
    next(node, copy);
  }
  if (!g_gamma.load(std::memory_order_relaxed)) return next(node, data);
  LogDecode(g_decoded.fetch_add(1, std::memory_order_relaxed) + 1, "shieldgamma: decode", prop, was, now);
  // This call only: Update hands the decoded rgb to SetMaterialProperty (ELF 0x272d7f8 ldp s0, s1, [x19, #0x38]); the node's own
  // value is put back after it. The bytes were just read, so the range is mapped; the buffer is the script thread's own.
  auto* val = reinterpret_cast<std::uint8_t*>(const_cast<void*>(data)) + kDVal;
  std::memcpy(val, now, sizeof now);
  const std::uint64_t r = next(node, data);
  std::memcpy(val, was, sizeof was);
  return r;
}

}  // namespace

ShieldGammaStats GetShieldGammaStats() { return {g_decoded.load(), g_mirrored.load(), g_unreadable.load()}; }

void ResetShieldGammaForTest() {
  g_next = 0;
  g_decoded = g_mirrored = g_unreadable = 0;
  g_gamma = g_mirror = false;
}

InstallResult InstallShieldGamma(std::uintptr_t base, bool gamma, bool mirror) {
  if (!gamma && !mirror) {
    InstallResult off;
    off.reason = "off";
    Rec().Emit(NEVR_LOG_INFO, "shieldgamma: off (gamma and tube mirror both off); nothing installed");
    return off;
  }
  g_gamma.store(gamma, std::memory_order_relaxed);  // before the swap, never changed after it
  g_mirror.store(mirror, std::memory_order_relaxed);
  const InstallResult r = InstallImportHook(kSym, base, kSlot, reinterpret_cast<std::uintptr_t>(&Wrapper), &g_next, kFn);
  Rec rec;
  rec.X("slot_elf_vaddr", kSlot).X("fn_elf_vaddr", kFn).S("status", StatusName(r.status)).S("reason", r.reason)
      .S("gamma", gamma ? "1" : "0").S("tube_mirror", mirror ? "1" : "0");
  if (r.status == SwapStatus::kRestoreFailed) rec.Emit(NEVR_LOG_ERROR, "shieldgamma: on, but the slot page's old protection could not be restored");
  else if (r.status == SwapStatus::kOk) rec.X("material", kMat).Emit(NEVR_LOG_INFO, "shieldgamma: on");
  else rec.Emit(NEVR_LOG_ERROR, "shieldgamma: OFF; nothing installed");
  return r;
}

}  // namespace q15t
