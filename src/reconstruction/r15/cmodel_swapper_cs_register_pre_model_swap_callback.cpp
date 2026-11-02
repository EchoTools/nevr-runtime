// NRadEngine::CModelSwapperCS::RegisterPreModelSwapCallback
// Quest libr15.so VA 0x1734eb0; ReVault binary 576.
// ReVault raw parent 100246514, reconstruction node 100600694; grade M: all 10 instructions (no branch, no call, one ret) are checked against the listing; there are no callees or globals; the 0x1d8-byte entry array and the u16 slot map are raw offsets; parameters 4 and 5 have no recovered names (arg4, arg5); the callback signature and the meaning of arg4/arg5 come from ReVault's demangled comment and a label, not from the 10 instructions.
// @0x1734eb0 — NRadEngine::CModelSwapperCS::RegisterPreModelSwapCallback
/* @confidence: M */
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace NRadEngine {

// SCompactPoolHandle arrives in x1 as one 64-bit register; this function reads only its low 16 bits.
using SCompactPoolHandle = std::uint64_t;

class CModelSwapperCS {
 public:
  // @0x1734eb0 (thunk 0xff1dc0). Stores the four arguments after the handle into one 0x1d8-byte entry.
  // arg4 and arg5 are stored unchanged; their meaning is not recovered.
  void RegisterPreModelSwapCallback(SCompactPoolHandle handle,
                                    void (*callback)(void*, std::uint64_t, std::uint64_t), void* context,
                                    std::uint64_t arg4, std::uint64_t arg5);

  // The layout of CModelSwapperCS is not recovered; raw covers the pointers read at +0xd0 and +0x100.
  // The base CComponentSystem (at +0x0) is not declared here.
  alignas(8) std::byte raw[0x108];
};
static_assert(sizeof(CModelSwapperCS) >= 0x108);

namespace {

// Bytes per entry of the array at +0x100: the mov #0x1d8 and madd in the listing.
constexpr std::size_t kEntryStride = 0x1d8;

template <typename T>
T Load(const std::byte* base, std::size_t offset) {
  T value{};
  std::memcpy(&value, base + offset, sizeof value);
  return value;
}

template <typename T>
void Store(std::byte* base, std::size_t offset, const T& value) {
  std::memcpy(base + offset, &value, sizeof value);
}

}  // namespace

void CModelSwapperCS::RegisterPreModelSwapCallback(SCompactPoolHandle handle,
                                                   void (*callback)(void*, std::uint64_t, std::uint64_t),
                                                   void* context, std::uint64_t arg4, std::uint64_t arg5) {
  static_assert(sizeof(callback) == sizeof(std::uint64_t), "each store is one 8-byte x register");
  std::byte* const self = raw;

  // 0x1734ec0-0x1734ec4: slot_index = u16 at this->[0xd0] + 4 * (handle & 0xffff), read by ldrh.
  const std::byte* const slot_map = Load<const std::byte*>(self, 0xd0);
  const auto slot_index = Load<std::uint16_t>(slot_map, static_cast<std::size_t>(handle & 0xffffULL) * 4);

  // 0x1734ebc-0x1734ec8: entry = this->[0x100] + slot_index * 0x1d8 (madd x8, x8, x9, x10).
  std::byte* const entries = Load<std::byte*>(self, 0x100);
  std::byte* const entry = entries + static_cast<std::size_t>(slot_index) * kEntryStride;

  // 0x1734ecc-0x1734ed4: stp x2, x3 at entry+0x88; stp x4, x5 at entry+0x98.
  Store(entry, 0x88, callback);
  Store(entry, 0x90, context);
  Store(entry, 0x98, arg4);
  Store(entry, 0xa0, arg5);
}

}  // namespace NRadEngine
