// script_image_builders.h: the synthetic images the rule tests and the map script tests build byte by byte: a PE32+ DLL shaped
// by a Shape (a stock script's layout, with one field changed per rule) and a minimal ELF64 shared object. Test-only, header-only;
// every function is inline, so a test that uses one builder links nothing else.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "core/script_image_rules.h"

namespace nevr_script_image_fixture {

constexpr std::uint32_t kScnCode = 0x00000020;
constexpr std::uint32_t kScnData = 0x00000040;
constexpr std::uint32_t kScnExecute = 0x20000000;
constexpr std::uint32_t kScnRead = 0x40000000;
constexpr std::uint32_t kScnWrite = 0x80000000;

// The layout of the synthetic image: headers in the first 0x200 bytes, .text at RVA 0x1000 (file 0x200) holding
// setup_bindings (one `ret`), .rdata at RVA 0x2000 (file 0x400) holding the export directory and its tables.
constexpr std::uint32_t kNt = 0x40;
constexpr std::uint32_t kOpt = kNt + 24;
constexpr std::uint32_t kOptBytes = 240;
constexpr std::uint32_t kSections = kOpt + kOptBytes;
constexpr std::uint32_t kTextRva = 0x1000;
constexpr std::uint32_t kRdataRva = 0x2000;
constexpr std::uint32_t kRdataFile = 0x400;
constexpr std::uint32_t kExportBytes = 0x60;

struct Shape {
  std::uint16_t machine = 0x8664;
  std::uint16_t magic = 0x20b;
  std::uint16_t characteristics = 0x2022;  // DLL | EXECUTABLE_IMAGE | LARGE_ADDRESS_AWARE
  std::uint32_t entry = 0;
  std::uint32_t text_flags = kScnCode | kScnExecute | kScnRead;
  std::uint32_t rdata_flags = kScnData | kScnRead;
  const char* rdata_name = ".rdata";
  bool exports = true;
  std::uint32_t functions = 1;
  std::uint32_t names = 1;
  const char* export_name = "setup_bindings";
  std::uint32_t function_rva = kTextRva;
  int nonempty_directory = -1;  // a directory index given a non-zero RVA and size
};

inline void Put16(std::vector<std::uint8_t>* b, std::size_t at, std::uint16_t v) {
  (*b)[at] = static_cast<std::uint8_t>(v);
  (*b)[at + 1] = static_cast<std::uint8_t>(v >> 8);
}

inline void Put32(std::vector<std::uint8_t>* b, std::size_t at, std::uint32_t v) {
  for (int i = 0; i < 4; ++i) (*b)[at + i] = static_cast<std::uint8_t>(v >> (8 * i));
}

inline void Put64(std::vector<std::uint8_t>* b, std::size_t at, std::uint64_t v) {
  for (int i = 0; i < 8; ++i) (*b)[at + i] = static_cast<std::uint8_t>(v >> (8 * i));
}

inline void PutSection(std::vector<std::uint8_t>* b, int index, const char* name, std::uint32_t rva, std::uint32_t file,
                std::uint32_t flags) {
  const std::size_t at = kSections + 40u * index;
  std::memcpy(b->data() + at, name, std::strlen(name));
  Put32(b, at + 8, 0x200);  // VirtualSize
  Put32(b, at + 12, rva);
  Put32(b, at + 16, 0x200);  // SizeOfRawData
  Put32(b, at + 20, file);
  Put32(b, at + 36, flags);
}

inline std::vector<std::uint8_t> BuildDll(const Shape& s = Shape{}) {
  std::vector<std::uint8_t> b(0x600, 0);
  Put16(&b, 0, 0x5a4d);
  Put32(&b, 0x3c, kNt);
  Put32(&b, kNt, 0x4550);
  Put16(&b, kNt + 4, s.machine);
  Put16(&b, kNt + 6, 2);  // sections
  Put16(&b, kNt + 20, kOptBytes);
  Put16(&b, kNt + 22, s.characteristics);

  Put16(&b, kOpt, s.magic);
  Put32(&b, kOpt + 4, 0x200);  // SizeOfCode
  Put32(&b, kOpt + 8, 0x200);  // SizeOfInitializedData
  Put32(&b, kOpt + 16, s.entry);
  Put32(&b, kOpt + 20, kTextRva);  // BaseOfCode
  Put64(&b, kOpt + 24, 0x180000000ULL);
  Put32(&b, kOpt + 32, 0x1000);  // SectionAlignment
  Put32(&b, kOpt + 36, 0x200);   // FileAlignment
  Put16(&b, kOpt + 40, 6);       // OS version
  Put16(&b, kOpt + 48, 6);       // subsystem version
  Put32(&b, kOpt + 56, 0x3000);  // SizeOfImage
  Put32(&b, kOpt + 60, 0x200);   // SizeOfHeaders
  Put16(&b, kOpt + 68, 2);       // GUI
  Put16(&b, kOpt + 70, 0x0160);  // HIGH_ENTROPY_VA | DYNAMIC_BASE | NX_COMPAT
  Put64(&b, kOpt + 72, 0x100000);
  Put64(&b, kOpt + 80, 0x1000);
  Put64(&b, kOpt + 88, 0x100000);
  Put64(&b, kOpt + 96, 0x1000);
  Put32(&b, kOpt + 108, 16);
  if (s.exports) {
    Put32(&b, kOpt + 112, kRdataRva);
    Put32(&b, kOpt + 116, kExportBytes);
  }
  if (s.nonempty_directory >= 0) {
    Put32(&b, kOpt + 112 + 8u * s.nonempty_directory, kRdataRva + 0x100);
    Put32(&b, kOpt + 116 + 8u * s.nonempty_directory, 0x20);
  }

  PutSection(&b, 0, ".text", kTextRva, 0x200, s.text_flags);
  PutSection(&b, 1, s.rdata_name, kRdataRva, kRdataFile, s.rdata_flags);
  b[0x200] = 0xc3;  // setup_bindings: ret

  // Export directory at .rdata+0: tables at +0x28, the name at +0x40, the DLL's name at +0x50.
  const std::size_t e = kRdataFile;
  Put32(&b, e + 12, kRdataRva + 0x50);
  Put32(&b, e + 16, 1);  // Base
  Put32(&b, e + 20, s.functions);
  Put32(&b, e + 24, s.names);
  Put32(&b, e + 28, kRdataRva + 0x28);
  Put32(&b, e + 32, kRdataRva + 0x30);
  Put32(&b, e + 36, kRdataRva + 0x38);
  Put32(&b, e + 0x28, s.function_rva);
  Put32(&b, e + 0x2c, kTextRva);
  Put32(&b, e + 0x30, kRdataRva + 0x40);
  Put32(&b, e + 0x34, kRdataRva + 0x40);
  Put16(&b, e + 0x38, 0);
  Put16(&b, e + 0x3a, 1);
  std::memcpy(b.data() + e + 0x40, s.export_name, std::strlen(s.export_name));
  std::memcpy(b.data() + e + 0x50, "map.dll", 7);
  return b;
}

inline std::string Verdict(const std::vector<std::uint8_t>& image) { return nevr_script_image::CheckPeScript(image.data(), image.size()).reason; }

inline void Put(std::vector<std::uint8_t>& b, std::size_t at, std::uint64_t value, int bytes) {
  if (b.size() < at + bytes) b.resize(at + bytes);
  for (int i = 0; i < bytes; ++i) b[at + i] = static_cast<std::uint8_t>(value >> (8 * i));
}

// One RX load at 0 (file 0..0x1000), one RW load holding the dynamic table, a DT_HASH with two symbols, the string
// table "\0libc.so\0setup_bindings\0".
struct Image {
  std::vector<std::uint8_t> bytes;
  std::vector<std::pair<std::int64_t, std::uint64_t>> dynamic{{1, 1}, {4, 0x200}, {5, 0x100}, {6, 0x300}, {10, 32}};
  std::uint32_t text_flags = 5, data_flags = 6;
  std::uint64_t export_value = 0x400;
  std::uint8_t export_info = 0x12;  // GLOBAL FUNC
  const char* export_name = "setup_bindings";
  const char* needed = "libc.so";
  std::uint16_t machine = 183, type = 3;

