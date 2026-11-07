// #600: the shared magazine's frisbee condition (runtime/combat/shared_magazine.h): the decisions, and the hook and the
// slot installed through the script hook table under MinHook on a fake shared magazine script.
//
// The fake condition at RVA 0x1F290 starts with the real first bytes (its movdqa reads a 16-byte constant placed at an
// aligned RVA), then does what the script's does: loads the type field's id as a dword (`mov edx, dword [rip+0x2B194]`,
// with a nonzero dword after it), calls the field accessor through the slot at 0x2BD60 with out = [rsp+0x28], and returns
// `qword [rsp+0x28] != arc`. The slot holds a function of this test executable standing in for the engine's: it writes
// the qword type the test chose.
//
// NOT shown by these tests: the shipped script's bytes in a run, and a DOT or a drone thrown in a match.

#include <gtest/gtest.h>

#include <winsock2.h>
#include <windows.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <MinHook.h>

#include "core/hooking.h"
#include "core/logging.h"
#include "runtime/combat/shared_magazine.h"
#include "runtime/hook/hook_guard.h"
#include "runtime/hook/script_hooks.h"

namespace {

namespace sm = nevr_combat::shared_magazine;
namespace sh = nevr_script_hooks;

constexpr std::uint32_t kTypeField = 0x77;
constexpr std::uint32_t kNextField = 0x5a5a5a5a;  // the dword after the id: nonzero, so a wider read of the id shows
constexpr std::uint64_t kDet = 0xc8e8d0b1a894e7feULL;
// The dot's low dword with other high bits: not the dot (his patch compared 16 bits; this compares the whole symbol).
constexpr std::uint64_t kNotDot = (std::uint64_t{0xdeadbeef} << 32) | (sm::kDot & 0xffffffffULL);
constexpr std::uint32_t kConstantRva = 0x291b0;
constexpr std::uint32_t kOtherCallerRva = 0x20000;

std::vector<std::string> g_logged;
void CaptureLog(EchoVR::LogLevel, const CHAR* message) { g_logged.push_back(message); }
int Count(const char* needle) {
  int n = 0;
  for (const std::string& line : g_logged) n += line.find(needle) != std::string::npos ? 1 : 0;
  return n;
}

std::uint64_t g_type = 0;  // what the stand-in engine writes for any field
int g_engine_calls = 0;
std::uint64_t EngineAccessor(void*, std::uint64_t, void* out) {
  ++g_engine_calls;
  std::memcpy(out, &g_type, sizeof g_type);
  return 0x55;
}

void Emit(std::vector<std::uint8_t>& v, std::initializer_list<std::uint8_t> b) { v.insert(v.end(), b); }
void Rel32(std::vector<std::uint8_t>& v, std::uint32_t at_rva, std::uint32_t target_rva) {
  const std::int32_t d = static_cast<std::int32_t>(target_rva) - static_cast<std::int32_t>(at_rva + v.size() + 4);
  for (int i = 0; i < 4; ++i) v.push_back(static_cast<std::uint8_t>(static_cast<std::uint32_t>(d) >> (8 * i)));
}
void Imm64(std::vector<std::uint8_t>& v, std::uint64_t x) {
  for (int i = 0; i < 8; ++i) v.push_back(static_cast<std::uint8_t>(x >> (8 * i)));
}

TEST(SharedMagazineDecision, NoFrisbeeForTheDotOrTheDrone) {
  EXPECT_FALSE(sm::Enables(false, true, kDet)) << "the game said no (the arc): no";
  EXPECT_TRUE(sm::Enables(true, false, 0)) << "no type recorded: the game's answer";
  EXPECT_TRUE(sm::Enables(true, false, sm::kDot)) << "a type not recorded by this condition is not used";
  EXPECT_FALSE(sm::Enables(true, true, sm::kDot));
  EXPECT_FALSE(sm::Enables(true, true, sm::kDrone));
  EXPECT_TRUE(sm::Enables(true, true, kDet));
  EXPECT_TRUE(sm::Enables(true, true, kNotDot)) << "the whole symbol is compared";
}

TEST(SharedMagazineField, OnlyTheTypeFieldInsideTheConditionIsRecorded) {
  sm::ResetForTest();
  g_type = sm::kDot;
  std::uint64_t out = 0;
  std::uint64_t type = 0;
  EXPECT_EQ(sm::ReadField(&EngineAccessor, kTypeField, false, nullptr, kTypeField, &out), 0x55u);
  EXPECT_FALSE(sm::RecordedType(&type)) << "outside the condition";
  sm::ReadField(&EngineAccessor, kTypeField, true, nullptr, kTypeField + 1, &out);
  EXPECT_FALSE(sm::RecordedType(&type)) << "another field";
  sm::ReadField(&EngineAccessor, kTypeField, true, nullptr, (std::uint64_t{0xffff} << 32) | kTypeField, &out);
  EXPECT_TRUE(sm::RecordedType(&type)) << "the id is a dword: the register's upper half does not matter";
  EXPECT_EQ(type, sm::kDot);
}

class SharedMagazineInstalled : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(Hooking::Initialize());
    nevr_hook_guard::ResetForTest();
    sh::ResetForTest();
    sm::ResetForTest();
    g_logged.clear();
    SetBootLogSink(&CaptureLog);
    base_ = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, 0x30000, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
    ASSERT_NE(base_, nullptr);
    // The condition: the real start (sub rsp, 0x388; mov rax, [rcx]; movdqa xmm0, [rip+const]), then the type read.
    const std::uint32_t at = sm::kConditionRva;
    std::vector<std::uint8_t> v(sm::kConditionPrologue, sm::kConditionPrologue + sizeof sm::kConditionPrologue);
    Rel32(v, at, kConstantRva);                         // (the movdqa's disp32)
    Emit(v, {0x8b, 0x15});                              // mov edx, dword [rip+type field id]
    Rel32(v, at, sm::kTypeFieldIdRva);
    Emit(v, {0x4c, 0x8d, 0x44, 0x24, 0x28});            // lea r8, [rsp+0x28]
    Emit(v, {0x48, 0x8d, 0x8c, 0x24, 0x80, 0, 0, 0});   // lea rcx, [rsp+0x80]
    Emit(v, {0xff, 0x15});                              // call [rip+slot]
    Rel32(v, at, sm::kAccessorSlotRva);
    Emit(v, {0x48, 0xb8});                              // mov rax, arc
    Imm64(v, sm::kArc);
    Emit(v, {0x48, 0x39, 0x44, 0x24, 0x28,              // cmp [rsp+0x28], rax
             0x0f, 0x94, 0xc1, 0x84, 0xc9, 0x0f, 0x94, 0xc0,  // sete cl; test cl, cl; sete al
             0x48, 0x81, 0xc4, 0x88, 0x03, 0x00, 0x00, 0xc3});  // add rsp, 0x388; ret
    std::memcpy(base_ + at, v.data(), v.size());
    // Another caller of the accessor, as the script's 20 others: it calls through the slot for the type field.
    std::vector<std::uint8_t> o;
    Emit(o, {0x48, 0x83, 0xec, 0x38});                  // sub rsp, 0x38
    Emit(o, {0x8b, 0x15});                              // mov edx, dword [rip+type field id]
    Rel32(o, kOtherCallerRva, sm::kTypeFieldIdRva);
    Emit(o, {0x4c, 0x8d, 0x44, 0x24, 0x28, 0x31, 0xc9, 0xff, 0x15});  // lea r8, [rsp+0x28]; xor ecx, ecx; call [rip+slot]
    Rel32(o, kOtherCallerRva, sm::kAccessorSlotRva);
    Emit(o, {0x48, 0x83, 0xc4, 0x38, 0xc3});            // add rsp, 0x38; ret
    std::memcpy(base_ + kOtherCallerRva, o.data(), o.size());
    void* engine = reinterpret_cast<void*>(&EngineAccessor);
    std::memcpy(base_ + sm::kAccessorSlotRva, &engine, sizeof engine);
    std::memcpy(base_ + sm::kTypeFieldIdRva, &kTypeField, sizeof kTypeField);
    std::memcpy(base_ + sm::kTypeFieldIdRva + 4, &kNextField, sizeof kNextField);
  }
  void TearDown() override {
    MH_RemoveHook(base_ + sm::kConditionRva);
    VirtualFree(base_, 0, MEM_RELEASE);
    sh::ResetForTest();
    sm::ResetForTest();
    SetBootLogSink(nullptr);
  }
  bool Condition(std::uint64_t type) {
    g_type = type;
    void* data[4] = {};  // the condition's `mov rax, [rcx]`
    void* thread[4] = {data, nullptr, nullptr, nullptr};
    return reinterpret_cast<bool (*)(void*)>(base_ + sm::kConditionRva)(thread);
  }
  void OtherCaller(std::uint64_t type) {
    g_type = type;
    reinterpret_cast<void (*)()>(base_ + kOtherCallerRva)();
  }
  void* Slot() {
    void* p = nullptr;
    std::memcpy(&p, base_ + sm::kAccessorSlotRva, sizeof p);
    return p;
  }
  std::uint8_t* base_ = nullptr;
};

