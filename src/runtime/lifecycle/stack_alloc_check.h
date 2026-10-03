#pragma once
// The game's own capacity check in CStackAllocator::DirectAlloc (echovr.exe 0x1400d4fb0), so the probe in
// crash_recovery.cpp can tell, before the call, that the game is about to log "Stack allocator ran out of
// memory" and trap (int3 at 0x1400d502b), and record who asked (#68, beta gate G11). Unit-tested apart from
// the hook.
//
// From the disassembly, 0x1400d4fbf..0x1400d500f:
//   request' = request + ((-request) & 3)                    NEG/AND 3/ADD
//   top      = entry[n-1].a + entry[n-1].b                   [entries + n*16 - 8] + [entries + n*16 - 16]
//   if align: top += (align - 1) & -top
//   fails when top + request' > [this+0x38] + [this+0x40]    CMP / JBE
//   and logs ([this+0x38] - [this+0x40]) + top + request'    SUB R9,RDX; ADD R9,RBX; ADD R9,RDI

#include <cstdint>

namespace StackAllocCheck {

struct Result {
  bool fits;
  std::uint64_t request;   // after the game's rounding up to a multiple of 4
  std::uint64_t start;     // where the allocation would begin
  std::uint64_t reported;  // the number the game prints when it does not fit
};

inline Result Check(std::uint64_t field38, std::uint64_t field40, std::uint64_t top, std::uint64_t request,
                    std::uint64_t align) {
  const std::uint64_t rounded = request + ((0 - request) & 3U);
  std::uint64_t start = top;
  if (align != 0) start += (align - 1) & (0 - start);
  return {start + rounded <= field38 + field40, rounded, start, (field38 - field40) + start + rounded};
}

}  // namespace StackAllocCheck
