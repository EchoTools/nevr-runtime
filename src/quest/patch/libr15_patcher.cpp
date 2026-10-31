#include "quest/patch/libr15_patcher.h"

#include <elf.h>

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <ostream>
#include <set>
#include <sstream>

#include "quest/data/sha256.h"
#include "quest/patch/stub_codec.h"
#include "quest/sentinel/arm64_reloc.h"

static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__, "the patcher reads and writes little-endian words with memcpy");

namespace nevr_quest::patch {
namespace {

using Json = nlohmann::json;

std::string Hex(std::uint64_t value) {
  char buf[24];
  std::snprintf(buf, sizeof buf, "0x%llx", static_cast<unsigned long long>(value));
  return buf;
}

std::string Hex8(std::uint32_t value) {
  char buf[16];
  std::snprintf(buf, sizeof buf, "0x%08x", value);
  return buf;
}

std::string BytesHex(const WordBytes& bytes) {
  char buf[16];
  std::snprintf(buf, sizeof buf, "%02x%02x%02x%02x", bytes[0], bytes[1], bytes[2], bytes[3]);
  return buf;
}

Refusal Refuse(const char* code, std::string id, std::string detail) { return Refusal{code, std::move(id), std::move(detail)}; }

// ---- the ELF, read from bytes -------------------------------------------------------------------------------

bool InFile(std::uint64_t offset, std::uint64_t length, std::size_t size) {
  return offset <= size && length <= size - offset;
}

std::uint32_t Read32(const std::vector<std::uint8_t>& d, std::uint64_t off) {
  std::uint32_t v;
  std::memcpy(&v, d.data() + off, sizeof v);
  return v;
}
void Write32(std::vector<std::uint8_t>& d, std::uint64_t off, std::uint32_t v) { std::memcpy(d.data() + off, &v, sizeof v); }
void Write64(std::vector<std::uint8_t>& d, std::uint64_t off, std::uint64_t v) { std::memcpy(d.data() + off, &v, sizeof v); }

struct Elf {
  Elf64_Ehdr eh{};
  std::vector<Elf64_Phdr> ph;
  std::vector<Elf64_Shdr> sh;
  std::string shstr;  // the section-name string table, when there is one

