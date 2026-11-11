// #600: the podium terminal's RULES button in Combat (runtime/combat/rules_button.h): the decision, and the two slots
// installed through the script hook table on a fake podium script.
//
// The fake script has two functions in the shape of the terminal's copies: each loads its flag's field id as a dword
// (`mov edx, dword [rip+id]`, with a nonzero dword after it), points r8 at the byte it is given, and calls the accessor
// through its slot (0x3E748 for matchisarena, 0x3E868 for matchiscombat). The slots hold a function of this test
// standing in for the engine's, which writes the game's own flag byte (a Combat match: arena 0, combat 1). The two bytes
// are adjacent in a test buffer with nonzero bytes around them, as +0x169 and +0x16A are.
//
// The first read of each flag on each load is logged once, with the engine's byte and the one written.
//
// NOT shown by these tests: the shipped script's bytes in a run, and the RULES button pressed in a Combat match.

#include <gtest/gtest.h>

#include <winsock2.h>
#include <windows.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <tuple>
#include <vector>

#include <MinHook.h>

#include "core/hooking.h"
#include "core/logging.h"
#include "runtime/combat/rules_button.h"
#include "runtime/hook/hook_guard.h"
#include "runtime/hook/script_hooks.h"

namespace {

namespace rb = nevr_combat::rules_button;
namespace sh = nevr_script_hooks;

constexpr std::uint32_t kArenaId = 0x61, kCombatId = 0x62, kNext = 0x5a5a5a5a;
constexpr std::uint32_t kArenaCopy = 0x31000, kCombatCopy = 0x31100, kOtherRead = 0x31200, kOtherIdRva = 0x3E900;
constexpr std::size_t kImage = 0x3F000;

std::vector<std::string> g_logged;
void CaptureLog(EchoVR::LogLevel, const CHAR* message) { g_logged.push_back(message); }
int Count(const char* needle) {
  int n = 0;
  for (const std::string& line : g_logged) n += line.find(needle) != std::string::npos ? 1 : 0;
  return n;
}

int g_engine_calls = 0;
std::uint64_t EngineFlag(void*, std::uint64_t field, void* out) {
  ++g_engine_calls;
  *static_cast<std::uint8_t*>(out) = static_cast<std::uint32_t>(field) == kCombatId ? 1 : 0;  // a Combat match
  return 0x55;
}

void Emit(std::vector<std::uint8_t>& v, std::initializer_list<std::uint8_t> b) { v.insert(v.end(), b); }
void Rel32(std::vector<std::uint8_t>& v, std::uint32_t at_rva, std::uint32_t target_rva) {
  const std::int32_t d = static_cast<std::int32_t>(target_rva) - static_cast<std::int32_t>(at_rva + v.size() + 4);
  for (int i = 0; i < 4; ++i) v.push_back(static_cast<std::uint8_t>(static_cast<std::uint32_t>(d) >> (8 * i)));
}
// `u64 (u8* out)`: sub rsp, 0x28; mov edx, dword [rip+id]; mov r8, rcx; xor ecx, ecx; call [rip+slot]; add rsp; ret.
std::vector<std::uint8_t> Copy(std::uint32_t at, std::uint32_t id_rva, std::uint32_t slot_rva) {
  std::vector<std::uint8_t> v;
  Emit(v, {0x48, 0x83, 0xec, 0x28, 0x8b, 0x15});
  Rel32(v, at, id_rva);
  Emit(v, {0x49, 0x89, 0xc8, 0x31, 0xc9, 0xff, 0x15});
  Rel32(v, at, slot_rva);
  Emit(v, {0x48, 0x83, 0xc4, 0x28, 0xc3});
  return v;
}

TEST(RulesButtonDecision, TheFlagByteIsWrittenForItsFieldOnly) {
  std::uint8_t data[4] = {0xa5, 0, 0, 0xa5};
  EXPECT_EQ(rb::ReadFlag(&EngineFlag, kArenaId, 1, nullptr, kArenaId, data + 1), 0x55u) << "the engine's answer";
  EXPECT_EQ(data[1], 1) << "matchisarena reads 1";
  EXPECT_EQ(data[2], 0) << "one byte written";
  EXPECT_EQ(data[0], 0xa5);
  EXPECT_EQ(rb::ReadFlag(&EngineFlag, kArenaId, 1, nullptr, kCombatId, data + 2), 0x55u);
  EXPECT_EQ(data[2], 1) << "another field: the engine's byte";
  EXPECT_EQ(rb::ReadFlag(&EngineFlag, kCombatId, 0, nullptr, (std::uint64_t{0xffff} << 32) | kCombatId, data + 2), 0x55u);
  EXPECT_EQ(data[2], 0) << "the id is a dword: the register's upper half does not matter";
  EXPECT_EQ(data[3], 0xa5);
}

class RulesButtonInstalled : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(Hooking::Initialize());
    nevr_hook_guard::ResetForTest();
    sh::ResetForTest();
    rb::ResetForTest();
    g_logged.clear();
    SetBootLogSink(&CaptureLog);
    g_engine_calls = 0;
    base_ = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, kImage, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
    ASSERT_NE(base_, nullptr);
    for (const auto& [at, id, slot] : {std::tuple{kArenaCopy, rb::kArenaFieldIdRva, rb::kArenaSlotRva},
                                       std::tuple{kCombatCopy, rb::kCombatFieldIdRva, rb::kCombatSlotRva},
                                       std::tuple{kOtherRead, kOtherIdRva, rb::kArenaSlotRva}}) {
      const std::vector<std::uint8_t> v = Copy(at, id, slot);
      std::memcpy(base_ + at, v.data(), v.size());
    }
    for (const auto& [rva, id] : {std::pair{rb::kArenaFieldIdRva, kArenaId}, std::pair{rb::kCombatFieldIdRva, kCombatId},
                                  std::pair{kOtherIdRva, kCombatId + 1}}) {
      std::memcpy(base_ + rva, &id, sizeof id);
      std::memcpy(base_ + rva + 4, &kNext, sizeof kNext);
    }
    void* engine = reinterpret_cast<void*>(&EngineFlag);
    std::memcpy(base_ + rb::kArenaSlotRva, &engine, sizeof engine);
    std::memcpy(base_ + rb::kCombatSlotRva, &engine, sizeof engine);
    std::memset(data_, 0xa5, sizeof data_);
  }
  void TearDown() override {
    VirtualFree(base_, 0, MEM_RELEASE);
    sh::ResetForTest();
    rb::ResetForTest();
    SetBootLogSink(nullptr);
  }
  std::uint64_t Call(std::uint32_t rva, std::uint8_t* out) {
    return reinterpret_cast<std::uint64_t (*)(void*)>(base_ + rva)(out);
  }
  void Load() {
    rb::Register();
    sh::OnLoaded(rb::kScript, base_);
  }
  std::uint8_t* base_ = nullptr;
  std::uint8_t data_[0x170];
};

