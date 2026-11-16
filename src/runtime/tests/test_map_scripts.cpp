// The map script provider (runtime/content/script_provider.h, docs/adr/0048 decision 6), the PE rules it applies
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

#include "core/logging.h"
#include "core/script_image_rules.h"
#include "runtime/content/content_pack.h"
#include "runtime/content/script_provider.h"
#include "runtime/hook/dll_load_hook.h"
#include "cdn/cdn_env.h"
#include "cdn/cdn_manifest.h"
#include "cdn/cdn_scripts.h"
#include "runtime/content/cdn_scripts_install.h"
#include "core/map_manifest.h"
#include "core/map_script_check.h"
#include "core/resource_set_registry.h"
#include "core/tests/script_image_builders.h"
#include "quest/data/sha256.h"

using namespace nevr_script_image_fixture;
using nevr_content::Narrow;

namespace nevr_test_waiver {
using Holds = bool (*)(const std::string& owner, std::uint64_t name);
}
namespace {
nevr_test_waiver::Holds g_waiver = nullptr;
}

// The resource-set registry's script waiver setter, recorded so the test sees what SetPolicy hands it.
namespace nevr_resource_sets {
void SetScriptWaiver(bool (*holds)(const std::string& owner, std::uint64_t name)) { g_waiver = holds; }
}

