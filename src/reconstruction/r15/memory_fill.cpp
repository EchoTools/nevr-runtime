// NRadEngine::CMemory::Fill
// Quest libr15.so VA 0x12cafcc; ReVault binary 576.
// ReVault raw parent 100219613, reconstruction node 100600490; grade M: the body is one tail jump (b 0xfe71f0) into the memset thunk with x0 to x2 passed through unchanged, and no clamp or assert exists; the void return is Ghidra's, and no call site was checked for a use of the x0 that memset leaves.
// @0x12cafcc — NRadEngine::CMemory::Fill
/* @confidence: M */
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace NRadEngine::CMemory {

// Quest libr15.so VA 0x12cafcc. Callers reach it through the thunk at 0x1047300 (adrp, ldr, br through GOT 0x37fb068).
// The whole body is `b 0xfe71f0`: a tail jump to the memset thunk (adrp, ldr, br through GOT 0x37cafe0, import memset).
// Argument 1 is an unsigned char in w1; memset takes it as int and stores its low byte.
void Fill(void* destination, unsigned char value, std::uint64_t count);

}  // namespace NRadEngine::CMemory

void NRadEngine::CMemory::Fill(void* destination, unsigned char value, std::uint64_t count) {
  // 0x12cafcc: b 0x00fe71f0. No clamp and no assert: the body has no other instruction.
  std::memset(destination, value, static_cast<std::size_t>(count));
}
