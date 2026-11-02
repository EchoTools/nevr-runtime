// NRadEngine::CSymbol64::Append
// Quest libr15.so VA 0x12ccb34; ReVault binary 576.
// ReVault raw parent 100219741, reconstruction node 100600563; grade H: all 5 instructions checked; a tail call through thunk 0x1046210 (leaves x0..x4 unchanged) to CalculateSymbolValue with x3 set to -1 and x4 forwarded unchanged; levelname.cpp:15 and allocate_exclusive.cpp:45 declare four parameters, so their fourth argument lands in x3, which this function overwrites.
// @0x12ccb34 — NRadEngine::CSymbol64::Append
/* @confidence: H */
#include <cstddef>
#include <cstdint>

namespace NRadEngine {

struct CSymbol64 {
  std::uint64_t bits;

  // NRadEngine::CSymbol64::CalculateSymbolValue, declared as in cjson_navigate_path.cpp. Append reaches it through
  // the 4-instruction thunk at 0x1046210 (adrp, ldr, add, br; ReVault: thunk_FUN_012cc580), which leaves x0..x4 unchanged.
  // That file names the second argument `length`; Append passes this symbol's bits there.
  static std::uint64_t CalculateSymbolValue(const char* text, std::int64_t length, std::uint32_t arg3, std::uint64_t arg4, std::uint32_t arg5);

  // Quest libr15.so VA 0x12ccb34, reached through thunk 0x1031ea0. Five arguments as the callee reads them: x0 symbol,
  // x1 suffix, w2 append, x3 overwritten before use (named, unused), w4 forwarded to CalculateSymbolValue unchanged.
  //
  // Merged declarations that disagree with this five-parameter form:
  //  - levelname.cpp:15 declares (CSymbol64, const char*, uint32_t append, uint32_t): four parameters. Its call at
  //    levelname.cpp:48 passes 1, 1, so its fourth parameter lands in x3, which Append overwrites; nothing reads it.
  //    The binary agrees: LoadLevelName (VA 0x1302ecc) sets x0 = requested, x1 = "_lowspec", w2 = 1, w3 = 1 and never
  //    writes x4, so the x4 that Append forwards is whatever LoadLevelName's own caller left in x4.
  //  - allocate_exclusive.cpp:45 declares std::uint64_t Append(std::uint64_t, const char*, int, int): four parameters,
  //    the same x3 overwrite, and a uint64_t return where this one returns CSymbol64 (both come back in x0).
  static CSymbol64 Append(CSymbol64 symbol, const char* suffix, std::uint32_t append, std::uint64_t overwritten_x3, std::uint32_t arg5);
};

static_assert(sizeof(CSymbol64) == 0x08);
static_assert(offsetof(CSymbol64, bits) == 0x00);

CSymbol64 CSymbol64::Append(CSymbol64 symbol, const char* suffix, std::uint32_t append, [[maybe_unused]] std::uint64_t overwritten_x3, std::uint32_t arg5) {
  // 0x12ccb34..0x12ccb40: x8 = symbol; x0 = suffix; x1 = symbol.
  // 0x12ccb38: x3 = -1, overwriting the incoming x3 (overwritten_x3 is never read).
  // 0x12ccb44: tail branch to 0x1046210 with x2 (append) and x4 (arg5) unchanged; the callee's x0 is the return value.
  const std::uint64_t value = CalculateSymbolValue(suffix, static_cast<std::int64_t>(symbol.bits), append, 0xffffffffffffffffULL, arg5);
  return CSymbol64{value};
}

}  // namespace NRadEngine
