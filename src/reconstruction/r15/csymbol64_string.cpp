// NRadEngine::CSymbol64::String
// Quest libr15.so VA 0x12ccb48; ReVault binary 576.
// ReVault raw parent 100219742, reconstruction node 100600586; grade M: all 52 instructions read to the ret at 0x12ccc14 (208-byte extent); 2 globals stay placeholders, DAT_03832050 (VA 0x3832050) and DAT_03832030 (VA 0x3832030), the emulated-TLS control blocks with no recoverable name.
// @0x12ccb48 — NRadEngine::CSymbol64::String
/* @confidence: M */
#include <cstddef>
#include <cstdint>

// libgcc emulated-TLS accessor, ReVault __emutls_get_address (VA 0x2c2e594), reached through the stub at 0x1021c10.
extern "C" void* __emutls_get_address(void* control);

// Placeholder: emulated-TLS control block of the signed slot-index byte. ReVault DAT_03832050, VA 0x3832050.
extern char DAT_03832050;
// Placeholder: emulated-TLS control block of the 16 slots of 0x20 bytes. ReVault DAT_03832030, VA 0x3832030.
extern char DAT_03832030;

namespace NRadEngine {

struct CSymbol64 {
  std::uint64_t bits;  // +0x00: the symbol value, read through *this

  // NRadEngine::CSymbol64::String() const, VA 0x12ccb48 (thunk 0x1038ec0).
  const char* String() const;
};
static_assert(sizeof(CSymbol64) == 0x08);
static_assert(offsetof(CSymbol64, bits) == 0x00);

class CSysString {
 public:
  // CSysString::SNPrintF(char*, unsigned long long, char const*, ...), VA 0x10830c4, reached through thunk 0x101c150.
  // The target returns long; this caller discards it.
  static long SNPrintF(char* buffer, unsigned long long size, const char* format, ...);
};

}  // namespace NRadEngine

namespace NRadEngine {

// NRadEngine::CSymbolLookup::findstring64 (ReVault _ZN10NRadEngine13CSymbolLookup12findstring64E): a static data member, a function pointer of type _func_char_ptr_CSymbol64 *. Declared here; its definition is outside this file.
struct CSymbolLookup {
  static char* (*findstring64)(CSymbol64);
};

const char* CSymbol64::String() const {
  // 0x12ccb50: x19 = *x0, the symbol value.
  const std::uint64_t value = bits;
  // 0x12ccb54..0x12ccb58: value == -1 returns the empty string literal.
  if (value == 0xffffffffffffffffULL) {
    return "";
  }
  // 0x12ccb5c..0x12ccb68: a non-null findstring64 is taken as the lookup.
  if (CSymbolLookup::findstring64 != nullptr) {
    // 0x12ccb6c: value == 0 returns the literal "Invalid Symbol".
    if (value == 0) {
      return "Invalid Symbol";
    }
    // 0x12ccb70..0x12ccb7c: x0 = value, the frame is restored, then br x1: a tail call.
    return CSymbolLookup::findstring64(CSymbol64{value});
  }
  // 0x12ccb98..0x12ccba0: fallback, no findstring64 installed.
  auto* const index_byte = static_cast<signed char*>(__emutls_get_address(&DAT_03832050));
  // 0x12ccba4: ldrsb, so the index is a signed char.
  const int index = *index_byte;
  int next = index + 1;  // iVar1
  // uVar2 = index + 0x10 when next < 0, else index + 1 (csinc at 0x12ccbb4).
  std::uint32_t slot_base = static_cast<std::uint32_t>(index + 0x10);
  if (next >= 0) {
    slot_base = static_cast<std::uint32_t>(index + 1);
  }
  // 0x12ccbb8..0x12ccbc0: next = next - (uVar2 & 0xfffffff0), stored back as one byte.
  next = static_cast<int>(static_cast<std::uint32_t>(next) - (slot_base & 0xfffffff0u));
  *index_byte = static_cast<signed char>(next);
  // 0x12ccbd0..0x12ccbd4: slot = ring + next * 0x20, ring = __emutls_get_address(&DAT_03832030).
  char* const slot = static_cast<char*>(__emutls_get_address(&DAT_03832030)) + static_cast<std::ptrdiff_t>(next) * 0x20;
  // 0x12ccbd8..0x12ccbec: SNPrintF(slot, 0x20, "0x%016llX", value); its result is discarded.
  CSysString::SNPrintF(slot, 0x20, "0x%016llX", value);
  return slot;
}

}  // namespace NRadEngine
