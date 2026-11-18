// The PC half of the published plugin SDK (sdk/include/nevr/pc/): safe_memory.h, hook_manager.h and the helpers
// in nevr_common.h that a plugin calls (ResolveVA_Checked, ResolveVA_Unchecked, LoadConfigFile). Plugin authors compile
// against these headers, so each one is held to what it decides here: a bad pointer is a false and not a fault, a
// HookManager removes only the hooks it made, and a config file is found where the header says it searches.
// Everything runs on pages and files the test makes; no game image is involved.

#include <windows.h>

#include <gtest/gtest.h>

#include <MinHook.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

#include "nevr/pc/hook_manager.h"
#include "nevr/pc/nevr_common.h"
#include "nevr/pc/safe_memory.h"
#include "runtime/tests/run_root.h"

namespace {

constexpr size_t kPageBytes = 4096;
constexpr uint64_t kPatternU64 = 0x1122334455667788ULL;
constexpr uint16_t kPatternU16 = 0xA1B2;
constexpr uint8_t kFillByte = 0xCC;
constexpr uint64_t kSentinelU64 = 0xDEADBEEFULL;
constexpr uint16_t kSentinelU16 = 0x1234;
constexpr size_t kSampleLen = 4;       // length of the byte samples below
constexpr size_t kShortSampleLen = 2;  // length of the two-byte prologue sample
constexpr size_t kRegionPages = 3;     // pages reserved around the fake module
constexpr size_t kCommittedPages = 2;  // pages committed in that region
constexpr uintptr_t kImageOffset = 0x10;
constexpr size_t kStubFillBytes = 32;  // int3 padding written over the stub page's head
constexpr std::array<uint8_t, kSampleLen> kSampleActual = {1, 2, 3, 4};
constexpr std::array<uint8_t, kSampleLen> kSampleSame = {1, 2, 3, 4};
constexpr std::array<uint8_t, kSampleLen> kSampleOther = {1, 2, 3, 5};
constexpr std::array<uint8_t, kSampleLen> kCopySource = {9, 8, 7, 6};

// A page that faults on every access: committed, then PAGE_NOACCESS.
class NoAccessPage {
 public:
  NoAccessPage() {
    base_ = static_cast<uint8_t*>(VirtualAlloc(nullptr, kPageBytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    DWORD old = 0;
    if (base_ != nullptr) VirtualProtect(base_, kPageBytes, PAGE_NOACCESS, &old);
  }
  ~NoAccessPage() {
    if (base_ != nullptr) VirtualFree(base_, 0, MEM_RELEASE);
  }
  NoAccessPage(const NoAccessPage&) = delete;
  NoAccessPage& operator=(const NoAccessPage&) = delete;
  uint8_t* get() const { return base_; }

 private:
  uint8_t* base_ = nullptr;
};

// ---- safe_memory.h ----

TEST(SafeMemory, MemcmpAnswersByContentOnReadableMemory) {
  const std::array<uint8_t, kSampleLen> actual = kSampleActual;
  const std::array<uint8_t, kSampleLen> same = kSampleSame;
  const std::array<uint8_t, kSampleLen> other = kSampleOther;
  EXPECT_TRUE(nevr::SafeMemcmp(actual.data(), same.data(), actual.size()));
  EXPECT_FALSE(nevr::SafeMemcmp(actual.data(), other.data(), actual.size()));
}

TEST(SafeMemory, MemcmpIsFalseOnAnInaccessiblePageAndOnNull) {
  NoAccessPage page;
  ASSERT_NE(page.get(), nullptr);
  const std::array<uint8_t, kSampleLen> expected = {0, 0, 0, 0};
  EXPECT_FALSE(nevr::SafeMemcmp(page.get(), expected.data(), expected.size()));
  EXPECT_FALSE(nevr::SafeMemcmp(nullptr, expected.data(), expected.size()));
}

TEST(SafeMemory, MemcpyCopiesReadableSourceAndLeavesDestinationOnAFault) {
  const std::array<uint8_t, kSampleLen> src = kCopySource;
  std::array<uint8_t, kSampleLen> dst = {0, 0, 0, 0};
  EXPECT_TRUE(nevr::SafeMemcpy(dst.data(), src.data(), src.size()));
  EXPECT_EQ(dst, src);

  NoAccessPage page;
  ASSERT_NE(page.get(), nullptr);
  std::array<uint8_t, kSampleLen> untouched = {kFillByte, kFillByte, kFillByte, kFillByte};
  EXPECT_FALSE(nevr::SafeMemcpy(untouched.data(), page.get(), untouched.size()));
  for (uint8_t b : untouched) EXPECT_EQ(b, kFillByte);
}

TEST(SafeMemory, ReadU64ReturnsTheValueOrLeavesTheOutputAlone) {
  uint64_t value = kPatternU64;
  uint64_t out = 0;
  EXPECT_TRUE(nevr::SafeReadU64(reinterpret_cast<uintptr_t>(&value), &out));
  EXPECT_EQ(out, kPatternU64);

  NoAccessPage page;
  ASSERT_NE(page.get(), nullptr);
  uint64_t kept = kSentinelU64;
  EXPECT_FALSE(nevr::SafeReadU64(reinterpret_cast<uintptr_t>(page.get()), &kept));
  EXPECT_EQ(kept, kSentinelU64);
  EXPECT_FALSE(nevr::SafeReadU64(0, &kept));
  EXPECT_EQ(kept, kSentinelU64);
}

TEST(SafeMemory, ReadU16ReturnsTheValueOrLeavesTheOutputAlone) {
  uint16_t value = kPatternU16;
  uint16_t out = 0;
  EXPECT_TRUE(nevr::SafeReadU16(reinterpret_cast<uintptr_t>(&value), &out));
  EXPECT_EQ(out, kPatternU16);

  NoAccessPage page;
  ASSERT_NE(page.get(), nullptr);
  uint16_t kept = kSentinelU16;
  EXPECT_FALSE(nevr::SafeReadU16(reinterpret_cast<uintptr_t>(page.get()), &kept));
  EXPECT_EQ(kept, kSentinelU16);
}

// A guarded read that faulted must not leave the thread armed: the handler is removed per call, and a later
// call on the same thread still works.
TEST(SafeMemory, ThreadIsDisarmedAfterAFaultAndLaterCallsStillWork) {
  NoAccessPage page;
  ASSERT_NE(page.get(), nullptr);
  uint64_t out = 0;
  EXPECT_FALSE(nevr::SafeReadU64(reinterpret_cast<uintptr_t>(page.get()), &out));
  EXPECT_FALSE(nevr::detail::g_safeActive);
  uint64_t value = kPatternU64;
  EXPECT_TRUE(nevr::SafeReadU64(reinterpret_cast<uintptr_t>(&value), &out));
  EXPECT_EQ(out, kPatternU64);
  // And after a call that did not fault.
  EXPECT_FALSE(nevr::detail::g_safeActive);
}

// ---- nevr_common.h: ResolveVA ----

class ResolveVaTest : public ::testing::Test {
 protected:
  // Three pages reserved: a committed page before the module base (so an underflowing VA would land on committed
  // memory), the committed first page of the "module" at base_, and an uncommitted page after it.
  void SetUp() override {
    region_ = static_cast<uint8_t*>(VirtualAlloc(nullptr, kRegionPages * kPageBytes, MEM_RESERVE, PAGE_READWRITE));
    ASSERT_NE(region_, nullptr);
    ASSERT_NE(VirtualAlloc(region_, kCommittedPages * kPageBytes, MEM_COMMIT, PAGE_READWRITE), nullptr);
    base_ = region_ + kPageBytes;
  }
  void TearDown() override {
    if (region_ != nullptr) VirtualFree(region_, 0, MEM_RELEASE);
  }
  uint8_t* region_ = nullptr;
  uint8_t* base_ = nullptr;
};

TEST_F(ResolveVaTest, UncheckedIsBasePlusTheOffsetFromTheDefaultImageBase) {
  const uintptr_t base = reinterpret_cast<uintptr_t>(base_);
  EXPECT_EQ(nevr::ResolveVA_Unchecked(base, nevr::ECHOVR_DEFAULT_IMAGE_BASE), base_);
  EXPECT_EQ(nevr::ResolveVA_Unchecked(base, nevr::ECHOVR_DEFAULT_IMAGE_BASE + kImageOffset), base_ + kImageOffset);
  // Unchecked does not validate: a VA below the image base wraps instead of returning null.
  EXPECT_NE(nevr::ResolveVA_Unchecked(base, nevr::ECHOVR_DEFAULT_IMAGE_BASE - 1), nullptr);
}

TEST_F(ResolveVaTest, CheckedRefusesBelowTheImageBaseAndUncommittedPages) {
  const uintptr_t base = reinterpret_cast<uintptr_t>(base_);
  EXPECT_EQ(nevr::ResolveVA_Checked(base, nevr::ECHOVR_DEFAULT_IMAGE_BASE - 1), nullptr);
  // The wrapped address of this VA is the committed page before base_: only the underflow check refuses it.
  EXPECT_EQ(nevr::ResolveVA_Checked(base, nevr::ECHOVR_DEFAULT_IMAGE_BASE - kPageBytes), nullptr);
  EXPECT_EQ(nevr::ResolveVA_Checked(base, 0), nullptr);
  EXPECT_EQ(nevr::ResolveVA_Checked(base, nevr::ECHOVR_DEFAULT_IMAGE_BASE), base_);
  EXPECT_EQ(nevr::ResolveVA_Checked(base, nevr::ECHOVR_DEFAULT_IMAGE_BASE + kPageBytes - 1), base_ + kPageBytes - 1);
  // The second page is reserved, not committed.
  EXPECT_EQ(nevr::ResolveVA_Checked(base, nevr::ECHOVR_DEFAULT_IMAGE_BASE + kPageBytes), nullptr);
}

// ---- nevr_common.h: ValidatePrologue ----

TEST(ValidatePrologue, IsTrueOnlyWhenEveryExpectedByteMatches) {
  constexpr std::array<uint8_t, kSampleLen> kActual = {0x48, 0x89, 0x5C, 0x24};
  constexpr std::array<uint8_t, kSampleLen> kSame = {0x48, 0x89, 0x5C, 0x24};
  constexpr std::array<uint8_t, kSampleLen> kLastDiffers = {0x48, 0x89, 0x5C, 0x25};
  constexpr std::array<uint8_t, kSampleLen> kFirstDiffers = {0x49, 0x89, 0x5C, 0x24};
  constexpr size_t kPrefixLen = kShortSampleLen;
  EXPECT_TRUE(nevr::ValidatePrologue(kActual.data(), kSame.data(), kSame.size()));
  EXPECT_FALSE(nevr::ValidatePrologue(kActual.data(), kLastDiffers.data(), kLastDiffers.size()));
  EXPECT_FALSE(nevr::ValidatePrologue(kActual.data(), kFirstDiffers.data(), kFirstDiffers.size()));
  // Only the first `len` bytes are compared.
  EXPECT_TRUE(nevr::ValidatePrologue(kActual.data(), kLastDiffers.data(), kPrefixLen));
}

TEST(ValidatePrologue, RefusesNullPointersAndAZeroLength) {
  constexpr std::array<uint8_t, kShortSampleLen> kBytes = {0x48, 0x89};
  EXPECT_FALSE(nevr::ValidatePrologue(nullptr, kBytes.data(), kBytes.size()));
  EXPECT_FALSE(nevr::ValidatePrologue(kBytes.data(), nullptr, kBytes.size()));
  EXPECT_FALSE(nevr::ValidatePrologue(kBytes.data(), kBytes.data(), 0));
}

// ---- nevr_common.h: LoadConfigFile ----

class LoadConfigFileTest : public ::testing::Test {
 protected:
  // A folder under the run root, which the harness removes; nothing is removed here.
  void SetUp() override {
    root_ = nevr_test::NewDir(L"sdk-config");
    ASSERT_FALSE(root_.empty());
    std::filesystem::create_directories(root_ / "a" / "b");
    savedCwd_ = std::filesystem::current_path();
    std::filesystem::current_path(root_ / "a" / "b");
  }
  void TearDown() override { std::filesystem::current_path(savedCwd_); }
  static void Write(const std::filesystem::path& p, const std::string& body) {
    std::ofstream f(p, std::ios::binary);
    f << body;
  }
  std::filesystem::path root_;
  std::filesystem::path savedCwd_;
};

TEST_F(LoadConfigFileTest, MissingFileIsEmpty) {
  EXPECT_TRUE(nevr::LoadConfigFile("nevr_no_such_file.json").empty());
}

TEST_F(LoadConfigFileTest, ReadsTheFileInTheWorkingDirectoryByteForByte) {
  const std::string body = std::string("{\"k\":1}\r\n") + std::string(1, '\0') + "tail";
  Write(root_ / "a" / "b" / "c.json", body);
  EXPECT_EQ(nevr::LoadConfigFile("c.json"), body);
}

TEST_F(LoadConfigFileTest, FindsTheFileInTheParentAndGrandparentDirectories) {
  Write(root_ / "a" / "up1.json", "one-up");
  Write(root_ / "up2.json", "two-up");
  EXPECT_EQ(nevr::LoadConfigFile("up1.json"), "one-up");
  EXPECT_EQ(nevr::LoadConfigFile("up2.json"), "two-up");
}

TEST_F(LoadConfigFileTest, ClosestDirectoryWins) {
  Write(root_ / "same.json", "grandparent");
  Write(root_ / "a" / "same.json", "parent");
  EXPECT_EQ(nevr::LoadConfigFile("same.json"), "parent");
  Write(root_ / "a" / "b" / "same.json", "here");
  EXPECT_EQ(nevr::LoadConfigFile("same.json"), "here");
}

TEST_F(LoadConfigFileTest, DoesNotSearchBeyondTheGrandparent) {
  std::filesystem::create_directories(root_ / "a" / "b" / "c" / "d");
  Write(root_ / "a" / "far.json", "three-up");
  std::filesystem::current_path(root_ / "a" / "b" / "c" / "d");
  EXPECT_TRUE(nevr::LoadConfigFile("far.json").empty());
}

// ---- hook_manager.h, under real MinHook ----

// A hand-assembled function in an executable page: `mov eax, imm32` (5 bytes, the least MinHook can patch), `ret`,
// then int3 padding. It touches no callee-saved register, so calling it unhooked is safe.
class StubFunction {
 public:
  explicit StubFunction(uint32_t result) {
    page_ = static_cast<uint8_t*>(VirtualAlloc(nullptr, kPageBytes, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (page_ == nullptr) return;
    std::memset(page_, kFillByte, kStubFillBytes);
    constexpr uint8_t kMovEaxImm32 = 0xB8;
    constexpr uint8_t kRet = 0xC3;
    page_[0] = kMovEaxImm32;
    std::memcpy(page_ + 1, &result, sizeof(result));
    page_[1 + sizeof(result)] = kRet;
  }
  ~StubFunction() {
    if (page_ != nullptr) VirtualFree(page_, 0, MEM_RELEASE);
  }
  StubFunction(const StubFunction&) = delete;
  StubFunction& operator=(const StubFunction&) = delete;
  void* address() const { return page_; }
  uint32_t Call() const { return reinterpret_cast<uint32_t (*)()>(page_)(); }

 private:
  uint8_t* page_ = nullptr;
};

constexpr uint32_t kStubResult = 7;
constexpr uint32_t kOtherStubResult = 11;
constexpr uint32_t kDetourResult = 100;
constexpr uint32_t kOtherDetourResult = 200;

uint32_t DetourA() { return kDetourResult; }
uint32_t DetourB() { return kOtherDetourResult; }

// The stub pages are fixture members, so they outlive TearDown: MH_Uninitialize must still find every hooked page mapped.
class HookManagerTest : public ::testing::Test {
 protected:
  StubFunction fn{kStubResult};
  StubFunction foreign{kOtherStubResult};

  void SetUp() override {
    const MH_STATUS s = MH_Initialize();
    ASSERT_TRUE(s == MH_OK || s == MH_ERROR_ALREADY_INITIALIZED) << MH_StatusToString(s);
  }
  void TearDown() override { MH_Uninitialize(); }
};

TEST_F(HookManagerTest, CreateAndEnableRedirectsTheTargetAndRemoveAllRestoresIt) {
  ASSERT_NE(fn.address(), nullptr);
  nevr::HookManager hooks;
  void* original = nullptr;
  EXPECT_EQ(hooks.count(), 0u);
  ASSERT_EQ(hooks.CreateAndEnable(fn.address(), reinterpret_cast<void*>(&DetourA), &original), MH_OK);
  EXPECT_EQ(hooks.count(), 1u);
  EXPECT_EQ(fn.Call(), kDetourResult);
  ASSERT_NE(original, nullptr);
  EXPECT_EQ(reinterpret_cast<uint32_t (*)()>(original)(), kStubResult);

  hooks.RemoveAll();
  EXPECT_EQ(hooks.count(), 0u);
  EXPECT_EQ(fn.Call(), kStubResult);
}

// The point of the class: RemoveAll touches only this manager's hooks, never a hook someone else made
// (MH_DisableHook(MH_ALL_HOOKS) would take the host's hooks down with it).
TEST_F(HookManagerTest, RemoveAllLeavesAForeignHookInPlace) {
  ASSERT_NE(fn.address(), nullptr);
  ASSERT_NE(foreign.address(), nullptr);
  void* original = nullptr;
  ASSERT_EQ(MH_CreateHook(foreign.address(), reinterpret_cast<void*>(&DetourB), &original), MH_OK);
  ASSERT_EQ(MH_EnableHook(foreign.address()), MH_OK);

  nevr::HookManager hooks;
  ASSERT_EQ(hooks.CreateAndEnable(fn.address(), reinterpret_cast<void*>(&DetourA), &original), MH_OK);
  hooks.RemoveAll();
  EXPECT_EQ(fn.Call(), kStubResult);
  EXPECT_EQ(foreign.Call(), kOtherDetourResult);
}

TEST_F(HookManagerTest, FailedCreateReturnsTheStatusAndTracksNothing) {
  ASSERT_NE(fn.address(), nullptr);
  void* original = nullptr;
  nevr::HookManager first;
  ASSERT_EQ(first.CreateAndEnable(fn.address(), reinterpret_cast<void*>(&DetourA), &original), MH_OK);

  nevr::HookManager second;
  EXPECT_EQ(second.CreateAndEnable(fn.address(), reinterpret_cast<void*>(&DetourB), &original),
            MH_ERROR_ALREADY_CREATED);
  EXPECT_EQ(second.count(), 0u);
  // The first manager's hook is untouched by the failed second attempt.
  EXPECT_EQ(fn.Call(), kDetourResult);
  first.RemoveAll();
}

TEST_F(HookManagerTest, TrackAdoptsAHookMadeDirectlyAndRemoveAllRemovesIt) {
  ASSERT_NE(fn.address(), nullptr);
  void* original = nullptr;
  ASSERT_EQ(MH_CreateHook(fn.address(), reinterpret_cast<void*>(&DetourA), &original), MH_OK);
  ASSERT_EQ(MH_EnableHook(fn.address()), MH_OK);
  nevr::HookManager hooks;
  hooks.Track(fn.address());
  EXPECT_EQ(hooks.count(), 1u);
  EXPECT_EQ(fn.Call(), kDetourResult);
  hooks.RemoveAll();
  EXPECT_EQ(fn.Call(), kStubResult);
}

// An enable that fails must remove the hook it just created, so the target can be hooked again.
// A read-only view of an executable section cannot be made writable, which MH_EnableHook needs to patch it.
TEST_F(HookManagerTest, FailedEnableRemovesTheCreatedHookAndTracksNothing) {
  HANDLE section = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_EXECUTE_READWRITE, 0, kPageBytes, nullptr);
  ASSERT_NE(section, nullptr);
  void* writable = MapViewOfFile(section, FILE_MAP_WRITE, 0, 0, kPageBytes);
  ASSERT_NE(writable, nullptr);
  constexpr uint8_t code[] = {0xB8, 0x07, 0x00, 0x00, 0x00, 0xC3, kFillByte, kFillByte, kFillByte, kFillByte, kFillByte};
  std::memcpy(writable, code, sizeof(code));
  UnmapViewOfFile(writable);
  void* readOnly = MapViewOfFile(section, FILE_MAP_READ | FILE_MAP_EXECUTE, 0, 0, kPageBytes);
  ASSERT_NE(readOnly, nullptr);

  nevr::HookManager hooks;
  void* original = nullptr;
  const MH_STATUS status = hooks.CreateAndEnable(readOnly, reinterpret_cast<void*>(&DetourA), &original);
  EXPECT_NE(status, MH_OK);
  EXPECT_EQ(hooks.count(), 0u);
  // The rollback removed the created hook: creating it again is not "already created".
  void* again = nullptr;
  EXPECT_NE(MH_CreateHook(readOnly, reinterpret_cast<void*>(&DetourA), &again), MH_ERROR_ALREADY_CREATED);
  MH_RemoveHook(readOnly);

  UnmapViewOfFile(readOnly);
  CloseHandle(section);
}

}  // namespace
