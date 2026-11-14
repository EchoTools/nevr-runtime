// The shim <-> runtime ABI of the Quest (core/quest_runtime_abi.h, built on core/runtime_abi_common.h), and the split that
// keeps it apart from the PC host <-> runtime ABI (core/runtime_abi.h).
//
// The layout is the AArch64 (LP64) layout the shim and the runtime are compiled for. Every offset below is written out here
// again, not read from the header, so moving a field in the header breaks this file's build, and the layout of one `.so` can
// never silently differ from the other's. LP64 lays these structs out the same way on the x86-64 host, so the static_asserts
// are the same proof on the host and under the NDK; the sizeof(void*) assert stops a 32-bit build from passing them.
//
// The common header carries no <windows.h>: this file compiles on Linux, which is itself the check that the split worked
// (docs/adr/0024-pc-self-update.md, "Relation to other records": ADR 0008 is extended).

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string_view>
#include <type_traits>

#include "core/quest_runtime_abi.h"
#include "quest/sentinel/build_variant.h"

#ifdef _WIN32
#error "core/quest_runtime_abi.h must not pull in <windows.h>; this file is compiled for Linux and Android"
#endif

namespace {

namespace abi = nevr_runtime_abi;
namespace qabi = nevr_quest_runtime_abi;

// Pinned AArch64 layout. Changing any of these is an ABI break: bump qabi::kMajor.
constexpr std::size_t kPointerSize = 8;
constexpr std::size_t kHeaderSize = 16;
constexpr std::size_t kOffsetShimHandle = 16;
constexpr std::size_t kOffsetLibr15Base = 24;
constexpr std::size_t kOffsetLibr15BuildId = 32;
constexpr std::size_t kOffsetUpdateRoot = 40;
constexpr std::size_t kOffsetShimVersion = 48;
constexpr std::size_t kOffsetLog = 56;
constexpr std::size_t kOffsetVariant = 64;
constexpr std::size_t kOffsetReserved = 68;
constexpr std::size_t kOffsetCapabilities = 72;
constexpr std::size_t kHostContextSize = 80;
constexpr std::size_t kOffsetInitialize = 16;
constexpr std::size_t kRuntimeApiSize = 24;
constexpr std::size_t kLogFieldSize = 16;
constexpr std::uint32_t kQuestMajor = 1;
constexpr std::uint32_t kQuestMinor = 0;
constexpr std::uint32_t kPcMajor = 1;
constexpr std::uint32_t kForeignMajor = 7;

static_assert(sizeof(void*) == kPointerSize, "the layout below is LP64: AArch64 on the Quest, x86-64 on the host");
static_assert(sizeof(NevrAbiHeader) == kHeaderSize);
static_assert(offsetof(NevrQuestHostContext, shim_handle) == kOffsetShimHandle);
static_assert(offsetof(NevrQuestHostContext, libr15_base) == kOffsetLibr15Base);
static_assert(offsetof(NevrQuestHostContext, libr15_build_id) == kOffsetLibr15BuildId);
static_assert(offsetof(NevrQuestHostContext, update_root) == kOffsetUpdateRoot);
static_assert(offsetof(NevrQuestHostContext, shim_version) == kOffsetShimVersion);
static_assert(offsetof(NevrQuestHostContext, log) == kOffsetLog);
static_assert(offsetof(NevrQuestHostContext, variant) == kOffsetVariant);
static_assert(offsetof(NevrQuestHostContext, reserved) == kOffsetReserved);
static_assert(offsetof(NevrQuestHostContext, capabilities) == kOffsetCapabilities);
static_assert(sizeof(NevrQuestHostContext) == kHostContextSize);
static_assert(alignof(NevrQuestHostContext) == kPointerSize);
static_assert(offsetof(NevrQuestRuntimeApi, initialize) == kOffsetInitialize);
static_assert(sizeof(NevrQuestRuntimeApi) == kRuntimeApiSize);
static_assert(sizeof(NevrQuestLogField) == kLogFieldSize);
static_assert(std::is_standard_layout_v<NevrQuestHostContext> && std::is_trivially_copyable_v<NevrQuestHostContext>);
static_assert(std::is_standard_layout_v<NevrQuestRuntimeApi> && std::is_trivially_copyable_v<NevrQuestRuntimeApi>);
static_assert(std::is_standard_layout_v<NevrQuestLogField> && std::is_trivially_copyable_v<NevrQuestLogField>);

// The sizes the validators compare a writer's struct_size against.
static_assert(qabi::kHostContextV1Size == offsetof(NevrQuestHostContext, capabilities) + sizeof(std::uint64_t));
static_assert(qabi::kHostContextV1Size == kHostContextSize);
static_assert(qabi::kRuntimeApiV1Size == kRuntimeApiSize);

// Quest starts at its own major 1, minor 0. PC's numbers are not here: they live in core/runtime_abi.h, which needs
// <windows.h>, and test_runtime_abi pins them.
static_assert(qabi::kMajor == kQuestMajor && qabi::kMinor == kQuestMinor);
// Bit 0 means on Quest what it means on PC: one definition, in the common header.
static_assert(abi::kHostCapabilitySwap == 1);

int g_failures = 0;

void Expect(bool ok, const char* what) {
  if (ok) return;
  std::fprintf(stderr, "FAIL %s\n", what);
  ++g_failures;
}

// The three pieces of identity a runtime needs before it may touch libr15: the shim's handle, the load base and the build ID.
// The static storage keeps the pointers valid for the whole test.
char g_shim_storage = 0;
char g_libr15_storage = 0;
constexpr char kBuildId[] = "0123456789abcdef0123456789abcdef01234567";
constexpr char kRoot[] = "/data/user/0/com.readyatdawn.r15/files/nevr";
constexpr char kShimVersion[] = "6.18.0";
constexpr std::uint32_t kVariantWord = 3;

void LogSink(std::uint32_t, const char*, const char*, const char*, const NevrQuestLogField*, std::uint32_t) {}
std::int32_t InitializeStub(const NevrQuestHostContext*) { return 0; }

NevrQuestHostContext ValidHost() {
  NevrQuestHostContext host{};
  host.header.struct_size = sizeof(NevrQuestHostContext);
  host.header.abi_major = qabi::kMajor;
  host.header.abi_minor = qabi::kMinor;
  host.shim_handle = &g_shim_storage;
  host.libr15_base = &g_libr15_storage;
  host.libr15_build_id = kBuildId;
  host.update_root = kRoot;
  host.shim_version = kShimVersion;
  host.log = &LogSink;
  host.variant = kVariantWord;
  return host;
}

NevrQuestRuntimeApi ValidApi() {
  NevrQuestRuntimeApi api{};
  api.header.struct_size = sizeof(NevrQuestRuntimeApi);
  api.header.abi_major = qabi::kMajor;
  api.header.abi_minor = qabi::kMinor;
  api.initialize = &InitializeStub;
  return api;
}

void TestRuntimeApi() {
  NevrQuestRuntimeApi api = ValidApi();
  Expect(qabi::ValidateRuntimeApi(&api) == abi::Status::kOk, "a complete descriptor validates");
  Expect(qabi::ValidateRuntimeApi(nullptr) == abi::Status::kNullDescriptor, "no descriptor");
  api.header.abi_major = kForeignMajor;
  Expect(qabi::ValidateRuntimeApi(&api) == abi::Status::kMajorMismatch, "another major is a hard reject");
  api = ValidApi();
  api.header.abi_major = kPcMajor + 1;
  Expect(qabi::ValidateRuntimeApi(&api) == abi::Status::kMajorMismatch, "PC's major plus one is not Quest's");
  api = ValidApi();
  api.header.struct_size = qabi::kRuntimeApiV1Size - 1;
  Expect(qabi::ValidateRuntimeApi(&api) == abi::Status::kStructTooSmall, "a descriptor shorter than v1");
  api.header.struct_size = kHeaderSize - 1;
  Expect(qabi::ValidateRuntimeApi(&api) == abi::Status::kHeaderTooSmall, "a descriptor shorter than the header");
  api = ValidApi();
  api.initialize = nullptr;
  Expect(qabi::ValidateRuntimeApi(&api) == abi::Status::kMissingEntry, "initialize is required");
}

void TestHostContext() {
  NevrQuestHostContext host = ValidHost();
  Expect(qabi::ValidateHostContext(&host) == abi::Status::kOk, "a complete context validates");
  Expect(qabi::ValidateHostContext(nullptr) == abi::Status::kNullDescriptor, "no context");
  host.header.abi_major = kForeignMajor;
  Expect(qabi::ValidateHostContext(&host) == abi::Status::kMajorMismatch, "another major is a hard reject");
  host = ValidHost();
  host.header.struct_size = qabi::kHostContextV1Size - 1;
  Expect(qabi::ValidateHostContext(&host) == abi::Status::kStructTooSmall, "a context shorter than v1");
  for (const auto clear : {+[](NevrQuestHostContext& h) { h.shim_handle = nullptr; },
                           +[](NevrQuestHostContext& h) { h.libr15_base = nullptr; },
                           +[](NevrQuestHostContext& h) { h.libr15_build_id = nullptr; }}) {
    host = ValidHost();
    clear(host);
    Expect(qabi::ValidateHostContext(&host) == abi::Status::kMissingEntry,
           "the shim handle, the libr15 base and the build ID are required");
  }
  host = ValidHost();
  host.update_root = nullptr;
  host.shim_version = nullptr;
  host.log = nullptr;
  Expect(qabi::ValidateHostContext(&host) == abi::Status::kOk,
         "the update root, the shim version and the log function may be absent");
}

void TestCapabilities() {
  NevrQuestHostContext host = ValidHost();
  Expect(qabi::HostCapabilities(&host) == 0, "no bit set");
  Expect(!qabi::HostCanSwap(&host), "no swap without the bit");
  host.capabilities = abi::kHostCapabilitySwap;
  Expect(qabi::HostCapabilities(&host) == abi::kHostCapabilitySwap, "the word is read from a context that covers it");
  Expect(qabi::HostCanSwap(&host), "bit 0 means this host performs the staged swap");
  host.capabilities = ~abi::kHostCapabilitySwap;
  Expect(!qabi::HostCanSwap(&host), "only bit 0 means swap; unknown bits mean nothing");
  host.capabilities = abi::kHostCapabilitySwap;
  host.header.struct_size = qabi::kHostContextV1Size - 1;
  Expect(qabi::HostCapabilities(&host) == 0, "the word is not read from a context that ends before it");
  Expect(qabi::HostCapabilities(nullptr) == 0 && !qabi::HostCanSwap(nullptr), "no context, no capability");
}

void TestCommonHeader() {
  NevrAbiHeader header{};
  header.struct_size = kHeaderSize;
  header.abi_major = kQuestMajor;
  Expect(abi::CheckHeader(&header, kHeaderSize, kQuestMajor) == abi::Status::kOk, "CheckHeader takes the major to expect");
  Expect(abi::CheckHeader(&header, kHeaderSize, kPcMajor + 1) == abi::Status::kMajorMismatch,
         "a header of another major is refused");
  Expect(abi::CheckHeader(nullptr, kHeaderSize, kQuestMajor) == abi::Status::kNullDescriptor, "no header");
  Expect(abi::DescriptorCovers(&header, kHeaderSize) && !abi::DescriptorCovers(&header, kHeaderSize + 1),
         "DescriptorCovers compares struct_size with the end of the field");
  Expect(!abi::DescriptorCovers(nullptr, 0), "no header covers nothing");
  Expect(std::string_view(abi::kGetRuntimeApiExport) == "NEVR_GetRuntimeApi",
         "the entry is one name on both platforms");
}

// The variant word the runtime accepts ends at build_variant::kLastVariantWord (runtime_entry.cpp refuses a larger one). That
// word names a variant, and the word after it has no name in VariantName (its fallback is "none"), so an enumerator added after
// kArena fails here until kLastVariantWord follows it.
void TestVariantWord() {
  namespace bv = nevr_quest::build_variant;
  Expect(std::string_view(bv::VariantName(static_cast<bv::Variant>(bv::kLastVariantWord))) != "none",
         "the last variant word names a variant");
  Expect(std::string_view(bv::VariantName(static_cast<bv::Variant>(bv::kLastVariantWord + 1))) == "none",
         "the word after the last variant word names no variant");
}

}  // namespace

int main() {
  TestRuntimeApi();
  TestHostContext();
  TestCapabilities();
  TestCommonHeader();
  TestVariantWord();
  if (g_failures != 0) return 1;
  std::printf("quest_runtime_abi_test: layout pinned (host context %zu bytes, runtime api %zu bytes) and validators pass\n",
              sizeof(NevrQuestHostContext), sizeof(NevrQuestRuntimeApi));
  return 0;
}
