// Quest libr15.so VA 0x108a460; ReVault binary 576.
// ReVault raw parent 100208092, reconstruction node 100600578; grade H.
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace NRadEngine {

class CSysString {
public:
    static long VSNPrintF(char* buffer, unsigned long long size,
                          const char* format, std::va_list args);
    static long Length(const char* text, unsigned long long max_length);
};

template <std::size_t N>
struct CFixedStringT {
    char text[N];

    long SPrintF(const char* format, ...);
};

static_assert(sizeof(CFixedStringT<512>) == 0x200);
static_assert(offsetof(CFixedStringT<512>, text) == 0);

// @0x108a460 — NRadEngine::CFixedStringT<512>::SPrintF
/* @confidence: H — formats into the 512-byte buffer, measures its bounded length, zeroes the remaining tail when it fits, and returns the formatter result; all 45 instructions match. */
template <>
long CFixedStringT<512>::SPrintF(const char* format, ...) {
    std::va_list args;
    va_start(args, format);
    const long result = CSysString::VSNPrintF(text, 0x200, format, args);
    va_end(args);

    const long length = CSysString::Length(text, 0x200);
    if (length + 1 < 0x200) {
        std::memset(text + length + 1, 0,
                    static_cast<std::size_t>(0x1ff - length));
    }
    return result;
}

}  // namespace NRadEngine
