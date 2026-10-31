// Host test for the PC-relative relocator (arm64_reloc.h).
//
// The host is x86_64, so the relocated words are never executed natively. This test decodes them instead:
// every kind is checked twice. First by shape: the words the relocator emits are decoded field by field (the
// inverted condition, the register, the literal pool entry that holds the target address). Then by behaviour: a
// small A64 interpreter, written here from the architecture manual and covering the instructions the relocator
// emits and the PC-relative kinds it consumes, runs the original window at its original address and the
// relocated trampoline at an address far from it, from the same register, flag and memory state, and the two
// must end at the same pc with the same registers.
//
// What this cannot show: that the words run on a Quest (the device run is separate), and that the interpreter
// agrees with the hardware beyond the encodings pinned by KnownEncodings below.
//
// Run: arm64_reloc_test [name-filter]

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "arm64_reloc.h"
#include "mini_test.h"
#include "quest/patch/libr15_patch_table.h"

namespace {

using sentinel::BuildTrampoline;
using sentinel::PcRelKind;
using sentinel::RelocRefusal;
using sentinel::Trampoline;

constexpr std::uint32_t kNop = 0xD503201Fu;
constexpr std::uint32_t kBrX16 = 0xD61F0200u;
constexpr std::uint32_t kBlrX16 = 0xD63F0200u;

// ---- encoders (from the architecture manual's field layouts) --------------------------------------------

std::uint32_t Words(std::int64_t bytes) { return static_cast<std::uint32_t>(bytes / 4); }
std::uint32_t EncB(std::int64_t bytes) { return 0x14000000u | (Words(bytes) & 0x3FFFFFFu); }
std::uint32_t EncBl(std::int64_t bytes) { return 0x94000000u | (Words(bytes) & 0x3FFFFFFu); }
std::uint32_t EncBCond(unsigned cond, std::int64_t bytes) {
  return 0x54000000u | ((Words(bytes) & 0x7FFFFu) << 5) | cond;
}
std::uint32_t EncCb(unsigned sf, unsigned nz, unsigned rt, std::int64_t bytes) {
  return (sf << 31) | 0x34000000u | (nz << 24) | ((Words(bytes) & 0x7FFFFu) << 5) | rt;
}
std::uint32_t EncTb(unsigned nz, unsigned rt, unsigned bit, std::int64_t bytes) {
  return ((bit >> 5) << 31) | 0x36000000u | (nz << 24) | ((bit & 31u) << 19) | ((Words(bytes) & 0x3FFFu) << 5) | rt;
}
std::uint32_t EncAdrLike(std::uint32_t base, unsigned rd, std::int64_t imm21) {
  const auto imm = static_cast<std::uint32_t>(imm21) & 0x1FFFFFu;
  return base | ((imm & 3u) << 29) | ((imm >> 2) << 5) | rd;
}
std::uint32_t EncAdr(unsigned rd, std::int64_t bytes) { return EncAdrLike(0x10000000u, rd, bytes); }
std::uint32_t EncAdrp(unsigned rd, std::int64_t pages) { return EncAdrLike(0x90000000u, rd, pages); }
std::uint32_t EncAddImm(unsigned rd, unsigned rn, unsigned imm12) {  // ADD Xd, Xn, #imm12
  return 0x91000000u | (imm12 << 10) | (rn << 5) | rd;
}
std::uint32_t EncLdrLit(unsigned opc, unsigned v, unsigned rt, std::int64_t bytes) {
  return (opc << 30) | 0x18000000u | (v << 26) | ((Words(bytes) & 0x7FFFFu) << 5) | rt;
}

// ---- decoders used by the shape checks ------------------------------------------------------------------

// The 64-bit value an LDR Xt, literal at words[index] loads, from the pool the trampoline carries.
bool LiteralOf(const Trampoline& t, std::size_t index, unsigned* rt, std::uint64_t* value) {
  const std::uint32_t w = t.words[index];
  if ((w & 0xFF000000u) != 0x58000000u) return false;
  const std::uint32_t imm19 = (w >> 5) & 0x7FFFFu;
  const std::size_t at = index + imm19;  // forward literals only: the pool is at the end
  if (imm19 >= 0x40000u || at + 1 >= t.count) return false;
  *rt = w & 0x1Fu;
  *value = static_cast<std::uint64_t>(t.words[at]) | (static_cast<std::uint64_t>(t.words[at + 1]) << 32);
  return true;
}

// ---- the interpreter ------------------------------------------------------------------------------------

struct Cpu {
  std::uint64_t x[31] = {};
  std::uint8_t v[32][16] = {};
  unsigned nzcv = 0;  // N Z C V in bits 3..0
  std::uint64_t pc = 0;
  bool unsupported = false;
};

struct Prog {
  std::uint64_t base;
  std::vector<std::uint32_t> words;
  bool Contains(std::uint64_t pc) const { return pc >= base && pc < base + 4 * words.size() && (pc & 3) == 0; }
};

std::uint64_t Reg(const Cpu& c, unsigned r) { return r == 31 ? 0 : c.x[r]; }

std::int64_t Sext(std::uint64_t value, unsigned bits) {
  const unsigned shift = 64 - bits;
  return static_cast<std::int64_t>(value << shift) >> shift;
}

bool CondHolds(unsigned cond, unsigned nzcv) {
  const bool n = (nzcv & 8) != 0, z = (nzcv & 4) != 0, c = (nzcv & 2) != 0, v = (nzcv & 1) != 0;
  bool r = false;
  switch (cond >> 1) {
    case 0: r = z; break;
    case 1: r = c; break;
    case 2: r = n; break;
    case 3: r = v; break;
    case 4: r = c && !z; break;
    case 5: r = n == v; break;
    case 6: r = (n == v) && !z; break;
    default: r = true; break;  // AL and NV
  }
  return ((cond & 1) != 0 && cond != 15) ? !r : r;
}

// The game's memory: a fixed function of the address. The running program's own words shadow it.
std::uint8_t MemByte(const Prog& p, std::uint64_t addr) {
  if (p.Contains(addr & ~std::uint64_t{3})) {
    const std::uint32_t w = p.words[(addr - p.base) / 4];
    return static_cast<std::uint8_t>(w >> (8 * (addr & 3)));
  }
  return static_cast<std::uint8_t>(((addr * 0x9E3779B97F4A7C15ull) >> 56) ^ (addr & 0xFF));
}

std::uint64_t MemRead(const Prog& p, std::uint64_t addr, unsigned size) {
  std::uint64_t v = 0;
  for (unsigned i = 0; i < size && i < 8; ++i) v |= static_cast<std::uint64_t>(MemByte(p, addr + i)) << (8 * i);
  return v;
}

void Load(Cpu& c, const Prog& p, unsigned opc, unsigned simd, unsigned rt, std::uint64_t addr) {
  if (simd != 0) {
    const unsigned size = opc == 0 ? 4 : opc == 1 ? 8 : 16;
    if (opc == 3) { c.unsupported = true; return; }
    std::memset(c.v[rt], 0, 16);
    for (unsigned i = 0; i < size; ++i) c.v[rt][i] = MemByte(p, addr + i);
    return;
  }
  if (opc == 3) return;  // PRFM: a hint
  std::uint64_t value = 0;
  if (opc == 0) value = MemRead(p, addr, 4);
  if (opc == 1) value = MemRead(p, addr, 8);
  if (opc == 2) value = static_cast<std::uint64_t>(Sext(MemRead(p, addr, 4), 32));
  if (rt != 31) c.x[rt] = value;
}

// Position-independent words of a real prologue (SUB sp, STP, ORR) that the interpreter does not model; a test
// that names them here compares only what the PC-relative words and the jumps do.
std::vector<std::uint32_t> g_opaque;

void Step(Cpu& c, const Prog& p) {
  const std::uint32_t i = p.words[(c.pc - p.base) / 4];
  const std::uint64_t pc = c.pc;
  c.pc = pc + 4;
  if (i == kNop) return;
  for (const std::uint32_t w : g_opaque) {
    if (i == w) return;
  }
  if ((i & 0x7C000000u) == 0x14000000u) {  // B, BL
    if ((i & 0x80000000u) != 0) c.x[30] = pc + 4;
    c.pc = pc + static_cast<std::uint64_t>(Sext(i & 0x3FFFFFFu, 26) * 4);
  } else if ((i & 0xFF000010u) == 0x54000000u) {  // B.cond
    if (CondHolds(i & 0xF, c.nzcv)) c.pc = pc + static_cast<std::uint64_t>(Sext((i >> 5) & 0x7FFFFu, 19) * 4);
  } else if ((i & 0x7E000000u) == 0x34000000u) {  // CBZ, CBNZ
    std::uint64_t value = Reg(c, i & 0x1Fu);
    if ((i & 0x80000000u) == 0) value &= 0xFFFFFFFFu;
    const bool nonzero = (i & (1u << 24)) != 0;
    if ((value == 0) != nonzero) c.pc = pc + static_cast<std::uint64_t>(Sext((i >> 5) & 0x7FFFFu, 19) * 4);
  } else if ((i & 0x7E000000u) == 0x36000000u) {  // TBZ, TBNZ
    const unsigned bit = ((i >> 31) << 5) | ((i >> 19) & 31u);
    const bool bitSet = ((Reg(c, i & 0x1Fu) >> bit) & 1u) != 0;
    const bool nonzero = (i & (1u << 24)) != 0;
    if (bitSet == nonzero) c.pc = pc + static_cast<std::uint64_t>(Sext((i >> 5) & 0x3FFFu, 14) * 4);
  } else if ((i & 0x1F000000u) == 0x10000000u) {  // ADR, ADRP
    const std::int64_t imm = Sext((((i >> 5) & 0x7FFFFu) << 2) | ((i >> 29) & 3u), 21);
    const std::uint64_t value = (i & 0x80000000u) != 0
                                    ? (pc & ~std::uint64_t{0xFFF}) + (static_cast<std::uint64_t>(imm) << 12)
                                    : pc + static_cast<std::uint64_t>(imm);
    if ((i & 0x1Fu) != 31) c.x[i & 0x1Fu] = value;
  } else if ((i & 0x3B000000u) == 0x18000000u) {  // LDR, LDRSW, PRFM (literal)
    Load(c, p, i >> 30, (i >> 26) & 1u, i & 0x1Fu, pc + static_cast<std::uint64_t>(Sext((i >> 5) & 0x7FFFFu, 19) * 4));
  } else if ((i & 0xFFC00000u) == 0x91000000u) {  // ADD Xd, Xn, #imm12 (no shift)
    const unsigned rn = (i >> 5) & 0x1Fu;
    if (rn == 31 || (i & 0x1Fu) == 31) { c.unsupported = true; return; }
    c.x[i & 0x1Fu] = c.x[rn] + ((i >> 10) & 0xFFFu);
  } else if ((i & 0xFFFFFC1Fu) == 0xD61F0000u || (i & 0xFFFFFC1Fu) == 0xD63F0000u) {  // BR, BLR
    const std::uint64_t target = Reg(c, (i >> 5) & 0x1Fu);
    if ((i & 0x00200000u) != 0) c.x[30] = pc + 4;
    c.pc = target;
  } else {
    const std::uint32_t form = i & 0xFFFFFC00u;  // LDR/LDRSW/PRFM/SIMD LDR [Xn], unsigned offset 0
    const unsigned rn = (i >> 5) & 0x1Fu;
    const unsigned rt = i & 0x1Fu;
    if (rn == 31) { c.unsupported = true; return; }
    const std::uint64_t addr = c.x[rn];
    if (form == 0xB9400000u) Load(c, p, 0, 0, rt, addr);
    else if (form == 0xF9400000u) Load(c, p, 1, 0, rt, addr);
    else if (form == 0xB9800000u) Load(c, p, 2, 0, rt, addr);
    else if (form == 0xF9800000u) Load(c, p, 3, 0, rt, addr);
    else if (form == 0xBD400000u) Load(c, p, 0, 1, rt, addr);
    else if (form == 0xFD400000u) Load(c, p, 1, 1, rt, addr);
    else if (form == 0x3DC00000u) Load(c, p, 2, 1, rt, addr);
    else c.unsupported = true;
  }
}

// Runs until the pc leaves the program. False when the program used an instruction the interpreter lacks or
// did not leave.
bool Run(Cpu& c, const Prog& p) {
  for (int steps = 0; steps < 64; ++steps) {
    if (!p.Contains(c.pc)) return !c.unsupported;
    Step(c, p);
    if (c.unsupported) return false;
  }
  return false;
}

Cpu RandomCpu(std::uint64_t seed) {
  Cpu c;
  std::uint64_t s = seed * 0x9E3779B97F4A7C15ull + 1;
  auto next = [&s] {
    s ^= s << 13;
    s ^= s >> 7;
    s ^= s << 17;
    return s;
  };
  for (auto& r : c.x) r = next();
  for (auto& q : c.v) for (auto& b : q) b = static_cast<std::uint8_t>(next());
  c.x[30] = 0x1111;
  c.nzcv = static_cast<unsigned>(seed & 0xF);
  return c;
}

// The original window sits at kOriginal; each placement puts the trampoline far away (more than 1 MiB for the
// 19-bit and 14-bit forms, more than 128 MiB for B and BL, both directions).
constexpr std::uint64_t kOriginal = 0x0000007AB1230010ull;
// A window whose first two words end one 4 KiB page and whose last two start the next: ADRP's page is the word's
// own, so it differs between them. (The install never takes such a window on a 4 KiB-page device, but a 16 KiB
// one holds it.)
constexpr std::uint64_t kStraddling = 0x0000007AB1230FF8ull;
const std::int64_t kTrampolineOffsets[] = {0x200000, -0x200000, 0x10000000, -0x10000000, 0x1234567000ll, -0x7AB0000000ll};

// Relocates `window` (at `windowAt`), runs the original and the relocated trampoline at each far placement and
// compares. `what` names the case.
template <std::size_t N>
void CheckWindowEquivalentAt(std::uint64_t windowAt, const std::string& what, const std::uint32_t (&window)[N],
                             const Cpu& init) {
  Trampoline tr;
  RelocRefusal refusal;
  if (!BuildTrampoline(windowAt, window, tr, refusal)) {
    std::fprintf(stderr, "  %s: refused (%s)\n", what.c_str(), refusal.reason);
    CHECK(false);
    return;
  }
  for (const std::int64_t offset : kTrampolineOffsets) {
    const std::uint64_t at = windowAt + static_cast<std::uint64_t>(offset);
    Prog original{windowAt, {window, window + N}};
    Prog relocated{at, {tr.words, tr.words + tr.count}};
    Cpu a = init;
    Cpu b = init;
    a.pc = windowAt;
    b.pc = at;
    bool ok = Run(a, original) && Run(b, relocated);
    ok = ok && a.pc == b.pc;
    const bool called = a.x[30] != init.x[30];
    if (ok && called) {
      // A call: the callee returns. The original returns into its window, the relocated into its trampoline;
      // both must then reach the resume address.
      ok = b.x[30] >= at && b.x[30] < at + 4 * tr.count;
      a.pc = a.x[30];
      b.pc = b.x[30];
      ok = ok && Run(a, original) && Run(b, relocated) && a.pc == b.pc;
    }
    int badReg = -1;
    for (unsigned r = 0; ok && r < 31; ++r) {
      // The scratch registers, when the original words left them alone.
      if ((r == 16 || r == 17) && a.x[r] == init.x[r]) continue;
      if (r == 30 && called) continue;
      if (a.x[r] != b.x[r]) {
        ok = false;
        badReg = static_cast<int>(r);
      }
    }
    for (unsigned q = 0; ok && q < 32; ++q) {
      ok = std::memcmp(a.v[q], b.v[q], 16) == 0;
      if (!ok) badReg = 100 + static_cast<int>(q);
    }
    if (!ok) {
      std::fprintf(stderr, "  %s: differs at trampoline offset %lld (original pc %llx, relocated pc %llx, register %d)\n",
                   what.c_str(), static_cast<long long>(offset), static_cast<unsigned long long>(a.pc),
                   static_cast<unsigned long long>(b.pc), badReg);
    }
    CHECK(ok);
  }
}

template <std::size_t N>
void CheckWindowEquivalent(const std::string& what, const std::uint32_t (&window)[N], const Cpu& init) {
  CheckWindowEquivalentAt(kOriginal, what, window, init);
  CheckWindowEquivalentAt(kStraddling, what, window, init);
}

// Places `insn` at `index` of a NOP window, then CheckWindowEquivalent.
void CheckEquivalent(const std::string& what, std::uint32_t insn, std::size_t index, const Cpu& init) {
  std::uint32_t window[4] = {kNop, kNop, kNop, kNop};
  window[index] = insn;
  CheckWindowEquivalent(what, window, init);
}

std::string Name(const char* kind, std::int64_t offset, std::size_t index) {
  char buf[96];
  std::snprintf(buf, sizeof buf, "%s offset %lld at word %zu", kind, static_cast<long long>(offset), index);
  return buf;
}

TEST(KnownEncodings) {
  // Words from a disassembler listing, pinning the encoders (and so the interpreter's decoding) to the
  // architecture rather than to this file.
  CHECK_EQ(EncB(0x40), 0x14000010u);
  CHECK_EQ(EncBl(0x10), 0x94000004u);
  CHECK_EQ(EncBCond(0, 8), 0x54000040u);             // B.EQ #8
  CHECK_EQ(EncCb(1, 0, 0, 8), 0xB4000040u);          // CBZ x0, #8
  CHECK_EQ(EncCb(0, 1, 0, 0x10), 0x35000080u);       // CBNZ w0, #0x10
  CHECK_EQ(EncTb(0, 0, 0, 8), 0x36000040u);          // TBZ w0, #0, #8
  CHECK_EQ(EncTb(1, 0, 31, 8), 0x37F80040u);         // TBNZ w0, #31, #8
  CHECK_EQ(EncAdr(0, 0x10), 0x10000080u);            // ADR x0, #0x10
  CHECK_EQ(EncAdrp(1, 1), 0xB0000001u);              // ADRP x1, #0x1000
  CHECK_EQ(EncLdrLit(1, 0, 2, 0x20), 0x58000102u);   // LDR x2, #0x20
  CHECK_EQ(EncBCond(1, 8), 0x54000041u);             // B.NE #8
}

TEST(ClassificationOfLiteralWords) {
  // Literal words with zero offsets, so the classifier is pinned to the architecture's encodings and not to the encoders
  // in this file. Each pc-relative word depends on its own address; each other word is position independent.
  const std::uint32_t pcRelative[] = {
      0x14000000u,  // B #0
      0x94000000u,  // BL #0
      0x54000000u,  // B.EQ #0
      0xB4000000u,  // CBZ x0, #0
      0x34000000u,  // CBZ w0, #0
      0x36000000u,  // TBZ w0, #0, #0
      0x37000000u,  // TBNZ w0, #0, #0
      0x10000000u,  // ADR x0, #0
      0x90000000u,  // ADRP x0, #0
      0x54000010u,  // BC.cond #0
      0x58000000u,  // LDR x0, #0 (literal)
      0x98000000u,  // LDRSW x0, #0 (literal)
      0x1C000000u,  // LDR s0, #0 (literal, SIMD)
  };
  for (std::uint32_t w : pcRelative) CHECK(sentinel::ClassifyPcRel(w) != PcRelKind::kNone);
  const std::uint32_t positionIndependent[] = {
      0xA9BF7BFDu,  // STP x29, x30, [sp, #-16]!
      0x910003FDu,  // MOV x29, sp
      0xD10083FFu,  // SUB sp, sp, #32
      0xF90013F3u,  // STR x19, [sp, #32]
      0x91000400u,  // ADD x0, x0, #1
      0xF9400020u,  // LDR x0, [x1]
      0x39400000u,  // LDRB w0, [x1]
      0xD65F03C0u,  // RET
      0xD61F0200u,  // BR x16
      0xD503201Fu,  // NOP
  };
  for (std::uint32_t w : positionIndependent) CHECK(sentinel::ClassifyPcRel(w) == PcRelKind::kNone);
}

TEST(ClassificationAndNames) {
  struct Row { std::uint32_t insn; PcRelKind kind; const char* name; };
  const Row rows[] = {
      {EncB(8), PcRelKind::kB, "B"},
      {EncBl(8), PcRelKind::kBl, "BL"},
      {EncBCond(3, 8), PcRelKind::kBCond, "B.cond"},
      {0x54000010u, PcRelKind::kBcCond, "BC.cond"},
      {EncCb(1, 0, 3, 8), PcRelKind::kCbz, "CBZ"},
      {EncCb(0, 1, 3, 8), PcRelKind::kCbnz, "CBNZ"},
      {EncTb(0, 3, 40, 8), PcRelKind::kTbz, "TBZ"},
      {EncTb(1, 3, 40, 8), PcRelKind::kTbnz, "TBNZ"},
      {EncAdr(3, 8), PcRelKind::kAdr, "ADR"},
      {EncAdrp(3, 1), PcRelKind::kAdrp, "ADRP"},
      {EncLdrLit(0, 0, 3, 8), PcRelKind::kLdrLiteral, "LDR (literal)"},
      {EncLdrLit(1, 0, 3, 8), PcRelKind::kLdrLiteral, "LDR (literal)"},
      {EncLdrLit(2, 0, 3, 8), PcRelKind::kLdrswLiteral, "LDRSW (literal)"},
      {EncLdrLit(3, 0, 3, 8), PcRelKind::kPrfmLiteral, "PRFM (literal)"},
      {EncLdrLit(0, 1, 3, 8), PcRelKind::kLdrLiteral, "LDR (literal)"},
      {EncLdrLit(2, 1, 3, 8), PcRelKind::kLdrLiteral, "LDR (literal)"},
      {EncLdrLit(3, 1, 3, 8), PcRelKind::kLdrLiteralReserved, "LDR (literal, SIMD, reserved)"},
      {0xA9BF7BFDu, PcRelKind::kNone, "none"},  // STP x29, x30, [sp, #-16]!
      {0xD65F03C0u, PcRelKind::kNone, "none"},  // RET
      {0xF9400020u, PcRelKind::kNone, "none"},  // LDR x0, [x1]
  };
  for (const Row& r : rows) {
    CHECK(sentinel::ClassifyPcRel(r.insn) == r.kind);
    CHECK(std::strcmp(sentinel::PcRelName(r.kind), r.name) == 0);
  }
}

TEST(PositionIndependentWordsAreCopiedAndTheTrampolineResumes) {
  const std::uint32_t window[4] = {0xA9BF7BFDu, 0x910003FDu, 0xD10083FFu, 0xF90013F3u};
  Trampoline tr;
  RelocRefusal refusal;
  CHECK(BuildTrampoline(kOriginal, window, tr, refusal));
  for (std::size_t i = 0; i < 4; ++i) CHECK_EQ(tr.words[i], window[i]);
  CHECK_EQ(tr.words[4], 0x58000050u);  // LDR x16, #8
  CHECK_EQ(tr.words[5], kBrX16);
  unsigned rt = 0;
  std::uint64_t value = 0;
  CHECK(LiteralOf(tr, 4, &rt, &value));
  CHECK_EQ(rt, 16u);
  CHECK_EQ(value, kOriginal + 16);
  CHECK_EQ(tr.count, static_cast<std::size_t>(8));
}

TEST(BranchIsAnAbsoluteJump) {
  const std::int64_t offsets[] = {0x40, -0x40, 0x7FFFFFC, -0x8000000};
  for (const std::int64_t off : offsets) {
    for (std::size_t idx = 0; idx < 4; ++idx) {
      const std::uint32_t insn = EncB(off);
      std::uint32_t window[4] = {kNop, kNop, kNop, kNop};
      window[idx] = insn;
      Trampoline tr;
      RelocRefusal refusal;
      CHECK(BuildTrampoline(kOriginal, window, tr, refusal));
      unsigned rt = 0;
      std::uint64_t value = 0;
      CHECK(LiteralOf(tr, idx, &rt, &value));
      CHECK_EQ(rt, 16u);
      CHECK_EQ(value, kOriginal + 4 * idx + static_cast<std::uint64_t>(off));
      CHECK_EQ(tr.words[idx + 1], kBrX16);
      CheckEquivalent(Name("B", off, idx), insn, idx, RandomCpu(1));
    }
  }
}

TEST(BranchWithLinkCallsThroughX16AndReturnsIntoTheTrampoline) {
  const std::int64_t offsets[] = {0x40, -0x40, 0x7FFFFFC, -0x8000000};
  for (const std::int64_t off : offsets) {
    for (std::size_t idx = 0; idx < 4; ++idx) {
      const std::uint32_t insn = EncBl(off);
      std::uint32_t window[4] = {kNop, kNop, kNop, kNop};
      window[idx] = insn;
      Trampoline tr;
      RelocRefusal refusal;
      CHECK(BuildTrampoline(kOriginal, window, tr, refusal));
      unsigned rt = 0;
      std::uint64_t value = 0;
      CHECK(LiteralOf(tr, idx, &rt, &value));
      CHECK_EQ(value, kOriginal + 4 * idx + static_cast<std::uint64_t>(off));
      CHECK_EQ(tr.words[idx + 1], kBlrX16);
      CheckEquivalent(Name("BL", off, idx), insn, idx, RandomCpu(2));
    }
  }
}

TEST(ConditionalBranchInvertsTheConditionAndSkipsTheJump) {
  const std::int64_t offsets[] = {0x40, -0x40, 0xFFFFC, -0x100000};
  for (unsigned cond = 0; cond < 16; ++cond) {
    for (const std::int64_t off : offsets) {
      for (std::size_t idx = 0; idx < 4; ++idx) {
        const std::uint32_t insn = EncBCond(cond, off);
        std::uint32_t window[4] = {kNop, kNop, kNop, kNop};
        window[idx] = insn;
        Trampoline tr;
        RelocRefusal refusal;
        CHECK(BuildTrampoline(kOriginal, window, tr, refusal));
        const std::uint32_t first = tr.words[idx];
        unsigned rt = 0;
        std::uint64_t value = 0;
        if (cond < 14) {
          CHECK_EQ(first & 0xFF000010u, 0x54000000u);
          CHECK_EQ(first & 0xFu, cond ^ 1u);  // the inverted condition
          CHECK_EQ((first >> 5) & 0x7FFFFu, 3u);  // over the LDR and the BR
          CHECK(LiteralOf(tr, idx + 1, &rt, &value));
          CHECK_EQ(tr.words[idx + 2], kBrX16);
        } else {
          CHECK(LiteralOf(tr, idx, &rt, &value));  // AL and NV always branch: a plain jump
          CHECK_EQ(tr.words[idx + 1], kBrX16);
        }
        CHECK_EQ(rt, 16u);
        CHECK_EQ(value, kOriginal + 4 * idx + static_cast<std::uint64_t>(off));
        for (unsigned flags = 0; flags < 16; ++flags) {
          Cpu c = RandomCpu(3 + flags);
          c.nzcv = flags;
          CheckEquivalent(Name("B.cond", off, idx), insn, idx, c);
        }
      }
    }
  }
}

TEST(CompareAndBranchKeepsWidthAndRegisterAndFlipsTheTest) {
  const std::int64_t offsets[] = {0x40, -0x40, 0xFFFFC, -0x100000};
  const std::uint64_t values[] = {0, 1, 0x100000000ull, 0xFFFFFFFF00000000ull, 0xFFFFFFFFFFFFFFFFull, 0x80000000ull};
  for (unsigned sf = 0; sf < 2; ++sf) {
    for (unsigned nz = 0; nz < 2; ++nz) {
      for (const unsigned reg : {0u, 5u, 17u, 30u, 31u}) {
        for (const std::int64_t off : offsets) {
          const std::size_t idx = (reg + sf + nz) % 4;
          const std::uint32_t insn = EncCb(sf, nz, reg, off);
          std::uint32_t window[4] = {kNop, kNop, kNop, kNop};
          window[idx] = insn;
          Trampoline tr;
          RelocRefusal refusal;
          CHECK(BuildTrampoline(kOriginal, window, tr, refusal));
          const std::uint32_t first = tr.words[idx];
          CHECK_EQ(first >> 31, sf);                 // same width
          CHECK_EQ((first >> 25) & 0x3Fu, 0x1Au);    // CBZ/CBNZ
          CHECK_EQ((first >> 24) & 1u, nz ^ 1u);     // the opposite test
          CHECK_EQ(first & 0x1Fu, reg);              // same register
          CHECK_EQ((first >> 5) & 0x7FFFFu, 3u);
          unsigned rt = 0;
          std::uint64_t value = 0;
          CHECK(LiteralOf(tr, idx + 1, &rt, &value));
          CHECK_EQ(value, kOriginal + 4 * idx + static_cast<std::uint64_t>(off));
          for (const std::uint64_t v : values) {
            Cpu c = RandomCpu(10 + reg);
            if (reg < 31) c.x[reg] = v;
            CheckEquivalent(Name(nz ? "CBNZ" : "CBZ", off, idx), insn, idx, c);
          }
        }
      }
    }
  }
}

TEST(TestAndBranchKeepsBitAndRegisterAndFlipsTheTest) {
  const std::int64_t offsets[] = {0x40, -0x40, 0x7FFC, -0x8000};
  for (unsigned nz = 0; nz < 2; ++nz) {
    for (const unsigned bit : {0u, 5u, 31u, 32u, 40u, 63u}) {
      for (const unsigned reg : {0u, 9u, 16u, 28u, 31u}) {
        for (const std::int64_t off : offsets) {
          const std::size_t idx = (bit + reg) % 4;
          const std::uint32_t insn = EncTb(nz, reg, bit, off);
          std::uint32_t window[4] = {kNop, kNop, kNop, kNop};
          window[idx] = insn;
          Trampoline tr;
          RelocRefusal refusal;
          CHECK(BuildTrampoline(kOriginal, window, tr, refusal));
          const std::uint32_t first = tr.words[idx];
          CHECK_EQ(first >> 31, bit >> 5);            // b5
          CHECK_EQ((first >> 19) & 31u, bit & 31u);   // b40
          CHECK_EQ((first >> 25) & 0x3Fu, 0x1Bu);     // TBZ/TBNZ
          CHECK_EQ((first >> 24) & 1u, nz ^ 1u);
          CHECK_EQ(first & 0x1Fu, reg);
          CHECK_EQ((first >> 5) & 0x3FFFu, 3u);
          unsigned rt = 0;
          std::uint64_t value = 0;
          CHECK(LiteralOf(tr, idx + 1, &rt, &value));
          CHECK_EQ(value, kOriginal + 4 * idx + static_cast<std::uint64_t>(off));
          for (const std::uint64_t v : {std::uint64_t{0}, ~std::uint64_t{0}, std::uint64_t{1} << bit, ~(std::uint64_t{1} << bit)}) {
            Cpu c = RandomCpu(20 + bit);
            if (reg < 31) c.x[reg] = v;
            CheckEquivalent(Name(nz ? "TBNZ" : "TBZ", off, idx), insn, idx, c);
          }
        }
      }
    }
  }
}

TEST(AdrLoadsTheAbsoluteAddressIntoTheSameRegister) {
  const std::int64_t offsets[] = {0x10, -0x10, 0xFFFFF, -0x100000, 0x41};
  for (const unsigned rd : {0u, 7u, 16u, 29u, 31u}) {
    for (const std::int64_t off : offsets) {
      for (std::size_t idx = 0; idx < 4; ++idx) {
        const std::uint32_t insn = EncAdr(rd, off);
        std::uint32_t window[4] = {kNop, kNop, kNop, kNop};
        window[idx] = insn;
        Trampoline tr;
        RelocRefusal refusal;
        CHECK(BuildTrampoline(kOriginal, window, tr, refusal));
        unsigned rt = 0;
        std::uint64_t value = 0;
        if (rd == 31) {
          CHECK_EQ(tr.words[idx], kNop);  // XZR: nothing to write
        } else {
          CHECK(LiteralOf(tr, idx, &rt, &value));
          CHECK_EQ(rt, rd);
          CHECK_EQ(value, kOriginal + 4 * idx + static_cast<std::uint64_t>(off));
        }
        CheckEquivalent(Name("ADR", off, idx), insn, idx, RandomCpu(30 + rd));
      }
    }
  }
}

TEST(AdrpLoadsTheAbsolutePageIntoTheSameRegister) {
  const std::int64_t pages[] = {1, -1, 0xFFFFF, -0x100000, 0};
  for (const unsigned rd : {0u, 3u, 16u, 28u, 31u}) {
    for (const std::int64_t page : pages) {
      for (std::size_t idx = 0; idx < 4; ++idx) {
        const std::uint32_t insn = EncAdrp(rd, page);
        std::uint32_t window[4] = {kNop, kNop, kNop, kNop};
        window[idx] = insn;
        Trampoline tr;
        RelocRefusal refusal;
        CHECK(BuildTrampoline(kOriginal, window, tr, refusal));
        unsigned rt = 0;
        std::uint64_t value = 0;
        if (rd != 31) {
          CHECK(LiteralOf(tr, idx, &rt, &value));
          CHECK_EQ(rt, rd);
          // The page of the original word's own address, not of the trampoline's or the window's start.
          CHECK_EQ(value, ((kOriginal + 4 * idx) & ~std::uint64_t{0xFFF}) + (static_cast<std::uint64_t>(page) << 12));
        }
        CheckEquivalent(Name("ADRP", page, idx), insn, idx, RandomCpu(40 + rd));
      }
    }
  }
}

TEST(LiteralLoadsLoadFromTheAbsoluteAddress) {
  struct Form { unsigned opc, simd; const char* name; };
  const Form forms[] = {{0, 0, "LDR w"}, {1, 0, "LDR x"}, {2, 0, "LDRSW"}, {0, 1, "LDR s"}, {1, 1, "LDR d"}, {2, 1, "LDR q"}};
  const std::int64_t offsets[] = {0x40, -0x40, 0xFFFFC, -0x100000};
  for (const Form& form : forms) {
    for (const unsigned rt : {0u, 8u, 16u, 29u, 31u}) {
      for (const std::int64_t off : offsets) {
        for (std::size_t idx = 0; idx < 4; ++idx) {
          const std::uint32_t insn = EncLdrLit(form.opc, form.simd, rt, off);
          std::uint32_t window[4] = {kNop, kNop, kNop, kNop};
          window[idx] = insn;
          Trampoline tr;
          RelocRefusal refusal;
          CHECK(BuildTrampoline(kOriginal, window, tr, refusal));
          unsigned base = 0;
          std::uint64_t value = 0;
          CHECK(LiteralOf(tr, idx, &base, &value));
          CHECK_EQ(value, kOriginal + 4 * idx + static_cast<std::uint64_t>(off));
          // A general register that is not XZR holds the address itself; SIMD and XZR use x16.
          CHECK_EQ(base, (form.simd != 0 || rt == 31) ? 16u : rt);
          const std::uint32_t load = tr.words[idx + 1];
          CHECK_EQ((load >> 5) & 0x1Fu, base);   // [Xn] is the register that holds the address
          CHECK_EQ(load & 0x1Fu, rt);            // the destination is the original's
          CHECK_EQ((load >> 10) & 0xFFFu, 0u);   // offset zero
          CheckEquivalent(Name(form.name, off, idx), insn, idx, RandomCpu(50 + rt));
        }
      }
    }
  }
}

TEST(PrefetchLiteralBecomesANop) {
  for (std::size_t idx = 0; idx < 4; ++idx) {
    std::uint32_t window[4] = {kNop, kNop, kNop, kNop};
    window[idx] = EncLdrLit(3, 0, 5, 0x40);
    Trampoline tr;
    RelocRefusal refusal;
    CHECK(BuildTrampoline(kOriginal, window, tr, refusal));
    CHECK_EQ(tr.words[idx], kNop);
    CheckEquivalent(Name("PRFM", 0x40, idx), window[idx], idx, RandomCpu(60));
  }
}

TEST(SeveralPcRelativeWordsInOneWindow) {
  // The common entry: ADRP + LDR through the page, then a compare-and-branch and a call.
  const std::uint32_t window[4] = {EncAdrp(1, 0x20), EncLdrLit(1, 0, 2, 0x80), EncCb(1, 0, 2, -0x200), EncBl(0x4000)};
  Trampoline tr;
  RelocRefusal refusal;
  CHECK(BuildTrampoline(kOriginal, window, tr, refusal));
  CHECK(tr.count <= sentinel::kMaxTrampolineWords);
  for (unsigned seed = 0; seed < 8; ++seed) {
    for (const std::int64_t offset : kTrampolineOffsets) {
      const std::uint64_t at = kOriginal + static_cast<std::uint64_t>(offset);
      Prog original{kOriginal, {window, window + 4}};
      Prog relocated{at, {tr.words, tr.words + tr.count}};
      Cpu a = RandomCpu(70 + seed);
      Cpu b = a;
      a.pc = kOriginal;
      b.pc = at;
      // x2 is loaded by the LDR before the CBZ reads it; both sides load the same word.
      CHECK(Run(a, original));
      CHECK(Run(b, relocated));
      CHECK_EQ(a.pc, b.pc);
      for (unsigned r = 0; r < 30; ++r) {
        if (r == 16) continue;
        CHECK_EQ(a.x[r], b.x[r]);
      }
    }
  }
}

TEST(WordsTheRelocatorRefusesNameTheInstruction) {
  struct Case { std::uint32_t window[4]; std::size_t index; PcRelKind kind; const char* why; };
  const std::uint32_t nop = kNop;
  const Case cases[] = {
      // Each of the four words of the window as a branch target, and the word just past it as the control.
      {{EncB(0), nop, nop, nop}, 0, PcRelKind::kB, "target is inside the patched bytes"},
      {{nop, EncB(-4), nop, nop}, 1, PcRelKind::kB, "target is inside the patched bytes"},
      {{nop, nop, EncBl(-8), nop}, 2, PcRelKind::kBl, "target is inside the patched bytes"},
      {{nop, nop, nop, EncBCond(1, -12)}, 3, PcRelKind::kBCond, "target is inside the patched bytes"},
      {{EncCb(1, 0, 0, 12), nop, nop, nop}, 0, PcRelKind::kCbz, "target is inside the patched bytes"},
      {{nop, EncCb(0, 1, 0, 4), nop, nop}, 1, PcRelKind::kCbnz, "target is inside the patched bytes"},
      {{nop, nop, EncTb(0, 1, 3, 4), nop}, 2, PcRelKind::kTbz, "target is inside the patched bytes"},
      {{nop, nop, nop, EncTb(1, 1, 3, -12)}, 3, PcRelKind::kTbnz, "target is inside the patched bytes"},
      {{nop, EncLdrLit(1, 0, 1, 4), nop, nop}, 1, PcRelKind::kLdrLiteral, "literal it loads is inside the patched bytes"},
      {{nop, nop, EncLdrLit(2, 0, 1, 4), nop}, 2, PcRelKind::kLdrswLiteral, "literal it loads is inside the patched bytes"},
      {{nop, EncLdrLit(2, 1, 1, 8), nop, nop}, 1, PcRelKind::kLdrLiteral, "literal it loads is inside the patched bytes"},
      // A 16-byte load that starts before the window and reaches into it.
      {{nop, nop, nop, EncLdrLit(2, 1, 1, -20)}, 3, PcRelKind::kLdrLiteral, "literal it loads is inside the patched bytes"},
      {{nop, 0x54000010u, nop, nop}, 1, PcRelKind::kBcCond, "BC.cond is not relocated"},
      {{nop, nop, EncLdrLit(3, 1, 1, 0x40), nop}, 2, PcRelKind::kLdrLiteralReserved, "unallocated literal-load encoding"},
      // x16 and x17 both written, then a word that needs a scratch register (the first PC-relative word is named).
      {{EncAdrp(16, 1), EncAdrp(17, 1), EncB(0x100), nop}, 0, PcRelKind::kAdrp, "x16 and x17"},
      {{nop, EncAddImm(16, 16, 1), EncAddImm(17, 17, 1), EncBCond(0, 0x100)}, 3,
       PcRelKind::kBCond, "x16 and x17"},
      {{EncLdrLit(1, 0, 16, 0x100), EncLdrLit(1, 0, 17, 0x100), nop, nop}, 0, PcRelKind::kLdrLiteral, "x16 and x17"},
  };
  for (const Case& c : cases) {
    Trampoline tr;
    RelocRefusal refusal{};
    const bool built = BuildTrampoline(kOriginal, c.window, tr, refusal);
    CHECK(!built);
    if (built) continue;
    CHECK_EQ(refusal.index, c.index);
    CHECK_EQ(refusal.insn, c.window[c.index]);
    CHECK(refusal.kind == c.kind);
    CHECK(std::strstr(refusal.reason, c.why) != nullptr);
  }
}

TEST(TargetsJustOutsideTheWindowAreRelocated) {
  // The next word after the 16 bytes, and the word before them: both are ordinary absolute targets.
  const std::uint32_t after[4] = {EncB(16), kNop, kNop, kNop};
  const std::uint32_t before[4] = {EncB(-4), kNop, kNop, kNop};
  Trampoline tr;
  RelocRefusal refusal;
  CHECK(BuildTrampoline(kOriginal, after, tr, refusal));
  CHECK(BuildTrampoline(kOriginal, before, tr, refusal));
  // A prefetch of the patched bytes reads nothing, so it is not refused.
  const std::uint32_t prefetch[4] = {EncLdrLit(3, 0, 1, 4), kNop, kNop, kNop};
  CHECK(BuildTrampoline(kOriginal, prefetch, tr, refusal));
  // An ADR of an address inside the window is a plain address, relocated as one.
  const std::uint32_t adr[4] = {EncAdr(1, 8), kNop, kNop, kNop};
  CHECK(BuildTrampoline(kOriginal, adr, tr, refusal));
  unsigned rt = 0;
  std::uint64_t value = 0;
  CHECK(LiteralOf(tr, 0, &rt, &value));
  CHECK_EQ(value, kOriginal + 8);
}

TEST(TheQuestSetTextPrologueRelocates) {
  // The first four words of CCanvasUICS::SetText in the pinned libr15 (src/quest/social/menu_version_label.cpp):
  // cbz x3, #0x48 ; sub sp, sp, #0x20 ; stp x19, x30, [sp, #16] ; mov x19, x3. The version label's install was
  // refused on the first word.
  const std::uint32_t window[4] = {0xB4000243u, 0xD10083FFu, 0xA9017BF3u, 0xAA0303F3u};
  CHECK_EQ(sentinel::ClassifyPcRel(window[0]) == PcRelKind::kCbz, true);
  Trampoline tr;
  RelocRefusal refusal;
  CHECK(BuildTrampoline(kOriginal, window, tr, refusal));
  CHECK_EQ(tr.words[0], 0xB5000063u);  // CBNZ x3, over the jump
  unsigned rt = 0;
  std::uint64_t value = 0;
  CHECK(LiteralOf(tr, 1, &rt, &value));
  CHECK_EQ(value, kOriginal + 0x48);
  CHECK_EQ(tr.words[2], kBrX16);
  for (std::size_t i = 1; i < 4; ++i) CHECK_EQ(tr.words[2 + i], window[i]);  // the rest is copied
  g_opaque.assign(window + 1, window + 4);
  for (const std::uint64_t v : {std::uint64_t{0}, std::uint64_t{1}, std::uint64_t{0x100000000ull}}) {
    Cpu c = RandomCpu(100);
    c.x[3] = v;
    CheckWindowEquivalent("SetText prologue", window, c);
  }
  g_opaque.clear();
}

TEST(AWindowThatWritesX16JumpsBackThroughX17) {
  // ADRP x16 then an ADD from it: the value x16 holds after the window must survive the jump back.
  const std::uint32_t windows[][4] = {
      {EncAdrp(16, 1), EncAddImm(2, 16, 8), kNop, kNop},
      {EncAdr(16, 0x40), EncB(0x100), kNop, kNop},
      {kNop, EncLdrLit(1, 0, 16, 0x80), EncBCond(1, 0x100), kNop},
      {EncAdrp(16, 3), EncCb(1, 0, 0, 0x100), EncTb(0, 3, 40, 0x100), EncBl(0x1000)},
      {EncAdrp(16, 1), EncLdrLit(0, 1, 2, 0x80), kNop, kNop},
  };
  for (const auto& window : windows) {
    Trampoline tr;
    RelocRefusal refusal;
    CHECK(BuildTrampoline(kOriginal, window, tr, refusal));
    for (unsigned seed = 0; seed < 4; ++seed) {
      Cpu c = RandomCpu(90 + seed);
      c.x[17] = 0x1717;
      CheckWindowEquivalent("window that writes x16", window, c);
    }
  }
}

TEST(AWindowWithoutX16UsesX16AndOneWithBothKeepsX16WhenNothingNeedsScratch) {
  const std::uint32_t plain[4] = {EncAdrp(1, 1), kNop, kNop, kNop};
  Trampoline tr;
  RelocRefusal refusal;
  CHECK(BuildTrampoline(kOriginal, plain, tr, refusal));
  unsigned rt = 0;
  std::uint64_t value = 0;
  CHECK(LiteralOf(tr, 4, &rt, &value));
  CHECK_EQ(rt, 16u);
  CHECK_EQ(tr.words[5], kBrX16);
  // x16 and x17 both written but no PC-relative word: copied as before, jumping back through x16.
  const std::uint32_t both[4] = {EncAddImm(16, 16, 1), EncAddImm(17, 17, 1), kNop, kNop};
  CHECK(BuildTrampoline(kOriginal, both, tr, refusal));
  CHECK(LiteralOf(tr, 4, &rt, &value));
  CHECK_EQ(rt, 16u);
  // x16 written, no x17: the jump back goes through x17.
  const std::uint32_t only16[4] = {EncAddImm(16, 16, 1), kNop, kNop, kNop};
  CHECK(BuildTrampoline(kOriginal, only16, tr, refusal));
  CHECK(LiteralOf(tr, 4, &rt, &value));
  CHECK_EQ(rt, 17u);
  CHECK_EQ(tr.words[5], 0xD61F0220u);  // BR x17
}

TEST(EveryLiteralIsEightByteAlignedAndTheImageIsWholeQuads) {
  // Code of every length (an odd number of words gets a pad) so the pool starts on a quad boundary.
  const std::uint32_t windows[][4] = {
      {EncB(0x40), kNop, kNop, kNop},
      {EncBCond(0, 0x40), kNop, kNop, kNop},
      {EncBCond(0, 0x40), EncB(0x80), kNop, kNop},
      {EncAdr(1, 8), kNop, kNop, kNop},
      {EncAdr(1, 8), EncAdr(2, 8), EncAdr(3, 8), EncAdr(4, 8)},
      {EncCb(1, 0, 0, 0x40), EncCb(1, 0, 1, 0x40), EncCb(1, 0, 2, 0x40), EncCb(1, 0, 3, 0x40)},
      {EncLdrLit(0, 1, 1, 0x40), EncLdrLit(0, 1, 2, 0x40), EncBl(0x80), EncBCond(1, 0x40)},
  };
  for (const auto& window : windows) {
    Trampoline tr;
    RelocRefusal refusal;
    CHECK(BuildTrampoline(kOriginal, window, tr, refusal));
    CHECK(tr.count <= sentinel::kMaxTrampolineWords);
    CHECK_EQ(tr.count % 2, static_cast<std::size_t>(0));
    for (std::size_t i = 0; i < tr.count; ++i) {
      unsigned rt = 0;
      std::uint64_t value = 0;
      if ((tr.words[i] & 0xFF000000u) != 0x58000000u) continue;  // an LDR x, literal (a pool word may look like one)
      const std::size_t at = i + ((tr.words[i] >> 5) & 0x7FFFFu);
      if (at >= tr.count) continue;
      CHECK(LiteralOf(tr, i, &rt, &value));
      CHECK_EQ(at % 2, static_cast<std::size_t>(0));
    }
  }
}

// ---- the window is a parameter (ADR 0021): the build-time stub overwrites three words ------------------------

TEST(AThreeWordWindowResumesTwelveBytesOn) {
  const std::uint32_t window[3] = {0xD10183FFu, 0xF9001BF6u, 0xA90453F5u};  // lobby_full.join's stock words
  Trampoline tr;
  RelocRefusal refusal;
  CHECK(BuildTrampoline(kOriginal, window, tr, refusal));
  for (std::size_t i = 0; i < 3; ++i) CHECK_EQ(tr.words[i], window[i]);
  unsigned rt = 0;
  std::uint64_t value = 0;
  CHECK(LiteralOf(tr, 3, &rt, &value));
  CHECK_EQ(rt, 16u);
  CHECK_EQ(value, kOriginal + 12);  // not +16: the fourth word is not displaced
  CHECK_EQ(tr.words[4], kBrX16);
  // The four-word default is unchanged: the same words and a fourth one resume at +16.
  const std::uint32_t four[4] = {window[0], window[1], window[2], 0xA9057BF3u};
  CHECK(BuildTrampoline(kOriginal, four, tr, refusal));
  CHECK(LiteralOf(tr, 4, &rt, &value));
  CHECK_EQ(value, kOriginal + 16);
  // One and two words work as well.
  const std::uint32_t one[1] = {window[0]};
  CHECK(BuildTrampoline(kOriginal, one, tr, refusal));
  CHECK(LiteralOf(tr, 1, &rt, &value));
  CHECK_EQ(value, kOriginal + 4);
}

TEST(AThreeWordWindowIsExactlyTwelveBytesForTheBranchChecks) {
  Trampoline tr;
  RelocRefusal refusal{};
  // A branch to site+12 lands outside a 3-word window (relocated), and inside a 4-word one (refused).
  const std::uint32_t three[3] = {EncB(12), kNop, kNop};
  CHECK(BuildTrampoline(kOriginal, three, tr, refusal));
  unsigned rt = 0;
  std::uint64_t value = 0;
  CHECK(LiteralOf(tr, 0, &rt, &value));
  CHECK_EQ(value, kOriginal + 12);
  const std::uint32_t four[4] = {EncB(12), kNop, kNop, kNop};
  CHECK(!BuildTrampoline(kOriginal, four, tr, refusal));
  // A branch to site+8 is inside both.
  const std::uint32_t inside[3] = {EncB(8), kNop, kNop};
  CHECK(!BuildTrampoline(kOriginal, inside, tr, refusal));
  CHECK(std::strstr(refusal.reason, "target is inside the patched bytes") != nullptr);
  // A literal load that reads site+12 (the first byte past a 3-word window) is allowed; one that reads site+8 is not.
  const std::uint32_t pastLoad[3] = {EncLdrLit(1, 0, 1, 12), kNop, kNop};
  CHECK(BuildTrampoline(kOriginal, pastLoad, tr, refusal));
  const std::uint32_t insideLoad[3] = {EncLdrLit(1, 0, 1, 8), kNop, kNop};
  CHECK(!BuildTrampoline(kOriginal, insideLoad, tr, refusal));
  CHECK(std::strstr(refusal.reason, "literal it loads is inside the patched bytes") != nullptr);
}

TEST(AWindowOfAnyOtherSizeIsRefused) {
  Trampoline tr;
  RelocRefusal refusal{};
  const std::uint32_t words[5] = {kNop, kNop, kNop, kNop, kNop};
  CHECK(!sentinel::BuildTrampolineWindow(kOriginal, words, 0, tr, refusal));
  CHECK(std::strstr(refusal.reason, "window size") != nullptr);
  refusal = {};
  CHECK(!sentinel::BuildTrampolineWindow(kOriginal, words, 5, tr, refusal));
  CHECK(std::strstr(refusal.reason, "window size") != nullptr);
  CHECK(!sentinel::BuildTrampolineWindow(kOriginal, nullptr, 3, tr, refusal));
  CHECK(sentinel::BuildTrampolineWindow(kOriginal, words, 4, tr, refusal));
}

TEST(TheQuestSetTextLeadingCbzRelocatesInAThreeWordWindow) {
  // The stock words of CCanvasUICS::SetText (libr15 disassembly 0x149111c; the menu version label hooks that function
  // through its GOT slot, so it is not a patch-table row): cbz x3, #0x48 ; sub sp, sp, #0x20 ; stp x19, x30,
  // [sp, #16]. The CBZ is the case that matters: its target is far outside the window and the trampoline is far
  // from it, so it becomes CBNZ over an absolute jump.
  const std::uint32_t window[3] = {0xB4000243u, 0xD10083FFu, 0xA9017BF3u};
  Trampoline tr;
  RelocRefusal refusal;
  CHECK(BuildTrampoline(kOriginal, window, tr, refusal));
  CHECK_EQ(tr.words[0], 0xB5000063u);  // CBNZ x3, over the jump
  unsigned rt = 0;
  std::uint64_t value = 0;
  CHECK(LiteralOf(tr, 1, &rt, &value));
  CHECK_EQ(value, kOriginal + 0x48);
  g_opaque.assign(window + 1, window + 3);
  for (const std::uint64_t v : {std::uint64_t{0}, std::uint64_t{1}, std::uint64_t{0x100000000ull}}) {
    Cpu c = RandomCpu(100);
    c.x[3] = v;
    CheckWindowEquivalent("SetText three-word window", window, c);
  }
  g_opaque.clear();
}

TEST(EveryInlineRowOfThePatchTableRelocatesInItsThreeWordWindow) {
  // The real stock words of each inline-hook row of src/quest/patch/libr15_patch_table.h, at the row's own pinned
  // vaddr (a real address, so a PC-relative word would resolve against it), original window against the trampoline.
  std::size_t inlineRows = 0;
  for (std::size_t r = 0; r < nevr_quest::patch::kRowCount; ++r) {
    const nevr_quest::patch::Row& row = nevr_quest::patch::kRows[r];
    if (row.kind != nevr_quest::patch::RowKind::kInlineHook) continue;
    ++inlineRows;
    CHECK_EQ(row.sites[0].wordCount, static_cast<std::uint8_t>(3));
    std::uint32_t window[3] = {row.sites[0].words[0], row.sites[0].words[1], row.sites[0].words[2]};
    Trampoline tr;
    RelocRefusal refusal{};
    if (!BuildTrampoline(row.sites[0].Vaddr(), window, tr, refusal)) {
      std::fprintf(stderr, "  %s: refused (%s)\n", row.id, refusal.reason);
      CHECK(false);
      continue;
    }
    g_opaque.clear();
    for (const std::uint32_t w : window) {
      if (sentinel::ClassifyPcRel(w) == PcRelKind::kNone) g_opaque.push_back(w);
    }
    for (unsigned seed = 0; seed < 3; ++seed) {
      Cpu c = RandomCpu(500 + seed);
      c.x[3] = seed;  // a window may start with a CBZ/CBNZ on x3: take it both ways
      CheckWindowEquivalentAt(row.sites[0].Vaddr(), row.id, window, c);
    }
    g_opaque.clear();
  }
  CHECK_EQ(inlineRows, static_cast<std::size_t>(5));
}

TEST(ThreeWordWindowsWithEveryPcRelativeKindRunLikeTheOriginal) {
  const std::uint32_t windows[][3] = {
      {EncBCond(1, 0x100), kNop, kNop},
      {kNop, EncCb(1, 0, 5, 0x1000), kNop},
      {kNop, kNop, EncTb(1, 6, 40, 0x100)},
      {EncAdrp(8, 0x1234), EncAddImm(8, 8, 0x123), kNop},
      {EncAdr(9, 0x40), kNop, EncB(0x100)},
      {kNop, EncBl(0x1000), kNop},
      {EncLdrLit(1, 0, 3, 0x800), EncLdrLit(1, 1, 4, 0x800), kNop},
  };
  for (const auto& window : windows) {
    Trampoline tr;
    RelocRefusal refusal;
    CHECK(BuildTrampoline(kOriginal, window, tr, refusal));
    for (unsigned seed = 0; seed < 4; ++seed) {
      Cpu c = RandomCpu(300 + seed);
      CheckWindowEquivalent("three-word window", window, c);
    }
  }
}

}  // namespace

int main(int argc, char** argv) { return mini_test::RunAll(argc, argv); }
