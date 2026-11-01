// Quest libr15.so VA 0x108188c; ReVault binary 576.
// ReVault raw parent 100207814, reconstruction node 100600571; grade H.
namespace NRadEngine {

class CSysString {
public:
    static long Length(const char* text, unsigned long long max_length);
};

// @0x108188c — NRadEngine::CSysString::Length
/* @confidence: H — all ten instructions match the bounded NUL scan and returned byte count. */
long CSysString::Length(const char* text, unsigned long long max_length) {
    unsigned long long length = 0;
    while (length < max_length && text[length] != '\0') {
        ++length;
    }
    return static_cast<long>(length);
}

}  // namespace NRadEngine
