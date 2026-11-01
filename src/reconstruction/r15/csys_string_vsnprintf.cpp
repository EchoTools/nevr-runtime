// Quest libr15.so VA 0x108312c; ReVault binary 576.
// ReVault raw parent 100207858, reconstruction node 100600575; grade H.
#include <cstdarg>
#include <cstddef>
#include <cstdio>

namespace NRadEngine {

class CSysString {
public:
    static long VSNPrintF(char* buffer, unsigned long long size,
                          const char* format, std::va_list args);
};

// @0x108312c — NRadEngine::CSysString::VSNPrintF
/* @confidence: H — copies the 32-byte AArch64 va_list, calls the imported vsnprintf thunk, and sign-extends its int result. */
long CSysString::VSNPrintF(char* buffer, unsigned long long size,
                           const char* format, std::va_list args) {
    std::va_list args_copy;
    va_copy(args_copy, args);
    const int result =
        std::vsnprintf(buffer, static_cast<std::size_t>(size), format, args_copy);
    va_end(args_copy);
    return static_cast<long>(result);
}

}  // namespace NRadEngine
