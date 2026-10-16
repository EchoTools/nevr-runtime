// #48: ProcessMemcpy is the one memory-patching idiom; it reports failure instead of swallowing it.

#include <gtest/gtest.h>

#include <cstring>

#include "runtime/hook/process_memory.h"
#include "runtime/patch/binary_bug_fixes.h"

TEST(BusyWaitPatch, FailedInstallDoesNotMarkBytePatched) {
  std::uint8_t byte = 0xC3;
  bool patched = false;
  const bool wrote = nevr_binary_bug_fixes::WriteBusyWaitByte(
      &byte, &byte, patched, true,
      [](void*, const std::uint8_t*, std::size_t) { return false; });
  EXPECT_FALSE(wrote);
  EXPECT_FALSE(patched);
}

TEST(BusyWaitPatch, FailedRestoreKeepsByteMarkedPatched) {
  std::uint8_t byte = 0x40;
  bool patched = true;
  const bool wrote = nevr_binary_bug_fixes::WriteBusyWaitByte(
      &byte, &byte, patched, false,
      [](void*, const std::uint8_t*, std::size_t) { return false; });
  EXPECT_FALSE(wrote);
  EXPECT_TRUE(patched);
}

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

// The write, restore and flush calls are injected so each failure branch of ProcessMemcpyWithOps
// runs: a real VirtualProtect/WriteProcessMemory/FlushInstructionCache cannot be made to fail on a
// page this test owns, and nothing else exercises the branches.
struct FakeOps {
  int protectCalls = 0;
  int writeCalls = 0;
  int flushCalls = 0;
  int failProtectOnCall = 0;  // 1-based: 2 is the restore call
  DWORD protectError = ERROR_ACCESS_DENIED;
  SIZE_T writeLimit = static_cast<SIZE_T>(-1);  // bytes the fake write lands
  bool writeFails = false;
  DWORD writeError = ERROR_NOACCESS;
  bool flushFails = false;
  DWORD flushError = ERROR_GEN_FAILURE;
  SIZE_T flushedSize = 0;
  DWORD protectsSeen[2] = {0, 0};

  bool Run(void* page, const void* src, size_t size, DWORD* error) {
    return ProcessMemcpyWithOps(
        page, src, size, error,
        [this](LPVOID address, SIZE_T len, DWORD newProtect, PDWORD oldProtect) -> BOOL {
          ++protectCalls;
          if (protectCalls <= 2) protectsSeen[protectCalls - 1] = newProtect;
          if (failProtectOnCall == protectCalls) {
            SetLastError(protectError);
            return FALSE;
          }
          return VirtualProtect(address, len, newProtect, oldProtect);
        },
        [this](HANDLE process, LPVOID address, LPCVOID source, SIZE_T len, SIZE_T* written) -> BOOL {
          ++writeCalls;
          if (writeFails) {
            *written = 0;
            SetLastError(writeError);
            return FALSE;
          }
          const SIZE_T n = len < writeLimit ? len : writeLimit;
          return WriteProcessMemory(process, address, source, n, written);
        },
        [this](HANDLE, LPCVOID, SIZE_T len) -> BOOL {
          ++flushCalls;
          flushedSize = len;
          if (flushFails) {
            SetLastError(flushError);
            return FALSE;
          }
          return TRUE;
        });
  }
};

struct ExecPage {
  void* page;
  ExecPage() : page(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READ)) {}
  ~ExecPage() {
    if (page != nullptr) VirtualFree(page, 0, MEM_RELEASE);
  }
};

