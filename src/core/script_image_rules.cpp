#include "core/script_image_rules.h"

#include <cstring>
#include <vector>

namespace nevr_script_image {

namespace {

constexpr std::uint16_t kDosMagic = 0x5a4d;      // "MZ"
constexpr std::uint32_t kNtSignature = 0x4550;   // "PE\0\0"
constexpr std::uint16_t kMachineAmd64 = 0x8664;
constexpr std::uint16_t kFileDll = 0x2000;
constexpr std::uint16_t kPe32PlusMagic = 0x20b;
constexpr std::uint32_t kScnExecute = 0x20000000;
constexpr std::uint32_t kScnWrite = 0x80000000;

// Offsets and sizes the PE/COFF format defines (Microsoft "PE Format": the MS-DOS stub, the COFF file header, the
// PE32+ optional header, the section table and the export directory table).
constexpr std::size_t kDosHeaderBytes = 0x40;
constexpr std::size_t kDosNtOffset = 0x3c;           // e_lfanew
constexpr std::size_t kNtSignatureBytes = 4;
constexpr std::size_t kFileHeaderBytes = 20;
constexpr std::size_t kFileSectionCount = 2;         // NumberOfSections
constexpr std::size_t kFileOptionalBytes = 16;       // SizeOfOptionalHeader
constexpr std::size_t kFileCharacteristics = 18;
constexpr std::size_t kOptEntryPoint = 16;           // AddressOfEntryPoint
constexpr std::size_t kOptRvaCount = 108;            // NumberOfRvaAndSizes (PE32+)
constexpr std::size_t kOptDirectories = 112;         // the first data directory (PE32+)
constexpr std::size_t kDataDirBytes = 8;             // RVA, size
constexpr std::size_t kDataDirSize = 4;
constexpr std::size_t kSectionBytes = 40;
constexpr std::size_t kSectionNameBytes = 8;
constexpr std::size_t kSectionVirtualSize = 8;
constexpr std::size_t kSectionRva = 12;
constexpr std::size_t kSectionRawSize = 16;
constexpr std::size_t kSectionRawOffset = 20;
constexpr std::size_t kSectionFlags = 36;
constexpr std::size_t kExportDirBytes = 40;
constexpr std::size_t kExportFunctionCount = 20;     // NumberOfFunctions
constexpr std::size_t kExportNameCount = 24;         // NumberOfNames
constexpr std::size_t kExportFunctions = 28;         // AddressOfFunctions
constexpr std::size_t kExportNames = 32;             // AddressOfNames
constexpr std::size_t kExportOrdinals = 36;          // AddressOfNameOrdinals
constexpr std::size_t kRvaBytes = 4;
constexpr std::size_t kOrdinalBytes = 2;
constexpr std::uint32_t kMaxDirectories = 16;

// Hex(): the digits of a u32, most significant first.
constexpr int kHexTopShift = 28;
constexpr int kHexDigitBits = 4;
constexpr unsigned kHexDigitMask = 0xf;

constexpr std::size_t kByteBits = 8;

enum Directory : std::uint32_t {
  kExport = 0,
  kImport = 1,
  kTls = 9,
  kBoundImport = 11,
  kIat = 12,
  kDelayImport = 13,
  kClr = 14,
};

constexpr char kExportName[] = "setup_bindings";

struct Section {
  std::string name;
  std::uint32_t rva = 0;
  std::uint32_t virtual_size = 0;
  std::uint32_t raw_size = 0;
  std::uint32_t raw_offset = 0;
  std::uint32_t flags = 0;
};

struct DataDir {
  std::uint32_t rva = 0;
  std::uint32_t size = 0;
};

class Reader {
 public:
  Reader(const std::uint8_t* data, std::size_t size) : data_(data), size_(size) {}

  bool Has(std::uint64_t offset, std::uint64_t bytes) const { return offset <= size_ && bytes <= size_ - offset; }

