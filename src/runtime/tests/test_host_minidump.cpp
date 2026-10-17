// Mutation-checked tests for the minidump parser shared with the crash harness
// (runtime/tests/host_crash/minidump.h). The parser is what proves the shim's
// dump is structurally valid and names the faulting thread, so a parser that
// accepts a corrupt dump or over-reads must fail these tests.

#include "runtime/tests/host_crash/minidump.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <vector>

#include <windows.h>
#include <dbghelp.h>

namespace {

using nevr_host_test::MinidumpView;

std::vector<std::uint8_t> BuildMinidump(std::uint32_t thread_id, std::uint32_t exception_code,
                                        std::uint64_t exception_address,
                                        std::uint32_t listed_thread_id) {
  constexpr std::size_t kHeaderOffset = 0;
  const std::size_t exception_offset = sizeof(MINIDUMP_HEADER);
  const std::size_t thread_list_offset = exception_offset + sizeof(MINIDUMP_EXCEPTION_STREAM);
  const std::size_t directory_offset =
      thread_list_offset + sizeof(MINIDUMP_THREAD_LIST) + sizeof(MINIDUMP_THREAD);
  const std::size_t total = directory_offset + 2 * sizeof(MINIDUMP_DIRECTORY);

  std::vector<std::uint8_t> buffer(total, 0);

  MINIDUMP_HEADER header{};
  header.Signature = nevr_host_test::kMinidumpSignature;
  header.Version = nevr_host_test::kMinidumpVersion;
  header.NumberOfStreams = 2;
  header.StreamDirectoryRva = static_cast<RVA>(directory_offset);
  std::memcpy(buffer.data() + kHeaderOffset, &header, sizeof(header));

  MINIDUMP_EXCEPTION_STREAM exception{};
  exception.ThreadId = thread_id;
  exception.ExceptionRecord.ExceptionCode = exception_code;
  exception.ExceptionRecord.ExceptionAddress = exception_address;
  std::memcpy(buffer.data() + exception_offset, &exception, sizeof(exception));

  MINIDUMP_THREAD_LIST list{};
  list.NumberOfThreads = 1;
  std::memcpy(buffer.data() + thread_list_offset, &list, sizeof(list));
  MINIDUMP_THREAD thread{};
  thread.ThreadId = listed_thread_id;
  std::memcpy(buffer.data() + thread_list_offset + sizeof(list), &thread, sizeof(thread));

  MINIDUMP_DIRECTORY directory[2]{};
  directory[0].StreamType = ExceptionStream;
  directory[0].Location.DataSize = static_cast<ULONG32>(sizeof(MINIDUMP_EXCEPTION_STREAM));
  directory[0].Location.Rva = static_cast<RVA>(exception_offset);
  directory[1].StreamType = ThreadListStream;
  directory[1].Location.DataSize =
      static_cast<ULONG32>(sizeof(MINIDUMP_THREAD_LIST) + sizeof(MINIDUMP_THREAD));
  directory[1].Location.Rva = static_cast<RVA>(thread_list_offset);
  std::memcpy(buffer.data() + directory_offset, directory, sizeof(directory));

  return buffer;
}

constexpr std::uint32_t kFaultThread = 4242;
constexpr std::uint32_t kAccessViolation = 0xC0000005;
constexpr std::uint64_t kFaultAddress = 0x1400DEADull;

}  // namespace

TEST(HostMinidump, AcceptsValidHeaderAndNamesTheFaultingThread) {
  const std::vector<std::uint8_t> buffer =
      BuildMinidump(kFaultThread, kAccessViolation, kFaultAddress, kFaultThread);
  MinidumpView view(buffer.data(), buffer.size());
  ASSERT_TRUE(view.Ok());
  EXPECT_TRUE(view.HasSignature());
  EXPECT_TRUE(view.ValidVersion());

  std::uint32_t thread_id = 0;
  std::uint32_t code = 0;
  std::uint64_t address = 0;
  ASSERT_TRUE(view.ExceptionInfo(&thread_id, &code, &address));
  EXPECT_EQ(thread_id, kFaultThread);
  EXPECT_EQ(code, kAccessViolation);
  EXPECT_EQ(address, kFaultAddress);
  EXPECT_TRUE(view.ContainsThread(kFaultThread));
}

