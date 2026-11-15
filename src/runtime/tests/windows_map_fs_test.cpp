// The PC's nevr_maps::MapFs (cdn/windows_map_fs.h), driven for real: Win32 files in a temp tree, through the same WindowsEnv the map
// channel runs on. The map channel's event tag is checked here too, because this file's Log() is the one that reads the lines back.
// No test reaches the network (the Get cases are in test_env_windows.cpp).
#include "cdn/env_windows.h"
#include "cdn/windows_map_fs.h"

#include <gtest/gtest.h>

#include <winsock2.h>  // before windows.h (tools/tests/test_header_include_order.py)
#include <windows.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "core/logging.h"
#include "core/nevr_root.h"

namespace {

std::mutex g_logMutex;
std::vector<std::pair<EchoVR::LogLevel, std::string>> g_logLines;
constexpr std::size_t kLogLineBytes = 2048;

}  // namespace

VOID Log(EchoVR::LogLevel level, const CHAR* format, ...) {
  char line[kLogLineBytes];
  va_list args;
  va_start(args, format);
  std::vsnprintf(line, sizeof(line), format, args);
  va_end(args);
  std::lock_guard<std::mutex> lock(g_logMutex);
  g_logLines.emplace_back(level, line);
}

namespace {

namespace nc = nevr_cdn;
namespace nm = nevr_maps;

constexpr char kTag[] = "[NEVR.MAPS]";
constexpr std::size_t kMountedBytes = 13;
constexpr std::size_t kReadBufferBytes = 64;
constexpr std::uint64_t kReadOffset = 8;
constexpr std::uint64_t kPastEnd = 100;
constexpr std::size_t kSmallReadMax = 100;
constexpr std::size_t kVersionOneBytes = 11;
constexpr std::size_t kVersionTwoBytes = 19;
constexpr std::size_t kFinalNameBytes = 11;  // "\\final.blob", its backslash included
constexpr int kEventBytes = 42;
constexpr std::uint64_t kEventFree = 7;
constexpr std::size_t kEventRecords = 4;
constexpr std::size_t kInfoLine = 2;
constexpr std::size_t kErrorLine = 3;

std::string Utf8(const std::wstring& wide) {
  const int n = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
  std::string out(static_cast<std::size_t>(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), &out[0], n, nullptr, nullptr);
  return out;
}

std::wstring Wide(const std::string& utf8) {
  const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
  std::wstring out(static_cast<std::size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), &out[0], n);
  return out;
}

std::string TempRoot() {
  wchar_t temp[MAX_PATH + 1] = {};
  const DWORD n = GetTempPathW(MAX_PATH + 1, temp);
  return Utf8(std::wstring(temp, n));
}

// A fresh folder under %TEMP% with a non-ASCII name, so every path crosses the UTF-8 to UTF-16 conversion.
class WindowsMapFsTest : public ::testing::Test {
 protected:
  void SetUp() override {
    static std::atomic<int> serial{0};
    dir_ = TempRoot() + "nevr_windows_map_fs_\xc3\xa9_" + std::to_string(GetCurrentProcessId()) + "_" +
           std::to_string(serial.fetch_add(1));
    env_ = std::make_unique<nc::WindowsEnv>(kTag);
    fs_ = nm::MakeWinMapFs();
    ASSERT_TRUE(fs_->RemoveTree(dir_));
    ASSERT_TRUE(env_->MakeDirs(dir_));
  }
  void TearDown() override { fs_->RemoveTree(dir_); }

  std::string Path(const char* name) const { return dir_ + "\\" + name; }

  nm::FileStamp StampNow(const std::string& path) {
    std::int64_t handle = 0;
    std::uint64_t size = 0;
    nm::FileStamp stamp;
    EXPECT_TRUE(fs_->OpenHeld(path, &handle, &size));
    EXPECT_TRUE(fs_->StampOf(handle, &stamp));
    env_->CloseBlob(handle);
    return stamp;
  }

  std::string dir_;
  std::unique_ptr<nc::Env> env_;
  std::unique_ptr<nm::MapFs> fs_;
};

