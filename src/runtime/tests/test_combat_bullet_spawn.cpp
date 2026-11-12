// #2336: the bullet spawn (CR15NetBulletCS::FireBullet, echovr.exe 0x140ce2020) has one detour, runtime/combat/
// bullet_spawn_hook.h, and PowerBoost's drop and DroneFix's tracer rule are filters on it.
//
// The detour is the production code (SetDrop, SetAfter, the write gate, PatchDetour, HookGuard), installed under MinHook
// into a fake game image: the spawn sits at its real RVA with its real 12 prologue bytes, followed by code that undoes the
// prologue's pushes and jumps to a C++ stand-in that records the call. The filters are stand-ins for the two modules'
// (powerboost.cpp DropBulletSpawn, dronefix.cpp AfterBulletSpawn), which read game state these tests do not model; that
// each module sets its filter through this owner is tools/tests/test_combat_bullet_spawn_wiring.py.
//
// NOT shown here: that echovr.exe has these bytes on a live process, and that OVERCHARGE and MEDIGUN start in a run with
// combat.medigun on (the live row of #2336).

#include <gtest/gtest.h>

#include <winsock2.h>
#include <windows.h>

#include <MinHook.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "core/hooking.h"
#include "core/logging.h"
#include "runtime/combat/bullet_spawn_hook.h"
#include "runtime/combat/write_gate.h"
#include "runtime/hook/hook_guard.h"

namespace {

namespace bs = nevr_combat::bullet_spawn;

constexpr std::size_t kImageBytes = bs::kRva + 0x1000;

// The prologue (MOV [RSP+0x10],RBX; PUSH RBP,RSI,RDI,R12,R13), then its undo (POP R13,R12,RDI,RSI,RBP; RBX was only
// stored), then `movabs rax, imm64; jmp rax`. The stack arguments are where the caller put them when the stand-in runs.
constexpr std::array<std::uint8_t, 7> kUndo = {0x41, 0x5D, 0x41, 0x5C, 0x5F, 0x5E, 0x5D};

std::vector<std::string> g_order;
bs::Call g_seen{};
bool g_dropAnswer = false;

void FakeSpawn(std::uint8_t* sys, std::uint64_t index, void* ref, float speed, const void* pose, const float* velocity,
               int remote, std::uint32_t flags) {
  g_order.push_back("spawn");
  g_seen = bs::Call{sys, index, ref, speed, pose, velocity, remote, flags};
}

bs::Call g_dropSaw{};
bool Drop(const bs::Call& call) {
  g_order.push_back("drop");
  g_dropSaw = call;
  return g_dropAnswer;
}

bs::Call g_afterSaw{};
void After(const bs::Call& call) {
  g_order.push_back("after");
  g_afterSaw = call;
}

std::vector<std::pair<EchoVR::LogLevel, std::string>> g_logged;
void CaptureLog(EchoVR::LogLevel level, const CHAR* message) { g_logged.emplace_back(level, message); }

int Count(const std::string& needle) {
  int n = 0;
  for (const auto& line : g_logged) n += line.second.find(needle) != std::string::npos ? 1 : 0;
  return n;
}

const std::pair<EchoVR::LogLevel, std::string>* Find(const std::string& needle) {
  for (const auto& line : g_logged) {
    if (line.second.find(needle) != std::string::npos) return &line;
  }
  return nullptr;
}

using SpawnFn = void (*)(std::uint8_t*, std::uint64_t, void*, float, const void*, const float*, int, std::uint32_t);

class CombatBulletSpawn : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(Hooking::Initialize());
    nevr_hook_guard::ResetForTest();
    g_order.clear();
    g_logged.clear();
    g_seen = g_dropSaw = g_afterSaw = bs::Call{};
    g_dropAnswer = false;
    SetBootLogSink(&CaptureLog);
    image_ = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, kImageBytes, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
    ASSERT_NE(image_, nullptr) << "VirtualAlloc error=" << GetLastError();
    std::vector<std::uint8_t> stub(std::begin(bs::kPrologue), std::end(bs::kPrologue));
    stub.insert(stub.end(), kUndo.begin(), kUndo.end());
    const auto fake = reinterpret_cast<std::uintptr_t>(&FakeSpawn);
    stub.push_back(0x48);  // movabs rax, imm64
    stub.push_back(0xB8);
    for (int i = 0; i < 8; ++i) stub.push_back(static_cast<std::uint8_t>((fake >> (8 * i)) & 0xFF));
    stub.push_back(0xFF);  // jmp rax
    stub.push_back(0xE0);
    std::memcpy(Target(), stub.data(), stub.size());
  }

  void TearDown() override {
    bs::SetDrop(Base(), nullptr, "test");
    bs::SetAfter(Base(), nullptr, "test");
    if (image_ != nullptr) {
      MH_RemoveHook(Target());
      VirtualFree(image_, 0, MEM_RELEASE);
    }
    image_ = nullptr;
    nevr_hook_guard::ResetForTest();
    SetBootLogSink(nullptr);
  }

  std::uintptr_t Base() const { return reinterpret_cast<std::uintptr_t>(image_); }
  std::uint8_t* Target() const { return image_ + bs::kRva; }

  // One call of the game's spawn, the way FireBulletLocal makes it: through the function's address.
  void Fire() {
    reinterpret_cast<SpawnFn>(Target())(sys_, 0x10007, &ref_, 120.5f, pose_, velocity_, 1, 0x40);
  }

  std::uint8_t* image_ = nullptr;
  std::uint8_t sys_[16] = {};
  int ref_ = 0;
  float pose_[8] = {};
  float velocity_[3] = {1.0f, 2.0f, 3.0f};
};

