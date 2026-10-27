#include "arm64_reloc.h"

namespace sentinel {
namespace {

constexpr std::uint32_t kNop = 0xD503201Fu;
constexpr std::uint32_t kBrBase = 0xD61F0000u;   // BR Xn; Rn filled in
constexpr std::uint32_t kBlrBase = 0xD63F0000u;  // BLR Xn
constexpr std::uint32_t kLdrLiteralBase = 0x58000000u;  // LDR Xt, label; Rt and imm19 filled in
constexpr unsigned kScratchPrimary = 16;    // IP0: the inline patch already clobbers it
constexpr unsigned kScratchSecondary = 17;  // IP1: used when the window itself writes x16

// "Skip the next two words": from the inverted branch to the word after the BR, in words.
constexpr std::uint32_t kSkipTwoWords = 3;

std::int64_t SignExtend(std::uint32_t value, unsigned bits) noexcept {
  const std::uint32_t sign = 1u << (bits - 1);
  return static_cast<std::int64_t>(value ^ sign) - static_cast<std::int64_t>(sign);
}

// The code, then a pool of 64-bit literals that the code loads with LDR (literal).
class Builder {
 public:
  void Code(std::uint32_t word) noexcept { code_[codeCount_++] = word; }

  // LDR Xrt, =value: the LDR's offset is fixed up in Finish, once the pool's place is known.
  void LdrLiteral(unsigned rt, std::uint64_t value) noexcept {
    fixupWord_[poolCount_] = codeCount_;
    pool_[poolCount_++] = value;
    Code(kLdrLiteralBase | rt);
  }

  void Finish(Trampoline& out) noexcept {
    if ((codeCount_ & 1u) != 0) Code(kNop);  // the pool starts 8-byte aligned (the page is)
    const std::size_t poolStart = codeCount_;
    for (std::size_t i = 0; i < codeCount_; ++i) out.words[i] = code_[i];
    for (std::size_t i = 0; i < poolCount_; ++i) {
      const std::size_t literal = poolStart + 2 * i;
      const std::uint32_t offsetWords = static_cast<std::uint32_t>(literal - fixupWord_[i]);
      out.words[fixupWord_[i]] |= (offsetWords & 0x7FFFFu) << 5;
      out.words[literal] = static_cast<std::uint32_t>(pool_[i]);
      out.words[literal + 1] = static_cast<std::uint32_t>(pool_[i] >> 32);
    }
    out.count = poolStart + 2 * poolCount_;
  }