  const Elf64_Phdr* Load(std::size_t i) const { return &ph[i]; }
  std::string SectionName(const Elf64_Shdr& s) const {
    if (s.sh_name >= shstr.size()) return "";
    return std::string(shstr.c_str() + s.sh_name);
  }
};

// Parses and checks the structure the patcher relies on (ELF64 little-endian AArch64 DYN, program and section tables
// inside the file, PT_LOADs sorted by vaddr and not overlapping, p_filesz <= p_memsz, file ranges inside the file).
bool ParseElf(const std::vector<std::uint8_t>& d, Elf* elf, std::string* why) {
  if (d.size() < sizeof(Elf64_Ehdr)) { *why = "shorter than an ELF header"; return false; }
  std::memcpy(&elf->eh, d.data(), sizeof(Elf64_Ehdr));
  const Elf64_Ehdr& eh = elf->eh;
  if (std::memcmp(eh.e_ident, ELFMAG, SELFMAG) != 0 || eh.e_ident[EI_CLASS] != ELFCLASS64 ||
      eh.e_ident[EI_DATA] != ELFDATA2LSB) {
    *why = "not a little-endian ELF64 file";
    return false;
  }
  if (eh.e_machine != EM_AARCH64 || eh.e_type != ET_DYN) { *why = "not an AArch64 shared object"; return false; }
  if (eh.e_phentsize != sizeof(Elf64_Phdr) || eh.e_shentsize != sizeof(Elf64_Shdr)) { *why = "unexpected header entry sizes"; return false; }
  if (!InFile(eh.e_phoff, static_cast<std::uint64_t>(eh.e_phnum) * sizeof(Elf64_Phdr), d.size())) { *why = "program header table outside the file"; return false; }
  if (!InFile(eh.e_shoff, static_cast<std::uint64_t>(eh.e_shnum) * sizeof(Elf64_Shdr), d.size())) { *why = "section header table outside the file"; return false; }
  elf->ph.resize(eh.e_phnum);
  for (std::size_t i = 0; i < elf->ph.size(); ++i) {
    std::memcpy(&elf->ph[i], d.data() + eh.e_phoff + i * sizeof(Elf64_Phdr), sizeof(Elf64_Phdr));
  }
  elf->sh.resize(eh.e_shnum);
  for (std::size_t i = 0; i < elf->sh.size(); ++i) {
    std::memcpy(&elf->sh[i], d.data() + eh.e_shoff + i * sizeof(Elf64_Shdr), sizeof(Elf64_Shdr));
  }
  bool haveLoad = false;
  std::uint64_t previousEnd = 0;
  std::uint64_t previousStart = 0;
  for (const Elf64_Phdr& p : elf->ph) {
    if (p.p_type != PT_LOAD) continue;
    if (p.p_filesz > p.p_memsz) { *why = "a PT_LOAD with p_filesz > p_memsz"; return false; }
    if (!InFile(p.p_offset, p.p_filesz, d.size())) { *why = "a PT_LOAD's file range is outside the file"; return false; }
    if (p.p_vaddr + p.p_memsz < p.p_vaddr) { *why = "a PT_LOAD wraps the address space"; return false; }
    if (haveLoad && (p.p_vaddr < previousStart || p.p_vaddr < previousEnd)) { *why = "PT_LOADs are not sorted by vaddr or overlap"; return false; }
    haveLoad = true;
    previousStart = p.p_vaddr;
    previousEnd = p.p_vaddr + p.p_memsz;
  }
  if (!haveLoad) { *why = "no PT_LOAD"; return false; }
  for (const Elf64_Shdr& s : elf->sh) {
    if (s.sh_type != SHT_NOBITS && s.sh_type != SHT_NULL && !InFile(s.sh_offset, s.sh_size, d.size())) {
      *why = "a section's file range is outside the file";
      return false;
    }
  }
  if (eh.e_shstrndx != SHN_UNDEF && eh.e_shstrndx < elf->sh.size()) {
    const Elf64_Shdr& str = elf->sh[eh.e_shstrndx];
    if (str.sh_type == SHT_STRTAB && InFile(str.sh_offset, str.sh_size, d.size())) {
      elf->shstr.assign(reinterpret_cast<const char*>(d.data() + str.sh_offset), str.sh_size);
      elf->shstr.push_back('\0');
    }
  }
  return true;
}

struct BuildIdNote {
  std::uint64_t descOffset = 0;
};

// The one GNU build-ID note (NT_GNU_BUILD_ID, name "GNU", 20 bytes) in the PT_NOTE segments.
bool FindBuildIdNote(const std::vector<std::uint8_t>& d, const Elf& elf, BuildIdNote* note, BuildIdBytes* id, std::string* why) {
  int found = 0;
  for (const Elf64_Phdr& p : elf.ph) {
    if (p.p_type != PT_NOTE || !InFile(p.p_offset, p.p_filesz, d.size())) continue;
    std::uint64_t at = p.p_offset;
    const std::uint64_t end = p.p_offset + p.p_filesz;
    while (at + sizeof(Elf64_Nhdr) <= end) {
      Elf64_Nhdr nh;
      std::memcpy(&nh, d.data() + at, sizeof nh);
      const std::uint64_t nameAt = at + sizeof nh;
      const std::uint64_t descAt = nameAt + ((nh.n_namesz + 3) & ~3u);
      const std::uint64_t next = descAt + ((nh.n_descsz + 3) & ~3u);
      if (next > end || next < at) break;
      if (nh.n_type == NT_GNU_BUILD_ID && nh.n_namesz == 4 && std::memcmp(d.data() + nameAt, "GNU", 4) == 0) {
        if (nh.n_descsz != kBuildIdBytes) { *why = "the GNU build-ID note is not 20 bytes"; return false; }
        note->descOffset = descAt;
        std::memcpy(id->data(), d.data() + descAt, kBuildIdBytes);
        ++found;
      }
      at = next;
    }
  }
  if (found != 1) { *why = found == 0 ? "no GNU build-ID note" : "more than one GNU build-ID note"; return false; }
  return true;
}

// The file offset of [va, va + length) when it lies wholly inside the file-backed part of an executable PT_LOAD.
bool TextOffset(const Elf& elf, std::uint64_t va, std::uint64_t length, std::uint64_t* offset) {
  for (const Elf64_Phdr& p : elf.ph) {
    if (p.p_type != PT_LOAD || (p.p_flags & PF_X) == 0) continue;
    if (va >= p.p_vaddr && length <= p.p_filesz && va - p.p_vaddr <= p.p_filesz - length) {
      *offset = p.p_offset + (va - p.p_vaddr);
      return true;
    }
  }
  return false;
}

// The six linker-defined end symbols of ADR section 4, as (name, value) from .dynsym.
const char* const kEndSymbols[] = {"__end__", "__bss_end__", "_bss_end__", "_end", "_edata", "__bss_start"};

Json EndSymbols(const std::vector<std::uint8_t>& d, const Elf& elf) {
  Json out = Json::array();
  for (const Elf64_Shdr& s : elf.sh) {
    if (s.sh_type != SHT_DYNSYM || s.sh_link >= elf.sh.size()) continue;
    const Elf64_Shdr& str = elf.sh[s.sh_link];
    if (!InFile(str.sh_offset, str.sh_size, d.size())) continue;
    std::set<std::string> want(std::begin(kEndSymbols), std::end(kEndSymbols));
    for (std::uint64_t at = 0; at + sizeof(Elf64_Sym) <= s.sh_size; at += sizeof(Elf64_Sym)) {
      Elf64_Sym sym;
      std::memcpy(&sym, d.data() + s.sh_offset + at, sizeof sym);
      if (sym.st_name >= str.sh_size) continue;
      const char* base = reinterpret_cast<const char*>(d.data() + str.sh_offset);
      const std::size_t room = str.sh_size - sym.st_name;
      const std::string name(base + sym.st_name, strnlen(base + sym.st_name, room));
      if (want.count(name) != 0) out.push_back({{"name", name}, {"value", Hex(sym.st_value)}});
    }
    std::sort(out.begin(), out.end(), [](const Json& a, const Json& b) { return a["name"].get<std::string>() < b["name"].get<std::string>(); });
  }
  return out;
}

// ---- the claims: every byte range the patch writes, to find overlaps and to account for every change ------------

struct Claim {
  std::uint64_t begin;  // vaddr
  std::uint64_t end;
  std::string id;
};

bool Overlap(const std::vector<Claim>& claims, const Claim& candidate, const Claim** hit) {
  for (const Claim& c : claims) {
    if (candidate.begin < c.end && c.begin < candidate.end) {
      *hit = &c;
      return true;
    }
  }
  return false;
}

struct Change {
  std::uint64_t offset;
  std::uint64_t length;
  std::string what;
};

// A byte range of the file that is not a row's to write: the file's own structure, and every byte the tool itself writes
// (the p_memsz of the grown segment, the sh_size of the section that ends it, the build ID). A row's word that reaches one
// would be written and then written over, or would change what the file's tables say, while the report listed it as a
// word of the row.
struct Region {
  std::uint64_t begin;  // file offset
  std::uint64_t end;
  std::string name;
};

const Region* ProtectedRegionAt(const std::vector<Region>& regions, std::uint64_t offset, std::uint64_t length) {
  for (const Region& r : regions) {
    if (offset < r.end && r.begin < offset + length) return &r;
  }
  return nullptr;
}

// ---- the input's own references to the slot area (#1737, row C8) ---------------------------------------------------
//
// An input libr15 that another patch already touched can keep state words of its own in the page slack after the image
// end, the bytes the slot area takes (the reference build's two, at 0x37d2540 and 0x37d2560, read and written by ADRP+ADD+LDR/STR
// pairs). The scan reads every executable PT_LOAD as A64 words. It follows each ADRP whose page lies at most kReferenceReach
// below the zone through the next kReferenceWindowWords words, tracking the registers that hold the page or an address derived
// from it (ADD immediate, MOV, and the base register of load/store, LDUR/STUR and LDP/STP), and reports the first address that
// lies within kReferenceAccessBytes of the zone. ADR is checked on its own. It looks at code only: a pointer held in data is not
// found (none exists in the three accepted inputs).
//
// Limits: the scan does NOT detect these forms. Each is a way the input can still reach the slot area after this check passes.
//   - the second register of an LDP/STP of Q registers: the scan checks the base and the first 16 bytes at the pair's address,
//     so a pair whose second register lands at address + 16 inside the zone is not reported;
//   - register-offset loads and stores (LDR Xt, [Xn, Xm {, LSL}]), whose address depends on Xm, which the scan does not evaluate;
//   - index arithmetic after a register-form ADD (ADD Xd, Xn, Xm), which the scan does not evaluate: Xd keeps the value it had
//     before the ADD, so a later access through Xd is judged on that stale value;
//   - absolute addresses built by MOVZ/MOVK (or MOVN), which the scan does not decode;
//   - any reference that sits more than kReferenceWindowWords (32) words after the ADRP that starts it, or in code the scan does not
//     reach through a branch (B, BR and RET end the window; BL does not).
// The known reference words stay clear of the slot area because of the gap (kSlotAreaGap), not because of this scan: the scan is
// a guard for other inputs, and the pinned test (ReferenceLibr15sArePatchedWithTheirStateWordsClearOfTheSlots) checks the
// reference build's own words with the gap in place.
struct SlotAreaReference {
  std::uint64_t site = 0;
  std::uint64_t target = 0;
  const char* form = "";
};

constexpr std::uint64_t kReferenceWindowWords = 32;
constexpr std::uint64_t kReferenceAccessBytes = 16;   // the widest single access: LDP of two X registers, or a Q register
constexpr std::uint64_t kReferenceReach = 0x1000000;  // ADD #imm12, LSL #12 reaches 0xFFF000 past its ADRP's page

bool FindSlotAreaReference(const std::vector<std::uint8_t>& d, const Elf& elf, std::uint64_t zoneBegin, std::uint64_t zoneEnd,
                           SlotAreaReference* hit) {
  auto touches = [&](std::uint64_t target) { return target < zoneEnd && target + kReferenceAccessBytes > zoneBegin; };
  auto report = [&](std::uint64_t site, std::uint64_t target, const char* form) {
    hit->site = site;
    hit->target = target;
    hit->form = form;
    return true;
  };
  for (const Elf64_Phdr& p : elf.ph) {
    if (p.p_type != PT_LOAD || (p.p_flags & PF_X) == 0) continue;
    const std::uint64_t count = p.p_filesz / 4;  // ParseElf checked that the file range is inside the file
    auto word = [&](std::uint64_t i) { return Read32(d, p.p_offset + 4 * i); };
    for (std::uint64_t i = 0; i < count; ++i) {
      const std::uint64_t pc = p.p_vaddr + 4 * i;
      const std::uint32_t w = word(i);
      if ((w & 0x9F000000u) == 0x10000000u) {  // ADR
        const std::uint32_t imm = (((w >> 5) & 0x7FFFFu) << 2) | ((w >> 29) & 3u);
        const std::int64_t offset = static_cast<std::int64_t>(imm ^ 0x100000u) - 0x100000;
        const std::uint64_t target = pc + static_cast<std::uint64_t>(offset);
        if (touches(target)) return report(pc, target, "adr");
        continue;
      }
      unsigned rd = 0;
      std::uint64_t page = 0;
      if (!DecodeAdrp(w, pc, &rd, &page) || rd == 31) continue;
      if (page >= zoneEnd || page + kReferenceReach <= zoneBegin) continue;
      std::uint64_t val[32] = {};
      bool live[32] = {};
      val[rd] = page;
      live[rd] = true;
      for (std::uint64_t j = 1; j <= kReferenceWindowWords && i + j < count; ++j) {
        const std::uint64_t pc2 = pc + 4 * j;
        const std::uint32_t w2 = word(i + j);
        const unsigned rt = w2 & 0x1Fu;
        const unsigned rn = (w2 >> 5) & 0x1Fu;
        const bool based = rn != 31 && live[rn];
        // B, BR and RET end the straight-line window (BL does not).
        if ((w2 & 0xFC000000u) == 0x14000000u || (w2 & 0xFFFFFC1Fu) == 0xD61F0000u || (w2 & 0xFFFFFC1Fu) == 0xD65F0000u) break;
        if ((w2 & 0xFF000000u) == 0x91000000u) {  // ADD Xd, Xn, #imm12 {, LSL #12}
          if (based) {
            const std::uint64_t v = val[rn] + (static_cast<std::uint64_t>((w2 >> 10) & 0xFFFu) << (((w2 >> 22) & 1u) != 0 ? 12 : 0));
            if (touches(v)) return report(pc2, v, "add");
            if (rt != 31) {
              val[rt] = v;
              live[rt] = true;
            }
          } else if (rt != 31) {
            live[rt] = false;
          }
          continue;
        }
        if ((w2 & 0x3B000000u) == 0x39000000u) {  // LDR/STR, unsigned immediate, any size, integer or vector
          const unsigned size = (w2 >> 30) & 3u;
          const bool vec = ((w2 >> 26) & 1u) != 0;
          const unsigned opc = (w2 >> 22) & 3u;
          const unsigned scale = (vec && size == 0 && (opc & 2u) != 0) ? 4u : size;
          if (based) {
            const std::uint64_t t = val[rn] + (static_cast<std::uint64_t>((w2 >> 10) & 0xFFFu) << scale);
            if (touches(t)) return report(pc2, t, "load/store");
          }
          if (!vec && opc != 0 && rt != 31) live[rt] = false;  // a load into Rt
          continue;
        }
        if ((w2 & 0x3B200C00u) == 0x38000000u) {  // LDUR/STUR
          const bool vec = ((w2 >> 26) & 1u) != 0;
          const unsigned opc = (w2 >> 22) & 3u;
          if (based) {
            const std::int64_t offset = static_cast<std::int64_t>(((w2 >> 12) & 0x1FFu) ^ 0x100u) - 0x100;
            const std::uint64_t t = val[rn] + static_cast<std::uint64_t>(offset);
            if (touches(t)) return report(pc2, t, "load/store");
          }
          if (!vec && opc != 0 && rt != 31) live[rt] = false;
          continue;
        }
        if ((w2 & 0x3A000000u) == 0x28000000u) {  // LDP/STP/LDNP/STNP, any addressing mode
          const unsigned opc = (w2 >> 30) & 3u;
          const bool vec = ((w2 >> 26) & 1u) != 0;
          const unsigned scale = vec ? 2u + opc : (opc == 2u ? 3u : 2u);
          if (based) {
            const std::int64_t offset = static_cast<std::int64_t>(((w2 >> 15) & 0x7Fu) ^ 0x40u) - 0x40;
            const std::uint64_t t = val[rn] + static_cast<std::uint64_t>(offset * (std::int64_t{1} << scale));
            if (touches(val[rn]) || touches(t)) return report(pc2, t, "pair");
          }
          continue;
        }
        if ((w2 & 0xFFE0FFE0u) == 0xAA0003E0u) {  // MOV Xd, Xm (ORR Xd, XZR, Xm)
          const unsigned rm = (w2 >> 16) & 0x1Fu;
          if (rt != 31) {
            if (rm != 31 && live[rm]) {
              val[rt] = val[rm];
              live[rt] = true;
            } else {
              live[rt] = false;
            }
          }
          continue;
        }
        if ((w2 & 0x1F000000u) == 0x10000000u) {  // another ADR or ADRP overwrites Rd
          if (rt != 31) live[rt] = false;
          continue;
        }
      }
    }
  }
  return false;
}

}  // namespace

namespace {

// Everything PatchLibr15 needs of one row after its words were read from the file.
struct Planned {
  std::string id;
  std::uint64_t vaddr;
  std::uint64_t fileOffset;
  std::vector<std::uint32_t> stock;
  std::vector<std::uint32_t> patched;
  std::string kind;
  std::uint8_t slot;  // 0xFF for none
  std::string what;
};

PatchResult Failed(Refusal r, const std::string& stockSha) {
  PatchResult out;
  out.refusal = std::move(r);
  out.summary = out.refusal.ToJson();
  if (!stockSha.empty()) out.summary["stock_sha256"] = stockSha;
  return out;
}

}  // namespace

PatchResult PatchLibr15(const std::vector<std::uint8_t>& stock, const PatchSpec& spec, const std::vector<WordPatchRow>& extra,
                        const std::string& extraRowsSha256) {
  std::string stockSha;  // computed only once the cheap checks pass
  Elf elf;
  std::string why;
  if (!ParseElf(stock, &elf, &why)) return Failed(Refuse("elf_invalid", "", why), stockSha);
  BuildIdNote note;
  BuildIdBytes liveId{};
  if (!FindBuildIdNote(stock, elf, &note, &liveId, &why)) return Failed(Refuse("elf_invalid", "", why), stockSha);

  // The target segment: the writable PT_LOAD with the greatest end. Its end is the image end.
  const Elf64_Phdr* target = nullptr;
  std::size_t targetIndex = 0;
  for (std::size_t i = 0; i < elf.ph.size(); ++i) {
    const Elf64_Phdr& p = elf.ph[i];
    if (p.p_type != PT_LOAD || (p.p_flags & PF_W) == 0) continue;
    if (target == nullptr || p.p_vaddr + p.p_memsz >= target->p_vaddr + target->p_memsz) {
      target = &p;
      targetIndex = i;
    }
  }
  if (target == nullptr) return Failed(Refuse("layout_not_writable", "", "no writable PT_LOAD"), stockSha);
  const std::uint64_t imageEnd = target->p_vaddr + target->p_memsz;

  // Identity first: an input that is already our output must say so, not read as "another build".
  const std::uint64_t pinnedEnd = spec.expectedImageEnd.value_or(imageEnd);
  const std::uint64_t pinnedSlotArea = SlotAreaFor(pinnedEnd, spec.slotAreaGap);
  if (liveId != spec.stockBuildId) {
    const BuildIdBytes derived = DeriveBuildId(spec.stockBuildId, CanonicalTable(spec.rows, spec.rowCount, extra, pinnedSlotArea));
    if (liveId == derived) {
      return Failed(Refuse("already_patched", "", "the input's build ID is this table's patched ID " + BuildIdHex(derived)), stockSha);
    }
    return Failed(Refuse("stock_build_id_mismatch", "", "build ID " + BuildIdHex(liveId) + ", expected " + BuildIdHex(spec.stockBuildId)), stockSha);
  }
  stockSha = data::Sha256Hex(stock.data(), stock.size());
  if (!spec.stockSha256.empty() && stockSha != spec.stockSha256 &&
      std::find(spec.alsoSha256.begin(), spec.alsoSha256.end(), stockSha) == spec.alsoSha256.end()) {
    return Failed(Refuse("stock_sha256_mismatch", "", "sha-256 " + stockSha + ", expected " + spec.stockSha256), stockSha);
  }

  // Layout: the grown segment must not reach another one, nor GNU_RELRO.
  if (spec.expectedImageEnd.has_value() && imageEnd != *spec.expectedImageEnd) {
    return Failed(Refuse("layout_image_end", "", "the writable PT_LOAD ends at " + Hex(imageEnd) + ", expected " + Hex(*spec.expectedImageEnd)), stockSha);
  }
  const std::uint64_t slotArea = SlotAreaFor(imageEnd, spec.slotAreaGap);
  const std::uint64_t newEnd = slotArea + kSlotBytes * spec.rowCount;
  const std::uint64_t newEndPage = RoundUp(newEnd, 4096);
  for (std::size_t i = 0; i < elf.ph.size(); ++i) {
    const Elf64_Phdr& p = elf.ph[i];
    if (p.p_type == PT_LOAD && i != targetIndex && p.p_vaddr < newEndPage && imageEnd < p.p_vaddr + p.p_memsz) {
      return Failed(Refuse("layout_overlap", "", "growing the writable PT_LOAD to " + Hex(newEnd) + " reaches the PT_LOAD at " + Hex(p.p_vaddr)), stockSha);
    }
    if (p.p_type == PT_GNU_RELRO && p.p_vaddr + p.p_memsz > slotArea) {
      return Failed(Refuse("layout_relro", "", "GNU_RELRO ends at " + Hex(p.p_vaddr + p.p_memsz) + ", past the slot area at " + Hex(slotArea)), stockSha);
    }
  }
  // The section that ends the segment: a NOBITS section ending at the image end (.bss).
  std::size_t bssIndex = elf.sh.size();
  for (std::size_t i = 0; i < elf.sh.size(); ++i) {
    const Elf64_Shdr& s = elf.sh[i];
    if (s.sh_type == SHT_NOBITS && (s.sh_flags & SHF_ALLOC) != 0 && s.sh_addr + s.sh_size == imageEnd) bssIndex = i;
  }
  if (bssIndex == elf.sh.size()) {
    return Failed(Refuse("layout_bss_section", "", "no NOBITS section ends at the image end " + Hex(imageEnd)), stockSha);
  }

  // The input's own code must not use the bytes the patch is about to claim: its state words would be the slots (#1737, row C8).
  SlotAreaReference reference;
  if (FindSlotAreaReference(stock, elf, slotArea, newEnd, &reference)) {
    PatchResult refused = Failed(Refuse("slot_area_referenced", "",
                                        "the input's code at " + Hex(reference.site) + " (" + reference.form + ") reaches " + Hex(reference.target) +
                                            ", inside the slot area " + Hex(slotArea) + ".." + Hex(newEnd) +
                                            ": the input keeps its own state there, so a slot would be one of its words"),
                                 stockSha);
    refused.summary["site"] = Hex(reference.site);
    refused.summary["target"] = Hex(reference.target);
    refused.summary["slot_area"] = Hex(slotArea);
    refused.summary["slot_area_end"] = Hex(newEnd);
    return refused;
  }

  // The bytes no row may reach, the ones the tool writes itself first so that they are named for what they are.
  const std::vector<Region> regions = {
      {elf.eh.e_phoff + targetIndex * sizeof(Elf64_Phdr) + offsetof(Elf64_Phdr, p_memsz),
       elf.eh.e_phoff + targetIndex * sizeof(Elf64_Phdr) + offsetof(Elf64_Phdr, p_memsz) + 8, "the p_memsz of the grown segment"},
      {elf.eh.e_shoff + bssIndex * sizeof(Elf64_Shdr) + offsetof(Elf64_Shdr, sh_size),
       elf.eh.e_shoff + bssIndex * sizeof(Elf64_Shdr) + offsetof(Elf64_Shdr, sh_size) + 8,
       "the sh_size of the " + elf.SectionName(elf.sh[bssIndex]) + " section"},
      {0, sizeof(Elf64_Ehdr), "the ELF header"},
      {elf.eh.e_phoff, elf.eh.e_phoff + static_cast<std::uint64_t>(elf.eh.e_phnum) * sizeof(Elf64_Phdr), "the program header table"},
      {elf.eh.e_shoff, elf.eh.e_shoff + static_cast<std::uint64_t>(elf.eh.e_shnum) * sizeof(Elf64_Shdr), "the section header table"},
      // The note's header (12 bytes), its name ("GNU", padded to 4) and the 20 bytes of the ID.
      {note.descOffset - sizeof(Elf64_Nhdr) - 4, note.descOffset + kBuildIdBytes, "the build-ID note"},
  };
  auto protectedRefusal = [&](const char* code, const std::string& id, const std::string& what, std::uint64_t fileOffset, std::uint64_t length,
                              Refusal* r) {
    const Region* hit = ProtectedRegionAt(regions, fileOffset, length);
    if (hit == nullptr) return false;
    *r = Refuse(code, id, what + " (file offset " + Hex(fileOffset) + ") lies in " + hit->name +
                              ": the file's structure and the bytes the tool writes itself are not a row's to patch");
    return true;
  };

  // ---- the plan: every word the patch writes ----
  std::vector<Planned> planned;
  std::vector<Claim> claims;
  const Json endSymbolsStock = EndSymbols(stock, elf);
  auto claim = [&](const std::string& id, std::uint64_t vaddr, std::uint64_t length, Refusal* r) {
    Claim c{vaddr, vaddr + length, id};
    const Claim* hit = nullptr;
    if (Overlap(claims, c, &hit)) {
      *r = Refuse("sites_overlap", id, "bytes " + Hex(vaddr) + ".." + Hex(vaddr + length) + " are also written by " + hit->id);
      return false;
    }
    claims.push_back(c);
    return true;
  };
  auto readWords = [&](std::uint64_t fileOffset, std::size_t count) {
    std::vector<std::uint32_t> w(count);
    for (std::size_t i = 0; i < count; ++i) w[i] = Read32(stock, fileOffset + 4 * i);
    return w;
  };

  for (std::size_t r = 0; r < spec.rowCount; ++r) {
    const Row& row = spec.rows[r];
    const std::string id = row.id;
    std::vector<std::uint64_t> siteOffsets;
    for (std::size_t s = 0; s < row.siteCount; ++s) {
      const Site& site = row.sites[s];
      Refusal rf;
      if (site.revaultVa < kRevaultBias) return Failed(Refuse("site_outside_text", id, "the ReVault address " + Hex(site.revaultVa) + " is below the bias"), stockSha);
      std::uint64_t off = 0;
      if (!TextOffset(elf, site.Vaddr(), 4ull * site.wordCount, &off)) {
        return Failed(Refuse("site_outside_text", id, "site " + std::to_string(s) + " at " + Hex(site.Vaddr()) + " is not in an executable segment"), stockSha);
      }
      if (protectedRefusal("protected_region", id, "site " + std::to_string(s) + " at " + Hex(site.Vaddr()), off, 4ull * site.wordCount, &rf)) {
        return Failed(rf, stockSha);
      }
      const std::vector<std::uint32_t> live = readWords(off, site.wordCount);
      for (std::size_t w = 0; w < site.wordCount; ++w) {
        if (live[w] != site.words[w]) {
          return Failed(Refuse("row_words_mismatch", id, "site " + std::to_string(s) + " word " + std::to_string(w) + " at " + Hex(site.Vaddr() + 4 * w) +
                                                           " is " + Hex8(live[w]) + ", the table has " + Hex8(site.words[w])), stockSha);
        }
      }
      if (!claim(id, site.Vaddr(), 4ull * site.wordCount, &rf)) return Failed(rf, stockSha);
      Planned p;
      p.id = id;
      p.vaddr = site.Vaddr();
      p.fileOffset = off;
      p.stock = live;
      p.slot = row.slot;
      siteOffsets.push_back(planned.size());
      planned.push_back(std::move(p));
    }
    const std::uint64_t slotVaddr = row.SlotVaddr(slotArea);
    if (row.kind == RowKind::kInlineHook) {
      Planned& p = planned[siteOffsets[0]];
      p.kind = "inline_hook";
      Stub stub{};
      if (!EncodeSlotStub(p.vaddr, slotVaddr, &stub)) {
        return Failed(Refuse("stub_out_of_range", id, "the slot " + Hex(slotVaddr) + " is out of ADRP range of " + Hex(p.vaddr)), stockSha);
      }
      // The trampoline the sentinel builds from these words must exist: refuse here, not on a headset.
      const std::uint32_t window[3] = {p.stock[0], p.stock[1], p.stock[2]};
      sentinel::Trampoline tramp;
      sentinel::RelocRefusal rr{};
      if (!sentinel::BuildTrampoline(p.vaddr, window, tramp, rr)) {
        return Failed(Refuse("row_not_relocatable", id, std::string("word ") + std::to_string(rr.index) + ": " + rr.reason), stockSha);
      }
      p.patched.assign(stub.words, stub.words + kStubWords);
      p.what = "stub";
    } else if (row.kind == RowKind::kCreditsPair) {
      std::uint64_t blob = 0;
      for (std::size_t pair = 0; pair < row.siteCount / 2; ++pair) {
        Planned& adrp = planned[siteOffsets[2 * pair]];
        Planned& add = planned[siteOffsets[2 * pair + 1]];
        unsigned rd = 0;
        std::uint64_t page = 0;
        if (!DecodeAdrp(adrp.stock[0], adrp.vaddr, &rd, &page)) {
          return Failed(Refuse("credits_shape", id, "pair " + std::to_string(pair) + ": the first word is not an ADRP"), stockSha);
        }
        const std::uint32_t a = add.stock[0];
        if ((a & 0xFFC00000u) != 0x91000000u || (a & 0x1F) != rd || ((a >> 5) & 0x1F) != rd) {
          return Failed(Refuse("credits_shape", id, "pair " + std::to_string(pair) + ": the second word is not ADD xN, xN, #imm12 on the ADRP's register"), stockSha);
        }
        const std::uint64_t target = page + ((a >> 10) & 0xFFF);
        if (pair == 0) blob = target;
        if (target != blob) {
          return Failed(Refuse("credits_blob_mismatch", id, "pair " + std::to_string(pair) + " loads " + Hex(target) + ", pair 0 loads " + Hex(blob)), stockSha);
        }
        SlotLoadPair load{};
        if (!EncodeSlotLoadPair(adrp.vaddr, slotVaddr, rd, &load)) {
          return Failed(Refuse("stub_out_of_range", id, "the credits slot " + Hex(slotVaddr) + " is out of reach of pair " + std::to_string(pair)), stockSha);
        }
        adrp.kind = add.kind = "credits_pair";
        adrp.patched = {load.adrp};
        add.patched = {load.ldr};
        adrp.what = "credits_adrp";
        add.what = "credits_ldr";
      }
    } else {
      return Failed(Refuse("row_kind", id, "a public row of this kind is not supported"), stockSha);
    }
  }

  // Word patches (extra rows), in revault_va order so the report does not depend on the file's row order.
  std::vector<const WordPatchRow*> orderedExtra;
  for (const WordPatchRow& r : extra) orderedExtra.push_back(&r);
  std::sort(orderedExtra.begin(), orderedExtra.end(), [](const WordPatchRow* a, const WordPatchRow* b) {
    return a->revaultVa != b->revaultVa ? a->revaultVa < b->revaultVa : a->id < b->id;
  });
  struct WordPlan {
    const WordPatchRow* row;
    const WordEdit* edit;
    std::uint64_t vaddr;
    std::uint64_t fileOffset;
  };
  std::vector<WordPlan> wordPlans;
  for (const WordPatchRow* row : orderedExtra) {
    if (row->revaultVa < kRevaultBias) return Failed(Refuse("word_outside_text", row->id, "the ReVault address " + Hex(row->revaultVa) + " is below the bias"), stockSha);
    for (const WordEdit& e : row->edits) {
      if ((e.offset & 3) != 0) {
        return Failed(Refuse("word_offset", row->id, "offset " + std::to_string(e.offset) + " is not a multiple of 4"), stockSha);
      }
      const std::uint64_t va = row->revaultVa - kRevaultBias + e.offset;
      std::uint64_t off = 0;
      if (!TextOffset(elf, va, 4, &off)) {
        return Failed(Refuse("word_outside_text", row->id, "offset " + std::to_string(e.offset) + " at " + Hex(va) + " is not in an executable segment"), stockSha);
      }
      Refusal reach;
      if (protectedRefusal("protected_region", row->id, "offset " + std::to_string(e.offset) + " at " + Hex(va), off, 4, &reach)) {
        return Failed(reach, stockSha);
      }
      WordBytes live{};
      std::memcpy(live.data(), stock.data() + off, 4);
      if (live != e.stock) {
        return Failed(Refuse("word_stock_mismatch", row->id, "offset " + std::to_string(e.offset) + " at " + Hex(va) + " holds " + BytesHex(live) +
                                                               ", the row has stock_bytes " + BytesHex(e.stock)), stockSha);
      }
      Refusal rf;
      if (!claim(row->id, va, 4, &rf)) {
        rf.code = "word_overlap";
        rf.detail = "offset " + std::to_string(e.offset) + ": " + rf.detail;
        return Failed(rf, stockSha);
      }
      wordPlans.push_back({row, &e, va, off});
    }
  }

  // ---- write ----
  std::vector<std::uint8_t> out = stock;
  std::vector<Change> changes;
  std::vector<Json> eventList;
  for (const Planned& p : planned) {
    for (std::size_t i = 0; i < p.patched.size(); ++i) Write32(out, p.fileOffset + 4 * i, p.patched[i]);
    changes.push_back({p.fileOffset, 4ull * p.patched.size(), p.id + ":" + p.what});
    Json stockWords = Json::array();
    Json stubWords = Json::array();
    for (const std::uint32_t w : p.stock) stockWords.push_back(Hex8(w));
    for (const std::uint32_t w : p.patched) stubWords.push_back(Hex8(w));
    eventList.push_back({{"event", "libr15_patch_site"}, {"id", p.id}, {"kind", p.kind}, {"revault_va", Hex(p.vaddr + kRevaultBias)},
                         {"vaddr", Hex(p.vaddr)}, {"file_offset", Hex(p.fileOffset)}, {"stock", stockWords}, {"stub", stubWords},
                         {"slot", p.slot}, {"slot_vaddr", Hex(slotArea + kSlotBytes * p.slot)}});
  }
  for (const WordPlan& wp : wordPlans) {
    std::memcpy(out.data() + wp.fileOffset, wp.edit->word.data(), 4);
    changes.push_back({wp.fileOffset, 4, wp.row->id + ":word"});
    eventList.push_back({{"event", "libr15_patch_word"}, {"id", wp.row->id}, {"offset", wp.edit->offset},
                         {"revault_va", Hex(wp.vaddr + kRevaultBias)}, {"vaddr", Hex(wp.vaddr)}, {"file_offset", Hex(wp.fileOffset)},
                         {"stock_bytes", BytesHex(wp.edit->stock)}, {"stock_word", Hex8(WordValue(wp.edit->stock))},
                         {"new_bytes", BytesHex(wp.edit->word)}, {"new_word", Hex8(WordValue(wp.edit->word))}});
  }
  // The segment and section sizes.
  const std::uint64_t memszOffset = elf.eh.e_phoff + targetIndex * sizeof(Elf64_Phdr) + offsetof(Elf64_Phdr, p_memsz);
  Write64(out, memszOffset, target->p_memsz + (newEnd - imageEnd));
  changes.push_back({memszOffset, 8, "p_memsz"});
  const std::uint64_t bssSizeOffset = elf.eh.e_shoff + bssIndex * sizeof(Elf64_Shdr) + offsetof(Elf64_Shdr, sh_size);
  Write64(out, bssSizeOffset, elf.sh[bssIndex].sh_size + (newEnd - imageEnd));
  changes.push_back({bssSizeOffset, 8, "bss_sh_size"});
  // The build ID.
  const BuildIdBytes patchedId = DeriveBuildId(spec.stockBuildId, CanonicalTable(spec.rows, spec.rowCount, extra, slotArea));
  std::memcpy(out.data() + note.descOffset, patchedId.data(), kBuildIdBytes);
  changes.push_back({note.descOffset, kBuildIdBytes, "build_id"});

  // ---- validate the output as an ELF, and account for every differing byte ----
  Elf outElf;
  Refusal bad = Refuse("output_invalid", "", "");
  auto invalid = [&](const std::string& detail) {
    bad.detail = detail;
    return Failed(bad, stockSha);
  };
  if (out.size() != stock.size()) return invalid("the size changed");
  if (!ParseElf(out, &outElf, &why)) return invalid("not an ELF: " + why);
  BuildIdNote outNote;
  BuildIdBytes outId{};
  if (!FindBuildIdNote(out, outElf, &outNote, &outId, &why)) return invalid("build-ID note: " + why);
  if (outNote.descOffset != note.descOffset || outId != patchedId) return invalid("the build-ID note is not the derived ID");
  const Elf64_Phdr& outTarget = outElf.ph[targetIndex];
  if (outTarget.p_vaddr + outTarget.p_memsz != newEnd || outTarget.p_filesz != target->p_filesz || outTarget.p_offset != target->p_offset) {
    return invalid("the writable PT_LOAD is not the stock one grown to the slot area's end");
  }
  if (outElf.sh[bssIndex].sh_addr + outElf.sh[bssIndex].sh_size != newEnd) return invalid("the NOBITS section does not end at the slot area's end");
  if (outElf.eh.e_shoff != elf.eh.e_shoff || outElf.eh.e_phoff != elf.eh.e_phoff || outElf.sh.size() != elf.sh.size()) return invalid("the tables moved");
  for (std::size_t i = 0; i < outElf.sh.size(); ++i) {
    const Elf64_Shdr& a = elf.sh[i];
    const Elf64_Shdr& b = outElf.sh[i];
    if (i != bssIndex && std::memcmp(&a, &b, sizeof a) != 0) return invalid("section " + std::to_string(i) + " changed");
    if ((b.sh_flags & SHF_ALLOC) != 0) {
      bool inside = false;
      for (const Elf64_Phdr& p : outElf.ph) {
        inside = inside || (p.p_type == PT_LOAD && b.sh_addr >= p.p_vaddr && b.sh_addr + b.sh_size <= p.p_vaddr + p.p_memsz);
      }
      if (!inside) return invalid("an allocated section lies outside every PT_LOAD");
    }
  }
  for (std::size_t i = 0; i < outElf.ph.size(); ++i) {
    if (i != targetIndex && std::memcmp(&elf.ph[i], &outElf.ph[i], sizeof(Elf64_Phdr)) != 0) return invalid("program header " + std::to_string(i) + " changed");
  }
  // The slot area is bss, outside GNU_RELRO, and every stub decodes back to its row's slot.
  if (slotArea < outTarget.p_vaddr + outTarget.p_filesz) return invalid("the slot area is not in the bss part");
  for (const Planned& p : planned) {
    if (p.kind == "inline_hook") {
      const std::uint32_t words[3] = {Read32(out, p.fileOffset), Read32(out, p.fileOffset + 4), Read32(out, p.fileOffset + 8)};
      std::uint64_t decoded = 0;
      if (!DecodeSlotStub(words, p.vaddr, &decoded) || decoded != slotArea + kSlotBytes * p.slot) return invalid("the stub of " + p.id + " does not decode to its slot");
    }
  }
  // Every differing byte is inside a declared change; every declared change differs somewhere or is a no-op rewrite.
  std::vector<std::pair<std::uint64_t, std::uint64_t>> ranges;
  for (const Change& c : changes) ranges.emplace_back(c.offset, c.offset + c.length);
  std::sort(ranges.begin(), ranges.end());
  std::size_t at = 0;
  std::uint64_t differing = 0;
  Json diffs = Json::array();
  for (std::uint64_t i = 0; i < out.size(); ++i) {
    if (out[i] == stock[i]) continue;
    while (at < ranges.size() && ranges[at].second <= i) ++at;
    if (at >= ranges.size() || ranges[at].first > i) return invalid("byte at file offset " + Hex(i) + " differs and no change accounts for it");
    ++differing;
  }
  for (const Change& c : changes) diffs.push_back({{"offset", Hex(c.offset)}, {"length", c.length}, {"what", c.what}});
  // The end symbols keep their values.
  const Json endSymbolsOut = EndSymbols(out, outElf);
  if (endSymbolsOut != endSymbolsStock) return invalid("an end symbol changed");

  PatchResult result;
  result.ok = true;
  result.output = std::move(out);
  result.events = std::move(eventList);
  Json endNames = endSymbolsStock;
  result.summary = {{"event", "libr15_patch"},
                   {"status", "patched"},
                   {"stock_build_id", BuildIdHex(spec.stockBuildId)},
                   {"build_id", BuildIdHex(patchedId)},
                   {"stock_sha256", stockSha},
                   {"patched_sha256", data::Sha256Hex(result.output.data(), result.output.size())},
                   {"rows", spec.rowCount},
                   {"slot_area", Hex(slotArea)},
                   {"slot_area_end", Hex(newEnd)},
                   {"p_memsz_stock", Hex(target->p_memsz)},
                   {"p_memsz_patched", Hex(target->p_memsz + (newEnd - imageEnd))},
                   {"bss_section", elf.SectionName(elf.sh[bssIndex])},
                   {"extra_rows", extra.size()},
                   {"extra_rows_sha256", extraRowsSha256},
                   {"bytes_changed", differing},
                   {"changes", diffs},
                   {"end_symbols", endNames}};
  return result;
}

}  // namespace nevr_quest::patch
