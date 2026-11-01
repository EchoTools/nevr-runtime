// NRadEngine::CFixedStringT<1024>::SPrintF
// Quest libr15.so VA 0x1081780; ReVault binary 576.
// ReVault raw parent 100207811, reconstruction node 100600564; grade H: all 45 instructions read to the ret at 0x1081830 (180-byte extent); the va_list is the va_start layout, VSNPrintF is the tree's declaration, Length takes the signature Ghidra gives its target 0x108188c, memset is the tree's thunk 0xfe71f0.
// @0x1081780 — NRadEngine::CFixedStringT<1024>::SPrintF
/* @confidence: H */
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace NRadEngine {

class CSysString {
 public:
  // CSysString::VSNPrintF(char*, unsigned long long, char const*, std::__va_list), 0x108312c
  // (reached through thunk 0xfdd250). The va_list is passed by address (AAPCS64).
  static long VSNPrintF(char* buffer, unsigned long long size, const char* format, va_list args);
  // CSysString::Length(char const*, unsigned long long), 0x108188c (reached through thunk 0xfd1370):
  // the characters before the first NUL, looking at no more than size of them.
  static long Length(const char* buffer, unsigned long long size);
};

// CFixedStringT<N>: the N-byte buffer; SPrintF's `this` is that buffer.
template <std::size_t N>
struct CFixedStringT {
  char text[N];
  // CFixedStringT<1024>::SPrintF(char const*, ...) at Quest libr15.so VA 0x1081780, reached through thunk 0x1011a90.
  std::uint64_t SPrintF(const char* format, ...);
};

// SPrintF formats into the whole 1024-byte buffer, then zero-fills the bytes after the terminator
// when the terminator is not in the last byte.
template <>
std::uint64_t CFixedStringT<1024>::SPrintF(const char* format, ...) {
  // 0x10817a0..0x10817e4: va_start builds the va_list at sp+0xb0 (gr_offs -0x30, vr_offs -0x80, gr_top sp+0xb0,
  // vr_top sp+0x80, stack sp+0x110), and the call gets a copy of it at sp+0xd0 by address.
  va_list args;
  va_start(args, format);
  // 0x10817e8: VSNPrintF(this, 0x400, format, &copy); the long result is returned unchanged.
  const long result = CSysString::VSNPrintF(text, 0x400, format, args);
  va_end(args);
  // 0x10817f0..0x10817fc: length = Length(this, 0x400).
  const long length = CSysString::Length(text, 0x400);
  // 0x1081800..0x1081804: the fill runs only when length + 1 < 0x400 (unsigned compare, b.hi skips).
  if (static_cast<unsigned long long>(length) + 1ULL < 0x400ULL) {
    // 0x1081808..0x108181c: memset(this + length + 1, 0, 0x3ff - length), memset is thunk 0xfe71f0.
    std::memset(text + length + 1, 0, static_cast<std::size_t>(0x3ff - length));
  }
  return static_cast<std::uint64_t>(result);
}

static_assert(sizeof(CFixedStringT<1024>) == 0x400);
static_assert(offsetof(CFixedStringT<1024>, text) == 0x0);

}  // namespace NRadEngine