TEST(ProcessMemcpyOps, ReportsAFailedProtectionRestoreAfterAGoodWrite) {
  ExecPage p;
  ASSERT_NE(p.page, nullptr);
  const unsigned char patch[3] = {0xC3, 0x90, 0x90};
  FakeOps ops;
  ops.failProtectOnCall = 2;
  ops.protectError = ERROR_INVALID_ADDRESS;
  DWORD error = 0;
  EXPECT_FALSE(ops.Run(p.page, patch, sizeof(patch), &error));
  EXPECT_EQ(error, ERROR_INVALID_ADDRESS) << "a page left writable-and-executable must be a reported failure";
  EXPECT_EQ(ops.protectsSeen[0], static_cast<DWORD>(PAGE_EXECUTE_READWRITE));
  EXPECT_EQ(std::memcmp(p.page, patch, sizeof(patch)), 0) << "the bytes landed";
  EXPECT_EQ(ops.flushCalls, 1) << "the bytes that landed are still flushed";
  EXPECT_EQ(ops.flushedSize, sizeof(patch));
}

TEST(ProcessMemcpyOps, ReportsAPartialWriteAndFlushesOnlyWhatLanded) {
  ExecPage p;
  ASSERT_NE(p.page, nullptr);
  const unsigned char patch[4] = {0xE9, 0x01, 0x02, 0x03};
  FakeOps ops;
  ops.writeLimit = 2;
  DWORD error = 0;
  EXPECT_FALSE(ops.Run(p.page, patch, sizeof(patch), &error));
  EXPECT_EQ(error, ERROR_PARTIAL_COPY);
  EXPECT_EQ(ops.flushCalls, 1);
  EXPECT_EQ(ops.flushedSize, static_cast<SIZE_T>(2)) << "flush covers the written bytes, not the requested size";
  EXPECT_EQ(ops.protectCalls, 2) << "protection is restored after a partial write";
}

TEST(ProcessMemcpyOps, ReportsAFailedWriteAndSkipsTheFlush) {
  ExecPage p;
  ASSERT_NE(p.page, nullptr);
  const unsigned char patch[1] = {0xC3};
  FakeOps ops;
  ops.writeFails = true;
  ops.writeError = ERROR_NOACCESS;
  DWORD error = 0;
  EXPECT_FALSE(ops.Run(p.page, patch, sizeof(patch), &error));
  EXPECT_EQ(error, ERROR_NOACCESS);
  EXPECT_EQ(ops.flushCalls, 0) << "nothing was written, so there is nothing to flush";
  EXPECT_EQ(ops.protectCalls, 2) << "protection is still restored after a failed write";
}

TEST(ProcessMemcpyOps, TheFirstErrorWinsOverALaterRestoreOrFlushFailure) {
  ExecPage p;
  ASSERT_NE(p.page, nullptr);
  const unsigned char patch[4] = {0x90, 0x90, 0x90, 0x90};
  FakeOps ops;
  ops.writeLimit = 1;
  ops.failProtectOnCall = 2;
  ops.protectError = ERROR_INVALID_ADDRESS;
  ops.flushFails = true;
  DWORD error = 0;
  EXPECT_FALSE(ops.Run(p.page, patch, sizeof(patch), &error));
  EXPECT_EQ(error, ERROR_PARTIAL_COPY) << "the write error was recorded first and must not be replaced";

  FakeOps restoreThenFlush;
  restoreThenFlush.failProtectOnCall = 2;
  restoreThenFlush.protectError = ERROR_INVALID_ADDRESS;
  restoreThenFlush.flushFails = true;
  error = 0;
  EXPECT_FALSE(restoreThenFlush.Run(p.page, patch, sizeof(patch), &error));
  EXPECT_EQ(error, ERROR_INVALID_ADDRESS) << "the restore error was recorded before the flush error";
}

TEST(ProcessMemcpyOps, AFailedFirstProtectChangeWritesAndFlushesNothing) {
  ExecPage p;
  ASSERT_NE(p.page, nullptr);
  const unsigned char patch[1] = {0xC3};
  FakeOps ops;
  ops.failProtectOnCall = 1;
  DWORD error = 0;
  EXPECT_FALSE(ops.Run(p.page, patch, sizeof(patch), &error));
  EXPECT_EQ(error, ERROR_ACCESS_DENIED);
  EXPECT_EQ(ops.writeCalls, 0);
  EXPECT_EQ(ops.flushCalls, 0);
  EXPECT_EQ(ops.protectCalls, 1);
}

}  // namespace