namespace {

using nevr_script_image::CheckPeScript;
namespace maps = nevr_map_scripts;

constexpr std::uint64_t kHash = 0x1234abcd5678ef90ULL;
constexpr std::uint64_t kOtherHash = 0x0fedcba987654321ULL;
constexpr std::uint64_t kStockHash = 0x190ed14fe6c74dd3ULL;

// The byte limit a run gives the provider: the device's share for a document (MapDocumentBytes), from a device fact.
constexpr std::uint64_t kTestMemoryAvailable = 1ULL << 30;
std::uint64_t TestByteLimit() {
  nevr_cdn::DeviceFacts facts;
  facts.have_memory = true;
  facts.memory_available = kTestMemoryAvailable;
  return nevr_maps::MapDocumentBytes(facts);
}

std::string Sha(const std::vector<std::uint8_t>& bytes) { return nevr_quest::data::Sha256Hex(bytes.data(), bytes.size()); }

std::wstring Hash16W(std::uint64_t hash) {
  wchar_t buf[17];
  swprintf(buf, 17, L"%016llx", static_cast<unsigned long long>(hash));
  return buf;
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

std::vector<std::uint8_t> ReadBytes(const std::wstring& path) {
  std::vector<std::uint8_t> bytes;
  HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return bytes;
  LARGE_INTEGER size{};
  if (GetFileSizeEx(h, &size)) {
    bytes.resize(static_cast<std::size_t>(size.QuadPart));
    DWORD got = 0;
    if (!ReadFile(h, bytes.data(), static_cast<DWORD>(bytes.size()), &got, nullptr)) bytes.clear();
  }
  CloseHandle(h);
  return bytes;
}

// The file's identity (volume and file index): a copy that was kept keeps it, one that was replaced does not.
bool FileIdentity(const std::wstring& path, BY_HANDLE_FILE_INFORMATION* out) {
  HANDLE h = CreateFileW(path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  const bool ok = GetFileInformationByHandle(h, out) != FALSE;
  CloseHandle(h);
  return ok;
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

  // The facts a run gives the provider, without applying them.
  maps::Inputs In(bool signed_only = false, std::uint64_t byte_limit = TestByteLimit()) const {
    maps::Inputs in;
    in.exe_dir = exe_dir_;
    in.signed_only = signed_only;
    in.byte_limit = byte_limit;
    in.pack_carries = &FakePackCarries;
    return in;
  }

  void Configure(bool signed_only = false) { maps::Configure(In(signed_only)); }

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
                              const std::string& owner = "map:arena1", bool official = false) {
    const std::int64_t h = Held(Hash16W(hash) + L"-" + std::to_wstring(++files_) + L".dll", image);
    const maps::Outcome o = maps::Register(owner, hash, h, Sha(image), true, 0, official);
    if (!o.ok()) Keep(h);
    return o;
  }

  // Where a rekey's copy goes: the channel's folder below a version's folder.
  std::wstring CopyDir() const { return cache_ + L"\\rekeyed"; }
  std::wstring CopyPath(std::uint64_t hash) const { return CopyDir() + L"\\scripts\\" + Hash16W(hash) + L".dll"; }

  std::wstring GamePath(std::uint64_t hash) const { return exe_dir_ + L"\\scripts\\" + Hash16W(hash) + L".dll"; }

  static std::wstring Lower(std::wstring s) {
    for (wchar_t& c : s) {
      if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
    }
    return s;
  }

  static void RemoveTree(const std::wstring& dir) {
    // Only a full path below a drive root: an empty one would turn L"\\*" into the current drive's root.
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
  maps::SetRevoked({std::string(nevr_cdn::kSha256HexChars, '0')});
  EXPECT_EQ(maps::HeldCount(), 1u);
  maps::SetRevoked({Sha(image)});
  EXPECT_EQ(maps::HeldCount(), 0u);
  EXPECT_FALSE(maps::Holds("map:arena1", kHash));
  std::wstring load;
  EXPECT_FALSE(maps::Rewrite(Lower(GamePath(kHash)).c_str(), &load, nullptr, nullptr));
}

TEST_F(MapScripts, ASignedOnlyRunRefusesAMapsScript) {
  Configure(/*signed_only=*/true);
  EXPECT_EQ(RegisterImage(BuildDll()).reason, "signed_only");
}

// There is no allow list on any run (ADR 0048 decision 18): the manifest signature is the channel's one authority, so every
// map id registers its script, on a dedicated server as on a client.
TEST_F(MapScripts, EveryMapIdRegistersItsScriptWithNoAllowList) {
  EXPECT_TRUE(RegisterImage(BuildDll(), kHash, "map:arena1").ok());
  EXPECT_TRUE(RegisterImage(BuildDll(), kOtherHash, "map:arena2").ok());
}

// A hash of 0 names no script: refused before the file is read (ADR 0048 decision 6, the same rule the Quest table applies).
TEST_F(MapScripts, AHashOfZeroIsRefusedAsBadHash) {
  EXPECT_EQ(RegisterImage(BuildDll(), 0).reason, "bad_hash");
  EXPECT_EQ(maps::HeldCount(), 0u);
}

// Under -require-signed-plugins each refused script is one Warning line naming the token (ADR 0048 decision 7).
std::vector<std::string> g_warnings;
void CaptureWarnings(EchoVR::LogLevel level, const CHAR* message) {
  if (level == EchoVR::LogLevel::Warning) g_warnings.push_back(message);
}

TEST_F(MapScripts, ASignedOnlyRunLogsOneWarningPerRefusedScript) {
  Configure(/*signed_only=*/true);
  g_warnings.clear();
  SetBootLogSink(&CaptureWarnings);
  EXPECT_EQ(RegisterImage(BuildDll()).reason, "signed_only");
  EXPECT_EQ(RegisterImage(BuildDll(), kOtherHash).reason, "signed_only");
  SetBootLogSink(nullptr);
  ASSERT_EQ(g_warnings.size(), 2u);
  for (const std::string& line : g_warnings) {
    EXPECT_NE(line.find("[NEVR.MAPS] map script refused"), std::string::npos) << line;
    EXPECT_NE(line.find("reason=signed_only"), std::string::npos) << line;
  }
}

TEST_F(MapScripts, AFileWhoseBytesAreNotItsDigestAndBadClaimsAreRefused) {
  const std::vector<std::uint8_t> image = BuildDll();
  const std::int64_t h = Held(L"x.dll", image);
  Keep(h);
  EXPECT_EQ(maps::Register("map:arena1", kHash, h, std::string(nevr_cdn::kSha256HexChars, 'a'), true).reason, "sha_mismatch");
  EXPECT_EQ(maps::Register("map:arena1", kHash, h, "ABC", true).reason, "bad_sha256");
  EXPECT_EQ(maps::Register("plugin-x", kHash, h, Sha(image), true).reason, "bad_owner");
  EXPECT_EQ(maps::Register("map:", kHash, h, Sha(image), true).reason, "bad_owner");
  EXPECT_EQ(maps::Register("map:arena1", kHash, -1, Sha(image), true).reason, "unreadable");
  EXPECT_EQ(RegisterImage({}).reason, "bad_size");
  maps::Forget();
  EXPECT_EQ(maps::Register("map:arena1", kHash, h, Sha(image), true).reason, "not_configured");
}

TEST_F(MapScripts, ARekeyServesACopyOfThePlayersStockFileAndRefusesAWrongDigest) {
  const std::vector<std::uint8_t> stock = BuildDll();
  WriteBytes(GamePath(kStockHash), stock);
  EXPECT_EQ(maps::RegisterRekey("map:arena1", kHash, kStockHash, std::string(nevr_cdn::kSha256HexChars, 'b'), false, CopyDir()).reason, "stock_sha_mismatch");
  EXPECT_EQ(maps::RegisterRekey("map:arena1", kHash, kOtherHash, Sha(stock), false, CopyDir()).reason, "stock_missing");
  EXPECT_EQ(maps::RegisterRekey("map:arena1", kStockHash, kStockHash, Sha(stock), false, CopyDir()).reason, "stock_hash");
  EXPECT_EQ(maps::RegisterRekey("map:arena1", kHash, kStockHash, Sha(stock), false, L"").reason, "bad_copy_dir");
  EXPECT_EQ(maps::HeldCount(), 0u);
  ASSERT_TRUE(maps::RegisterRekey("map:arena1", kHash, kStockHash, Sha(stock), false, CopyDir()).ok());
  std::wstring load;
  ASSERT_TRUE(maps::Rewrite(Lower(GamePath(kHash)).c_str(), &load, nullptr, nullptr));
  EXPECT_TRUE(SameFile(load, CopyPath(kHash))) << Narrow(load);
  EXPECT_FALSE(SameFile(load, GamePath(kStockHash)));
  EXPECT_TRUE(maps::Holds("map:arena1", kHash));
}

// The point of the copy: the game's load of the rekeyed path must be a module of its own. A load of the stock file's path gives the
// stock module (one HMODULE per file), so a script that sets up bindings twice would hand the engine one module for two scripts.
// The bytes of the two files are the same, so the digests are equal.
TEST_F(MapScripts, TheCopyAndTheStockFileLoadAsTwoModulesWithEqualDigests) {
  const std::vector<std::uint8_t> stock = BuildDll();
  WriteBytes(GamePath(kStockHash), stock);
  ASSERT_TRUE(maps::RegisterRekey("map:arena1", kHash, kStockHash, Sha(stock), false, CopyDir()).ok());
  std::wstring load;
  ASSERT_TRUE(maps::Rewrite(Lower(GamePath(kHash)).c_str(), &load, nullptr, nullptr));
  HMODULE from_stock = LoadLibraryExW(GamePath(kStockHash).c_str(), nullptr, 0);
  ASSERT_NE(from_stock, nullptr) << GetLastError();
  HMODULE from_copy = LoadLibraryExW(load.c_str(), nullptr, 0);
  ASSERT_NE(from_copy, nullptr) << GetLastError();
  EXPECT_NE(from_copy, from_stock);
  EXPECT_EQ(Sha(ReadBytes(load)), Sha(ReadBytes(GamePath(kStockHash))));
  EXPECT_EQ(Sha(ReadBytes(load)), Sha(stock));
  FreeLibrary(from_copy);
  FreeLibrary(from_stock);
}

// A copy that is already there with the stock's digest is kept (the same file, not written again); any other file in its place is
// replaced, and the bytes held are the stock's.
TEST_F(MapScripts, ARightCopyIsKeptAndAWrongOneIsReplaced) {
  const std::vector<std::uint8_t> stock = BuildDll();
  WriteBytes(GamePath(kStockHash), stock);
  WriteBytes(CopyPath(kHash), {9, 9, 9, 9});
  ASSERT_TRUE(maps::RegisterRekey("map:arena1", kHash, kStockHash, Sha(stock), false, CopyDir()).ok());
  EXPECT_EQ(ReadBytes(CopyPath(kHash)), stock);
  BY_HANDLE_FILE_INFORMATION written{};
  ASSERT_TRUE(FileIdentity(CopyPath(kHash), &written));
  maps::Forget();
  Configure();
  ASSERT_TRUE(maps::RegisterRekey("map:arena1", kHash, kStockHash, Sha(stock), false, CopyDir()).ok());
  BY_HANDLE_FILE_INFORMATION kept{};
  ASSERT_TRUE(FileIdentity(CopyPath(kHash), &kept));
  EXPECT_EQ(kept.nFileIndexLow, written.nFileIndexLow);
  EXPECT_EQ(kept.nFileIndexHigh, written.nFileIndexHigh);
  EXPECT_EQ(ReadBytes(CopyPath(kHash)), stock);
}

// The copy is held for the process with read sharing only: nothing can rewrite or delete it, so the file the loader maps is the one
// that was hashed.
TEST_F(MapScripts, TheHeldCopyCanNeitherBeWrittenNorDeleted) {
  const std::vector<std::uint8_t> stock = BuildDll();
  WriteBytes(GamePath(kStockHash), stock);
  ASSERT_TRUE(maps::RegisterRekey("map:arena1", kHash, kStockHash, Sha(stock), false, CopyDir()).ok());
  EXPECT_FALSE(DeleteFileW(CopyPath(kHash).c_str()));
  HANDLE writer = CreateFileW(CopyPath(kHash).c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  EXPECT_EQ(writer, INVALID_HANDLE_VALUE);
  if (writer != INVALID_HANDLE_VALUE) CloseHandle(writer);
  // The stock file is only read: it is not held, and no stock byte is written anywhere but the copy.
  EXPECT_EQ(ReadBytes(GamePath(kStockHash)), stock);
  HANDLE stock_writer = CreateFileW(GamePath(kStockHash).c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
  EXPECT_NE(stock_writer, INVALID_HANDLE_VALUE) << "the stock file is still held";
  if (stock_writer != INVALID_HANDLE_VALUE) CloseHandle(stock_writer);
}

// A copy that cannot be written is a refusal that holds nothing: here the folder the copy needs is a file.
TEST_F(MapScripts, ACopyThatCannotBeWrittenRefusesAndHoldsNothing) {
  const std::vector<std::uint8_t> stock = BuildDll();
  WriteBytes(GamePath(kStockHash), stock);
  WriteBytes(CopyDir(), {1});
  EXPECT_EQ(maps::RegisterRekey("map:arena1", kHash, kStockHash, Sha(stock), false, CopyDir()).reason, "copy_unwritable");
  EXPECT_EQ(maps::HeldCount(), 0u);
}

// Holds is the resource-set registry's script waiver (SetScriptWaiver): true only for the owner the provider holds
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

// A cargo owner's rekey is held with no maps channel: the CDN's registrar sets the policy first, and a cargo label that
// cannot be made to match a label or a map is refused bad_owner.
TEST_F(MapScripts, ACargoOwnersRekeyIsHeldWithNoMapsChannel) {
  maps::Forget();  // no maps channel: nothing configured the provider
  const std::vector<std::uint8_t> stock = BuildDll();
  WriteBytes(GamePath(kStockHash), stock);
  EXPECT_EQ(maps::RegisterRekey("cargo:combat_content", kHash, kStockHash, Sha(stock), false, CopyDir()).reason,
            "not_configured");
  ASSERT_EQ(maps::ApplyPolicy(maps::PolicyCaller::kCargo, In()), maps::PolicyOutcome::kSet);
  ASSERT_TRUE(maps::RegisterRekey("cargo:combat_content", kHash, kStockHash, Sha(stock), false, CopyDir()).ok());
  EXPECT_TRUE(maps::Holds("cargo:combat_content", kHash));
  EXPECT_FALSE(maps::Holds("map:combat_content", kHash));
  EXPECT_EQ(maps::RegisterRekey("cargo:Bad", kOtherHash, kStockHash, Sha(stock), false, CopyDir()).reason, "bad_owner");
  EXPECT_EQ(maps::RegisterRekey("x:y", kOtherHash, kStockHash, Sha(stock), false, CopyDir()).reason, "bad_owner");
}

// Withdraw is the mount's all or nothing (docs/adr/0048 decision 6): a map whose set is refused keeps no script live.
TEST_F(MapScripts, WithdrawDropsEveryScriptAndRekeyOfTheOwnerAndKeepsAnotherOwners) {
  constexpr std::uint64_t kThirdHash = 0x5555aaaa5555aaaaULL;
  const std::vector<std::uint8_t> stock = BuildDll();
  WriteBytes(GamePath(kStockHash), stock);
  ASSERT_TRUE(RegisterImage(BuildDll()).ok());
  ASSERT_TRUE(maps::RegisterRekey("map:arena1", kOtherHash, kStockHash, Sha(stock), false, CopyDir()).ok());
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

// A rekeyed script the recipe rebuild wrote sits at <version dir>\scripts\<hash16>.dll in the maps cache (core/map_derive.h).
std::wstring RebuiltName(std::uint64_t hash) { return L"v1\\scripts\\" + Hash16W(hash) + L".dll"; }
const std::string kStockSha = std::string(nevr_cdn::kSha256HexChars, 'c');

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
  EXPECT_EQ(maps::RegisterDerived("map:arena1", kHash, h, std::string(nevr_cdn::kSha256HexChars, 'a'), kStockSha).reason, "sha_mismatch");
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

// Must-fail: the held file must be the one the recipe rebuild wrote for this hash.
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
TEST_F(MapScripts, SetPolicyHandsHoldsToTheWaiverOnceAndKeepsTheFirstCallersFacts) {
  g_waiver = nullptr;
  EXPECT_EQ(maps::SetPolicy(maps::PolicyCaller::kMaps, /*signed_only=*/false, TestByteLimit()), maps::PolicyOutcome::kSet);
  EXPECT_EQ(g_waiver, &maps::Holds);
  EXPECT_TRUE(RegisterImage(BuildDll(), kHash, "map:arena1").ok());
  g_waiver = nullptr;
  EXPECT_EQ(maps::SetPolicy(maps::PolicyCaller::kCargo, /*signed_only=*/true, TestByteLimit()), maps::PolicyOutcome::kRefused);
  EXPECT_EQ(g_waiver, nullptr) << "only the first call hands the waiver over";
  EXPECT_TRUE(RegisterImage(BuildDll(), kOtherHash, "map:arena1", /*official=*/false).ok())
      << "the first caller's signed_only=false stays in force";
}

// A second call with the same signed_only is kKept and changes no fact: the first byte limit stays.
TEST_F(MapScripts, ASecondPolicyCallWithTheSameModeIsKeptAndChangesNoFact) {
  EXPECT_EQ(maps::ApplyPolicy(maps::PolicyCaller::kCargo, In()), maps::PolicyOutcome::kSet);
  EXPECT_EQ(maps::ApplyPolicy(maps::PolicyCaller::kMaps, In(/*signed_only=*/false, /*byte_limit=*/1)), maps::PolicyOutcome::kKept);
  EXPECT_TRUE(RegisterImage(BuildDll()).ok()) << "a kept call does not replace the first byte limit";
}

// Forget clears the policy latch, so the next call configures the provider again.
TEST_F(MapScripts, ForgetClearsThePolicyLatch) {
  EXPECT_EQ(maps::ApplyPolicy(maps::PolicyCaller::kCargo, In(/*signed_only=*/true)), maps::PolicyOutcome::kSet);
  maps::Forget();
  EXPECT_EQ(maps::ApplyPolicy(maps::PolicyCaller::kMaps, In(/*signed_only=*/false)), maps::PolicyOutcome::kSet);
  EXPECT_TRUE(RegisterImage(BuildDll(), kHash, "map:arena1", /*official=*/false).ok());
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
  in.byte_limit = TestByteLimit();
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

// A cargo owner's copy is served by the same map rewriter, with no install of its own: the rewriter serves any held hash.
// Runs after the lazy-install test, which needs the rewriter absent until InstallMapScriptsRewriter.
TEST_F(MapScripts, ACargoOwnersCopyIsServedThroughTheMapRewriter) {
  ASSERT_EQ(maps::ApplyPolicy(maps::PolicyCaller::kCargo, In()), maps::PolicyOutcome::kSet);
  const std::vector<std::uint8_t> stock = BuildDll();
  WriteBytes(GamePath(kStockHash), stock);
  ASSERT_TRUE(maps::RegisterRekey("cargo:combat_content", kHash, kStockHash, Sha(stock), false, CopyDir()).ok());
  ASSERT_TRUE(maps::InstallMapScriptsRewriter());
  std::wstring load;
  ASSERT_TRUE(nevr_dll_load_hook::RewriteLoad(Lower(GamePath(kHash)).c_str(), &load));
  EXPECT_TRUE(SameFile(load, CopyPath(kHash))) << Narrow(load);
}

// The owner is the registry's map owner with a valid id (core/map_script_check.h): a script is never held for an owner the
// channel would refuse.
TEST_F(MapScripts, AnOwnerIsAValidMapIdUnderTheRegistrysPrefix) {
  EXPECT_EQ(RegisterImage(BuildDll(), kHash, "map:Arena1").reason, "bad_owner");
  EXPECT_EQ(RegisterImage(BuildDll(), kHash, "map:1arena").reason, "bad_owner");
  EXPECT_EQ(RegisterImage(BuildDll(), kHash, "map:arena-1").reason, "bad_owner");
  EXPECT_EQ(RegisterImage(BuildDll(), kHash, "map:" + std::string(nevr_resource_sets::kMaxMapOwnerBytes, 'a')).reason,
            "bad_owner");
  EXPECT_TRUE(RegisterImage(BuildDll(), kHash, "map:arena_1").ok());
  EXPECT_EQ(maps::HeldCount(), 1u);
}

// Under -require-signed-plugins a script is served only when its manifest is official (ADR 0048 decision 7): the fact core
// map_run uses. A manifest the official key did not sign is refused signed_only.
TEST_F(MapScripts, ASignedOnlyRunServesOnlyAnOfficialManifestsScripts) {
  Configure(/*signed_only=*/true);
  EXPECT_EQ(RegisterImage(BuildDll(), kHash, "map:arena1", /*official=*/false).reason, "signed_only");
  EXPECT_TRUE(RegisterImage(BuildDll(), kOtherHash, "map:arena1", /*official=*/true).ok());
}

// The package index pins a member's size: a file of another size is refused bad_size and nothing is held for it.
TEST_F(MapScripts, APinnedSizeMustBeTheIndexMembersSize) {
  const std::vector<std::uint8_t> image = BuildDll();
  const std::int64_t refused = Held(L"pinned-1.dll", image);
  Keep(refused);
  EXPECT_EQ(maps::Register("map:arena1", kHash, refused, Sha(image), true, image.size() + 1).reason, "bad_size");
  EXPECT_EQ(maps::HeldCount(), 0u);
  EXPECT_TRUE(maps::Register("map:arena1", kHash, Held(L"pinned-2.dll", image), Sha(image), true, image.size()).ok());
}

// The device's share bounds a script's file: one byte over it is refused bad_size, and a run whose device gave no memory fact
// refuses every script not_configured. The limit is a device fact, never a constant.
TEST_F(MapScripts, AScriptOverTheDeviceShareIsRefusedAndNoDeviceFactRefusesAll) {
  const std::vector<std::uint8_t> image = BuildDll();
  maps::Inputs in;
  in.exe_dir = exe_dir_;
  in.pack_carries = &FakePackCarries;
  in.byte_limit = image.size() - 1;
  maps::Configure(in);
  EXPECT_EQ(RegisterImage(image).reason, "bad_size");
  in.byte_limit = 0;
  maps::Configure(in);
  EXPECT_EQ(RegisterImage(image).reason, "not_configured");
}

// The served line is Info for a script's first serve and Debug for the reloads after it (logging.md, Rule 13).
std::vector<std::pair<EchoVR::LogLevel, std::string>> g_served;
void CaptureServed(EchoVR::LogLevel level, const CHAR* message) {
  if (std::string(message).find("map script served") != std::string::npos) g_served.emplace_back(level, message);
}

TEST_F(MapScripts, AServedScriptIsInfoOnceThenDebug) {
  ASSERT_TRUE(RegisterImage(BuildDll()).ok());
  ASSERT_TRUE(maps::InstallMapScriptsRewriter());
  std::wstring load;
  g_served.clear();
  SetBootLogSink(&CaptureServed);
  ASSERT_TRUE(nevr_dll_load_hook::RewriteLoad(Lower(GamePath(kHash)).c_str(), &load));
  ASSERT_TRUE(nevr_dll_load_hook::RewriteLoad(Lower(GamePath(kHash)).c_str(), &load));
  SetBootLogSink(nullptr);
  ASSERT_EQ(g_served.size(), 2u);
  EXPECT_EQ(g_served[0].first, EchoVR::LogLevel::Info);
  EXPECT_EQ(g_served[1].first, EchoVR::LogLevel::Debug);
  EXPECT_NE(g_served[0].second.find("file_len="), std::string::npos) << g_served[0].second;
}

std::vector<std::pair<EchoVR::LogLevel, std::string>> g_policy;
void CapturePolicy(EchoVR::LogLevel level, const CHAR* message) {
  if (std::string(message).find("map script policy") != std::string::npos) g_policy.emplace_back(level, message);
}

// Each policy call logs once: Info for the first caller, Debug for a kept call, Warning for a refused one, each naming the callers.
TEST_F(MapScripts, ThePolicyLinesNameTheCallerAndTheFirstCaller) {
  g_policy.clear();
  SetBootLogSink(&CapturePolicy);
  maps::ApplyPolicy(maps::PolicyCaller::kCargo, In());
  maps::ApplyPolicy(maps::PolicyCaller::kMaps, In());
  maps::ApplyPolicy(maps::PolicyCaller::kMaps, In(/*signed_only=*/true));
  SetBootLogSink(nullptr);
  ASSERT_EQ(g_policy.size(), 3u);
  EXPECT_EQ(g_policy[0].first, EchoVR::LogLevel::Info);
  EXPECT_NE(g_policy[0].second.find("map script policy caller=cargo "), std::string::npos) << g_policy[0].second;
  EXPECT_EQ(g_policy[1].first, EchoVR::LogLevel::Debug);
  EXPECT_NE(g_policy[1].second.find("map script policy kept caller=maps first=cargo "), std::string::npos) << g_policy[1].second;
  EXPECT_EQ(g_policy[2].first, EchoVR::LogLevel::Warning);
  EXPECT_NE(g_policy[2].second.find("map script policy refused caller=maps first=cargo signed_only=1 kept_signed_only=0"),
            std::string::npos)
      << g_policy[2].second;
}

// A set that failed before any of its scripts was held keeps its later scripts out too (all or nothing).
TEST_F(MapScripts, AWithdrawnOwnerIsBarredEvenWhenItHeldNothing) {
  EXPECT_EQ(maps::Withdraw("map:arena1"), 0u);
  EXPECT_EQ(RegisterImage(BuildDll()).reason, "withdrawn");
}

// A rekeyed script runs the PE rules on the player stock bytes too (ADR 0048 decision 6): a stock file that imports is
// refused pe_imports, and nothing is held for it.
TEST_F(MapScripts, ARekeyedStockFileRunsThePeRulesToo) {
  Shape import;
  import.nonempty_directory = 1;
  const std::vector<std::uint8_t> stock = BuildDll(import);
  WriteBytes(GamePath(kStockHash), stock);
  EXPECT_EQ(maps::RegisterRekey("map:arena1", kHash, kStockHash, Sha(stock), false, CopyDir()).reason, "pe_imports");
  EXPECT_EQ(maps::HeldCount(), 0u);
}

TEST(MapScriptsPath, TheCanonicalKeyIsTheGamesPathFolded) {
  EXPECT_EQ(maps::CanonicalScriptPath(L"C:\\Games\\EchoVR\\bin\\win10", 0x00ab0000000000cdULL),
            L"c:\\games\\echovr\\bin\\win10\\scripts\\00ab0000000000cd.dll");
  EXPECT_EQ(maps::CanonicalScriptPath(L"C:/Games/EchoVR/bin/win10/", 1),
            L"c:\\games\\echovr\\bin\\win10\\scripts\\0000000000000001.dll");
}

// The PC registrar (runtime/content/cdn_scripts_install.cpp; PLAN-v5 5.3 item 2) claims a label's listed copy through the provider as
// "cargo:<label>": with the player's stock file present the copy is held under the cargo owner and nothing is refused; with the stock file
// gone the claim is refused stock_missing, the label's refusal names it, and the copy the earlier claim held is withdrawn with the label.
// The policy is applied first (configure-once), so the registrar's own SetPolicy is kept and this test's facts stay in force.
TEST_F(MapScripts, ThePcRegistrarHoldsAListedCopyAndWithdrawsTheLabelOnAMissingStock) {
  maps::Forget();
  ASSERT_EQ(maps::ApplyPolicy(maps::PolicyCaller::kMaps, In()), maps::PolicyOutcome::kSet);
  const std::vector<std::uint8_t> stock = BuildDll();
  WriteBytes(GamePath(kStockHash), stock);
  nevr_cdn::Manifest m;
  m.labels = {"combat_content"};
  nevr_cdn::ScriptEntry s;
  s.label = "combat_content";
  s.platform = nevr_cdn::kWindowsX64.name;
  s.name = kHash;
  s.stock_source = kStockHash;
  s.stock_sha256 = Sha(stock);
  m.scripts.push_back(s);
  const std::string cache(cache_.begin(), cache_.end());  // the test's temporary folder is ASCII
  const nevr_cdn::ScriptRegistrar registrar = nevr_cdn_scripts::PcRegistrar(cache, /*signed_only=*/false);

  const nevr_cdn::ScriptRegistration held = registrar(m, nevr_cdn::kWindowsX64);
  EXPECT_TRUE(held.refused.empty());
  ASSERT_EQ(held.held.size(), 1u);
  EXPECT_TRUE(maps::Holds("cargo:combat_content", kHash));
  EXPECT_FALSE(maps::Holds("map:arena1", kHash));

  ASSERT_TRUE(DeleteFileW(GamePath(kStockHash).c_str()));
  s.name = kOtherHash;
  m.scripts = {s};
  const nevr_cdn::ScriptRegistration refused = registrar(m, nevr_cdn::kWindowsX64);
  ASSERT_EQ(refused.refused.size(), 1u);
  EXPECT_EQ(refused.refused[0].reason, "stock_missing");
  EXPECT_TRUE(refused.refused[0].pack.empty());
  EXPECT_TRUE(refused.held.empty());
  EXPECT_FALSE(maps::Holds("cargo:combat_content", kOtherHash));
  EXPECT_FALSE(maps::Holds("cargo:combat_content", kHash)) << "the label's earlier copy is withdrawn with the label";
  maps::Forget();
}

// The registrar is the run's first policy caller when the maps channel has not started: its own SetPolicy installs the run's signed-only
// fact, so the copy is refused signed_only with no policy applied beforehand (the provider configures on this call).
TEST_F(MapScripts, ThePcRegistrarSetsTheSignedOnlyPolicyWhenItIsTheFirstCaller) {
  maps::Forget();
  MakeDirs(cache_);
  nevr_cdn::Manifest m;
  m.labels = {"combat_content"};
  nevr_cdn::ScriptEntry s;
  s.label = "combat_content";
  s.platform = nevr_cdn::kWindowsX64.name;
  s.name = kHash;
  s.stock_source = kStockHash;
  s.stock_sha256 = std::string(nevr_cdn::kSha256HexChars, 'c');
  m.scripts.push_back(s);
  const std::string cache(cache_.begin(), cache_.end());
  const nevr_cdn::ScriptRegistration r = nevr_cdn_scripts::PcRegistrar(cache, /*signed_only=*/true)(m, nevr_cdn::kWindowsX64);
  ASSERT_EQ(r.refused.size(), 1u);
  EXPECT_EQ(r.refused[0].reason, "signed_only");
  EXPECT_FALSE(maps::Holds("cargo:combat_content", kHash));
  maps::Forget();
}

// KNOWN-WRONG #2242: this pins the refusal that exists only because no manifest verifier is in this client (cdn_scripts.cpp
// ManifestVerified answers false). With official = false, the signed-only gate refuses the copy with reason signed_only, and the label's
// refusal says so. The policy is applied signed-only first, so the registrar's call is kept with the same fact. The assertion pins the
// gate's behaviour for official = false; it does not assert the absence of the verifier.
TEST_F(MapScripts, ThePcRegistrarRefusesACopyUnderSignedOnlyWhileNoManifestIsVerified) {
  maps::Forget();
  ASSERT_EQ(maps::ApplyPolicy(maps::PolicyCaller::kMaps, In(/*signed_only=*/true)), maps::PolicyOutcome::kSet);
  const std::vector<std::uint8_t> stock = BuildDll();
  WriteBytes(GamePath(kStockHash), stock);
  nevr_cdn::Manifest m;
  m.labels = {"combat_content"};
  nevr_cdn::ScriptEntry s;
  s.label = "combat_content";
  s.platform = nevr_cdn::kWindowsX64.name;
  s.name = kHash;
  s.stock_source = kStockHash;
  s.stock_sha256 = Sha(stock);
  m.scripts.push_back(s);
  const std::string cache(cache_.begin(), cache_.end());
  const nevr_cdn::ScriptRegistration r = nevr_cdn_scripts::PcRegistrar(cache, /*signed_only=*/true)(m, nevr_cdn::kWindowsX64);
  ASSERT_EQ(r.refused.size(), 1u);
  EXPECT_EQ(r.refused[0].reason, "signed_only");
  EXPECT_TRUE(r.held.empty());
  EXPECT_FALSE(maps::Holds("cargo:combat_content", kHash));
  maps::Forget();
}

}  // namespace
