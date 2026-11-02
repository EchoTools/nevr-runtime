// NRadEngine::CSysString::Length
// Quest libr15.so VA 0x1081878; ReVault binary 576.
// ReVault raw parent 100207813, reconstruction node 100600548; grade H: all five AArch64 instructions matched; class and name are Ghidra's, unverified.
// @0x1081878 — NRadEngine::CSysString::Length
/* @confidence: H */
namespace NRadEngine {

class CSysString {
public:
    // The only argument is x0 (the C string); there is no `this` in the disassembly.
    static long Length(const char* str) {
        // Disassembly: sub x8,x0,#1; then ldrb w9,[x8,#1]! / cbnz w9 loop; sub x0,x8,x0; ret.
        // The -1 pre-index is a codegen detail: the loop reads str[0], str[1], ... in order
        // and stops at the first NUL byte, the same bytes as this loop.
        const char* p = str;
        while (*p != '\0') {
            ++p;
        }
        return static_cast<long>(p - str);
    }
};

}  // namespace NRadEngine
