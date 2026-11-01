// Game function: NRadEngine::NRadGame::CR15Game::LoadLevelName
// Quest libr15.so (ReVault binary 576) RVA: 0x1202ecc (ReVault VA 0x1302ecc)
// PC echovr.exe RVA: not located; only a Quest annotation exists in get @ 0x1407138a0
// ReVault node: 100600144; tier: reconstructed; grade: H, assessed by revault-codex.
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace NRadEngine {

struct CSymbol64 {
  std::uint64_t bits;

  static CSymbol64 Append(CSymbol64 symbol, const char* suffix, std::uint32_t append, std::uint32_t intern);
};

static_assert(sizeof(CSymbol64) == 0x08);
static_assert(offsetof(CSymbol64, bits) == 0x00);

class CncaGame {
 public:
  CSymbol64 LoadLevelName(CSymbol64 requested) const;
};

namespace NRadGame {

class CR15Game : public CncaGame {
 public:
  CSymbol64 LoadLevelName(CSymbol64 requested) const;
};

// @0x1302ecc — NRadEngine::NRadGame::CR15Game::LoadLevelName [Quest: confirmed]
/* @confidence: H */
CSymbol64 CR15Game::LoadLevelName(CSymbol64 requested) const {
  const auto* object = reinterpret_cast<const std::byte*>(this);
  std::int32_t quality_flags;
  std::memcpy(&quality_flags, object + 0x7ad8, sizeof(quality_flags));

  if ((static_cast<std::uint32_t>(quality_flags) & 0x80000000u) == 0) {
    return CncaGame::LoadLevelName(requested);
  }

  return CSymbol64::Append(requested, "_lowspec", 1, 1);
}

}  // namespace NRadGame
}  // namespace NRadEngine
