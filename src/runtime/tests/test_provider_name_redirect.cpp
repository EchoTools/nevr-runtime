#include <gtest/gtest.h>
#include <winsock2.h>  // before windows.h (tools/tests/test_header_include_order.py)

#include <windows.h>

#include <cstdint>
#include <cstring>

#include <MinHook.h>

#include "runtime/hook/addresses.h"
#include "runtime/patch/provider_name_redirect.h"

// The redirect is a detour on the provider loader (CNSProvider::LoadByName, echovr.exe 0x140606690). These tests drive
// the real detour through MinHook on a stand-in: an 11-byte function that returns its second argument (`mov rax,rdx`),
// so the value it returns is the name the hook handed to the original. That proves the argument the loader receives,
// not the loader itself.

namespace {

// mov rax,rdx; nop x7; ret. The nops give MinHook room for its 5-byte jump.
constexpr std::uint8_t kStandIn[] = {0x48, 0x89, 0xD0, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0xC3};

using LoaderFn = std::uint64_t (*)(void* slot, const char* name, void* cfg, void* out);

char* MakeStandIn() {
  auto* base = static_cast<char*>(VirtualAlloc(nullptr, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
  if (base != nullptr) std::memcpy(base, kStandIn, sizeof(kStandIn));
  return base;
}

// The name the loader received: the stand-in returns it.
std::uint64_t NameReceived(char* detour, const char* name) {
  return reinterpret_cast<LoaderFn>(detour)(nullptr, name, nullptr, nullptr);
}

}  // namespace

TEST(ProviderNameRedirect, PnsovrAndPnsdemoReachTheLoaderAsPnsrad) {
  ASSERT_EQ(MH_Initialize(), MH_OK);
  char* base = MakeStandIn();
  ASSERT_NE(base, nullptr);
  ASSERT_TRUE(nevr_provider_name_redirect::InstallAt(base, kStandIn, sizeof(kStandIn)));
  EXPECT_STREQ(reinterpret_cast<const char*>(NameReceived(base, "pnsovr")), "pnsrad");
  EXPECT_STREQ(reinterpret_cast<const char*>(NameReceived(base, "pnsdemo")), "pnsrad");
  MH_Uninitialize();
  VirtualFree(base, 0, MEM_RELEASE);
}

TEST(ProviderNameRedirect, AnyOtherNameIsPassedThroughUnchanged) {
  ASSERT_EQ(MH_Initialize(), MH_OK);
  char* base = MakeStandIn();
  ASSERT_NE(base, nullptr);
  ASSERT_TRUE(nevr_provider_name_redirect::InstallAt(base, kStandIn, sizeof(kStandIn)));
  // "pnsrad" is the third provider slot's own call: it must reach the loader as the same pointer.
  const char* radName = "pnsrad";
  const char* other = "other";
  const char* prefixOnly = "pnsxyz";  // a pns* name that is not one of the two: a prefix match would rewrite it
  EXPECT_EQ(NameReceived(base, radName), reinterpret_cast<std::uintptr_t>(radName));
  EXPECT_EQ(NameReceived(base, other), reinterpret_cast<std::uintptr_t>(other));
  EXPECT_EQ(NameReceived(base, prefixOnly), reinterpret_cast<std::uintptr_t>(prefixOnly));
  // The game's loader tolerates a null name (its param_2 == 0 branch): the hook must pass it on, not dereference it.
  EXPECT_EQ(NameReceived(base, nullptr), 0u);
  MH_Uninitialize();
  VirtualFree(base, 0, MEM_RELEASE);
}

TEST(ProviderNameRedirect, AnUnexpectedPrologueLeavesTheLoaderUntouched) {
  ASSERT_EQ(MH_Initialize(), MH_OK);
  char* base = MakeStandIn();
  ASSERT_NE(base, nullptr);
  std::uint8_t wrong[sizeof(kStandIn)];
  std::memcpy(wrong, kStandIn, sizeof(kStandIn));
  wrong[0] = 0xCC;  // not this build's bytes
  EXPECT_FALSE(nevr_provider_name_redirect::InstallAt(base, wrong, sizeof(wrong)));
  EXPECT_EQ(std::memcmp(base, kStandIn, sizeof(kStandIn)), 0) << "a refused install must write nothing";
  const char* ovr = "pnsovr";
  EXPECT_EQ(NameReceived(base, ovr), reinterpret_cast<std::uintptr_t>(ovr))
      << "with no detour, the stand-in returns the name it was given";
  MH_Uninitialize();
  VirtualFree(base, 0, MEM_RELEASE);
}

TEST(ProviderNameRedirect, AFailedDetourReturnsFalseAndNamesNothingInstalled) {
  // MinHook is not initialised: MH_CreateHook refuses, so PatchDetour fails for real.
  char* base = MakeStandIn();
  ASSERT_NE(base, nullptr);
  EXPECT_FALSE(nevr_provider_name_redirect::InstallAt(base, kStandIn, sizeof(kStandIn)));
  VirtualFree(base, 0, MEM_RELEASE);
}

TEST(ProviderNameRedirect, InstallChecksThePinnedPrologueAtThePinnedRva) {
  using namespace nevr_patch_addresses;
  ASSERT_EQ(MH_Initialize(), MH_OK);
  const std::size_t imageSize = static_cast<std::size_t>(CNSPROVIDER_LOAD_BY_NAME) + 0x1000;
  auto* image = static_cast<char*>(VirtualAlloc(nullptr, imageSize, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
  ASSERT_NE(image, nullptr);
  const auto gameBase = reinterpret_cast<std::uintptr_t>(image);
  // Zeroed memory is not the loader: Install refuses and writes nothing.
  EXPECT_FALSE(nevr_provider_name_redirect::Install(gameBase));
  EXPECT_EQ(image[CNSPROVIDER_LOAD_BY_NAME], 0) << "a refused install must write nothing";
  // The pinned bytes at the pinned RVA are accepted: the detour is written over the first bytes.
  std::memcpy(image + CNSPROVIDER_LOAD_BY_NAME, CNSPROVIDER_LOAD_BY_NAME_PROLOGUE,
              sizeof(CNSPROVIDER_LOAD_BY_NAME_PROLOGUE));
  EXPECT_TRUE(nevr_provider_name_redirect::Install(gameBase));
  EXPECT_EQ(static_cast<unsigned char>(image[CNSPROVIDER_LOAD_BY_NAME]), 0xE9u)
      << "the loader's first byte becomes MinHook's jump to the detour (never run: the bytes are not code to call)";
  MH_Uninitialize();
  VirtualFree(image, 0, MEM_RELEASE);
}