TEST_F(RulesButtonInstalled, ThePodiumReadsArenaNotCombat) {
  Load();
  EXPECT_EQ(Count("script hooks installed script=d4653fc2065e8184"), 1);
  EXPECT_EQ(Count("hooks=0 slots=2"), 2) << "the registration line and the install line";
  EXPECT_EQ(Call(kArenaCopy, data_ + 0x169), 0x55u);
  EXPECT_EQ(Call(kCombatCopy, data_ + 0x16A), 0x55u);
  EXPECT_EQ(data_[0x169], 1) << "matchisarena";
  EXPECT_EQ(data_[0x16A], 0) << "matchiscombat";
  EXPECT_EQ(data_[0x168], 0xa5);
  EXPECT_EQ(data_[0x16B], 0xa5);
  EXPECT_EQ(g_engine_calls, 2) << "the engine is still asked";
  // Another field through the arena slot: the engine's byte.
  data_[0x100] = 0x77;
  Call(kOtherRead, data_ + 0x100);
  EXPECT_EQ(data_[0x100], 0) << "the engine's answer for another field";
}

TEST_F(RulesButtonInstalled, StockAfterTheFree) {
  Load();
  sh::OnUnloading(rb::kScript, base_);
  Call(kArenaCopy, data_ + 0x169);
  Call(kCombatCopy, data_ + 0x16A);
  EXPECT_EQ(data_[0x169], 0) << "the game's own: a Combat match is not Arena";
  EXPECT_EQ(data_[0x16A], 1);
}

TEST_F(RulesButtonInstalled, EachLoadLogsEachFlagsFirstReadOnce) {
  Load();
  Call(kArenaCopy, data_ + 0x169);
  Call(kArenaCopy, data_ + 0x169);
  Call(kCombatCopy, data_ + 0x16A);
  Call(kCombatCopy, data_ + 0x16A);
  Call(kOtherRead, data_ + 0x100);
  EXPECT_EQ(Count("rules button: first read on this load field=matchisarena engine=0 written=1 "
                  "script=d4653fc2065e8184"), 1);
  EXPECT_EQ(Count("rules button: first read on this load field=matchiscombat engine=1 written=0 "
                  "script=d4653fc2065e8184"), 1);
  EXPECT_EQ(Count("rules button: first read"), 2) << "one line a flag, not one a read; another field logs nothing";
  EXPECT_EQ(data_[0x169], 1) << "still written after the first read";
  EXPECT_EQ(data_[0x16A], 0);
  // The game frees the script and loads it again at the same base: a new load, a new line.
  sh::OnUnloading(rb::kScript, base_);
  sh::OnLoaded(rb::kScript, base_);
  Call(kOtherRead, data_ + 0x100);
  EXPECT_EQ(Count("rules button: first read"), 2) << "another field through the arena slot is not its first read";
  Call(kArenaCopy, data_ + 0x169);
  Call(kArenaCopy, data_ + 0x169);
  EXPECT_EQ(Count("rules button: first read on this load field=matchisarena"), 2);
  EXPECT_EQ(Count("rules button: first read on this load field=matchiscombat"), 1) << "not read on this load yet";
}

TEST_F(RulesButtonInstalled, WithoutTheModuleTheSlotOnlyPassesThrough) {
  Load();
  rb::ForgetModulesForTest();
  const std::uint32_t zero = 0;
  std::memcpy(base_ + rb::kArenaFieldIdRva, &zero, sizeof zero);
  EXPECT_EQ(Call(kArenaCopy, data_ + 0x169), 0x55u) << "the engine's answer";
  EXPECT_EQ(data_[0x169], 0) << "field 0 is not taken for matchisarena: the engine's byte";
  EXPECT_EQ(Count("rules button: first read"), 0);
}

TEST_F(RulesButtonInstalled, RegisteringTwiceAddsTheSlotsOnce) {
  rb::Register();
  rb::Register();
  sh::OnLoaded(rb::kScript, base_);
  EXPECT_EQ(Count("rules button fix registered"), 1);
  EXPECT_EQ(Count("script hooks installed script=d4653fc2065e8184 base="), 1);
}

}  // namespace
