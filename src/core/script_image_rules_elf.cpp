#include "core/script_image_rules.h"

#include <cstring>
#include <vector>

namespace nevr_script_image {

namespace {

// Offsets and values the ELF-64 format defines (the System V gABI, "ELF Header", "Program Header", "Dynamic Section",
// "Symbol Table", and the DT_HASH table's nbucket/nchain words).
constexpr std::size_t kMagicBytes = 4;
constexpr std::size_t kIdentClass = 4;          // e_ident[EI_CLASS]
constexpr std::size_t kIdentData = 5;           // e_ident[EI_DATA]
constexpr std::size_t kHeaderType = 16;         // e_type
constexpr std::size_t kHeaderMachine = 18;      // e_machine
constexpr std::size_t kHeaderPhoff = 32;        // e_phoff
constexpr std::size_t kHeaderPhentsize = 54;    // e_phentsize
constexpr std::size_t kHeaderPhnum = 56;        // e_phnum
constexpr std::size_t kPhdrFlags = 4;           // p_flags
constexpr std::size_t kPhdrOffset = 8;          // p_offset
constexpr std::size_t kPhdrVaddr = 16;          // p_vaddr
constexpr std::size_t kPhdrFileSize = 32;       // p_filesz
constexpr std::size_t kDynValue = 8;            // d_val
constexpr std::size_t kHashNchain = 4;          // DT_HASH: nbucket, then nchain
constexpr std::size_t kSymInfo = 4;             // st_info
constexpr std::size_t kSymShndx = 6;            // st_shndx
constexpr std::size_t kSymValue = 8;            // st_value
constexpr int kHalfBytes = 2;
constexpr int kWordBytes = 4;
constexpr int kXwordBytes = 8;
constexpr unsigned kSymBindShift = 4;           // ELF64_ST_BIND
constexpr std::uint8_t kSymTypeMask = 0xf;      // ELF64_ST_TYPE
constexpr std::uint64_t kHashHeaderBytes = 8;   // nbucket, nchain
constexpr unsigned kByteBits = 8;

constexpr std::uint8_t kElfClass64 = 2;
constexpr std::uint8_t kElfDataLittle = 1;
constexpr std::uint16_t kTypeDyn = 3;
constexpr std::uint16_t kMachineAarch64 = 183;
constexpr std::size_t kElfHeaderBytes = 64;
constexpr std::size_t kPhdrBytes = 56;
constexpr std::size_t kDynBytes = 16;
constexpr std::size_t kSymBytes = 24;

constexpr std::uint32_t kPtLoad = 1;
constexpr std::uint32_t kPtDynamic = 2;
constexpr std::uint32_t kPfExecute = 1;
constexpr std::uint32_t kPfWrite = 2;

constexpr std::int64_t kDtNull = 0;
constexpr std::int64_t kDtNeeded = 1;
constexpr std::int64_t kDtHash = 4;
constexpr std::int64_t kDtStrtab = 5;
constexpr std::int64_t kDtSymtab = 6;
constexpr std::int64_t kDtStrsz = 10;
constexpr std::int64_t kDtInit = 12;
constexpr std::int64_t kDtTextrel = 22;
constexpr std::int64_t kDtInitArraySz = 27;
constexpr std::int64_t kDtFlags = 30;
constexpr std::int64_t kDtPreinitArraySz = 33;
constexpr std::uint64_t kDfTextrel = 0x4;

constexpr std::uint8_t kStbGlobal = 1;
constexpr std::uint8_t kStbWeak = 2;
constexpr std::uint8_t kSttFunc = 2;

constexpr char kExportName[] = "setup_bindings";
constexpr const char* kAllowedNeeded[] = {"libc.so", "libm.so", "libdl.so"};

struct Load {
  std::uint64_t vaddr = 0, offset = 0, file_size = 0;
  std::uint32_t flags = 0;
};

class Reader {
 public:
  Reader(const std::uint8_t* data, std::size_t size) : data_(data), size_(size) {}
  bool Has(std::uint64_t offset, std::uint64_t bytes) const { return offset <= size_ && bytes <= size_ - offset; }
  std::uint64_t Le(std::uint64_t offset, int bytes) const {
    std::uint64_t value = 0;
    for (int i = bytes - 1; i >= 0; --i) value = (value << kByteBits) | data_[offset + static_cast<std::uint64_t>(i)];
    return value;
  }
  std::uint8_t U8(std::uint64_t offset) const { return data_[offset]; }
  const std::uint8_t* At(std::uint64_t offset) const { return data_ + offset; }