TEST_F(CombatBulletSpawn, ASecondDetourOnTheSpawnIsRefusedButASecondFilterIsSet) {
  // The refusal #2336 logged: DroneFix's hook is on the spawn, and a second detour on the same bytes (PowerBoost's, as
  // ported) is refused by the write gate.
  ASSERT_TRUE(bs::SetAfter(Base(), &After, "dronefix"));
  EXPECT_FALSE(nevr_combat::GateWrite(Target(), 16, "Hooked_BulletSpawn"));
  EXPECT_EQ(Count("overlap refused what=Hooked_BulletSpawn"), 1);
  // The fix: PowerBoost's rule is a filter on the one detour, so it is set and nothing is refused.
  g_logged.clear();
  EXPECT_TRUE(bs::SetDrop(Base(), &Drop, "powerboost"));
  EXPECT_EQ(Count("overlap refused"), 0);
  EXPECT_EQ(Count("bullet spawn filter set filter=drop module=powerboost"), 1);
  EXPECT_EQ(Count("bullet spawn hook applied"), 0) << "the detour was already in place";
}

TEST_F(CombatBulletSpawn, BothFiltersRunInOrderForOneCall) {
  ASSERT_TRUE(bs::SetAfter(Base(), &After, "dronefix"));
  ASSERT_TRUE(bs::SetDrop(Base(), &Drop, "powerboost"));
  Fire();
  EXPECT_EQ(g_order, (std::vector<std::string>{"drop", "spawn", "after"}));
  for (const bs::Call* c : {&g_seen, &g_dropSaw, &g_afterSaw}) {
    EXPECT_EQ(c->sys, sys_);
    EXPECT_EQ(c->index, 0x10007u);
    EXPECT_EQ(c->ref, &ref_);
    EXPECT_FLOAT_EQ(c->speed, 120.5f);
    EXPECT_EQ(c->pose, pose_);
    EXPECT_EQ(c->velocity, velocity_);
    EXPECT_EQ(c->remote, 1);
    EXPECT_EQ(c->flags, 0x40u);
  }
}

TEST_F(CombatBulletSpawn, ADroppedBulletReachesNeitherTheSpawnNorTheAfterFilter) {
  ASSERT_TRUE(bs::SetAfter(Base(), &After, "dronefix"));
  ASSERT_TRUE(bs::SetDrop(Base(), &Drop, "powerboost"));
  g_dropAnswer = true;
  Fire();
  EXPECT_EQ(g_order, (std::vector<std::string>{"drop"}));
}