 private:
  std::uint32_t code_[16] = {};
  std::uint64_t pool_[6] = {};
  std::size_t fixupWord_[6] = {};
  std::size_t codeCount_ = 0;
  std::size_t poolCount_ = 0;
};

// True when [addr, addr + size) overlaps the patched bytes at [window, window + 16). A size of 0 is a point.
bool TouchesWindow(std::uint64_t addr, std::uint64_t size, std::uint64_t window) noexcept {
  return (addr - window) < kRelocWindowBytes || (size != 0 && (window - addr) < size);
}

bool Refuse(RelocRefusal& r, std::size_t index, std::uint32_t insn, PcRelKind kind, const char* reason) noexcept {
  r.index = index;
  r.insn = insn;
  r.kind = kind;
  r.reason = reason;
  return false;
}

// Kinds whose bits 4..0 are a register the word writes (ADR, ADRP, GPR literal loads), and the plain words, for
// which the field is the destination or a stored register: the check on them is deliberately coarse, since a
// false match only moves the scratch to x17.
bool WritesItsRegisterField(PcRelKind kind, std::uint32_t insn) noexcept {
  if (kind == PcRelKind::kLdrLiteral && (insn & (1u << 26)) != 0) return false;  // a SIMD register
  return kind == PcRelKind::kNone || kind == PcRelKind::kAdr || kind == PcRelKind::kAdrp ||
         kind == PcRelKind::kLdrLiteral || kind == PcRelKind::kLdrswLiteral;
}

struct LiteralLoad {
  std::uint32_t encoding;  // the LDR/LDRSW/PRFM (unsigned offset 0) of the same size, Rn = Rt = 0
  std::uint64_t size;      // bytes read; 0 for the hint
  bool simd;
};

LiteralLoad DecodeLiteralLoad(std::uint32_t insn) noexcept {
  const std::uint32_t opc = insn >> 30;
  if ((insn & (1u << 26)) != 0) {
    switch (opc) {
      case 0: return {0xBD400000u, 4, true};   // LDR St, [Xn]
      case 1: return {0xFD400000u, 8, true};   // LDR Dt, [Xn]
      default: return {0x3DC00000u, 16, true};  // LDR Qt, [Xn]
    }
  }
  switch (opc) {
    case 0: return {0xB9400000u, 4, false};  // LDR Wt, [Xn]
    case 1: return {0xF9400000u, 8, false};  // LDR Xt, [Xn]
    case 2: return {0xB9800000u, 4, false};  // LDRSW Xt, [Xn]
    default: return {0xF9800000u, 0, false};  // PRFM, [Xn]
  }
}

}  // namespace

PcRelKind ClassifyPcRel(std::uint32_t insn) noexcept {
  if ((insn & 0xFC000000u) == 0x14000000u) return PcRelKind::kB;
  if ((insn & 0xFC000000u) == 0x94000000u) return PcRelKind::kBl;
  if ((insn & 0xFF000010u) == 0x54000000u) return PcRelKind::kBCond;
  if ((insn & 0xFF000010u) == 0x54000010u) return PcRelKind::kBcCond;
  if ((insn & 0x7E000000u) == 0x34000000u) return (insn & (1u << 24)) != 0 ? PcRelKind::kCbnz : PcRelKind::kCbz;
  if ((insn & 0x7E000000u) == 0x36000000u) return (insn & (1u << 24)) != 0 ? PcRelKind::kTbnz : PcRelKind::kTbz;
  if ((insn & 0x9F000000u) == 0x10000000u) return PcRelKind::kAdr;
  if ((insn & 0x9F000000u) == 0x90000000u) return PcRelKind::kAdrp;
  if ((insn & 0x3B000000u) == 0x18000000u) {
    const std::uint32_t opc = insn >> 30;
    if ((insn & (1u << 26)) != 0) return opc == 3 ? PcRelKind::kLdrLiteralReserved : PcRelKind::kLdrLiteral;
    if (opc == 2) return PcRelKind::kLdrswLiteral;
    return opc == 3 ? PcRelKind::kPrfmLiteral : PcRelKind::kLdrLiteral;
  }
  return PcRelKind::kNone;
}

const char* PcRelName(PcRelKind kind) noexcept {
  switch (kind) {
    case PcRelKind::kNone: return "none";
    case PcRelKind::kB: return "B";
    case PcRelKind::kBl: return "BL";
    case PcRelKind::kBCond: return "B.cond";
    case PcRelKind::kBcCond: return "BC.cond";
    case PcRelKind::kCbz: return "CBZ";
    case PcRelKind::kCbnz: return "CBNZ";
    case PcRelKind::kTbz: return "TBZ";
    case PcRelKind::kTbnz: return "TBNZ";
    case PcRelKind::kAdr: return "ADR";
    case PcRelKind::kAdrp: return "ADRP";
    case PcRelKind::kLdrLiteral: return "LDR (literal)";
    case PcRelKind::kLdrswLiteral: return "LDRSW (literal)";
    case PcRelKind::kPrfmLiteral: return "PRFM (literal)";
    case PcRelKind::kLdrLiteralReserved: return "LDR (literal, SIMD, reserved)";
  }
  return "unknown";
}

bool BuildTrampoline(std::uint64_t originalPc, const std::uint32_t (&prologue)[kRelocPrologueWords],
                     Trampoline& out, RelocRefusal& refusal) noexcept {
  // The scratch register. The trampoline ends in a jump through it, so it must not hold a value the window
  // defines for the code after the window: x16, unless a word of the window has Rd/Rt = x16, then x17. A window
  // with a PC-relative word that writes both is refused; one without keeps x16 (the plain copy), as before.
  bool writes16 = false;
  bool writes17 = false;
  std::size_t firstPcRel = kRelocPrologueWords;
  for (std::size_t i = 0; i < kRelocPrologueWords; ++i) {
    const PcRelKind kind = ClassifyPcRel(prologue[i]);
    if (kind != PcRelKind::kNone && firstPcRel == kRelocPrologueWords) firstPcRel = i;
    if (WritesItsRegisterField(kind, prologue[i])) {
      writes16 = writes16 || (prologue[i] & 0x1Fu) == kScratchPrimary;
      writes17 = writes17 || (prologue[i] & 0x1Fu) == kScratchSecondary;
    }
  }
  unsigned scratch = kScratchPrimary;
  if (writes16) scratch = kScratchSecondary;
  if (writes16 && writes17 && firstPcRel != kRelocPrologueWords) {
    return Refuse(refusal, firstPcRel, prologue[firstPcRel], ClassifyPcRel(prologue[firstPcRel]),
                  "the window writes x16 and x17, which the relocated words need as scratch");
  }
  if (writes16 && writes17) scratch = kScratchPrimary;
  const std::uint32_t brScratch = kBrBase | (scratch << 5);
  const std::uint32_t blrScratch = kBlrBase | (scratch << 5);

  Builder b;
  for (std::size_t i = 0; i < kRelocPrologueWords; ++i) {
    const std::uint32_t insn = prologue[i];
    const std::uint64_t pc = originalPc + 4 * i;
    const PcRelKind kind = ClassifyPcRel(insn);
    const unsigned rt = insn & 0x1Fu;
    // The branch kinds share their tail; `target` and `condition` are set by the case.
    std::uint64_t target = 0;
    std::uint32_t inverted = 0;
    bool conditional = false;
    bool call = false;
    bool branch = false;

    switch (kind) {
      case PcRelKind::kNone:
        b.Code(insn);
        break;
      case PcRelKind::kB:
      case PcRelKind::kBl:
        target = pc + static_cast<std::uint64_t>(SignExtend(insn & 0x3FFFFFFu, 26) * 4);
        branch = true;
        call = kind == PcRelKind::kBl;
        break;
      case PcRelKind::kBCond: {
        target = pc + static_cast<std::uint64_t>(SignExtend((insn >> 5) & 0x7FFFFu, 19) * 4);
        branch = true;
        const std::uint32_t cond = insn & 0xFu;
        // AL (0xE) and NV (0xF) both always branch; their inverse would not be a skip.
        if (cond < 0xE) {
          conditional = true;
          inverted = 0x54000000u | (kSkipTwoWords << 5) | (cond ^ 1u);
        }
        break;
      }
      case PcRelKind::kCbz:
      case PcRelKind::kCbnz:
        target = pc + static_cast<std::uint64_t>(SignExtend((insn >> 5) & 0x7FFFFu, 19) * 4);
        branch = conditional = true;
        inverted = ((insn & 0xFF00001Fu) ^ (1u << 24)) | (kSkipTwoWords << 5);
        break;
      case PcRelKind::kTbz:
      case PcRelKind::kTbnz:
        target = pc + static_cast<std::uint64_t>(SignExtend((insn >> 5) & 0x3FFFu, 14) * 4);
        branch = conditional = true;
        inverted = ((insn & 0xFFF8001Fu) ^ (1u << 24)) | (kSkipTwoWords << 5);
        break;
      case PcRelKind::kAdr:
      case PcRelKind::kAdrp: {
        const std::uint32_t imm = (((insn >> 5) & 0x7FFFFu) << 2) | ((insn >> 29) & 3u);
        const std::int64_t offset = SignExtend(imm, 21);
        const std::uint64_t value = kind == PcRelKind::kAdr
                                        ? pc + static_cast<std::uint64_t>(offset)
                                        : (pc & ~std::uint64_t{0xFFF}) + (static_cast<std::uint64_t>(offset) << 12);
        if (rt == 31) {
          b.Code(kNop);  // the destination is XZR: nothing to write
        } else {
          b.LdrLiteral(rt, value);
        }
        break;
      }
      case PcRelKind::kPrfmLiteral:
        b.Code(kNop);
        break;
      case PcRelKind::kLdrLiteral:
      case PcRelKind::kLdrswLiteral: {
        const std::uint64_t address = pc + static_cast<std::uint64_t>(SignExtend((insn >> 5) & 0x7FFFFu, 19) * 4);
        const LiteralLoad load = DecodeLiteralLoad(insn);
        if (TouchesWindow(address, load.size, originalPc)) {
          return Refuse(refusal, i, insn, kind, "the literal it loads is inside the patched bytes");
        }
        // A general register that is not XZR holds the address itself; SIMD and XZR need the scratch.
        const unsigned base = (load.simd || rt == 31) ? scratch : rt;
        b.LdrLiteral(base, address);
        b.Code(load.encoding | (base << 5) | rt);
        break;
      }
      case PcRelKind::kBcCond:
        return Refuse(refusal, i, insn, kind, "BC.cond is not relocated");
      case PcRelKind::kLdrLiteralReserved:
        return Refuse(refusal, i, insn, kind, "unallocated literal-load encoding");
    }

    if (branch) {
      if (TouchesWindow(target, 0, originalPc)) {
        return Refuse(refusal, i, insn, kind, "the branch target is inside the patched bytes");
      }
      if (conditional) b.Code(inverted);
      b.LdrLiteral(scratch, target);
      b.Code(call ? blrScratch : brScratch);
    }
  }
  b.LdrLiteral(scratch, originalPc + kRelocWindowBytes);
  b.Code(brScratch);
  b.Finish(out);
  return true;
}

}  // namespace sentinel
