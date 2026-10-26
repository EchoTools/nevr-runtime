// Host <-> runtime ABI descriptor validation (core/runtime_abi.h).
// The version/size checks the host runs on nevr-runtime.dll's descriptor before it calls through it.

#include "core/runtime_abi.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <set>
#include <string>
#include <type_traits>

#include <windows.h>

namespace {

std::int32_t Initialize(const NevrHostContext*) { return 0; }
std::int64_t GameMain(std::int64_t command_line) { return command_line; }
std::int64_t OriginalWrapper(std::int64_t command_line) { return command_line + 1; }

NevrRuntimeApi ValidApi() {
  NevrRuntimeApi api{};
  api.header.struct_size = sizeof(NevrRuntimeApi);
  api.header.abi_major = nevr_runtime_abi::kMajor;
  api.header.abi_minor = nevr_runtime_abi::kMinor;
  api.initialize = &Initialize;
  api.game_main = &GameMain;
  return api;
}

NevrHostContext ValidHost() {
  NevrHostContext host{};
  host.header.struct_size = sizeof(NevrHostContext);
  host.header.abi_major = nevr_runtime_abi::kMajor;
  host.header.abi_minor = nevr_runtime_abi::kMinor;
  host.host_module = GetModuleHandleW(nullptr);
  host.game_module = GetModuleHandleW(nullptr);
  host.original_game_main_wrapper = &OriginalWrapper;
  host.wrapper_hook_target = reinterpret_cast<const void*>(&OriginalWrapper);
  for (std::size_t at = 0; at < sizeof(host.wrapper_hook_bytes); ++at) {
    host.wrapper_hook_bytes[at] = static_cast<unsigned char>(0xE9 + at);
  }
  host.wrapper_hook_bytes_valid = 1;
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
  EXPECT_EQ(nevr_runtime_abi::ValidateRuntimeApi(&api), nevr_runtime_abi::Status::kOk);
}

TEST(RuntimeAbi, AcceptsSameMajorWithNewerMinorAndLargerStruct) {
  FutureMinorApi future{};
  future.v1 = ValidApi();
  future.v1.header.abi_minor = nevr_runtime_abi::kMinor + 7;
  future.v1.header.struct_size = sizeof(FutureMinorApi);
  EXPECT_EQ(nevr_runtime_abi::ValidateRuntimeApi(&future.v1), nevr_runtime_abi::Status::kOk);
  EXPECT_TRUE(nevr_runtime_abi::DescriptorCovers(&future.v1.header, sizeof(FutureMinorApi)));
}

TEST(RuntimeAbi, NewerFieldIsNotCoveredByAnOlderWriter) {
  const NevrRuntimeApi api = ValidApi();
  EXPECT_TRUE(nevr_runtime_abi::DescriptorCovers(&api.header, sizeof(NevrRuntimeApi)));
  EXPECT_FALSE(nevr_runtime_abi::DescriptorCovers(&api.header, sizeof(FutureMinorApi)));
  EXPECT_FALSE(nevr_runtime_abi::DescriptorCovers(nullptr, 0));
}

TEST(RuntimeAbi, RejectsNullDescriptor) {
  EXPECT_EQ(nevr_runtime_abi::ValidateRuntimeApi(nullptr), nevr_runtime_abi::Status::kNullDescriptor);
  EXPECT_EQ(nevr_runtime_abi::ValidateHostContext(nullptr), nevr_runtime_abi::Status::kNullDescriptor);
}

TEST(RuntimeAbi, RejectsMajorMismatchInEitherDirection) {
  for (const std::uint32_t major : {nevr_runtime_abi::kMajor - 1, nevr_runtime_abi::kMajor + 1, 0xFFFFFFFFu}) {
    NevrRuntimeApi api = ValidApi();
    api.header.abi_major = major;
    EXPECT_EQ(nevr_runtime_abi::ValidateRuntimeApi(&api), nevr_runtime_abi::Status::kMajorMismatch) << major;
  }
}

TEST(RuntimeAbi, RejectsStructSmallerThanV1) {
  for (const std::uint32_t size : {16u, 24u, nevr_runtime_abi::kRuntimeApiV1Size - 1}) {
    NevrRuntimeApi api = ValidApi();
    api.header.struct_size = size;
    EXPECT_EQ(nevr_runtime_abi::ValidateRuntimeApi(&api), nevr_runtime_abi::Status::kStructTooSmall) << size;
  }
  for (const std::uint32_t size : {0u, 1u, 15u}) {
    NevrRuntimeApi api = ValidApi();
    api.header.struct_size = size;
    EXPECT_EQ(nevr_runtime_abi::ValidateRuntimeApi(&api), nevr_runtime_abi::Status::kHeaderTooSmall) << size;
  }
}

TEST(RuntimeAbi, RejectsEachMissingRequiredEntry) {
  NevrRuntimeApi api = ValidApi();
  api.initialize = nullptr;
  EXPECT_EQ(nevr_runtime_abi::ValidateRuntimeApi(&api), nevr_runtime_abi::Status::kMissingEntry);
  api = ValidApi();
  api.game_main = nullptr;
  EXPECT_EQ(nevr_runtime_abi::ValidateRuntimeApi(&api), nevr_runtime_abi::Status::kMissingEntry);
}

// The v1 layout is the contract: the rendezvous is the game's main wrapper, so the descriptor
// carries `game_main` and the context carries the wrapper's trampoline and a record of the
// host's hook on it. Pinned here as well as by the header's static_asserts, so a change to
// either fails a test with a name.
TEST(RuntimeAbi, V1LayoutIsPinned) {
  EXPECT_EQ(sizeof(NevrAbiHeader), 16u);
  EXPECT_EQ(offsetof(NevrRuntimeApi, initialize), 16u);
  EXPECT_EQ(offsetof(NevrRuntimeApi, game_main), 24u);
  EXPECT_EQ(nevr_runtime_abi::kRuntimeApiV1Size, 32u);
  EXPECT_EQ(sizeof(NevrRuntimeApi), 32u);
  EXPECT_EQ(offsetof(NevrHostContext, host_module), 16u);
  EXPECT_EQ(offsetof(NevrHostContext, game_module), 24u);
  EXPECT_EQ(offsetof(NevrHostContext, original_game_main_wrapper), 32u);
  EXPECT_EQ(offsetof(NevrHostContext, wrapper_hook_target), 40u);
  EXPECT_EQ(offsetof(NevrHostContext, wrapper_hook_bytes), 48u);
  EXPECT_EQ(sizeof(NevrHostContext::wrapper_hook_bytes), nevr_runtime_abi::kHookBytesSize);
  EXPECT_EQ(nevr_runtime_abi::kHookBytesSize, 16u);
  EXPECT_EQ(offsetof(NevrHostContext, wrapper_hook_bytes_valid), 64u);
  EXPECT_EQ(nevr_runtime_abi::kHostContextV1Size, 72u);
  EXPECT_EQ(sizeof(NevrHostContext), 72u);
}

// The wrapper takes the command-line pointer and returns the process exit code as a 64-bit
// integer: the whole range goes through the descriptor unchanged.
TEST(RuntimeAbi, GameMainCarriesAFullWidthArgumentAndResult) {
  static_assert(std::is_same_v<NevrGameMainFn, std::int64_t (*)(std::int64_t)>);
  const NevrRuntimeApi api = ValidApi();
  for (const std::int64_t value : {std::int64_t{0}, std::int64_t{-1}, INT64_MIN, INT64_MAX}) {
    EXPECT_EQ(api.game_main(value), value);
  }
  const NevrHostContext host = ValidHost();
  EXPECT_EQ(host.original_game_main_wrapper(41), 42);
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
  header.abi_major = nevr_runtime_abi::kMajor;
  std::memcpy(header_at, &header, sizeof(header));

  const auto* api = reinterpret_cast<const NevrRuntimeApi*>(header_at);
  EXPECT_EQ(nevr_runtime_abi::ValidateRuntimeApi(api), nevr_runtime_abi::Status::kStructTooSmall);
  const auto* host = reinterpret_cast<const NevrHostContext*>(header_at);
  EXPECT_EQ(nevr_runtime_abi::ValidateHostContext(host), nevr_runtime_abi::Status::kStructTooSmall);

  // Same placement, wrong major: rejected before the size check ever matters.
  header.abi_major = nevr_runtime_abi::kMajor + 1;
  std::memcpy(header_at, &header, sizeof(header));
  EXPECT_EQ(nevr_runtime_abi::ValidateRuntimeApi(api), nevr_runtime_abi::Status::kMajorMismatch);
  EXPECT_NE(VirtualFree(base, 0, MEM_RELEASE), FALSE);
}

TEST(RuntimeAbi, HostContextAcceptsValidAndRejectsMissingRendezvousEntries) {
  NevrHostContext host = ValidHost();
  EXPECT_EQ(nevr_runtime_abi::ValidateHostContext(&host), nevr_runtime_abi::Status::kOk);
  host.original_game_main_wrapper = nullptr;
  EXPECT_EQ(nevr_runtime_abi::ValidateHostContext(&host), nevr_runtime_abi::Status::kMissingEntry);
  host = ValidHost();
  host.wrapper_hook_target = nullptr;
  EXPECT_EQ(nevr_runtime_abi::ValidateHostContext(&host), nevr_runtime_abi::Status::kMissingEntry);
  host = ValidHost();
  host.game_module = nullptr;
  EXPECT_EQ(nevr_runtime_abi::ValidateHostContext(&host), nevr_runtime_abi::Status::kMissingEntry);
  host = ValidHost();
  host.host_module = nullptr;
  EXPECT_EQ(nevr_runtime_abi::ValidateHostContext(&host), nevr_runtime_abi::Status::kMissingEntry);
  host = ValidHost();
  host.header.abi_major = nevr_runtime_abi::kMajor + 1;
  EXPECT_EQ(nevr_runtime_abi::ValidateHostContext(&host), nevr_runtime_abi::Status::kMajorMismatch);
  host = ValidHost();
  host.header.struct_size = nevr_runtime_abi::kHostContextV1Size - 1;
  EXPECT_EQ(nevr_runtime_abi::ValidateHostContext(&host), nevr_runtime_abi::Status::kStructTooSmall);
}

// A host that reports a size which stops short of the hook record is a v1 writer without the
// record: the runtime must not read the bytes it would register with HookGuard.
TEST(RuntimeAbi, HostContextNeedsTheWholeHookRecord) {
  NevrHostContext host = ValidHost();
  for (const std::uint32_t size : {static_cast<std::uint32_t>(offsetof(NevrHostContext, wrapper_hook_target)),
                                   static_cast<std::uint32_t>(offsetof(NevrHostContext, wrapper_hook_bytes)),
                                   static_cast<std::uint32_t>(offsetof(NevrHostContext, wrapper_hook_bytes) +
                                                              nevr_runtime_abi::kHookBytesSize - 1),
                                   static_cast<std::uint32_t>(offsetof(NevrHostContext, wrapper_hook_bytes_valid))}) {
    host.header.struct_size = size;
    EXPECT_EQ(nevr_runtime_abi::ValidateHostContext(&host), nevr_runtime_abi::Status::kStructTooSmall) << size;
  }
}

// A record whose bytes were never read is accepted (the runtime can still run) but is not
// usable: the runtime must not register zeros with HookGuard. A value that is neither 0 nor 1 is
// a malformed writer and is rejected.
TEST(RuntimeAbi, AnUnreadHookRecordIsAcceptedButNotUsable) {
  NevrHostContext host = ValidHost();
  EXPECT_TRUE(nevr_runtime_abi::HookRecordUsable(&host));
  host.wrapper_hook_bytes_valid = 0;
  EXPECT_EQ(nevr_runtime_abi::ValidateHostContext(&host), nevr_runtime_abi::Status::kOk);
  EXPECT_FALSE(nevr_runtime_abi::HookRecordUsable(&host));
  host.wrapper_hook_bytes_valid = 2;
  EXPECT_EQ(nevr_runtime_abi::ValidateHostContext(&host), nevr_runtime_abi::Status::kInvalidField);
  EXPECT_FALSE(nevr_runtime_abi::HookRecordUsable(&host));
  EXPECT_FALSE(nevr_runtime_abi::HookRecordUsable(nullptr));
  // A writer whose size stops before the flag cannot claim the bytes are usable.
  host = ValidHost();
  host.header.struct_size = static_cast<std::uint32_t>(offsetof(NevrHostContext, wrapper_hook_bytes_valid));
  EXPECT_FALSE(nevr_runtime_abi::HookRecordUsable(&host));
}

TEST(RuntimeAbi, StatusNamesAreDistinctDiagnosticTokens) {
  std::set<std::string> names;
  for (std::uint32_t raw = 0; raw <= 6; ++raw) {
    names.insert(nevr_runtime_abi::StatusName(static_cast<nevr_runtime_abi::Status>(raw)));
  }
  EXPECT_EQ(names.size(), 7u);
  EXPECT_EQ(names.count("unknown"), 0u);
  EXPECT_STREQ(nevr_runtime_abi::StatusName(nevr_runtime_abi::Status::kMajorMismatch), "major_mismatch");
}

TEST(RuntimeAbi, EntryPointNameIsPinned) {
  EXPECT_STREQ(nevr_runtime_abi::kGetRuntimeApiExport, "NEVR_GetRuntimeApi");
}
