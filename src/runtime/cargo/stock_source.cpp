#include "runtime/cargo/stock_source.h"

#include <winsock2.h>  // before windows.h (tools/tests/test_header_include_order.py)
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <utility>
#include <vector>

#include "core/device_memory.h"
#include "core/stock_source.h"
#include "runtime/cargo/win_env.h"
#include "runtime/server/installed_levels.h"

namespace nevr_maps {

namespace {

// ReadFile takes its length as a DWORD (fileapi.h); an OVERLAPPED offset is two DWORDs.
constexpr std::size_t kMaxReadFileBytes = std::numeric_limits<DWORD>::max();
constexpr unsigned kDwordBits = 32;
// "%016llx": a package set's id and a script's hash as the game names their files.
constexpr std::size_t kHex16Bytes = 17;

// A path as the shared half keeps it (UTF-8), and back: the runtime builds as C++20, where path::u8string is a u8string.
std::string Utf8(const std::wstring& wide) {
  if (wide.empty()) return std::string();
  const int n = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
  if (n <= 0) return std::string();
  std::string out(static_cast<std::size_t>(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), &out[0], n, nullptr, nullptr);
  return out;
}

std::wstring Wide(const std::string& utf8) {
  if (utf8.empty()) return std::wstring();
  const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
  if (n <= 0) return std::wstring();
  std::wstring out(static_cast<std::size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()), &out[0], n);
  return out;
}

std::string Hex16(std::uint64_t value) {
  char text[kHex16Bytes];
  std::snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(value));
  return text;
}

class WinStockFile : public StockFile {
 public:
  WinStockFile(HANDLE file, MapFs& fs) : file_(file), fs_(fs) {}
  ~WinStockFile() override { CloseHandle(file_); }
  WinStockFile(const WinStockFile&) = delete;
  WinStockFile& operator=(const WinStockFile&) = delete;

  bool Stamp(FileStamp* out) override {
    return fs_.StampOf(static_cast<std::int64_t>(reinterpret_cast<std::intptr_t>(file_)), out);
  }

  bool Read(std::uint64_t offset, void* buffer, std::size_t length) override {
    auto* out = static_cast<std::uint8_t*>(buffer);
    while (length > 0) {
      const DWORD want = static_cast<DWORD>(std::min(length, kMaxReadFileBytes));
      OVERLAPPED where = {};
      where.Offset = static_cast<DWORD>(offset);
      where.OffsetHigh = static_cast<DWORD>(offset >> kDwordBits);
      DWORD got = 0;
      if (!ReadFile(file_, out, want, &got, &where) || got == 0) return false;
      out += got;
      offset += got;
      length -= got;
    }
    return true;
  }

 private:
  HANDLE file_;
  MapFs& fs_;
};

class WinStockSource : public PackageStockSource {
 public:
  explicit WinStockSource(const std::wstring& install_root)
      : PackageStockSource(&nevr_device::MemoryAvailable), root_(install_root), fs_(MakeWinMapFs()) {}

  bool ReadScript(std::uint64_t hash, std::vector<std::uint8_t>* out, std::vector<StockStamp>* read_from) override {
    const std::filesystem::path path = root_ / L"bin" / L"win10" / L"scripts" / (Hex16(hash) + ".dll");
    return ReadWholeFile(Utf8(path.wstring()), out, read_from);
  }

 protected:
  std::vector<ManifestPlace> Manifests() override {
    std::vector<ManifestPlace> places;
    for (const std::filesystem::path& dir : nevr_game_server::FindManifestDirs(root_ / L"bin" / L"win10" / L"echovr.exe")) {
      for (const std::uint64_t set : nevr_game_server::BootPackageSets()) {
        ManifestPlace place;
        place.manifest_path = Utf8((dir / Hex16(set)).wstring());
        place.package_prefix = Utf8((dir.parent_path() / L"packages" / Hex16(set)).wstring());
        places.push_back(std::move(place));
      }
    }
    return places;
  }

  // Full sharing: a reader here never stops the game, the launcher or an update from opening, writing or replacing the file.
  std::unique_ptr<StockFile> Open(const std::string& path) override {
    const std::wstring wide = Wide(path);
    if (wide.empty()) return nullptr;
    const HANDLE file = CreateFileW(wide.c_str(), GENERIC_READ,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return nullptr;
    if (GetFileType(file) != FILE_TYPE_DISK) {
      CloseHandle(file);
      return nullptr;
    }
    return std::unique_ptr<StockFile>(new WinStockFile(file, *fs_));
  }

 private:
  std::filesystem::path root_;
  std::unique_ptr<MapFs> fs_;
};

}  // namespace

std::unique_ptr<StockSource> MakeWinStockSource(const std::wstring& install_root) {
  return std::unique_ptr<StockSource>(new WinStockSource(install_root));
}

}  // namespace nevr_maps
