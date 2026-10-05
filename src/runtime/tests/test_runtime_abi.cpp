// Host <-> runtime ABI descriptor validation (core/runtime_abi.h).
// Step 1 of docs/design/2026-10-05-bugsplat-nevr-split.md: the version/size
// checks the host runs on nevr.dll's descriptor, before any cross-DLL load exists.

#include "core/runtime_abi.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <set>
#include <string>

#include <windows.h>

namespace {

std::int32_t Initialize(const NevrHostContext*) { return 0; }
void Before(void*) {}
void After(void*, std::uint64_t) {}
std::uint64_t Original(void*) { return 0; }

NevrRuntimeApi ValidApi() {
  NevrRuntimeApi api{};
  api.header.struct_size = sizeof(NevrRuntimeApi);
  api.header.abi_major = NevrAbi::kMajor;
  api.header.abi_minor = NevrAbi::kMinor;
  api.initialize = &Initialize;
  api.preprocess_before = &Before;
  api.preprocess_after = &After;
  return api;
}

NevrHostContext ValidHost() {
  NevrHostContext host{};
  host.header.struct_size = sizeof(NevrHostContext);
  host.header.abi_major = NevrAbi::kMajor;
  host.header.abi_minor = NevrAbi::kMinor;
  host.host_module = GetModuleHandleW(nullptr);
  host.game_module = GetModuleHandleW(nullptr);
  host.original_preprocess = &Original;
  return host;
}

// A future v1.x runtime descriptor: the v1.0 fields, then an appended field.
struct FutureMinorApi {
  NevrRuntimeApi v1;
  void (*appended_entry)();
};

}  // namespace

TEST(RuntimeAbi, AcceptsExactV1Descriptor) {
  const NevrRuntimeApi api = ValidApi();
  EXPECT_EQ(NevrAbi::ValidateRuntimeApi(&api), NevrAbi::Status::kOk);
}

TEST(RuntimeAbi, AcceptsSameMajorWithNewerMinorAndLargerStruct) {
  FutureMinorApi future{};
  future.v1 = ValidApi();
  future.v1.header.abi_minor = NevrAbi::kMinor + 7;
  future.v1.header.struct_size = sizeof(FutureMinorApi);
  EXPECT_EQ(NevrAbi::ValidateRuntimeApi(&future.v1), NevrAbi::Status::kOk);
  EXPECT_TRUE(NevrAbi::DescriptorCovers(&future.v1.header, sizeof(FutureMinorApi)));
}

TEST(RuntimeAbi, NewerFieldIsNotCoveredByAnOlderWriter) {
  const NevrRuntimeApi api = ValidApi();
  EXPECT_TRUE(NevrAbi::DescriptorCovers(&api.header, sizeof(NevrRuntimeApi)));
  EXPECT_FALSE(NevrAbi::DescriptorCovers(&api.header, sizeof(FutureMinorApi)));
  EXPECT_FALSE(NevrAbi::DescriptorCovers(nullptr, 0));
}

TEST(RuntimeAbi, RejectsNullDescriptor) {
  EXPECT_EQ(NevrAbi::ValidateRuntimeApi(nullptr), NevrAbi::Status::kNullDescriptor);
  EXPECT_EQ(NevrAbi::ValidateHostContext(nullptr), NevrAbi::Status::kNullDescriptor);
}

TEST(RuntimeAbi, RejectsMajorMismatchInEitherDirection) {
  for (const std::uint32_t major : {NevrAbi::kMajor - 1, NevrAbi::kMajor + 1, 0xFFFFFFFFu}) {
    NevrRuntimeApi api = ValidApi();
    api.header.abi_major = major;
    EXPECT_EQ(NevrAbi::ValidateRuntimeApi(&api), NevrAbi::Status::kMajorMismatch) << major;
  }
}

TEST(RuntimeAbi, RejectsStructSmallerThanV1) {
  for (const std::uint32_t size : {16u, 24u, 32u, NevrAbi::kRuntimeApiV1Size - 1}) {
    NevrRuntimeApi api = ValidApi();
    api.header.struct_size = size;
    EXPECT_EQ(NevrAbi::ValidateRuntimeApi(&api), NevrAbi::Status::kStructTooSmall) << size;
  }
  for (const std::uint32_t size : {0u, 1u, 15u}) {
    NevrRuntimeApi api = ValidApi();
    api.header.struct_size = size;
    EXPECT_EQ(NevrAbi::ValidateRuntimeApi(&api), NevrAbi::Status::kHeaderTooSmall) << size;
  }
}