TEST_F(SharedMagazineInstalled, TheDotAndTheDroneGetNoFrisbee) {
  EXPECT_FALSE(Condition(sm::kArc)) << "stock: not the arc";
  EXPECT_TRUE(Condition(sm::kDot)) << "stock: the DOT asks for a frisbee";
  sm::Register();
  sh::OnLoaded(sm::kScript, base_);
  EXPECT_EQ(Count("script hooks installed script=190ed14fe6c74dd3"), 1);
  EXPECT_EQ(Count("hooks=1 slots=1"), 2) << "the registration line and the install line";
  EXPECT_FALSE(Condition(sm::kArc));
  EXPECT_FALSE(Condition(sm::kDot));
  EXPECT_FALSE(Condition(sm::kDrone));
  EXPECT_TRUE(Condition(kDet)) << "another ordnance keeps its frisbee";
  EXPECT_TRUE(Condition(kNotDot)) << "the type is read and compared as the qword it is";
  // The accessor's other callers record nothing, and a condition after one starts afresh.
  OtherCaller(sm::kDot);
  std::uint64_t type = 0;
  EXPECT_TRUE(sm::RecordedType(&type)) << "the last condition's record stands";
  EXPECT_EQ(type, kNotDot) << "the other caller's read was not recorded";
  EXPECT_TRUE(Condition(kDet));
  sh::OnUnloading(sm::kScript, base_);
  EXPECT_EQ(Slot(), reinterpret_cast<void*>(&EngineAccessor));
  EXPECT_TRUE(Condition(sm::kDot)) << "stock again after the free";
}

TEST_F(SharedMagazineInstalled, AConditionThatReadsNoTypeKeepsTheGamesAnswer) {
  sm::Register();
  sh::OnLoaded(sm::kScript, base_);
  EXPECT_FALSE(Condition(sm::kDot)) << "a DOT, recorded";
  // The condition now loads the next dword as its field id, so the accessor's read is not the type field: nothing is
  // recorded, and the DOT recorded by the condition before must not decide this one.
  std::uint8_t* load = base_ + sm::kConditionRva + sizeof sm::kConditionPrologue + 4 + 2;
  std::int32_t disp = 0;
  std::memcpy(&disp, load, sizeof disp);
  disp += 4;
  std::memcpy(load, &disp, sizeof disp);
  EXPECT_TRUE(Condition(sm::kDot)) << "no type read by this condition: the game's answer";
}

TEST_F(SharedMagazineInstalled, RegisteringTwiceAddsTheRowsOnce) {
  sm::Register();
  sm::Register();
  sh::OnLoaded(sm::kScript, base_);
  EXPECT_EQ(Count("shared magazine fix registered"), 1);
  EXPECT_EQ(Count("script hooks installed script=190ed14fe6c74dd3 base="), 1);
}

}  // namespace
