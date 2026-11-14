// The shape a map's script must have (core/script_image_rules.h; docs/adr/0048, decision 6): the PE rules for a PC
// script DLL and the ELF rules for a Quest script library, one must-fail case per rule. Every image here is a minimal
// one this test builds byte by byte; nothing of the game's is read. Pure: no windows.h.
#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "core/script_image_rules.h"
#include "core/tests/script_image_builders.h"

namespace {

using namespace nevr_script_image_fixture;

using nevr_script_image::CheckElfScript;
using nevr_script_image::CheckPeScript;

// ---------------------------------------------------------------------------------------------------------------------
// PE: a minimal stock-shaped script DLL.
// ---------------------------------------------------------------------------------------------------------------------

TEST(ScriptImageRules, TheStockShapePasses) {
  const std::vector<std::uint8_t> dll = BuildDll();
  const nevr_script_image::ImageVerdict v = CheckPeScript(dll.data(), dll.size());
  EXPECT_TRUE(v.ok()) << v.reason << " " << v.detail;
}

TEST(ScriptImageRules, EachRuleIsARefusalNamingIt) {
  struct Case {
    const char* expect;
    std::function<void(Shape*)> change;
  };
  const Case cases[] = {
      {"pe_not_amd64", [](Shape* s) { s->machine = 0x14c; }},
      {"pe_not_pe32plus", [](Shape* s) { s->magic = 0x10b; }},
      {"pe_not_dll", [](Shape* s) { s->characteristics = 0x0022; }},
      {"pe_imports", [](Shape* s) { s->nonempty_directory = 1; }},
      {"pe_tls", [](Shape* s) { s->nonempty_directory = 9; }},
      {"pe_bound_imports", [](Shape* s) { s->nonempty_directory = 11; }},
      {"pe_iat", [](Shape* s) { s->nonempty_directory = 12; }},
      {"pe_delay_imports", [](Shape* s) { s->nonempty_directory = 13; }},
      {"pe_clr", [](Shape* s) { s->nonempty_directory = 14; }},
      {"pe_entry_point", [](Shape* s) { s->entry = kTextRva; }},
      {"pe_no_exports", [](Shape* s) { s->exports = false; }},
      {"pe_export_count", [](Shape* s) { s->functions = 2; }},
      {"pe_export_count", [](Shape* s) { s->names = 2; }},
      {"pe_export_count", [](Shape* s) { s->names = 0; }},
      {"pe_export_name", [](Shape* s) { s->export_name = "setup_binding"; }},
      {"pe_export_name", [](Shape* s) { s->export_name = "setup_bindings2"; }},
      // A forwarder: the RVA points at "map.dll"-style text inside the export directory itself.
      {"pe_export_forwarded", [](Shape* s) { s->function_rva = kRdataRva + 0x50; }},
      {"pe_export_not_code", [](Shape* s) { s->function_rva = kRdataRva + 0x100; }},
      {"pe_export_not_code", [](Shape* s) { s->function_rva = 0; }},
      {"pe_section_name", [](Shape* s) { s->rdata_name = ".rsrc"; }},
      {"pe_section_wx", [](Shape* s) { s->text_flags |= kScnWrite; }},
      {"pe_section_wx", [](Shape* s) { s->rdata_flags |= kScnWrite | kScnExecute; }},
  };
  for (const Case& c : cases) {
    Shape s;
    c.change(&s);
    EXPECT_EQ(Verdict(BuildDll(s)), c.expect);
  }
}

TEST(ScriptImageRules, AllowedSectionsAndADebugDirectoryPass) {
  Shape s;
  s.rdata_name = ".pdata";
  EXPECT_EQ(Verdict(BuildDll(s)), "ok");
  s.rdata_name = ".reloc";
  EXPECT_EQ(Verdict(BuildDll(s)), "ok");
  s = Shape{};
  s.nonempty_directory = 6;  // IMAGE_DIRECTORY_ENTRY_DEBUG
  EXPECT_EQ(Verdict(BuildDll(s)), "ok");
  s.nonempty_directory = 3;  // the exception directory, .pdata's
  EXPECT_EQ(Verdict(BuildDll(s)), "ok");
}

TEST(ScriptImageRules, TruncatedAndForeignBytesAreRefusedWithoutReadingPastTheEnd) {
  const std::vector<std::uint8_t> dll = BuildDll();
  EXPECT_EQ(CheckPeScript(nullptr, 0).reason, "pe_truncated");
  for (std::size_t size : {0u, 0x3fu, 0x60u, 0x100u, 0x1f0u, 0x3ffu, 0x430u}) {
    std::vector<std::uint8_t> cut(dll.begin(), dll.begin() + size);
    EXPECT_NE(Verdict(cut), "ok") << size;
  }
  std::vector<std::uint8_t> wrong = dll;
  wrong[0] = 'X';
  EXPECT_EQ(Verdict(wrong), "pe_not_pe");
  wrong = dll;
  Put32(&wrong, 0x3c, 0xfffffff0);
  EXPECT_EQ(Verdict(wrong), "pe_truncated");
  wrong = dll;
  Put16(&wrong, kNt + 6, 0xffff);  // section count past the file
  EXPECT_EQ(Verdict(wrong), "pe_truncated");
  wrong = dll;
  Put32(&wrong, kOpt + 108, 17);
  EXPECT_EQ(Verdict(wrong), "pe_truncated");
}

// ---------------------------------------------------------------------------------------------------------------------
// ELF: a minimal stock-shaped Quest script library.
// ---------------------------------------------------------------------------------------------------------------------

std::string ElfVerdict(Image image) {
  const std::vector<std::uint8_t> b = image.Build();
  return CheckElfScript(b.data(), b.size()).reason;
}

std::string ElfVerdictOf(const std::vector<std::uint8_t>& b) { return CheckElfScript(b.data(), b.size()).reason; }

TEST(ScriptImageRulesElf, TheStockShapePasses) { EXPECT_EQ(ElfVerdict(Image{}), "ok"); }

TEST(ScriptImageRulesElf, HeaderRulesRefuseByName) {
  std::vector<std::uint8_t> b = Image{}.Build();
  EXPECT_EQ(CheckElfScript(b.data(), 40).reason, "elf_truncated");
  EXPECT_EQ(CheckElfScript(nullptr, 0).reason, "elf_truncated");
  b = Image{}.Build();
  b[1] = 'X';
  EXPECT_EQ(ElfVerdictOf(b), "elf_not_elf");
  b = Image{}.Build();
  b[4] = 1;
  EXPECT_EQ(ElfVerdictOf(b), "elf_not_64");
  b = Image{}.Build();
  b[5] = 2;
  EXPECT_EQ(ElfVerdictOf(b), "elf_not_le");
  Image i;
  i.type = 2;
  EXPECT_EQ(ElfVerdict(i), "elf_not_dyn");
  i = Image{};
  i.machine = 62;
  EXPECT_EQ(ElfVerdict(i), "elf_not_aarch64");
}

TEST(ScriptImageRulesElf, SegmentAndDynamicRulesRefuseByName) {
  Image i;
  i.data_flags = 7;
  EXPECT_EQ(ElfVerdict(i), "elf_segment_wx");
  std::vector<std::uint8_t> b = Image{}.Build();
  Put(b, 64 + 112, 4, 4);  // the PT_DYNAMIC header becomes a PT_NOTE
  EXPECT_EQ(ElfVerdictOf(b), "elf_no_dynamic");
  i = Image{};
  i.dynamic.push_back({22, 0});
  EXPECT_EQ(ElfVerdict(i), "elf_textrel");
  i = Image{};
  i.dynamic.push_back({30, 4});
  EXPECT_EQ(ElfVerdict(i), "elf_textrel");
  i = Image{};
  i.dynamic.erase(i.dynamic.begin() + 1);
  EXPECT_EQ(ElfVerdict(i), "elf_no_symbols");
  i = Image{};
  i.needed = "liblog.s";
  EXPECT_EQ(ElfVerdict(i), "elf_needed");
}

TEST(ScriptImageRulesElf, AnInitializerIsRefusedAndAnEmptyInitArrayIsNot) {
  for (const std::pair<std::int64_t, std::uint64_t> entry :
       {std::pair<std::int64_t, std::uint64_t>{12, 0x400}, {27, 8}, {33, 8}}) {
    Image i;
    i.dynamic.push_back(entry);
    EXPECT_EQ(ElfVerdict(i), "elf_init") << entry.first;
  }
  Image i;
  i.dynamic.push_back({27, 0});
  EXPECT_EQ(ElfVerdict(i), "ok");
}

TEST(ScriptImageRulesElf, TheExportMustBeADefinedFunctionInCode) {
  Image i;
  i.export_name = "setup_bindingz";
  EXPECT_EQ(ElfVerdict(i), "elf_no_export");
  i = Image{};
  i.export_info = 0x11;  // an OBJECT of that name is not the export
  EXPECT_EQ(ElfVerdict(i), "elf_no_export");
  i = Image{};
  i.export_value = 0x1400;
  EXPECT_EQ(ElfVerdict(i), "elf_export_not_code");
}

}  // namespace
