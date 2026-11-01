// CR15Game::LoadLevelName
// Quest libr15.so VA 0x1302ecc; ReVault binary 576.
// ReVault raw parent 100221568, reconstruction node 100600471; grade H: all 11 instructions checked (two branches, two tail calls); stored size 44 ends at ToggleDebugCamera 0x1302ef8.
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace NRadEngine {

struct CSymbol64 {
  std::uint64_t bits;

  // Quest libr15.so VA 0x12ccb34, reached through thunk 0x1031ea0.
  // The fourth argument is not read: the wrapper sets x3 to -1 and forwards x4 unchanged.
  static CSymbol64 Append(CSymbol64 symbol, const char* suffix, std::uint32_t append, std::uint32_t);
};

static_assert(sizeof(CSymbol64) == 0x08);
static_assert(offsetof(CSymbol64, bits) == 0x00);

class CncaGame {
 public:
  // Quest libr15.so VA 0x18f162c, reached through thunk 0x10133d0; returns its argument.
  CSymbol64 LoadLevelName(CSymbol64 requested) const;
};

namespace NRadGame {

class CR15Game : public CncaGame {
 public:
  CSymbol64 LoadLevelName(CSymbol64 requested) const;
};

// @0x1302ecc — CR15Game::LoadLevelName
/* @confidence: H */
CSymbol64 CR15Game::LoadLevelName(CSymbol64 requested) const {
  // 0x1302ed0: ldr w8, [x0, #0x7ad8]; signed 32-bit word at this + 0x7ad8.
  const auto* object = reinterpret_cast<const std::byte*>(this);
  std::int32_t setting;
  std::memcpy(&setting, object + 0x7ad8, sizeof(setting));

  // 0x1302ed4: tbnz w8, #31 to 0x1302edc; bit 31 clear falls to the tail call at 0x1302ed8.
  if ((static_cast<std::uint32_t>(setting) & 0x80000000u) == 0) {
    return CncaGame::LoadLevelName(requested);
  }

  // 0x1302edc-0x1302ef4: x1 = "_lowspec" (0x2ca1bf6), w2 = 1, w3 = 1, x0 = requested; tail call.
  return CSymbol64::Append(requested, "_lowspec", 1, 1);
}

}  // namespace NRadGame
}  // namespace NRadEngine