TEST(HostMinidump, RejectsABadSignature) {
  std::vector<std::uint8_t> buffer =
      BuildMinidump(kFaultThread, kAccessViolation, kFaultAddress, kFaultThread);
  auto* header = reinterpret_cast<MINIDUMP_HEADER*>(buffer.data());
  header->Signature = 0;
  MinidumpView view(buffer.data(), buffer.size());
  EXPECT_TRUE(view.Ok());
  EXPECT_FALSE(view.HasSignature());
}

TEST(HostMinidump, RejectsABadVersion) {
  std::vector<std::uint8_t> buffer =
      BuildMinidump(kFaultThread, kAccessViolation, kFaultAddress, kFaultThread);
  auto* header = reinterpret_cast<MINIDUMP_HEADER*>(buffer.data());
  header->Version = 0;
  MinidumpView view(buffer.data(), buffer.size());
  EXPECT_TRUE(view.Ok());
  EXPECT_FALSE(view.ValidVersion());
}

TEST(HostMinidump, ATruncatedBufferIsNotAHeader) {
  const std::vector<std::uint8_t> buffer =
      BuildMinidump(kFaultThread, kAccessViolation, kFaultAddress, kFaultThread);
  MinidumpView view(buffer.data(), sizeof(MINIDUMP_HEADER) - 1);
  EXPECT_FALSE(view.Ok());
  EXPECT_EQ(view.Header(), nullptr);
  EXPECT_FALSE(view.HasSignature());
}

TEST(HostMinidump, AnOutOfBoundsDirectoryIsRejected) {
  std::vector<std::uint8_t> buffer =
      BuildMinidump(kFaultThread, kAccessViolation, kFaultAddress, kFaultThread);
  auto* header = reinterpret_cast<MINIDUMP_HEADER*>(buffer.data());
  header->StreamDirectoryRva = static_cast<RVA>(buffer.size() + 0x1000);
  MinidumpView view(buffer.data(), buffer.size());
  EXPECT_EQ(view.Directory(), nullptr);
  EXPECT_EQ(view.Stream(ExceptionStream), nullptr);
  std::uint32_t thread_id = 0;
  EXPECT_FALSE(view.ExceptionInfo(&thread_id, nullptr, nullptr));
  EXPECT_FALSE(view.ContainsThread(kFaultThread));
}

TEST(HostMinidump, AnOutOfBoundsStreamIsRejected) {
  std::vector<std::uint8_t> buffer =
      BuildMinidump(kFaultThread, kAccessViolation, kFaultAddress, kFaultThread);
  const std::size_t directory_offset =
      sizeof(MINIDUMP_HEADER) + sizeof(MINIDUMP_EXCEPTION_STREAM) +
      sizeof(MINIDUMP_THREAD_LIST) + sizeof(MINIDUMP_THREAD);
  auto* directory = reinterpret_cast<MINIDUMP_DIRECTORY*>(buffer.data() + directory_offset);
  directory[0].Location.Rva = static_cast<RVA>(buffer.size() + 0x2000);
  MinidumpView view(buffer.data(), buffer.size());
  std::uint32_t thread_id = 0;
  EXPECT_FALSE(view.ExceptionInfo(&thread_id, nullptr, nullptr));
}

TEST(HostMinidump, AThreadNotInTheListIsNotReported) {
  const std::vector<std::uint8_t> buffer =
      BuildMinidump(kFaultThread, kAccessViolation, kFaultAddress, 9999);
  MinidumpView view(buffer.data(), buffer.size());
  EXPECT_FALSE(view.ContainsThread(kFaultThread));
  EXPECT_TRUE(view.ContainsThread(9999));
}