TEST(RuntimeAbi, RejectsEachMissingRequiredEntry) {
  NevrRuntimeApi api = ValidApi();
  api.initialize = nullptr;
  EXPECT_EQ(NevrAbi::ValidateRuntimeApi(&api), NevrAbi::Status::kMissingEntry);
  api = ValidApi();
  api.preprocess_before = nullptr;
  EXPECT_EQ(NevrAbi::ValidateRuntimeApi(&api), NevrAbi::Status::kMissingEntry);
  api = ValidApi();
  api.preprocess_after = nullptr;
  EXPECT_EQ(NevrAbi::ValidateRuntimeApi(&api), NevrAbi::Status::kMissingEntry);
}

// A truthful pre-v1 writer whose descriptor is only the 16-byte header, placed
// so the next byte is an inaccessible page. The validator must reject it from
// struct_size alone; reading any entry pointer would fault this process.
TEST(RuntimeAbi, NeverReadsPastAShortDescriptor) {
  SYSTEM_INFO info{};
  GetSystemInfo(&info);
  const SIZE_T page = info.dwPageSize;
  auto* base = static_cast<BYTE*>(VirtualAlloc(nullptr, page * 2, MEM_RESERVE | MEM_COMMIT,
                                               PAGE_READWRITE));
  ASSERT_NE(base, nullptr);
  DWORD old_protection = 0;
  ASSERT_NE(VirtualProtect(base + page, page, PAGE_NOACCESS, &old_protection), FALSE);

  BYTE* header_at = base + page - sizeof(NevrAbiHeader);
  NevrAbiHeader header{};
  header.struct_size = sizeof(NevrAbiHeader);
  header.abi_major = NevrAbi::kMajor;
  std::memcpy(header_at, &header, sizeof(header));

  const auto* api = reinterpret_cast<const NevrRuntimeApi*>(header_at);
  EXPECT_EQ(NevrAbi::ValidateRuntimeApi(api), NevrAbi::Status::kStructTooSmall);
  const auto* host = reinterpret_cast<const NevrHostContext*>(header_at);
  EXPECT_EQ(NevrAbi::ValidateHostContext(host), NevrAbi::Status::kStructTooSmall);

  // Same placement, wrong major: rejected before the size check ever matters.
  header.abi_major = NevrAbi::kMajor + 1;
  std::memcpy(header_at, &header, sizeof(header));
  EXPECT_EQ(NevrAbi::ValidateRuntimeApi(api), NevrAbi::Status::kMajorMismatch);
  EXPECT_NE(VirtualFree(base, 0, MEM_RELEASE), FALSE);
}

TEST(RuntimeAbi, HostContextAcceptsValidAndRejectsMissingRendezvousEntries) {
  NevrHostContext host = ValidHost();
  EXPECT_EQ(NevrAbi::ValidateHostContext(&host), NevrAbi::Status::kOk);
  host.original_preprocess = nullptr;
  EXPECT_EQ(NevrAbi::ValidateHostContext(&host), NevrAbi::Status::kMissingEntry);
  host = ValidHost();
  host.game_module = nullptr;
  EXPECT_EQ(NevrAbi::ValidateHostContext(&host), NevrAbi::Status::kMissingEntry);
  host = ValidHost();
  host.header.abi_major = NevrAbi::kMajor + 1;
  EXPECT_EQ(NevrAbi::ValidateHostContext(&host), NevrAbi::Status::kMajorMismatch);
  host = ValidHost();
  host.header.struct_size = NevrAbi::kHostContextV1Size - 1;
  EXPECT_EQ(NevrAbi::ValidateHostContext(&host), NevrAbi::Status::kStructTooSmall);
}

TEST(RuntimeAbi, StatusNamesAreDistinctDiagnosticTokens) {
  std::set<std::string> names;
  for (std::uint32_t raw = 0; raw <= 5; ++raw) {
    names.insert(NevrAbi::StatusName(static_cast<NevrAbi::Status>(raw)));
  }
  EXPECT_EQ(names.size(), 6u);
  EXPECT_EQ(names.count("unknown"), 0u);
  EXPECT_STREQ(NevrAbi::StatusName(NevrAbi::Status::kMajorMismatch), "major_mismatch");
}

TEST(RuntimeAbi, EntryPointNameIsPinned) {
  EXPECT_STREQ(NevrAbi::kGetRuntimeApiExport, "NEVR_GetRuntimeApi");
}
