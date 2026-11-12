// script_image_rules.h: the shape a map's script DLL must have before the runtime hands it to the game (CDN_PC PC6).
//
// The game loads `<exe dir>\scripts\<hash16>.dll` with LoadLibraryW and calls its one export, setup_bindings
// (CScriptResource Load, echovr.exe 0x1412ebb50). Every stock script DLL has the shape checked here: PE32+ AMD64, no
// imports, no entry point, the single export, sections .text .rdata .data .pdata. A DLL of this shape runs none of its
// code when it is loaded, so the load itself cannot reach another module or the network; code runs only when the engine
// calls setup_bindings. Pure: no windows.h, no Log(), bounds-checked reads of the bytes given.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace nevr_script_image {

struct ImageVerdict {
  std::string reason = "ok";  // "ok", or the token of the first rule that fails
  std::string detail;         // what the token is about (a section name, a directory, a value)
  bool ok() const { return reason == "ok"; }
};

// The rules, in the order they are checked; the verdict names the first one that fails:
// pe_truncated (a header, table or name runs past the bytes), pe_not_pe, pe_not_amd64, pe_not_pe32plus, pe_not_dll,
// pe_imports, pe_delay_imports, pe_bound_imports, pe_iat, pe_tls, pe_clr (the directory is not empty), pe_entry_point,
// pe_no_exports, pe_export_count (not exactly one function and one name), pe_export_name (not setup_bindings),
// pe_export_forwarded (the export's RVA points into the export directory: a forwarder string, which GetProcAddress
// follows into another DLL), pe_export_not_code (the RVA is not inside an executable section), pe_section_name (a
// section other than .text .rdata .data .pdata .reloc), pe_section_wx (a section both writable and executable).
// A debug directory is allowed; it lives in .rdata.
ImageVerdict CheckPeScript(const std::uint8_t* image, std::size_t size);

// The Quest twin (CDN_DESIGN Q3, script_image_rules_elf.cpp): the shape of a map's lib<hash16>.so. Every stock Quest script
// library (465 in the r15 APK) has it: ELF64 little-endian ET_DYN AArch64, needs only libc.so libm.so libdl.so, no text
// relocations, no writable and executable segment, no initializer (DT_INIT, DT_INIT_ARRAY, DT_PREINIT_ARRAY), and a defined
// function setup_bindings in an executable segment. Like the PE rules, loading it runs none of its code. Stock libraries
// export many C++ symbols, so unlike the PE rules the export count is not checked. Finalizers (DT_FINI, DT_FINI_ARRAY) are
// allowed: they run at dlclose, not at load, and the 465 stock libraries were checked for initializers only, so refusing them
// could refuse stock-shaped scripts. The rules, in check order:
// elf_truncated, elf_not_elf, elf_not_64, elf_not_le, elf_not_dyn, elf_not_aarch64, elf_segment_wx, elf_no_dynamic,
// elf_textrel, elf_no_symbols (no DT_STRTAB, DT_SYMTAB or DT_HASH it can read), elf_needed, elf_init, elf_no_export,
// elf_export_not_code.
ImageVerdict CheckElfScript(const std::uint8_t* image, std::size_t size);

}  // namespace nevr_script_image