 private:
  const std::uint8_t* data_;
  std::size_t size_;
};

ImageVerdict Fail(const char* reason, std::string detail) {
  ImageVerdict v;
  v.reason = reason;
  v.detail = std::move(detail);
  return v;
}

// The file offset of `bytes` bytes at `vaddr`, when they lie in one PT_LOAD's file image.
bool OffsetOf(const std::vector<Load>& loads, std::uint64_t vaddr, std::uint64_t bytes, std::uint64_t* offset) {
  for (const Load& l : loads) {
    if (vaddr < l.vaddr) continue;
    const std::uint64_t into = vaddr - l.vaddr;
    if (into > l.file_size || bytes > l.file_size - into) continue;
    *offset = l.offset + into;
    return true;
  }
  return false;
}

}  // namespace

ImageVerdict CheckElfScript(const std::uint8_t* image, std::size_t size) {
  const Reader r(image, size);
  if (image == nullptr || !r.Has(0, kElfHeaderBytes)) return Fail("elf_truncated", "elf header");
  if (std::memcmp(image, "\x7f" "ELF", kMagicBytes) != 0) return Fail("elf_not_elf", "");
  if (r.U8(kIdentClass) != kElfClass64) return Fail("elf_not_64", "");
  if (r.U8(kIdentData) != kElfDataLittle) return Fail("elf_not_le", "");
  if (r.Le(kHeaderType, kHalfBytes) != kTypeDyn) return Fail("elf_not_dyn", "");
  if (r.Le(kHeaderMachine, kHalfBytes) != kMachineAarch64) return Fail("elf_not_aarch64", "");

  const std::uint64_t phoff = r.Le(kHeaderPhoff, kXwordBytes);
  const std::uint64_t phentsize = r.Le(kHeaderPhentsize, kHalfBytes);
  const std::uint64_t phnum = r.Le(kHeaderPhnum, kHalfBytes);
  if (phentsize != kPhdrBytes || !r.Has(phoff, phnum * kPhdrBytes)) return Fail("elf_truncated", "program headers");
  std::vector<Load> loads;
  bool has_dynamic = false;
  std::uint64_t dyn_offset = 0, dyn_size = 0;
  for (std::uint64_t i = 0; i < phnum; ++i) {
    const std::uint64_t at = phoff + i * kPhdrBytes;
    const std::uint64_t type = r.Le(at, kWordBytes);
    if (type == kPtLoad) {
      Load l;
      l.flags = static_cast<std::uint32_t>(r.Le(at + kPhdrFlags, kWordBytes));
      l.offset = r.Le(at + kPhdrOffset, kXwordBytes);
      l.vaddr = r.Le(at + kPhdrVaddr, kXwordBytes);
      l.file_size = r.Le(at + kPhdrFileSize, kXwordBytes);
      if ((l.flags & kPfWrite) != 0 && (l.flags & kPfExecute) != 0) return Fail("elf_segment_wx", "phdr " + std::to_string(i));
      if (!r.Has(l.offset, l.file_size)) return Fail("elf_truncated", "load segment " + std::to_string(i));
      loads.push_back(l);
    } else if (type == kPtDynamic && !has_dynamic) {
      has_dynamic = true;
      dyn_offset = r.Le(at + kPhdrOffset, kXwordBytes);
      dyn_size = r.Le(at + kPhdrFileSize, kXwordBytes);
    }
  }
  if (!has_dynamic) return Fail("elf_no_dynamic", "");
  if (!r.Has(dyn_offset, dyn_size)) return Fail("elf_truncated", "dynamic segment");

  std::vector<std::uint64_t> needed;
  std::uint64_t strtab = 0, strsz = 0, symtab = 0, hash = 0;
  bool has_strtab = false, has_symtab = false, has_hash = false;
  const char* textrel = nullptr;
  const char* init = nullptr;
  for (std::uint64_t at = dyn_offset; at + kDynBytes <= dyn_offset + dyn_size; at += kDynBytes) {
    const auto tag = static_cast<std::int64_t>(r.Le(at, kXwordBytes));
    const std::uint64_t value = r.Le(at + kDynValue, kXwordBytes);
    if (tag == kDtNull) break;
    switch (tag) {
      case kDtNeeded: needed.push_back(value); break;
      case kDtHash: hash = value; has_hash = true; break;
      case kDtStrtab: strtab = value; has_strtab = true; break;
      case kDtSymtab: symtab = value; has_symtab = true; break;
      case kDtStrsz: strsz = value; break;
      case kDtTextrel: textrel = "DT_TEXTREL"; break;
      case kDtFlags:
        if ((value & kDfTextrel) != 0) textrel = "DF_TEXTREL";
        break;
      case kDtInit: init = "DT_INIT"; break;
      case kDtInitArraySz:
        if (value != 0) init = "DT_INIT_ARRAY";
        break;
      case kDtPreinitArraySz:
        if (value != 0) init = "DT_PREINIT_ARRAY";
        break;
      default: break;
    }
  }

  if (textrel != nullptr) return Fail("elf_textrel", textrel);
  std::uint64_t str_at = 0, hash_at = 0;
  if (!has_strtab || !has_symtab || !has_hash || strsz == 0 || !OffsetOf(loads, strtab, strsz, &str_at) ||
      !OffsetOf(loads, hash, kHashHeaderBytes, &hash_at)) {
    return Fail("elf_no_symbols", "");
  }
  // The name at `name` in the string table, bounded by the table: empty when it does not end inside it.
  auto name_at = [&](std::uint64_t name) -> std::string {
    if (name >= strsz) return std::string();
    const auto* begin = reinterpret_cast<const char*>(r.At(str_at + name));
    const void* end = std::memchr(begin, '\0', static_cast<std::size_t>(strsz - name));
    return end == nullptr ? std::string() : std::string(begin, static_cast<const char*>(end));
  };

  for (const std::uint64_t name : needed) {
    const std::string lib = name_at(name);
    bool allowed = false;
    for (const char* ok : kAllowedNeeded) allowed = allowed || lib == ok;
    if (!allowed) return Fail("elf_needed", lib.empty() ? "(unreadable name)" : lib);
  }

  if (init != nullptr) return Fail("elf_init", init);

  // DT_HASH's nchain is the dynamic symbol count (every stock script library carries DT_HASH beside DT_GNU_HASH).
  const std::uint64_t symbol_count = r.Le(hash_at + kHashNchain, kWordBytes);
  std::uint64_t sym_at = 0;
  if (!OffsetOf(loads, symtab, symbol_count * kSymBytes, &sym_at)) return Fail("elf_no_symbols", "symbol table");
  bool found = false;
  std::uint64_t export_value = 0;
  for (std::uint64_t i = 1; i < symbol_count && !found; ++i) {
    const std::uint64_t at = sym_at + i * kSymBytes;
    const std::uint8_t info = r.U8(at + kSymInfo);
    const std::uint64_t shndx = r.Le(at + kSymShndx, kHalfBytes);
    const std::uint8_t bind = static_cast<std::uint8_t>(info >> kSymBindShift);
    if (shndx == 0 || (info & kSymTypeMask) != kSttFunc || (bind != kStbGlobal && bind != kStbWeak)) continue;
    if (name_at(r.Le(at, kWordBytes)) != kExportName) continue;
    found = true;
    export_value = r.Le(at + kSymValue, kXwordBytes);
  }
  if (!found) return Fail("elf_no_export", kExportName);
  for (const Load& l : loads) {
    if ((l.flags & kPfExecute) != 0 && export_value >= l.vaddr && export_value - l.vaddr < l.file_size) {
      return ImageVerdict{};
    }
  }
  return Fail("elf_export_not_code", std::to_string(export_value));
}

}  // namespace nevr_script_image
