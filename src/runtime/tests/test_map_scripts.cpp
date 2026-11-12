// CDN_PC PC6: the map script provider (runtime/content/script_provider.h), the PE rules it applies
// (core/script_image_rules.h) and the resource-set script waiver. Every DLL here is a minimal PE32+ image this test
// builds byte by byte; every "stock" script and cache file is written in the test's own temporary folder. Nothing of the
// game's is read.
#include <winsock2.h>
#include <windows.h>

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/script_image_rules.h"
#include "runtime/content/content_pack.h"
#include "runtime/content/script_provider.h"
#include "runtime/hook/dll_load_hook.h"
#include "runtime/host/file_digest.h"

namespace nevr_test_waiver {
using Holds = bool (*)(const std::string& owner, std::uint64_t name);
}
namespace {
nevr_test_waiver::Holds g_waiver = nullptr;
}

// PC3's waiver setter (runtime/patch/resource_set_registry.h), recorded so the test sees what SetPolicy hands it.
namespace nevr_resource_sets {
void SetScriptWaiver(bool (*holds)(const std::string& owner, std::uint64_t name)) { g_waiver = holds; }
}

namespace {

using nevr_script_image::CheckPeScript;
namespace maps = nevr_map_scripts;

constexpr std::uint64_t kHash = 0x1234abcd5678ef90ULL;
constexpr std::uint64_t kOtherHash = 0x0fedcba987654321ULL;
constexpr std::uint64_t kStockHash = 0x190ed14fe6c74dd3ULL;

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

void Put16(std::vector<std::uint8_t>* b, std::size_t at, std::uint16_t v) {
  (*b)[at] = static_cast<std::uint8_t>(v);
  (*b)[at + 1] = static_cast<std::uint8_t>(v >> 8);
}

void Put32(std::vector<std::uint8_t>* b, std::size_t at, std::uint32_t v) {
  for (int i = 0; i < 4; ++i) (*b)[at + i] = static_cast<std::uint8_t>(v >> (8 * i));
}

void Put64(std::vector<std::uint8_t>* b, std::size_t at, std::uint64_t v) {
  for (int i = 0; i < 8; ++i) (*b)[at + i] = static_cast<std::uint8_t>(v >> (8 * i));
}

void PutSection(std::vector<std::uint8_t>* b, int index, const char* name, std::uint32_t rva, std::uint32_t file,
                std::uint32_t flags) {
  const std::size_t at = kSections + 40u * index;
  std::memcpy(b->data() + at, name, std::strlen(name));
  Put32(b, at + 8, 0x200);  // VirtualSize
  Put32(b, at + 12, rva);
  Put32(b, at + 16, 0x200);  // SizeOfRawData
  Put32(b, at + 20, file);
  Put32(b, at + 36, flags);
}

std::vector<std::uint8_t> BuildDll(const Shape& s = Shape{}) {
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

std::string Verdict(const std::vector<std::uint8_t>& image) { return CheckPeScript(image.data(), image.size()).reason; }

std::string Sha(const std::vector<std::uint8_t>& bytes) { return nevr_host::Sha256Hex(bytes); }

std::wstring Hash16W(std::uint64_t hash) {
  wchar_t buf[17];
  swprintf(buf, 17, L"%016llx", static_cast<unsigned long long>(hash));
  return buf;
}

TEST(ScriptImageRules, TheStockShapePasses) {
  const nevr_script_image::ImageVerdict v = nevr_script_image::CheckPeScript(BuildDll().data(), BuildDll().size());
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

std::string Narrow(const std::wstring& w) {
  std::string out;
  for (wchar_t c : w) out += static_cast<char>(c);
  return out;
}

void MakeDirs(const std::wstring& path) {
  for (std::size_t i = 3; i <= path.size(); ++i) {
    if (i == path.size() || path[i] == L'\\') CreateDirectoryW(path.substr(0, i).c_str(), nullptr);
  }
}

void WriteBytes(const std::wstring& path, const std::vector<std::uint8_t>& bytes) {
  MakeDirs(path.substr(0, path.find_last_of(L'\\')));
  HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  ASSERT_NE(h, INVALID_HANDLE_VALUE) << Narrow(path);
  DWORD wrote = 0;
  ASSERT_TRUE(WriteFile(h, bytes.data(), static_cast<DWORD>(bytes.size()), &wrote, nullptr));
  CloseHandle(h);
}

bool SameFile(const std::wstring& a, const std::wstring& b) {
  BY_HANDLE_FILE_INFORMATION ia{}, ib{};
  HANDLE ha = CreateFileW(a.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
  HANDLE hb = CreateFileW(b.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
  const bool ok = ha != INVALID_HANDLE_VALUE && hb != INVALID_HANDLE_VALUE && GetFileInformationByHandle(ha, &ia) &&
                  GetFileInformationByHandle(hb, &ib) && ia.dwVolumeSerialNumber == ib.dwVolumeSerialNumber &&
                  ia.nFileIndexHigh == ib.nFileIndexHigh && ia.nFileIndexLow == ib.nFileIndexLow;
  if (ha != INVALID_HANDLE_VALUE) CloseHandle(ha);
  if (hb != INVALID_HANDLE_VALUE) CloseHandle(hb);
  return ok;
}

std::wstring g_packScript;
bool FakePackCarries(const wchar_t* lower_path) { return g_packScript == lower_path; }

class MapScripts : public ::testing::Test {
 protected:
  void SetUp() override {
    maps::Forget();
    wchar_t tmp[MAX_PATH];
    ASSERT_NE(GetTempPathW(MAX_PATH, tmp), 0u);
    base_ = std::wstring(tmp) + L"nevr-map-scripts-test-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
            std::to_wstring(GetTickCount64());
    exe_dir_ = base_ + L"\\echovr\\bin\\win10";
    MakeDirs(exe_dir_ + L"\\scripts");
    cache_ = base_ + L"\\maps\\arena1";
    g_packScript.clear();
    Configure();
  }
  void TearDown() override {
    maps::Forget();
    for (HANDLE h : kept_) CloseHandle(h);
    RemoveTree(base_);
  }

  void Configure(bool signed_only = false, bool server = false, std::vector<std::string> allow = {}) {
    maps::Inputs in;
    in.exe_dir = exe_dir_;
    in.signed_only = signed_only;
    in.server = server;
    in.server_allow = std::move(allow);
    in.pack_carries = &FakePackCarries;
    maps::Configure(in);
  }

  // A cache file holding `bytes`, opened as the maps channel holds it: read access, read sharing only.
  std::int64_t Held(const std::wstring& name, const std::vector<std::uint8_t>& bytes) {
    const std::wstring path = cache_ + L"\\" + name;
    WriteBytes(path, bytes);
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    EXPECT_NE(h, INVALID_HANDLE_VALUE);
    return static_cast<std::int64_t>(reinterpret_cast<std::intptr_t>(h));
  }
  // A handle a refusal left with the caller, closed at the end.
  void Keep(std::int64_t handle) { kept_.push_back(reinterpret_cast<HANDLE>(static_cast<std::intptr_t>(handle))); }

  maps::Outcome RegisterImage(const std::vector<std::uint8_t>& image, std::uint64_t hash = kHash,
                              const std::string& owner = "map:arena1") {
    const std::int64_t h = Held(Hash16W(hash) + L"-" + std::to_wstring(++files_) + L".dll", image);
    const maps::Outcome o = maps::Register(owner, hash, h, Sha(image), true);
    if (!o.ok()) Keep(h);
    return o;
  }

  std::wstring GamePath(std::uint64_t hash) const { return exe_dir_ + L"\\scripts\\" + Hash16W(hash) + L".dll"; }

  static std::wstring Lower(std::wstring s) {
    for (wchar_t& c : s) {
      if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
    }
    return s;
  }

  static void RemoveTree(const std::wstring& dir) {
    // Only a full path below a drive root: an empty one turns L"\\*" into the current drive's root, which is
    // how a failed SetUp wiped D:\b on 2026-11-12.
    if (dir.size() < 4 || dir[1] != L':' || dir[2] != L'\\') return;
    WIN32_FIND_DATAW found;
    HANDLE find = FindFirstFileW((dir + L"\\*").c_str(), &found);
    if (find != INVALID_HANDLE_VALUE) {
      do {
        const std::wstring name = found.cFileName;
        if (name == L"." || name == L"..") continue;
        const std::wstring path = dir + L"\\" + name;
        if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
          RemoveTree(path);
        } else {
          DeleteFileW(path.c_str());
        }
      } while (FindNextFileW(find, &found));
      FindClose(find);
    }
    RemoveDirectoryW(dir.c_str());
  }

  std::wstring base_, exe_dir_, cache_;
  std::vector<HANDLE> kept_;
  int files_ = 0;
};

TEST_F(MapScripts, AHeldScriptIsServedForTheCanonicalPathInAnyCaseAndEitherSlash) {
  ASSERT_TRUE(RegisterImage(BuildDll()).ok());
  EXPECT_TRUE(maps::Holds("map:arena1", kHash));
  EXPECT_FALSE(maps::Holds("map:other", kHash));
  EXPECT_FALSE(maps::Holds("map:arena1", kOtherHash));

  std::wstring load;
  std::string owner;
  std::uint64_t hash = 0;
  ASSERT_TRUE(maps::Rewrite(Lower(GamePath(kHash)).c_str(), &load, &owner, &hash));
  EXPECT_EQ(owner, "map:arena1");
  EXPECT_EQ(hash, kHash);
  EXPECT_TRUE(SameFile(load, cache_ + L"\\" + Hash16W(kHash) + L"-1.dll")) << Narrow(load);

  std::wstring upper = GamePath(kHash);
  for (wchar_t& c : upper) {
    if (c >= L'a' && c <= L'z') c = static_cast<wchar_t>(c - L'a' + L'A');
    if (c == L'\\') c = L'/';
  }
  std::wstring again;
  EXPECT_TRUE(maps::Rewrite(upper.c_str(), &again, nullptr, nullptr));
  EXPECT_EQ(again, load);
  EXPECT_TRUE(maps::Rewrite((exe_dir_ + L"\\scripts\\.\\" + Hash16W(kHash) + L".dll").c_str(), &again, nullptr, nullptr));
}

TEST_F(MapScripts, APathThatIsNotTheCanonicalKeyIsNotServed) {
  ASSERT_TRUE(RegisterImage(BuildDll()).ok());
  const std::wstring h = Hash16W(kHash);
  const std::wstring not_keys[] = {
      exe_dir_ + L"\\scripts\\sub\\" + h + L".dll",
      exe_dir_ + L"\\" + h + L".dll",
      base_ + L"\\other\\scripts\\" + h + L".dll",
      exe_dir_ + L"\\scripts\\" + h + L".dll.bak",
      exe_dir_ + L"\\scripts\\" + h,
      h + L".dll",
      exe_dir_ + L"\\scripts\\" + Hash16W(kOtherHash) + L".dll",
  };
  for (const std::wstring& path : not_keys) {
    std::wstring load;
    EXPECT_FALSE(maps::Rewrite(Lower(path).c_str(), &load, nullptr, nullptr)) << Narrow(path);
    EXPECT_TRUE(load.empty());
  }
}

TEST_F(MapScripts, TheServedFileLoadsRunsNoCodeAndReloadsAfterAFreeFromTheSameHeldFile) {
  ASSERT_TRUE(RegisterImage(BuildDll()).ok());
  std::wstring load;
  ASSERT_TRUE(maps::Rewrite(Lower(GamePath(kHash)).c_str(), &load, nullptr, nullptr));
  for (int round = 0; round < 2; ++round) {
    HMODULE module = LoadLibraryExW(load.c_str(), nullptr, 0);
    ASSERT_NE(module, nullptr) << "error=" << GetLastError();
    EXPECT_NE(GetProcAddress(module, "setup_bindings"), nullptr);
    EXPECT_TRUE(FreeLibrary(module));
    std::wstring reload;
    ASSERT_TRUE(maps::Rewrite(Lower(GamePath(kHash)).c_str(), &reload, nullptr, nullptr));
    EXPECT_EQ(reload, load);
  }
  // Held with read sharing only: the file cannot be opened for writing or deleted while it is served.
  HANDLE w = CreateFileW(load.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                         OPEN_EXISTING, 0, nullptr);
  EXPECT_EQ(w, INVALID_HANDLE_VALUE);
  if (w != INVALID_HANDLE_VALUE) CloseHandle(w);
  EXPECT_FALSE(DeleteFileW(load.c_str()));
}

TEST_F(MapScripts, AnImageThatBreaksARuleIsRefusedWithItsTokenAndTheCallerKeepsTheHandle) {
  Shape import;
  import.nonempty_directory = 1;
  Shape forwarded;
  forwarded.function_rva = kRdataRva + 0x50;
  Shape second;
  second.functions = 2;
  second.names = 2;
  Shape wx;
  wx.text_flags |= kScnWrite;
  Shape entry;
  entry.entry = kTextRva;
  const std::pair<const char*, Shape> cases[] = {
      {"pe_imports", import}, {"pe_export_forwarded", forwarded}, {"pe_export_count", second},
      {"pe_section_wx", wx},  {"pe_entry_point", entry}};
  for (const auto& [token, shape] : cases) {
    EXPECT_EQ(RegisterImage(BuildDll(shape)).reason, token);
  }
  EXPECT_EQ(maps::HeldCount(), 0u);
  for (HANDLE h : kept_) {
    LARGE_INTEGER size{};
    EXPECT_TRUE(GetFileSizeEx(h, &size)) << "a refused handle was closed";
  }
  std::wstring load;
  EXPECT_FALSE(maps::Rewrite(Lower(GamePath(kHash)).c_str(), &load, nullptr, nullptr));
}

TEST_F(MapScripts, AStockHashAPackHashAndADuplicateAreRefused) {
  WriteBytes(GamePath(kStockHash), {1, 2, 3});
  EXPECT_EQ(RegisterImage(BuildDll(), kStockHash).reason, "stock_hash");

  g_packScript = Lower(GamePath(kOtherHash));
  EXPECT_EQ(RegisterImage(BuildDll(), kOtherHash).reason, "pack_hash");

  ASSERT_TRUE(RegisterImage(BuildDll(), kHash, "map:arena1").ok());
  EXPECT_EQ(RegisterImage(BuildDll(), kHash, "map:arena2").reason, "duplicate");
  EXPECT_EQ(RegisterImage(BuildDll(), kHash, "map:arena1").reason, "duplicate");
  EXPECT_TRUE(maps::Holds("map:arena1", kHash));
  EXPECT_FALSE(maps::Holds("map:arena2", kHash));
  EXPECT_EQ(maps::HeldCount(), 1u);
}

TEST_F(MapScripts, ARevokedHashIsRefusedAndOneAlreadyHeldIsDroppedBeforeItLoads) {
  const std::vector<std::uint8_t> image = BuildDll();
  maps::SetRevoked({Sha(image)});
  EXPECT_EQ(RegisterImage(image).reason, "revoked");

  maps::Forget();
  Configure();
  ASSERT_TRUE(RegisterImage(image).ok());
  maps::SetRevoked({std::string(64, '0')});
  EXPECT_EQ(maps::HeldCount(), 1u);
  maps::SetRevoked({Sha(image)});
  EXPECT_EQ(maps::HeldCount(), 0u);
  EXPECT_FALSE(maps::Holds("map:arena1", kHash));
  std::wstring load;
  EXPECT_FALSE(maps::Rewrite(Lower(GamePath(kHash)).c_str(), &load, nullptr, nullptr));
}

TEST_F(MapScripts, ASignedOnlyRunAndAServerOutsideItsAllowListRefuse) {
  Configure(/*signed_only=*/true);
  EXPECT_EQ(RegisterImage(BuildDll()).reason, "signed_only");
  Configure(false, /*server=*/true, {"arena2"});
  EXPECT_EQ(RegisterImage(BuildDll(), kHash, "map:arena1").reason, "server_not_allowed");
  EXPECT_TRUE(RegisterImage(BuildDll(), kHash, "map:arena2").ok());
}

TEST_F(MapScripts, AFileWhoseBytesAreNotItsDigestAndBadClaimsAreRefused) {
  const std::vector<std::uint8_t> image = BuildDll();
  const std::int64_t h = Held(L"x.dll", image);
  Keep(h);
  EXPECT_EQ(maps::Register("map:arena1", kHash, h, std::string(64, 'a'), true).reason, "sha_mismatch");
  EXPECT_EQ(maps::Register("map:arena1", kHash, h, "ABC", true).reason, "bad_sha256");
  EXPECT_EQ(maps::Register("plugin-x", kHash, h, Sha(image), true).reason, "bad_owner");
  EXPECT_EQ(maps::Register("map:", kHash, h, Sha(image), true).reason, "bad_owner");
  EXPECT_EQ(maps::Register("map:arena1", kHash, -1, Sha(image), true).reason, "unreadable");
  EXPECT_EQ(RegisterImage({}).reason, "bad_size");
  maps::Forget();
  EXPECT_EQ(maps::Register("map:arena1", kHash, h, Sha(image), true).reason, "not_configured");
}

TEST_F(MapScripts, ARekeyServesThePlayersOwnStockFileAndRefusesAWrongDigest) {
  const std::vector<std::uint8_t> stock = BuildDll();
  WriteBytes(GamePath(kStockHash), stock);
  EXPECT_EQ(maps::RegisterRekey("map:arena1", kHash, kStockHash, std::string(64, 'b')).reason, "stock_sha_mismatch");
  EXPECT_EQ(maps::RegisterRekey("map:arena1", kHash, kOtherHash, Sha(stock)).reason, "stock_missing");
  EXPECT_EQ(maps::RegisterRekey("map:arena1", kStockHash, kStockHash, Sha(stock)).reason, "stock_hash");
  ASSERT_TRUE(maps::RegisterRekey("map:arena1", kHash, kStockHash, Sha(stock)).ok());
  std::wstring load;
  ASSERT_TRUE(maps::Rewrite(Lower(GamePath(kHash)).c_str(), &load, nullptr, nullptr));
  EXPECT_TRUE(SameFile(load, GamePath(kStockHash)));
  EXPECT_TRUE(maps::Holds("map:arena1", kHash));
}

// Holds is the resource-set registry's script waiver (PC3's SetScriptWaiver): true only for the owner the provider holds
// the hash for, so a plugin's set, another map's set and an unheld hash keep the registry's kScript refusal.
TEST_F(MapScripts, TheWaiverAnswersOnlyForTheOwnerTheScriptIsHeldFor) {
  EXPECT_FALSE(maps::Holds("map:arena1", kHash));
  ASSERT_TRUE(RegisterImage(BuildDll()).ok());
  EXPECT_TRUE(maps::Holds("map:arena1", kHash));
  EXPECT_FALSE(maps::Holds("plugin-x", kHash));
  EXPECT_FALSE(maps::Holds("map:arena2", kHash));
  EXPECT_FALSE(maps::Holds("map:arena1", kOtherHash));
  maps::Forget();
  EXPECT_FALSE(maps::Holds("map:arena1", kHash));
}

// Withdraw is the mount's all or nothing (CDN_DESIGN finding 2): a map whose set is refused keeps no script live.
TEST_F(MapScripts, WithdrawDropsEveryScriptAndRekeyOfTheOwnerAndKeepsAnotherOwners) {
  constexpr std::uint64_t kThirdHash = 0x5555aaaa5555aaaaULL;
  const std::vector<std::uint8_t> stock = BuildDll();
  WriteBytes(GamePath(kStockHash), stock);
  ASSERT_TRUE(RegisterImage(BuildDll()).ok());
  ASSERT_TRUE(maps::RegisterRekey("map:arena1", kOtherHash, kStockHash, Sha(stock)).ok());
  ASSERT_TRUE(RegisterImage(BuildDll(), kThirdHash, "map:arena2").ok());
  const std::wstring held = cache_ + L"\\" + Hash16W(kHash) + L"-1.dll";
  EXPECT_FALSE(DeleteFileW(held.c_str()));

  EXPECT_EQ(maps::Withdraw("map:arena1"), 2u);
  EXPECT_EQ(maps::HeldCount(), 1u);
  std::wstring load;
  EXPECT_FALSE(maps::Rewrite(Lower(GamePath(kHash)).c_str(), &load, nullptr, nullptr));
  EXPECT_FALSE(maps::Rewrite(Lower(GamePath(kOtherHash)).c_str(), &load, nullptr, nullptr));
  EXPECT_TRUE(DeleteFileW(held.c_str())) << "the withdrawn script's handle is closed";
  EXPECT_TRUE(maps::Holds("map:arena2", kThirdHash));
  EXPECT_TRUE(maps::Rewrite(Lower(GamePath(kThirdHash)).c_str(), &load, nullptr, nullptr));
  EXPECT_EQ(maps::Withdraw("map:arena1"), 0u);
  EXPECT_EQ(maps::Withdraw("map:nothing"), 0u);
  EXPECT_EQ(maps::HeldCount(), 1u);
}

TEST_F(MapScripts, WithdrawAfterTheRewriterServedAScriptRefusesAndDropsNothing) {
  ASSERT_TRUE(RegisterImage(BuildDll()).ok());
  ASSERT_TRUE(RegisterImage(BuildDll(), kOtherHash).ok());
  ASSERT_TRUE(RegisterImage(BuildDll(), kStockHash, "map:arena2").ok());
  std::wstring load;
  ASSERT_TRUE(maps::Rewrite(Lower(GamePath(kOtherHash)).c_str(), &load, nullptr, nullptr));

  EXPECT_EQ(maps::Withdraw("map:arena1"), 0u);
  EXPECT_EQ(maps::HeldCount(), 3u);
  EXPECT_TRUE(maps::Holds("map:arena1", kHash));
  EXPECT_TRUE(maps::Rewrite(Lower(GamePath(kHash)).c_str(), &load, nullptr, nullptr));
  EXPECT_TRUE(maps::Rewrite(Lower(GamePath(kOtherHash)).c_str(), &load, nullptr, nullptr));
  EXPECT_EQ(maps::Withdraw("map:arena2"), 1u) << "another owner's served script does not hold this one";
}

TEST_F(MapScripts, HoldsIsFalseAfterWithdrawAndTheOwnerIsNotTakenAgain) {
  ASSERT_TRUE(RegisterImage(BuildDll()).ok());
  EXPECT_TRUE(maps::Holds("map:arena1", kHash));
  EXPECT_EQ(maps::Withdraw("map:arena1"), 1u);
  EXPECT_FALSE(maps::Holds("map:arena1", kHash));
  EXPECT_EQ(RegisterImage(BuildDll()).reason, "withdrawn");
  EXPECT_EQ(RegisterImage(BuildDll(), kOtherHash).reason, "withdrawn");
  EXPECT_FALSE(maps::Holds("map:arena1", kHash));
  EXPECT_TRUE(RegisterImage(BuildDll(), kHash, "map:arena2").ok()) << "the hash is free for another map";
  maps::Forget();
  Configure();
  EXPECT_TRUE(RegisterImage(BuildDll(), kOtherHash).ok()) << "Forget clears the withdrawn owners";
}

// A rekeyed script PC10 rebuilt sits at <version dir>\scripts\<hash16>.dll in the maps cache (map_derive.cpp).
std::wstring RebuiltName(std::uint64_t hash) { return L"v1\\scripts\\" + Hash16W(hash) + L".dll"; }
const std::string kStockSha = std::string(64, 'c');

TEST_F(MapScripts, ADerivedScriptIsHeldAndServedFromItsRebuiltFile) {
  const std::vector<std::uint8_t> image = BuildDll();
  const std::int64_t h = Held(RebuiltName(kHash), image);
  ASSERT_TRUE(maps::RegisterDerived("map:arena1", kHash, h, Sha(image), kStockSha).ok());
  std::wstring load;
  std::string owner;
  ASSERT_TRUE(maps::Rewrite(Lower(GamePath(kHash)).c_str(), &load, &owner, nullptr));
  EXPECT_TRUE(SameFile(load, cache_ + L"\\" + RebuiltName(kHash)));
  EXPECT_EQ(owner, "map:arena1");
  EXPECT_TRUE(maps::Holds("map:arena1", kHash));
  EXPECT_FALSE(DeleteFileW((cache_ + L"\\" + RebuiltName(kHash)).c_str())) << "the provider holds the handle";
}

// Must-fail: the boot's stamp check does not stand for the hash here; the recipe's result digest is always checked.
TEST_F(MapScripts, ADerivedScriptWhoseBytesAreNotTheResultDigestIsRefused) {
  const std::vector<std::uint8_t> image = BuildDll();
  const std::int64_t h = Held(RebuiltName(kHash), image);
  Keep(h);
  EXPECT_EQ(maps::RegisterDerived("map:arena1", kHash, h, std::string(64, 'a'), kStockSha).reason, "sha_mismatch");
  EXPECT_EQ(maps::RegisterDerived("map:arena1", kHash, h, Sha(image), "c0ffee").reason, "bad_sha256");
  EXPECT_EQ(maps::RegisterDerived("map:arena1", kHash, h, Sha(image), "").reason, "bad_sha256");
  EXPECT_EQ(maps::HeldCount(), 0u);
  LARGE_INTEGER size{};
  EXPECT_TRUE(GetFileSizeEx(reinterpret_cast<HANDLE>(static_cast<std::intptr_t>(h)), &size)) << "the caller keeps it";
}

// Must-fail: a rebuilt DLL passes the same PE image rules as a downloaded one, and keeps the stock and pack rules.
TEST_F(MapScripts, ADerivedScriptThatBreaksAnImageRuleOrTakesAStockHashIsRefused) {
  Shape import;
  import.nonempty_directory = 1;
  Shape entry;
  entry.entry = kTextRva;
  Shape wx;
  wx.text_flags |= kScnWrite;
  const std::pair<const char*, Shape> cases[] = {{"pe_imports", import}, {"pe_entry_point", entry}, {"pe_section_wx", wx}};
  for (const auto& [token, shape] : cases) {
    const std::vector<std::uint8_t> image = BuildDll(shape);
    const std::int64_t h = Held(RebuiltName(kHash), image);
    Keep(h);
    EXPECT_EQ(maps::RegisterDerived("map:arena1", kHash, h, Sha(image), kStockSha).reason, token);
    CloseHandle(reinterpret_cast<HANDLE>(static_cast<std::intptr_t>(h)));
    kept_.pop_back();
  }
  const std::vector<std::uint8_t> image = BuildDll();
  WriteBytes(GamePath(kStockHash), image);
  const std::int64_t stock = Held(RebuiltName(kStockHash), image);
  Keep(stock);
  EXPECT_EQ(maps::RegisterDerived("map:arena1", kStockHash, stock, Sha(image), kStockSha).reason, "stock_hash");
  g_packScript = Lower(GamePath(kOtherHash));
  const std::int64_t pack = Held(RebuiltName(kOtherHash), image);
  Keep(pack);
  EXPECT_EQ(maps::RegisterDerived("map:arena1", kOtherHash, pack, Sha(image), kStockSha).reason, "pack_hash");
  EXPECT_EQ(maps::HeldCount(), 0u);
}

// Must-fail: the held file must be the one PC10 wrote for this hash.
TEST_F(MapScripts, ADerivedScriptHeldFromAnotherPathIsRefused) {
  const std::vector<std::uint8_t> image = BuildDll();
  for (const std::wstring& name : {std::wstring(L"v1\\" + Hash16W(kHash) + L".dll"), RebuiltName(kOtherHash),
                                   std::wstring(L"v1\\scripts\\x" + Hash16W(kHash) + L".dll")}) {
    const std::int64_t h = Held(name, image);
    Keep(h);
    EXPECT_EQ(maps::RegisterDerived("map:arena1", kHash, h, Sha(image), kStockSha).reason, "derived_path") << Narrow(name);
  }
  EXPECT_TRUE(maps::Register("map:arena1", kHash, Held(L"v1\\" + Hash16W(kHash) + L"-own.dll", image), Sha(image), true).ok())
      << "a downloaded script has no path rule";
}

TEST_F(MapScripts, WithdrawDropsTheOwnersDerivedScriptsWithItsOthers) {
  const std::vector<std::uint8_t> image = BuildDll();
  ASSERT_TRUE(maps::RegisterDerived("map:arena1", kHash, Held(RebuiltName(kHash), image), Sha(image), kStockSha).ok());
  ASSERT_TRUE(RegisterImage(BuildDll(), kOtherHash).ok());
  EXPECT_EQ(maps::Withdraw("map:arena1"), 2u);
  EXPECT_EQ(maps::HeldCount(), 0u);
  std::wstring load;
  EXPECT_FALSE(maps::Rewrite(Lower(GamePath(kHash)).c_str(), &load, nullptr, nullptr));
  EXPECT_FALSE(maps::Holds("map:arena1", kHash));
  EXPECT_TRUE(DeleteFileW((cache_ + L"\\" + RebuiltName(kHash)).c_str())) << "the derived script's handle is closed";
  const std::int64_t again = Held(RebuiltName(kHash), image);
  Keep(again);
  EXPECT_EQ(maps::RegisterDerived("map:arena1", kHash, again, Sha(image), kStockSha).reason, "withdrawn");
}

std::string ShaText(const std::string& text) { return Sha(std::vector<std::uint8_t>(text.begin(), text.end())); }

// A content pack in the fixture's folders: one manifest it replaces and one script, `pack_script`, the game has no
// file for (content_pack_test's shape).
void MakePack(const std::wstring& base, std::uint64_t pack_script, const std::vector<std::uint8_t>& script) {
  const std::wstring data = base + L"\\echovr\\_data\\5932408047\\rad15\\win10";
  const std::string stock_manifest = "stock manifest bytes", manifest = "pack manifest";
  WriteBytes(data + L"\\manifests\\48037dc70b0ecab2", {stock_manifest.begin(), stock_manifest.end()});
  const std::wstring dir = base + L"\\root\\content\\testpack";
  WriteBytes(dir + L"\\manifests\\48037dc70b0ecab2", {manifest.begin(), manifest.end()});
  const std::wstring leaf = Hash16W(pack_script) + L".dll";
  WriteBytes(dir + L"\\scripts\\" + leaf, script);
  const std::string rel = "scripts/" + Narrow(leaf);
  nlohmann::json j = {{"format", 1}, {"id", "testpack"}, {"name", "Test"}, {"version", "1.0.0"},
                      {"data_dir", "_data/5932408047/rad15/win10"}};
  j["manifests"] = nlohmann::json::array({{{"package", "48037dc70b0ecab2"}, {"stock_sha256", ShaText(stock_manifest)},
                                           {"stock_packages", 1},{"file", "manifests/48037dc70b0ecab2"},
                                           {"packages", nlohmann::json::array()}}});
  j["files"] = {{"manifests/48037dc70b0ecab2", ShaText(manifest)}, {rel, Sha(script)}};
  j["scripts"] = {rel};
  const std::string text = j.dump();
  WriteBytes(dir + L"\\content.json", {text.begin(), text.end()});
}

// Runs before the lazy-install test, so a SetPolicy that added the rewriter itself fails that test's first check.
TEST_F(MapScripts, SetPolicyHandsHoldsToTheWaiverAndAppliesTheChannelsFacts) {
  g_waiver = nullptr;
  maps::SetPolicy(/*is_server=*/true, {"arena2"}, /*signed_only=*/false);
  EXPECT_EQ(g_waiver, &maps::Holds);
  EXPECT_EQ(RegisterImage(BuildDll(), kHash, "map:arena1").reason, "server_not_allowed");
  maps::SetPolicy(false, {}, /*signed_only=*/true);
  EXPECT_EQ(RegisterImage(BuildDll(), kHash, "map:arena1").reason, "signed_only");
}

// The rewriters are process-wide and never removed, so this one test owns the whole order: the content pack's first, as
// content_pack_install.cpp registers it at boot, then the map rewriter, which exists only once InstallMapScriptsRewriter
// is called after a granted Register.
TEST_F(MapScripts, TheMapRewriterIsAddedLazilyAfterThePacksAndThePackKeepsItsOwnScripts) {
  const std::vector<std::uint8_t> pack_script = BuildDll();
  MakePack(base_, kOtherHash, pack_script);
  nevr_content::Inputs pack_in;
  pack_in.content_dir = base_ + L"\\root\\content";
  pack_in.game_dir = base_ + L"\\echovr";
  pack_in.exe_dir = exe_dir_;
  const nevr_content::Decision d = nevr_content::Decide(pack_in);
  ASSERT_EQ(d.outcome, nevr_content::Outcome::kServed) << d.reason << " " << d.detail;
  nevr_dll_load_hook::AddLoadRewriter("content-pack", &nevr_content::RewriteScript);

  maps::Inputs in;
  in.exe_dir = exe_dir_;
  in.pack_carries = &nevr_content::CarriesScript;
  maps::Configure(in);
  EXPECT_EQ(RegisterImage(BuildDll(), kOtherHash).reason, "pack_hash");
  ASSERT_TRUE(RegisterImage(BuildDll(), kHash).ok());

  std::wstring load;
  EXPECT_FALSE(nevr_dll_load_hook::RewriteLoad(Lower(GamePath(kHash)).c_str(), &load)) << "rewriter added before install";
  ASSERT_TRUE(maps::InstallMapScriptsRewriter());
  ASSERT_TRUE(maps::InstallMapScriptsRewriter());
  ASSERT_TRUE(nevr_dll_load_hook::RewriteLoad(Lower(GamePath(kHash)).c_str(), &load));
  EXPECT_TRUE(SameFile(load, cache_ + L"\\" + Hash16W(kHash) + L"-2.dll")) << Narrow(load);

  std::wstring pack_load;
  ASSERT_TRUE(nevr_dll_load_hook::RewriteLoad(Lower(GamePath(kOtherHash)).c_str(), &pack_load));
  EXPECT_TRUE(SameFile(pack_load, base_ + L"\\root\\content\\testpack\\scripts\\" + Hash16W(kOtherHash) + L".dll"))
      << Narrow(pack_load);
  nevr_content::Forget();
}

TEST(MapScriptsPath, TheCanonicalKeyIsTheGamesPathFolded) {
  EXPECT_EQ(maps::CanonicalScriptPath(L"C:\\Games\\EchoVR\\bin\\win10", 0x00ab0000000000cdULL),
            L"c:\\games\\echovr\\bin\\win10\\scripts\\00ab0000000000cd.dll");
  EXPECT_EQ(maps::CanonicalScriptPath(L"C:/Games/EchoVR/bin/win10/", 1),
            L"c:\\games\\echovr\\bin\\win10\\scripts\\0000000000000001.dll");
}

}  // namespace