  std::uint16_t U16(std::uint64_t offset) const {
    return static_cast<std::uint16_t>(data_[offset] | (data_[offset + 1] << kByteBits));
  }
  std::uint32_t U32(std::uint64_t offset) const {
    std::uint32_t value = 0;
    for (std::size_t i = sizeof(value); i-- > 0;) value = (value << kByteBits) | data_[offset + i];
    return value;
  }
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

std::string Hex(std::uint32_t value) {
  static const char kDigits[] = "0123456789abcdef";
  std::string out = "0x";
  bool started = false;
  for (int shift = kHexTopShift; shift >= 0; shift -= kHexDigitBits) {
    const unsigned digit = (value >> shift) & kHexDigitMask;
    if (digit != 0 || started || shift == 0) {
      out += kDigits[digit];
      started = true;
    }
  }
  return out;
}

bool AllowedSectionName(const std::string& name) {
  return name == ".text" || name == ".rdata" || name == ".data" || name == ".pdata" || name == ".reloc";
}

// The file offset of `bytes` bytes at `rva`, when they lie in one section's raw data.
bool OffsetOf(const std::vector<Section>& sections, std::uint32_t rva, std::uint64_t bytes, std::uint64_t* offset) {
  for (const Section& s : sections) {
    if (rva < s.rva) continue;
    const std::uint64_t into = static_cast<std::uint64_t>(rva) - s.rva;
    if (into + bytes > s.raw_size) continue;
    *offset = static_cast<std::uint64_t>(s.raw_offset) + into;
    return true;
  }
  return false;
}

const Section* SectionHolding(const std::vector<Section>& sections, std::uint32_t rva) {
  for (const Section& s : sections) {
    const std::uint32_t extent = s.virtual_size > s.raw_size ? s.virtual_size : s.raw_size;
    if (rva >= s.rva && static_cast<std::uint64_t>(rva) < static_cast<std::uint64_t>(s.rva) + extent) return &s;
  }
  return nullptr;
}

}  // namespace

std::vector<FileRange> ExecutableSectionFileRanges(const std::uint8_t* image, std::size_t size) {
  std::vector<FileRange> ranges;
  if (image == nullptr) return ranges;
  const Reader r(image, size);
  if (!r.Has(0, kDosHeaderBytes) || r.U16(0) != kDosMagic) return ranges;
  const std::uint64_t nt = r.U32(kDosNtOffset);
  if (!r.Has(nt, kNtSignatureBytes + kFileHeaderBytes) || r.U32(nt) != kNtSignature) return ranges;
  const std::uint64_t file_header = nt + kNtSignatureBytes;
  const std::uint16_t section_count = r.U16(file_header + kFileSectionCount);
  const std::uint64_t table = file_header + kFileHeaderBytes + r.U16(file_header + kFileOptionalBytes);
  if (!r.Has(table, kSectionBytes * section_count)) return ranges;
  for (std::uint16_t i = 0; i < section_count; ++i) {
    const std::uint64_t at = table + kSectionBytes * i;
    const std::uint64_t raw_offset = r.U32(at + kSectionRawOffset);
    if ((r.U32(at + kSectionFlags) & kScnExecute) == 0 || raw_offset >= size) continue;
    const std::uint64_t raw_end = raw_offset + r.U32(at + kSectionRawSize);
    ranges.push_back(FileRange{raw_offset, raw_end < size ? raw_end : size});
  }
  return ranges;
}

ImageVerdict CheckPeScript(const std::uint8_t* image, std::size_t size) {
  if (image == nullptr) return Fail("pe_truncated", "no bytes");
  const Reader r(image, size);
  if (!r.Has(0, kDosHeaderBytes)) return Fail("pe_truncated", "dos header");
  if (r.U16(0) != kDosMagic) return Fail("pe_not_pe", "no MZ");
  const std::uint64_t nt = r.U32(kDosNtOffset);
  if (!r.Has(nt, kNtSignatureBytes + kFileHeaderBytes)) return Fail("pe_truncated", "nt headers");
  if (r.U32(nt) != kNtSignature) return Fail("pe_not_pe", "no PE signature");

  const std::uint64_t file_header = nt + kNtSignatureBytes;
  const std::uint16_t machine = r.U16(file_header);
  const std::uint16_t section_count = r.U16(file_header + kFileSectionCount);
  const std::uint16_t optional_bytes = r.U16(file_header + kFileOptionalBytes);
  const std::uint16_t characteristics = r.U16(file_header + kFileCharacteristics);
  if (machine != kMachineAmd64) return Fail("pe_not_amd64", "machine=" + Hex(machine));

  const std::uint64_t opt = file_header + kFileHeaderBytes;
  if (optional_bytes < kOptDirectories || !r.Has(opt, optional_bytes)) return Fail("pe_truncated", "optional header");
  if (r.U16(opt) != kPe32PlusMagic) return Fail("pe_not_pe32plus", "magic=" + Hex(r.U16(opt)));
  if ((characteristics & kFileDll) == 0) return Fail("pe_not_dll", "characteristics=" + Hex(characteristics));

  const std::uint32_t dir_count = r.U32(opt + kOptRvaCount);
  if (dir_count > kMaxDirectories || kOptDirectories + kDataDirBytes * dir_count > optional_bytes) {
    return Fail("pe_truncated", "data directories count=" + std::to_string(dir_count));
  }
  DataDir dirs[kMaxDirectories];
  for (std::uint32_t i = 0; i < dir_count; ++i) {
    dirs[i].rva = r.U32(opt + kOptDirectories + kDataDirBytes * i);
    dirs[i].size = r.U32(opt + kOptDirectories + kDataDirBytes * i + kDataDirSize);
  }

  const std::uint64_t table = opt + optional_bytes;
  if (!r.Has(table, kSectionBytes * section_count)) return Fail("pe_truncated", "section table");
  std::vector<Section> sections;
  sections.reserve(section_count);
  for (std::uint16_t i = 0; i < section_count; ++i) {
    const std::uint64_t at = table + kSectionBytes * i;
    Section s;
    const char* name = reinterpret_cast<const char*>(r.At(at));
    s.name.assign(name, strnlen(name, kSectionNameBytes));
    s.virtual_size = r.U32(at + kSectionVirtualSize);
    s.rva = r.U32(at + kSectionRva);
    s.raw_size = r.U32(at + kSectionRawSize);
    s.raw_offset = r.U32(at + kSectionRawOffset);
    s.flags = r.U32(at + kSectionFlags);
    if (!r.Has(s.raw_offset, s.raw_size)) return Fail("pe_truncated", "section " + s.name + " raw data");
    sections.push_back(std::move(s));
  }

  struct Empty {
    Directory index;
    const char* reason;
  };
  static constexpr Empty kMustBeEmpty[] = {{kImport, "pe_imports"}, {kDelayImport, "pe_delay_imports"},
                                           {kBoundImport, "pe_bound_imports"}, {kIat, "pe_iat"},
                                           {kTls, "pe_tls"}, {kClr, "pe_clr"}};
  for (const Empty& e : kMustBeEmpty) {
    if (e.index >= dir_count) continue;
    const DataDir& d = dirs[e.index];
    if (d.rva != 0 || d.size != 0) return Fail(e.reason, "rva=" + Hex(d.rva) + " size=" + Hex(d.size));
  }

  const std::uint32_t entry = r.U32(opt + kOptEntryPoint);
  if (entry != 0) return Fail("pe_entry_point", "rva=" + Hex(entry));

  if (dir_count <= kExport || dirs[kExport].rva == 0 || dirs[kExport].size == 0) return Fail("pe_no_exports", "");
  const DataDir exports = dirs[kExport];
  std::uint64_t dir_at = 0;
  if (!OffsetOf(sections, exports.rva, kExportDirBytes, &dir_at)) return Fail("pe_truncated", "export directory");
  const std::uint32_t function_count = r.U32(dir_at + kExportFunctionCount);
  const std::uint32_t name_count = r.U32(dir_at + kExportNameCount);
  if (function_count != 1 || name_count != 1) {
    return Fail("pe_export_count", "functions=" + std::to_string(function_count) + " names=" + std::to_string(name_count));
  }
  std::uint64_t functions_at = 0, names_at = 0, ordinals_at = 0;
  if (!OffsetOf(sections, r.U32(dir_at + kExportFunctions), kRvaBytes, &functions_at) ||
      !OffsetOf(sections, r.U32(dir_at + kExportNames), kRvaBytes, &names_at) ||
      !OffsetOf(sections, r.U32(dir_at + kExportOrdinals), kOrdinalBytes, &ordinals_at)) {
    return Fail("pe_truncated", "export tables");
  }
  std::uint64_t name_at = 0;
  if (!OffsetOf(sections, r.U32(names_at), sizeof(kExportName), &name_at) ||
      std::memcmp(r.At(name_at), kExportName, sizeof(kExportName)) != 0) {
    return Fail("pe_export_name", "the one name is not setup_bindings");
  }
  if (r.U16(ordinals_at) != 0) return Fail("pe_export_count", "the name's ordinal index is not the one function");
  const std::uint32_t target = r.U32(functions_at);
  if (target >= exports.rva && static_cast<std::uint64_t>(target) < static_cast<std::uint64_t>(exports.rva) + exports.size) {
    return Fail("pe_export_forwarded", "rva=" + Hex(target));
  }
  const Section* code = SectionHolding(sections, target);
  if (target == 0 || code == nullptr || (code->flags & kScnExecute) == 0) {
    return Fail("pe_export_not_code", "rva=" + Hex(target));
  }

  for (const Section& s : sections) {
    if (!AllowedSectionName(s.name)) return Fail("pe_section_name", s.name);
  }
  for (const Section& s : sections) {
    if ((s.flags & kScnWrite) != 0 && (s.flags & kScnExecute) != 0) return Fail("pe_section_wx", s.name);
  }
  return ImageVerdict{};
}

}  // namespace nevr_script_image
