// NRadEngine::MakeTStrPrintF<512>
// Quest libr15.so VA 0x1086b9c; ReVault binary 576.
// ReVault raw parent 100207967, reconstruction node 100600484; grade H: both branches checked against all 35 instructions; the spilled va_list equals va_start (gr_offs -56, vr_offs -128, gr_top sp+0xc0, vr_top sp+0x80, stack sp+0x110); the zero-fill the array rule requires is the one departure, and no byte of the written string differs.
// @0x1086b9c — NRadEngine::MakeTStrPrintF<512>
/* @confidence: H */
#include <cstdarg>
#include <cstddef>

namespace NRadEngine {

// STempString<N>: the temporary that MakeTStrPrintF returns in the caller's frame.
template <std::size_t N>
struct STempString {
  unsigned char storage[N];
};

// MakeTStrPrintF<N>(char const*, ...). Callers reach it through thunk 0x102b370 (adrp, ldr, br).
template <std::size_t N>
STempString<N> MakeTStrPrintF(const char* format, ...);

class CSysString {
 public:
  // CSysString::VSNPrintF(char*, unsigned long long, char const*, std::__va_list), 0x108312c
  // (reached through thunk 0xfdd250). The va_list is passed by address (AAPCS64).
  static long VSNPrintF(char* buffer, unsigned long long size, const char* format, va_list args);
};

// The MakeTStrPrintF<512> instance, 0x1086b9c. Its 0x200 buffer size is the size VSNPrintF receives.
template <>
STempString<512> MakeTStrPrintF<512>(const char* format, ...) {
  // 0x1086ba0..0x1086bc4: the prologue spills x1..x7 and q0..q7 (the variadic save area) and tests
  // format with cbz x0. The 512-byte result is written through x8 on both paths.
  STempString<512> result{};
  if (format == nullptr) {
    // 0x1086c18: strb wzr, [x8]; the result is the empty string.
    result.storage[0] = 0;
    return result;
  }
  // 0x1086bcc..0x1086c04: the va_list the listing builds at sp+0xc0 is the one va_start produces.
  va_list args;
  va_start(args, format);
  // 0x1086c08: bl thunk 0xfdd250 with x0 = x8 (result), x1 = 0x200, x2 = format, x3 = &args.
  // The callee's long result stays in x0 when the listing returns; this function's result is the
  // STempString, so that value is discarded.
  CSysString::VSNPrintF(reinterpret_cast<char*>(result.storage), 512, format, args);
  va_end(args);
  return result;
}

}  // namespace NRadEngine