TEST_F(WindowsMapFsTest, AHeldFileRefusesAWriterAndADelete) {
  const std::string blob = Path("held.blob");
  ASSERT_TRUE(env_->WriteFileAtomically(blob, "mounted bytes"));
  for (const bool through_map_fs : {false, true}) {
    std::int64_t handle = 0;
    std::uint64_t size = 0;
    ASSERT_TRUE(through_map_fs ? fs_->OpenHeld(blob, &handle, &size) : env_->OpenBlob(blob, &handle, &size));
    EXPECT_EQ(size, kMountedBytes);

    const std::wstring wide = Wide(blob);
    const HANDLE writer = CreateFileW(wide.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    EXPECT_EQ(writer, INVALID_HANDLE_VALUE);
    EXPECT_EQ(GetLastError(), static_cast<DWORD>(ERROR_SHARING_VIOLATION));
    EXPECT_FALSE(DeleteFileW(wide.c_str()));
    EXPECT_FALSE(env_->WriteFileAtomically(blob, "replacement"));  // a rename over it is refused too
    EXPECT_EQ(env_->OpenAppend(blob), nullptr);                    // and an appender

    const HANDLE reader = CreateFileW(wide.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    EXPECT_NE(reader, INVALID_HANDLE_VALUE);  // other readers are welcome
    if (reader != INVALID_HANDLE_VALUE) CloseHandle(reader);

    char buffer[kReadBufferBytes] = {};
    std::size_t got = 0;
    ASSERT_TRUE(env_->ReadBlob(handle, kReadOffset, buffer, sizeof(buffer), &got));
    EXPECT_EQ(std::string(buffer, got), "bytes");
    ASSERT_TRUE(env_->ReadBlob(handle, kPastEnd, buffer, sizeof(buffer), &got));
    EXPECT_EQ(got, 0u);
    env_->CloseBlob(handle);
  }
  EXPECT_TRUE(DeleteFileW(Wide(blob).c_str()));
}

TEST_F(WindowsMapFsTest, StampOfChangesOnARewriteAndIsStableOnAReopen) {
  const std::string path = Path("map.blob");
  ASSERT_TRUE(env_->WriteFileAtomically(path, "version one"));
  const nm::FileStamp first = StampNow(path);
  EXPECT_EQ(first.size, kVersionOneBytes);
  EXPECT_NE(first.last_write, 0u);
  EXPECT_TRUE(StampNow(path) == first);

  ASSERT_TRUE(env_->WriteFileAtomically(path, "version one"));  // same bytes, a new file renamed over it
  const nm::FileStamp replaced = StampNow(path);
  EXPECT_FALSE(replaced == first);
  EXPECT_TRUE(replaced.id_hi != first.id_hi || replaced.id_lo != first.id_lo);

  const HANDLE in_place = CreateFileW(Wide(path).c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  ASSERT_NE(in_place, INVALID_HANDLE_VALUE);
  DWORD wrote = 0;
  ASSERT_TRUE(WriteFile(in_place, "version two, longer", kVersionTwoBytes, &wrote, nullptr));
  CloseHandle(in_place);
  const nm::FileStamp rewritten = StampNow(path);
  EXPECT_FALSE(rewritten == replaced);
  EXPECT_EQ(rewritten.size, kVersionTwoBytes);
  EXPECT_EQ(rewritten.id_lo, replaced.id_lo);

  nm::FileStamp none;
  EXPECT_FALSE(fs_->StampOf(reinterpret_cast<std::intptr_t>(INVALID_HANDLE_VALUE), &none));
}

TEST_F(WindowsMapFsTest, FinalPathOfNamesTheOpenFileInUtf8) {
  const std::string path = Path("final.blob");
  ASSERT_TRUE(env_->WriteFileAtomically(path, "x"));
  std::int64_t handle = 0;
  std::uint64_t size = 0;
  ASSERT_TRUE(fs_->OpenHeld(path, &handle, &size));
  const std::string final_path = fs_->FinalPathOf(handle);
  env_->CloseBlob(handle);
  ASSERT_FALSE(final_path.empty());
  EXPECT_NE(final_path.rfind("\\\\?\\", 0), 0u);
  EXPECT_EQ(final_path[1], ':');
  EXPECT_NE(final_path.find("nevr_windows_map_fs_\xc3\xa9_"), std::string::npos);
  EXPECT_EQ(final_path.substr(final_path.size() - kFinalNameBytes), "\\final.blob");
  EXPECT_TRUE(fs_->FinalPathOf(reinterpret_cast<std::intptr_t>(INVALID_HANDLE_VALUE)).empty());
}

TEST_F(WindowsMapFsTest, RemoveTreeIsRefusedWhileAFileInsideIsHeld) {
  ASSERT_TRUE(env_->MakeDirs(dir_ + "\\tree\\set"));
  ASSERT_TRUE(env_->WriteFileAtomically(dir_ + "\\tree\\package.json", "{}"));
  ASSERT_TRUE(env_->WriteFileAtomically(dir_ + "\\tree\\set\\windows-x86_64.nevrres", "set"));
  std::int64_t handle = 0;
  std::uint64_t size = 0;
  ASSERT_TRUE(fs_->OpenHeld(dir_ + "\\tree\\set\\windows-x86_64.nevrres", &handle, &size));
  EXPECT_FALSE(fs_->RemoveTree(dir_ + "\\tree"));
  std::string text;
  EXPECT_TRUE(env_->ReadSmallFile(dir_ + "\\tree\\set\\windows-x86_64.nevrres", &text, kSmallReadMax));
  env_->CloseBlob(handle);
  EXPECT_TRUE(fs_->RemoveTree(dir_ + "\\tree\\"));
  EXPECT_EQ(GetFileAttributesW(Wide(dir_ + "\\tree").c_str()), INVALID_FILE_ATTRIBUTES);
  EXPECT_TRUE(fs_->RemoveTree(dir_ + "\\tree"));  // already gone
  EXPECT_FALSE(fs_->RemoveTree("relative\\tree"));
  ASSERT_TRUE(env_->WriteFileAtomically(Path("file"), "x"));
  EXPECT_FALSE(fs_->RemoveTree(Path("file")));
}

TEST_F(WindowsMapFsTest, RemoveTreeRemovesALinkNotItsTarget) {
  ASSERT_TRUE(env_->MakeDirs(dir_ + "\\outside"));
  ASSERT_TRUE(env_->WriteFileAtomically(dir_ + "\\outside\\keep.txt", "keep"));
  ASSERT_TRUE(env_->MakeDirs(dir_ + "\\tree"));
  const std::wstring command = L"mklink /J \"" + Wide(dir_ + "\\tree\\link") + L"\" \"" + Wide(dir_ + "\\outside") + L"\" >nul";
  ASSERT_EQ(_wsystem(command.c_str()), 0);
  std::string text;
  ASSERT_TRUE(env_->ReadSmallFile(dir_ + "\\tree\\link\\keep.txt", &text, kSmallReadMax));

  EXPECT_TRUE(fs_->RemoveTree(dir_ + "\\tree"));
  EXPECT_TRUE(env_->ReadSmallFile(dir_ + "\\outside\\keep.txt", &text, kSmallReadMax));
  EXPECT_EQ(text, "keep");
}

TEST_F(WindowsMapFsTest, ListDirAndRenameDir) {
  ASSERT_TRUE(env_->MakeDirs(dir_ + "\\a.tmp\\scripts"));
  ASSERT_TRUE(env_->WriteFileAtomically(dir_ + "\\a.tmp\\package.json", "{}"));
  std::vector<std::string> names;
  ASSERT_TRUE(fs_->ListDir(dir_ + "\\a.tmp", &names));
  EXPECT_EQ(names, (std::vector<std::string>{"package.json", "scripts"}));
  ASSERT_TRUE(fs_->ListDir(dir_ + "\\a.tmp\\scripts\\", &names));
  EXPECT_TRUE(names.empty());
  EXPECT_FALSE(fs_->ListDir(dir_ + "\\absent", &names));

  ASSERT_TRUE(fs_->RenameDir(dir_ + "\\a.tmp", dir_ + "\\a"));
  std::string text;
  EXPECT_TRUE(env_->ReadSmallFile(dir_ + "\\a\\package.json", &text, kSmallReadMax));
  ASSERT_TRUE(env_->MakeDirs(dir_ + "\\b"));
  EXPECT_FALSE(fs_->RenameDir(dir_ + "\\b", dir_ + "\\a"));  // never over an existing folder
  EXPECT_FALSE(fs_->RenameDir(dir_ + "\\absent", dir_ + "\\c"));
}

TEST(WindowsMapFsPaths, MapsCacheDirIsUnderTheCache) {
  EXPECT_EQ(nevr_root::MapsCacheDir(std::wstring(L"C:\\L\\nevr\\echovr")), L"C:\\L\\nevr\\echovr\\cache\\maps");
  EXPECT_EQ(nevr_root::MapsCacheDir(std::string("C:\\L\\nevr\\echovr\\")), "C:\\L\\nevr\\echovr\\cache\\maps");
  EXPECT_TRUE(nevr_root::MapsCacheDir(std::string()).empty());
}

// The map channel's env writes its records under "[NEVR.MAPS]": one tagged line per event, at the event's level.
TEST(WindowsMapFsEvents, TheMapChannelsEventIsOneTaggedLineAtItsLevel) {
  nc::WindowsEnv env(kTag);
  {
    std::lock_guard<std::mutex> lock(g_logMutex);
    g_logLines.clear();
  }
  env.Event(nc::EventLevel::kWarn, "map_piece",
            {{"reason", "over_cap"}, {"bytes", kEventBytes}, {"detail", "two words"}, {"empty", ""}, {"q", "a\"b"}, {"nl", "a\nb"}});
  env.Event(nc::EventLevel::kDebug, "map_limits", {{"free", kEventFree}});
  env.Event(nc::EventLevel::kInfo, "map_ready", {});
  env.Event(nc::EventLevel::kError, "map_done", {});
  std::lock_guard<std::mutex> lock(g_logMutex);
  ASSERT_EQ(g_logLines.size(), kEventRecords);
  EXPECT_EQ(g_logLines[0].first, EchoVR::LogLevel::Warning);
  EXPECT_EQ(g_logLines[0].second,
            "[NEVR.MAPS] map_piece reason=over_cap bytes=42 detail=\"two words\" empty=\"\" q=\"a\\\"b\" nl=\"a b\"");
  EXPECT_EQ(g_logLines[1].first, EchoVR::LogLevel::Debug);
  EXPECT_EQ(g_logLines[1].second, "[NEVR.MAPS] map_limits free=7");
  EXPECT_EQ(g_logLines[kInfoLine].first, EchoVR::LogLevel::Info);
  EXPECT_EQ(g_logLines[kErrorLine].first, EchoVR::LogLevel::Error);
}

}  // namespace
