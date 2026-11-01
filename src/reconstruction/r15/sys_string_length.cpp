// NRadEngine::CSysString::Length
// Quest libr15.so VA 0xff4b80; ReVault binary 576.
// ReVault raw parent 100182825, reconstruction node 100600491; grade M: declaration only; the stub is four instructions (stored size 16) through GOT 0x37d1ca8, whose data xref and Ghidra thunk name 0x1081878, a strlen-shaped body; no import row names the slot, and the ReVault name PEM_read_bio_SSL_SESSION is not supported by these bytes.
// @0xff4b80 — NRadEngine::CSysString::Length
/* @confidence: M */
#include <cstddef>

namespace NRadEngine {

class CSysString {
 public:
  // Declared, not defined: the stub at VA 0xff4b80 is a PLT-form thunk with no body of its own.
  //   0xff4b80: adrp x16, 0x37d1000
  //   0xff4b84: ldr  x17, [x16, #0xca8]       ; GOT slot 0x37d1ca8
  //   0xff4b88: add  x16, x16, #0xca8
  //   0xff4b8c: br   x17
  // The slot's data xref names Quest libr15.so VA 0x1081878 (ReVault name Length; Ghidra's thunk target is
  // NRadEngine::CSysString::Length). That body is: sub x8,x0,#1; ldrb w9,[x8,#1]! loop until the NUL byte;
  // sub x0,x8,x0; ret. The argument is the string in x0 and the return is its length in x0.
  // The slot's file bytes, 0x00ecfb40, are the placeholder the Move and Fill GOT slots (0x37dfc30, 0x37fb068)
  // also hold, so they do not name the target. No import row names the slot or the thunk 0xff4b80.
  static std::size_t Length(const char* text);
};

}  // namespace NRadEngine