  std::vector<std::uint8_t> Build() {
    bytes.assign(0x2000, 0);
    std::memcpy(bytes.data(), "\x7f" "ELF", 4);
    bytes[4] = 2;
    bytes[5] = 1;
    Put(bytes, 16, type, 2);
    Put(bytes, 18, machine, 2);
    Put(bytes, 32, 64, 8);
    Put(bytes, 54, 56, 2);
    Put(bytes, 56, 3, 2);
    const std::uint64_t ph[3][6] = {{1, text_flags, 0, 0, 0x1000, 0x1000},
                                    {1, data_flags, 0x1000, 0x1000, 0x1000, 0x1000},
                                    {2, 6, 0x1000, 0x1000, 0x200, 0x200}};
    for (int i = 0; i < 3; ++i) {
      const std::size_t at = 64 + 56 * i;
      Put(bytes, at, ph[i][0], 4);
      Put(bytes, at + 4, ph[i][1], 4);
      Put(bytes, at + 8, ph[i][2], 8);
      Put(bytes, at + 16, ph[i][3], 8);
      Put(bytes, at + 32, ph[i][4], 8);
      Put(bytes, at + 40, ph[i][5], 8);
    }
    std::memcpy(bytes.data() + 0x101, needed, std::strlen(needed));
    std::memcpy(bytes.data() + 0x109, export_name, std::strlen(export_name));
    Put(bytes, 0x204, 2, 4);  // nchain
    Put(bytes, 0x300 + 24, 9, 4);
    bytes[0x300 + 24 + 4] = export_info;
    Put(bytes, 0x300 + 24 + 6, 10, 2);
    Put(bytes, 0x300 + 24 + 8, export_value, 8);
    std::size_t at = 0x1000;
    for (const auto& d : dynamic) {
      Put(bytes, at, static_cast<std::uint64_t>(d.first), 8);
      Put(bytes, at + 8, d.second, 8);
      at += 16;
    }
    return bytes;
  }
};

}  // namespace nevr_script_image_fixture
