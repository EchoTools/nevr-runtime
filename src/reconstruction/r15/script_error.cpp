// NRadEngine::ScriptError
// Quest libr15.so VA 0x2710d68; ReVault binary 576.
// ReVault raw parent 100324455, reconstruction node 100600777; grade M: all 24 instructions (0x2710d68-0x2710dc4) checked to the ret, 0 placeholders, every callee and type named; RadScriptError's 2048-byte result comes back through x8 and is declared as a by-value struct return, with the arguments in x0-x3 as the listing passes them.
// @0x2710d68 — NRadEngine::ScriptError
/* @confidence: M */
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace {

// Reads a T at a byte offset. The layout of SScriptThread is not recovered, so each field is read
// where the listing reads it.
template <typename T>
T ReadAt(const void* base, std::size_t offset) {
  T value;
  std::memcpy(&value, static_cast<const std::byte*>(base) + offset, sizeof value);
  return value;
}

}  // namespace

namespace NRadEngine {

struct SScriptThread;

struct CSymbol64 {
  std::uint64_t bits;  // +0x00: the symbol value, read through *this

  // NRadEngine::CSymbol64::String() const, VA 0x12ccb48 (thunk 0x1038ec0).
  const char* String() const;
};
static_assert(sizeof(CSymbol64) == 0x08);

namespace NWriteLog {
// Enumerators are not established. This function is called here with level 8.
enum ELogLevel : std::int32_t {};
// NRadEngine::NWriteLog::WriteLog @ 0x1088c20 (thunk 0x104b960). Variadic.
void WriteLog(ELogLevel level, unsigned long long channel, const char* format, ...);
}  // namespace NWriteLog

// The 2048-byte result RadScriptError writes through x8 (the AAPCS64 indirect result register).
// WriteLog prints it as a C string. Only the size is established by the listing (sub sp, sp, #0x800).
struct SScriptErrorText {
  char text[0x800];
};
static_assert(sizeof(SScriptErrorText) == 0x800);

class CMetaErrorUtil {
 public:
  // NRadEngine::CMetaErrorUtil::RadScriptError, ReVault decompile signature (char*, ulonglong,
  // char*, char*), reached through thunk 0x1019610 (adrp, ldr, br). RadScriptError null-checks x0 as
  // the message (cbz x0 at 0x12d0edc), takes x1, x2, x3 as the other three arguments and saves x8 as
  // the destination of its 0x800-byte copy, so the result is returned by value; const qualifiers on
  // the strings are inferred from the caller.
  static SScriptErrorText RadScriptError(const char* message, std::uint64_t arg1, const char* arg2,
                                         const char* arg3);
};

// NRadEngine::ScriptError(SScriptThread const*, char const*), VA 0x2710d68 (thunk 0x10460c0).
void ScriptError(const SScriptThread* thread, const char* message) {
  // 0x2710d74: x19 = *(thread + 0x10), a 64-bit value kept across both calls.
  const std::uint64_t arg1 = ReadAt<std::uint64_t>(thread, 0x10);
  // 0x2710d78: x0 = *(thread + 0x00), the receiver of CSymbol64::String, read as a pointer (as
  // set_material_property_node_update.cpp reads it).
  const CSymbol64* symbol = ReadAt<const CSymbol64*>(thread, 0x00);
  // 0x2710d80: bl CSymbol64::String; its x0 result is the x2 argument of the next call.
  const char* symbol_text = symbol->String();
  // 0x2710d90..0x2710d9c: x8 = sp (the indirect result), x0 = message, x1 = arg1, x2 = symbol_text,
  // x3 = "" (the empty literal at 0x2c419ad); bl the thunk at 0x1019610.
  const SScriptErrorText text = CMetaErrorUtil::RadScriptError(message, arg1, symbol_text, "");
  // 0x2710da0..0x2710db4: x0 = 8, x1 = 0, x2 = "%s" (0x2d53c79), x3 = sp, the start of the result
  // (0x2710d70: sub sp, sp, #0x800 reserves its 2048 bytes); bl the thunk at 0x104b960.
  NWriteLog::WriteLog(static_cast<NWriteLog::ELogLevel>(8), 0, "%s", text.text);
}

}  // namespace NRadEngine
