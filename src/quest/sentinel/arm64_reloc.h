/* Relocation of PC-relative arm64 instructions into the inline hook's trampoline.
 *
 * inline_hook.h overwrites the first four instructions of a function and runs those four, displaced, from a
 * trampoline in a fresh page. The window is a parameter: the build-time patch (src/quest/patch, ADR 0021)
 * overwrites three words, so BuildTrampoline takes one to four. A word that depends on its own address (B, BL, B.cond, CBZ/CBNZ, TBZ/TBNZ, ADR,
 * ADRP, LDR literal) would read a different address there, and the trampoline can be anywhere in the address
 * space, farther from the target than a B (+-128 MiB) or a CBZ/LDR literal (+-1 MiB) reaches. So each such word is
 * rewritten, from the address it had in the target, into a sequence that holds the absolute address it meant in
 * a literal pool at the end of the trampoline. The result is position independent: it does not depend on where
 * the trampoline is mapped.
 *
 * The sequences (S is the scratch register: x16, IP0, which the inline patch already clobbers, or x17 when a word of
 * the window has x16 as its Rd/Rt, so the jump back to the function does not destroy a value the window defined):
 *
 *   B   t              LDR S, =t ; BR S
 *   BL  t              LDR S, =t ; BLR S          (the callee returns to the next trampoline word)
 *   B.cond t           B.!cond .+12 ; LDR S, =t ; BR S
 *   CBZ/CBNZ Rt, t     CBNZ/CBZ Rt, .+12 ; LDR S, =t ; BR S        (same width, same Rt)
 *   TBZ/TBNZ Rt,#b, t  TBNZ/TBZ Rt,#b, .+12 ; LDR S, =t ; BR S
 *   ADR/ADRP Rd, a     LDR Xd, =a                     (a computed from the original PC; Rd = XZR becomes NOP)
 *   LDR Xt/Wt/LDRSW    LDR Xt, =a ; LDR Xt/Wt/LDRSW Xt, [Xt]        (the destination holds the address first)
 *   LDR St/Dt/Qt       LDR S, =a ; LDR St/Dt/Qt, [S]
 *   PRFM               NOP                            (a hint with no architectural effect)
 *
 * After the window's words the trampoline continues at original+4*count (16 for four words, 12 for three):
 * LDR S, =original+4*count ; BR S. Words that are not PC-relative are copied unchanged.
 *
 * What is refused, with the instruction named (RelocRefusal):
 *   - a branch whose target is inside the patched bytes (it would run the detour, not the original word),
 *   - a literal load that reads inside the patched bytes (it would read the detour's bytes),
 *   - a window with a PC-relative word whose words have both x16 and x17 as Rd/Rt: neither is free as scratch.
 *     The check reads bits 4..0 of every word that is not a branch, so it also fires on a store of x16 and of
 *     x17; an LDP second register or a write-back base is not seen. A window with no PC-relative word is copied as
 *     before, with x16 as the jump register whatever it writes,
 *   - BC.cond (the hinted conditional branch) and the unallocated SIMD literal form.
 *
 * A relocated BL makes the callee return into the trampoline, not to original+4 (which is patched). The callee
 * sees a different link register value; code that inspects its own return address is not equivalent.
 *
 * Pure: no memory is mapped and no log is written here; inline_hook.cpp does both.
 */
#pragma once

#include <cstddef>
#include <cstdint>

namespace sentinel {

// The PC-relative A64 instruction kinds the backend knows, plus the two it names to refuse.
enum class PcRelKind : std::uint8_t {
  kNone,  // position independent: copied unchanged
  kB,
  kBl,
  kBCond,
  kBcCond,  // BC.cond (FEAT_HBC): recognised, refused
  kCbz,
  kCbnz,
  kTbz,
  kTbnz,
  kAdr,
  kAdrp,
  kLdrLiteral,          // LDR Wt/Xt/St/Dt/Qt (literal)
  kLdrswLiteral,        // LDRSW Xt (literal)
  kPrfmLiteral,         // PRFM (literal)
  kLdrLiteralReserved,  // SIMD literal with opc 11: unallocated, refused
};

PcRelKind ClassifyPcRel(std::uint32_t insn) noexcept;

// The architectural mnemonic, for log lines: "B", "BL", "B.cond", "CBZ", "ADRP", "LDR (literal)", ...
// "none" for kNone.
const char* PcRelName(PcRelKind kind) noexcept;

// The bytes the run-time inline patch overwrites and the trampoline displaces: four words. This is the default
// window and the largest one; BuildTrampoline accepts any count from 1 to kRelocPrologueWords.
inline constexpr std::size_t kRelocPrologueWords = 4;
inline constexpr std::size_t kRelocWindowBytes = kRelocPrologueWords * 4;
// The most words a trampoline holds: 4 x 3 for the sequences, 2 for the return jump, 1 of padding, then at most
// 5 pool entries of 2 words.
inline constexpr std::size_t kMaxTrampolineWords = 32;

struct Trampoline {
  std::uint32_t words[kMaxTrampolineWords];
  std::size_t count;
};

struct RelocRefusal {
  std::size_t index;  // which word of the window
  std::uint32_t insn;
  PcRelKind kind;
  const char* reason;  // a static string
};

// Builds the trampoline for the `count` words (1 to kRelocPrologueWords) that sit at `originalPc` (and are about
// to be overwritten). True on success: out.words[0..out.count) is the code and then the pool, a multiple of 8
// bytes, and the first word is the first relocated word. False on a refusal, with `refusal` set; `out` is then
// unspecified. A count outside 1..kRelocPrologueWords is a refusal too (reason "the window size is not 1 to 4").
bool BuildTrampolineWindow(std::uint64_t originalPc, const std::uint32_t* prologue, std::size_t count,
                           Trampoline& out, RelocRefusal& refusal) noexcept;

// The same for a window held in an array: four words (the run-time inline hook) or three (the build-time stub).
template <std::size_t N>
bool BuildTrampoline(std::uint64_t originalPc, const std::uint32_t (&prologue)[N], Trampoline& out,
                     RelocRefusal& refusal) noexcept {
  static_assert(N >= 1 && N <= kRelocPrologueWords, "the relocation window is one to four words");
  return BuildTrampolineWindow(originalPc, prologue, N, out, refusal);
}

}  // namespace sentinel
