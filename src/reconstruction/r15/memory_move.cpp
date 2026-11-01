// NRadEngine::CMemory::Move
// Quest libr15.so VA 0x12cb90c; ReVault binary 576.
// ReVault raw parent 100219678, reconstruction node 100600489; grade M: the body is one tail jump (b 0xffa880) into the memmove thunk with x0 to x2 passed through unchanged, and no clamp or assert exists; the void return is Ghidra's, and no call site was checked for a use of the x0 that memmove leaves.
// @0x12cb90c — NRadEngine::CMemory::Move
/* @confidence: M */
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace NRadEngine::CMemory {

// Quest libr15.so VA 0x12cb90c. Callers reach it through the thunk at 0x1010a90 (adrp, ldr, br through GOT 0x37dfc30).
// The whole body is `b 0xffa880`: a tail jump to the memmove thunk (adrp, ldr, br through GOT 0x37d4b28, import memmove).
// Arguments 0 to 2 reach memmove unchanged, so memmove's return in x0 is what the caller receives.
void Move(void* destination, const void* source, std::uint64_t count);

}  // namespace NRadEngine::CMemory

void NRadEngine::CMemory::Move(void* destination, const void* source, std::uint64_t count) {
  // 0x12cb90c: b 0x00ffa880. No clamp and no assert: the body has no other instruction.
  std::memmove(destination, source, static_cast<std::size_t>(count));
}
