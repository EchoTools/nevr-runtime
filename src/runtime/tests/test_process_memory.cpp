// #48: ProcessMemcpy is the one memory-patching idiom; it reports failure instead of swallowing it.

#include <gtest/gtest.h>

#include <cstring>

#include "runtime/hook/process_memory.h"

namespace {

TEST(ProcessMemcpy, WritesThroughAReadOnlyPageAndRestoresItsProtection) {
  void* page = VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READONLY);
  ASSERT_NE(page, nullptr);
  const unsigned char patch[3] = {0xDE, 0xAD, 0xBE};
  DWORD error = 99;
  EXPECT_TRUE(ProcessMemcpy(page, patch, sizeof(patch), &error));
  EXPECT_EQ(error, 0U);
  EXPECT_EQ(std::memcmp(page, patch, sizeof(patch)), 0);
  MEMORY_BASIC_INFORMATION info{};
  ASSERT_NE(VirtualQuery(page, &info, sizeof(info)), 0U);
  EXPECT_EQ(info.Protect, static_cast<DWORD>(PAGE_READONLY));
  VirtualFree(page, 0, MEM_RELEASE);
}

TEST(ProcessMemcpy, ReportsAFailedProtectChangeWithTheErrorCode) {
  const unsigned char patch[1] = {0x90};
  DWORD error = 0;
  EXPECT_FALSE(ProcessMemcpy(nullptr, patch, sizeof(patch), &error));
  EXPECT_NE(error, 0U);
  EXPECT_FALSE(ProcessMemcpy(nullptr, patch, sizeof(patch))) << "the error out-parameter is optional";
}

TEST(ProcessMemcpy, FlushesEveryWrittenByteAfterRestoringProtection) {
  void* page = VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READ);
  ASSERT_NE(page, nullptr);
  const unsigned char patch[3] = {0xC3, 0x90, 0x90};
  int calls = 0;
  DWORD error = 99;
  const bool ok = ProcessMemcpyWithFlush(
      page, patch, sizeof(patch), &error,
      [&](HANDLE process, LPCVOID address, SIZE_T size) {
        ++calls;
        EXPECT_EQ(process, GetCurrentProcess());
        EXPECT_EQ(address, page);
        EXPECT_EQ(size, sizeof(patch));
        MEMORY_BASIC_INFORMATION info{};
        EXPECT_NE(VirtualQuery(page, &info, sizeof(info)), 0U);
        EXPECT_EQ(info.Protect, static_cast<DWORD>(PAGE_EXECUTE_READ));
        return FlushInstructionCache(process, address, size);
      });
  EXPECT_TRUE(ok);
  EXPECT_EQ(error, ERROR_SUCCESS);
  EXPECT_EQ(calls, 1);
  EXPECT_EQ(std::memcmp(page, patch, sizeof(patch)), 0);
  VirtualFree(page, 0, MEM_RELEASE);
}

TEST(ProcessMemcpy, ReportsInstructionCacheFlushFailure) {
  void* page = VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READ);
  ASSERT_NE(page, nullptr);
  const unsigned char patch = 0xC3;
  DWORD error = 0;
  EXPECT_FALSE(ProcessMemcpyWithFlush(
      page, &patch, sizeof(patch), &error,
      [](HANDLE, LPCVOID, SIZE_T) {
        SetLastError(ERROR_GEN_FAILURE);
        return FALSE;
      }));
  EXPECT_EQ(error, ERROR_GEN_FAILURE);
  // The injected failure does not invalidate the real instruction cache.
  EXPECT_TRUE(FlushInstructionCache(GetCurrentProcess(), page, sizeof(patch)));
  VirtualFree(page, 0, MEM_RELEASE);
}

}  // namespace