TEST_F(CombatBulletSpawn, WhicheverModuleAsksFirstInstallsTheOneDetour) {
  ASSERT_TRUE(bs::SetDrop(Base(), &Drop, "powerboost"));
  ASSERT_TRUE(bs::SetAfter(Base(), &After, "dronefix"));
  EXPECT_TRUE(bs::Installed());
  EXPECT_EQ(Count("bullet spawn hook applied module=powerboost"), 1);
  EXPECT_EQ(Count("bullet spawn hook applied"), 1);
  Fire();
  EXPECT_EQ(g_order, (std::vector<std::string>{"drop", "spawn", "after"}));
}

TEST_F(CombatBulletSpawn, OneFilterAloneRuns) {
  ASSERT_TRUE(bs::SetDrop(Base(), &Drop, "powerboost"));
  Fire();
  EXPECT_EQ(g_order, (std::vector<std::string>{"drop", "spawn"}));
  ASSERT_TRUE(bs::SetDrop(Base(), nullptr, "powerboost"));
  ASSERT_TRUE(bs::SetAfter(Base(), &After, "dronefix"));
  g_order.clear();
  Fire();
  EXPECT_EQ(g_order, (std::vector<std::string>{"spawn", "after"}));
}

TEST_F(CombatBulletSpawn, ClearingTheLastFilterRemovesTheDetour) {
  ASSERT_TRUE(bs::SetDrop(Base(), &Drop, "powerboost"));
  ASSERT_TRUE(bs::SetAfter(Base(), &After, "dronefix"));
  ASSERT_TRUE(bs::SetDrop(Base(), nullptr, "powerboost"));
  EXPECT_EQ(Count("bullet spawn filter cleared filter=drop module=powerboost"), 1);
  EXPECT_TRUE(bs::Installed()) << "DroneFix's filter is still on it";
  EXPECT_NE(std::memcmp(Target(), bs::kPrologue, sizeof(bs::kPrologue)), 0);
  ASSERT_TRUE(bs::SetAfter(Base(), nullptr, "dronefix"));
  EXPECT_FALSE(bs::Installed());
  EXPECT_EQ(std::memcmp(Target(), bs::kPrologue, sizeof(bs::kPrologue)), 0) << "the game's bytes are back";
  EXPECT_EQ(Count("bullet spawn hook removed"), 1);
  nevr_hook_guard::Overlap hit{};
  EXPECT_FALSE(nevr_hook_guard::FindOverlap(Target(), 16, &hit)) << "HookGuard forgot the address";
  g_order.clear();
  Fire();
  EXPECT_EQ(g_order, (std::vector<std::string>{"spawn"}));
}

TEST_F(CombatBulletSpawn, AChangedPrologueInstallsNothingAndSaysWhy) {
  Target()[3] = 0x25;
  EXPECT_FALSE(bs::SetDrop(Base(), &Drop, "powerboost"));
  EXPECT_FALSE(bs::Installed());
  const auto* line = Find("bullet spawn hook not installed module=powerboost");
  ASSERT_NE(line, nullptr);
  EXPECT_EQ(line->first, EchoVR::LogLevel::Warning);
  EXPECT_NE(line->second.find("reason=prologue_mismatch expected=48895c241055565741544155 actual=48895c251055565741544155"),
            std::string::npos)
      << line->second;
  EXPECT_EQ(Target()[0], 0x48) << "nothing was written";
}

TEST_F(CombatBulletSpawn, ARangeTheRuntimeAlreadyOwnsInstallsNothing) {
  nevr_hook_guard::Record(Target(), "SomeoneElse");
  EXPECT_FALSE(bs::SetAfter(Base(), &After, "dronefix"));
  EXPECT_FALSE(bs::Installed());
  EXPECT_EQ(Count("overlap refused what=CR15NetBulletCS::FireBullet "), 1);
  const auto* line = Find("bullet spawn hook not installed module=dronefix");
  ASSERT_NE(line, nullptr);
  EXPECT_NE(line->second.find("reason=overlap"), std::string::npos);
  EXPECT_EQ(std::memcmp(Target(), bs::kPrologue, sizeof(bs::kPrologue)), 0) << "nothing was written";
}

}  // namespace
