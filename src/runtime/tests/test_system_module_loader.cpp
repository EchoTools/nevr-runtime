#include "runtime/lifecycle/system_module_loader.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <string>

namespace {
std::wstring g_systemDirectory;
UINT g_reportedLength = 0;
HMODULE g_loadedModule = reinterpret_cast<HMODULE>(static_cast<uintptr_t>(0x1234));
std::wstring g_loadedPath;
UINT g_loadCalls = 0;
bool g_writeTerminator = true;

UINT WINAPI FakeGetSystemDirectoryW(LPWSTR buffer, UINT capacity) {
  if (g_reportedLength < capacity) {
    const size_t copied = std::min(static_cast<size_t>(g_reportedLength), static_cast<size_t>(capacity - 1));
    std::copy_n(g_systemDirectory.begin(), std::min(copied, g_systemDirectory.size()), buffer);
    buffer[g_reportedLength] = g_writeTerminator ? L'\0' : L'X';
  }
  return g_reportedLength;
}

HMODULE WINAPI FakeLoadLibraryW(LPCWSTR path) {
  ++g_loadCalls;
  g_loadedPath = path;
  return g_loadedModule;
}

void ConfigureDirectory(std::wstring directory, UINT reportedLength) {
  g_systemDirectory = std::move(directory);
  g_reportedLength = reportedLength;
  g_loadCalls = 0;
  g_loadedPath.clear();
  g_writeTerminator = true;
}
TEST(SystemModuleLoader, RejectsZeroResultWithoutCallingLoadLibrary) {
  ConfigureDirectory(L"C:\\Windows\\System32", 0);
  EXPECT_EQ(Nevr::Lifecycle::LoadSystemDbgCore(FakeGetSystemDirectoryW, FakeLoadLibraryW), nullptr);
  EXPECT_EQ(g_loadCalls, 0U);
}

TEST(SystemModuleLoader, RejectsReturnedLengthAtOrBeyondBufferCapacity) {
  ConfigureDirectory(std::wstring(MAX_PATH, L'x'), MAX_PATH);
  EXPECT_EQ(Nevr::Lifecycle::LoadSystemDbgCore(FakeGetSystemDirectoryW, FakeLoadLibraryW), nullptr);
  EXPECT_EQ(g_loadCalls, 0U);

  ConfigureDirectory(std::wstring(MAX_PATH + 10, L'x'), MAX_PATH + 10);
  EXPECT_EQ(Nevr::Lifecycle::LoadSystemDbgCore(FakeGetSystemDirectoryW, FakeLoadLibraryW), nullptr);
  EXPECT_EQ(g_loadCalls, 0U);
}

TEST(SystemModuleLoader, RejectsDirectoryWithoutTerminatorAtReportedLength) {
  constexpr auto directory = L"C:\\Windows\\System32";
  ConfigureDirectory(directory, static_cast<UINT>(std::wstring(directory).size()));
  g_writeTerminator = false;
  EXPECT_EQ(Nevr::Lifecycle::LoadSystemDbgCore(FakeGetSystemDirectoryW, FakeLoadLibraryW), nullptr);
  EXPECT_EQ(g_loadCalls, 0U);
}

TEST(SystemModuleLoader, RejectsWhenSuffixAndTerminatorDoNotFit) {
  constexpr size_t suffixLength = sizeof(L"\\dbgcore.dll") / sizeof(wchar_t) - 1;
  const UINT directoryLength = static_cast<UINT>(MAX_PATH - suffixLength);
  ConfigureDirectory(std::wstring(directoryLength, L'd'), directoryLength);
  EXPECT_EQ(Nevr::Lifecycle::LoadSystemDbgCore(FakeGetSystemDirectoryW, FakeLoadLibraryW), nullptr);
  EXPECT_EQ(g_loadCalls, 0U);
}

TEST(SystemModuleLoader, LoadsValidatedSystemPathAndExactBoundaryThatFits) {
  constexpr auto directory = L"C:\\Windows\\System32";
  ConfigureDirectory(directory, static_cast<UINT>(std::wstring(directory).size()));
  ASSERT_EQ(Nevr::Lifecycle::LoadSystemDbgCore(FakeGetSystemDirectoryW, FakeLoadLibraryW), g_loadedModule);
  EXPECT_EQ(g_loadCalls, 1U);
  EXPECT_EQ(g_loadedPath, L"C:\\Windows\\System32\\dbgcore.dll");

  constexpr size_t suffixLength = sizeof(L"\\dbgcore.dll") / sizeof(wchar_t) - 1;
  const UINT directoryLength = static_cast<UINT>(MAX_PATH - suffixLength - 1);
  ConfigureDirectory(std::wstring(directoryLength, L'd'), directoryLength);
  ASSERT_EQ(Nevr::Lifecycle::LoadSystemDbgCore(FakeGetSystemDirectoryW, FakeLoadLibraryW), g_loadedModule);
  EXPECT_EQ(g_loadCalls, 1U);
  EXPECT_EQ(g_loadedPath.size(), static_cast<size_t>(MAX_PATH - 1));
}
}  // namespace
