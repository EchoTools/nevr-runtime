// Map scripts on Quest (docs/adr/0048 decision 6): the ELF rules (one must-fail per rule), the map-script table, Register
// against a temporary library directory, and that the stock redirect's answers do not change.
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "core/script_image_rules.h"
#include "hook_log.h"
#include "quest/data/sha256.h"
#include "quest/plugin/script_redirect.h"
#include "quest/tests/test_check.h"

namespace {

namespace sr = nevr_quest::script_redirect;
namespace ms = nevr_quest::script_redirect::map_scripts;
using nevr_script_image::CheckElfScript;

std::string g_lines;
sentinel::LogLevel g_level = sentinel::LogLevel::kInfo;  // the level of the last record the sink received
void Capture(sentinel::LogLevel level, const char* line) {
  g_level = level;
  g_lines += line;
  g_lines += '\n';
}

void Put(std::vector<std::uint8_t>& b, std::size_t at, std::uint64_t value, int bytes) {
  if (b.size() < at + bytes) b.resize(at + bytes);
  for (int i = 0; i < bytes; ++i) b[at + i] = static_cast<std::uint8_t>(value >> (8 * i));
}

// A minimal stock-shaped script: one RX load at 0 (file 0..0x1000), one RW load holding the dynamic table, a DT_HASH with
// two symbols, the string table "\0libc.so\0setup_bindings\0".
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

std::string Verdict(Image image) {
  const std::vector<std::uint8_t> b = image.Build();
  return CheckElfScript(b.data(), b.size()).reason;
}

void ElfRules() {
  QCHECK(Verdict(Image{}) == "ok");
  { Image i; std::vector<std::uint8_t> b = i.Build(); QCHECK(CheckElfScript(b.data(), 40).reason == "elf_truncated"); }
  { std::vector<std::uint8_t> b = Image{}.Build(); b[1] = 'X'; QCHECK(CheckElfScript(b.data(), b.size()).reason == "elf_not_elf"); }
  { std::vector<std::uint8_t> b = Image{}.Build(); b[4] = 1; QCHECK(CheckElfScript(b.data(), b.size()).reason == "elf_not_64"); }
  { std::vector<std::uint8_t> b = Image{}.Build(); b[5] = 2; QCHECK(CheckElfScript(b.data(), b.size()).reason == "elf_not_le"); }
  { Image i; i.type = 2; QCHECK(Verdict(i) == "elf_not_dyn"); }
  { Image i; i.machine = 62; QCHECK(Verdict(i) == "elf_not_aarch64"); }
  { Image i; i.data_flags = 7; QCHECK(Verdict(i) == "elf_segment_wx"); }
  { std::vector<std::uint8_t> b = Image{}.Build(); Put(b, 64 + 112, 4, 4); QCHECK(CheckElfScript(b.data(), b.size()).reason == "elf_no_dynamic"); }
  { Image i; i.dynamic.push_back({22, 0}); QCHECK(Verdict(i) == "elf_textrel"); }
  { Image i; i.dynamic.push_back({30, 4}); QCHECK(Verdict(i) == "elf_textrel"); }
  { Image i; i.dynamic.erase(i.dynamic.begin() + 1); QCHECK(Verdict(i) == "elf_no_symbols"); }
  { Image i; i.needed = "liblog.s"; QCHECK(Verdict(i) == "elf_needed"); }
  { Image i; i.dynamic.push_back({12, 0x400}); QCHECK(Verdict(i) == "elf_init"); }
  { Image i; i.dynamic.push_back({27, 8}); QCHECK(Verdict(i) == "elf_init"); }
  { Image i; i.dynamic.push_back({33, 8}); QCHECK(Verdict(i) == "elf_init"); }
  { Image i; i.dynamic.push_back({27, 0}); QCHECK(Verdict(i) == "ok"); }
  { Image i; i.export_name = "setup_bindingz"; QCHECK(Verdict(i) == "elf_no_export"); }
  { Image i; i.export_info = 0x11; QCHECK(Verdict(i) == "elf_no_export"); }  // an OBJECT of that name is not the export
  { Image i; i.export_value = 0x1400; QCHECK(Verdict(i) == "elf_export_not_code"); }
}

std::string MakeDir() {
  char dir[] = "/tmp/nevr_map_scripts_XXXXXX";
  QCHECK(mkdtemp(dir) != nullptr);
  return dir;
}

std::string WriteLib(const std::string& dir, const char* name, const std::vector<std::uint8_t>& bytes) {
  const std::string path = dir + "/" + name;
  FILE* f = std::fopen(path.c_str(), "wb");
  QCHECK(f != nullptr);
  std::fwrite(bytes.data(), 1, bytes.size(), f);
  std::fclose(f);
  return nevr_quest::data::Sha256Hex(bytes.data(), bytes.size());
}

constexpr std::uint64_t kMap = 0x0123456789abcdefULL;
const char kMapName[] = "lib0123456789abcdef.so";

void TableRules() {
  ms::ResetForTest();
  QCHECK(ms::Admit("map:a", kMap) == nullptr);
  QCHECK(std::strcmp(ms::Admit("map:b", kMap), "duplicate") == 0);
  QCHECK(std::strcmp(ms::Admit("map:a", 0), "bad_hash") == 0);
  QCHECK(std::strcmp(ms::Admit("", 1), "bad_owner") == 0);
  QCHECK(ms::Holds("map:a", kMap) && !ms::Holds("map:b", kMap));
  QCHECK(ms::Withdraw("map:a") == 1 && !ms::Holds("map:a", kMap) && ms::HeldCount() == 0);
  QCHECK(std::strcmp(ms::Admit("map:a", 2), "withdrawn") == 0);
  QCHECK(ms::Admit("map:b", kMap) == nullptr);  // the hash is free for another map
  g_lines.clear();
  ms::NoteLoad(kMapName);
  ms::NoteLoad(kMapName);
  QCHECK(g_lines.find("map_script_served") != std::string::npos);
  QCHECK(g_lines.find("map_script_served") == g_lines.rfind("map_script_served"));
  QCHECK(ms::Withdraw("map:b") == 0 && ms::Holds("map:b", kMap));  // loaded: never pulled
  QCHECK(g_lines.find("withdraw_after_load") != std::string::npos);

  ms::ResetForTest();
  for (std::uint64_t i = 1; i <= ms::kMaxMapScripts; ++i) QCHECK(ms::Admit("map:c", i) == nullptr);
  QCHECK(std::strcmp(ms::Admit("map:c", 999), "table_full") == 0);
  ms::ResetForTest();
}

void StockRedirectUnchanged() {
  ms::ResetForTest();
  sr::ResetForTest();
  const std::string dir = MakeDir();
  WriteLib(dir, "lib0123456789abcdef_nevr.so", {1});
  QCHECK(sr::RegisterIn(dir.c_str(), "combat", &kMap, 1) == NEVR_SCRIPT_REDIRECT_OK);
  QCHECK(std::strcmp(ms::Admit("map:a", kMap), "stock_hash") == 0);
  char buf[sr::kNameCapacity];
  QCHECK(std::strcmp(sr::RewriteLoadName(kMapName, buf, sizeof buf), "lib0123456789abcdef_nevr.so") == 0);
  g_lines.clear();
  ms::NoteLoad(kMapName);
  QCHECK(g_lines.empty());  // nothing held: the handler's extra step says nothing
  sr::ResetForTest();
}

void RegisterChecksThePackagedFile() {
  ms::ResetForTest();
  const std::string dir = MakeDir();
  const std::string sha = WriteLib(dir, kMapName, Image{}.Build());
  const std::string zeros(64, '0');
  QCHECK(std::strcmp(ms::RegisterIn(dir.c_str(), "a", kMap, sha.c_str()), "bad_owner") == 0);
  QCHECK(std::strcmp(ms::RegisterIn(dir.c_str(), "map:a", kMap, "ABC"), "bad_sha256") == 0);
  QCHECK(std::strcmp(ms::RegisterIn(dir.c_str(), "map:a", 0x1111, sha.c_str()), "missing") == 0);
  QCHECK(std::strcmp(ms::RegisterIn(dir.c_str(), "map:a", kMap, zeros.c_str()), "sha_mismatch") == 0);
  Image bad;
  bad.needed = "liblog.s";
  const std::string badSha = WriteLib(dir, "lib00000000000000aa.so", bad.Build());
  QCHECK(std::strcmp(ms::RegisterIn(dir.c_str(), "map:a", 0xaa, badSha.c_str()), "elf_needed") == 0);
  QCHECK(g_level == sentinel::LogLevel::kWarn);  // a refused script is a Warning, as on PC
  QCHECK(g_lines.find("\"reason\":\"elf_needed\"") != std::string::npos);
  // The shared rules, in the shared order: a zero hash, an empty file and an over-long owner are refused before the table.
  QCHECK(std::strcmp(ms::RegisterIn(dir.c_str(), "map:a", 0, sha.c_str()), "bad_hash") == 0);
  const std::string emptySha = WriteLib(dir, "lib0000000000000001.so", {});
  QCHECK(std::strcmp(ms::RegisterIn(dir.c_str(), "map:a", 1, emptySha.c_str()), "bad_size") == 0);
  const std::string longOwner = "map:" + std::string(ms::kOwnerCapacity, 'x');
  QCHECK(std::strcmp(ms::RegisterIn(dir.c_str(), longOwner.c_str(), kMap, sha.c_str()), "bad_owner") == 0);
  QCHECK(ms::HeldCount() == 0);
  QCHECK(ms::RegisterIn(dir.c_str(), "map:a", kMap, sha.c_str()) == nullptr);
  QCHECK(ms::Holds("map:a", kMap));
  ms::ResetForTest();
}

}  // namespace

int main() {
  sentinel::SetLogSink(&Capture);
  ElfRules();
  TableRules();
  StockRedirectUnchanged();
  RegisterChecksThePackagedFile();
  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "map_scripts_test: %d check(s) failed\n", quest_test::Failures());
    return 1;
  }
  std::printf("map_scripts_test: all checks pass\n");
  return 0;
}
