// #600: the RULES page's starting scores become the capture points' points (runtime/combat/capture_scores.h): the
// decisions, and the rows installed through the script hook table under MinHook on two fake scripts.
//
// Each fake function is its row's pinned bytes, the stock code, and where a pin stops before the function's end, a
// tail this test writes in the stock function's shape:
//  - a publish node (6fa8): the pin (its `sub rsp, 0xA8` through `mov r9, rcx`), then `add rsp, 0xA8; mov eax, 0x77;
//    ret`;
//  - a reset node (b7d10): the whole stock node, pinned;
//  - the dispatcher: its pinned head, then `cmp rax, blue id; je 0xB9C1; cmp rax, orange id; je 0xB8DE; mov eax, 1;
//    add rsp, 0x28; ret`, jumping into the two pinned inline resets, which run as shipped;
//  - the initializer: its pinned head, a jump to its pinned orange write at 0x1CE53, the stock +0xF4 write between,
//    its pinned blue write at 0x1CE69, then `add rsp, 0x28; ret`;
//  - a HUD node: the stock node through its `call 0x2640` (pinned), then `add rsp, 0x58; ret`; at 0x2640 a stub jumps
//    to a setter of this test that records the record and the node it is given.
// The node, its context and the globals are test buffers with nonzero bytes around every field the hooks read or write.
//
// NOT shown by these tests: the shipped scripts' bytes in a run, and a RULES starting score in a Combat match.

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
#include "runtime/combat/capture_scores.h"
#include "runtime/combat/rules_state.h"
#include "runtime/hook/hook_guard.h"
#include "runtime/hook/script_hooks.h"

namespace {

namespace cs = nevr_combat::capture_scores;
namespace rules = nevr_combat::rules;
namespace sh = nevr_script_hooks;

constexpr std::uint32_t kPublishRvas[] = {0xCB60, 0xCCA0, 0xD480, 0x16380, 0x16A60, 0x184A0};
const std::uint8_t* const kPublishPins[] = {cs::kPublishCB60, cs::kPublishCCA0, cs::kPublishD480,
                                            cs::kPublish16380, cs::kPublish16A60, cs::kPublish184A0};
constexpr std::size_t kModeImage = 0x19000;
constexpr std::size_t kPointImage = 0x1D000;

std::vector<std::string> g_logged;
void CaptureLog(EchoVR::LogLevel, const CHAR* message) { g_logged.push_back(message); }
int Count(const char* needle) {
  int n = 0;
  for (const std::string& line : g_logged) n += line.find(needle) != std::string::npos ? 1 : 0;
  return n;
}

cs::ProgressRecord g_record{};
void* g_record_node = nullptr;
int g_setter_calls = 0;
std::uint64_t Setter(void* record, void* node) {
  ++g_setter_calls;
  std::memcpy(g_record.bytes, record, sizeof g_record.bytes);
  g_record_node = node;
  return 0x99;
}

void Emit(std::vector<std::uint8_t>& v, std::initializer_list<std::uint8_t> b) { v.insert(v.end(), b); }
void Rel32(std::vector<std::uint8_t>& v, std::uint32_t at_rva, std::uint32_t target_rva) {
  const std::int32_t d = static_cast<std::int32_t>(target_rva) - static_cast<std::int32_t>(at_rva + v.size() + 4);
  for (int i = 0; i < 4; ++i) v.push_back(static_cast<std::uint8_t>(static_cast<std::uint32_t>(d) >> (8 * i)));
}
void Imm32(std::vector<std::uint8_t>& v, std::uint32_t x) {
  for (int i = 0; i < 4; ++i) v.push_back(static_cast<std::uint8_t>(x >> (8 * i)));
}
void Put(std::uint8_t* base, std::uint32_t rva, const std::vector<std::uint8_t>& v) {
  std::memcpy(base + rva, v.data(), v.size());
}
std::vector<std::uint8_t> Pin(const std::uint8_t* p, std::size_t n) {
  std::vector<std::uint8_t> v(p, p + n);
  v.reserve(n + 64);
  return v;
}
float Float(const std::uint8_t* at) {
  float f = 0;
  std::memcpy(&f, at, sizeof f);
  return f;
}
void SetFloat(std::uint8_t* at, float f) { std::memcpy(at, &f, sizeof f); }

TEST(CaptureScoresDecision, TheScoresAreCopiedAsFloatsAndTheHudShowsAHundredth) {
  std::uint8_t globals[0x100];
  std::memset(globals, 0xa5, sizeof globals);
  const std::int32_t blue = 3, red = -2;
  std::memcpy(globals + cs::kBlueScore, &blue, sizeof blue);
  std::memcpy(globals + cs::kRedScore, &red, sizeof red);
  std::uint8_t box[0x40];
  std::memset(box, 0xa5, sizeof box);
  cs::CopyScores(globals, rules::RulesState(box));
  EXPECT_EQ(Float(box + rules::kBlueStartScore), 3.0f);
  EXPECT_EQ(Float(box + rules::kOrangeStartScore), -2.0f) << "6fa8's red is the capture point's orange";
  EXPECT_EQ(box[0x08], 0xa5) << "two floats written, nothing after";
  EXPECT_EQ(cs::Progress(75.0f), 0.75f);
  const cs::ProgressRecord r = cs::MakeProgressRecord(0x1122334455667788ULL, 0.5f);
  std::uint64_t q = 1;
  std::memcpy(&q, r.bytes + 0x00, 8);
  EXPECT_EQ(q, 0u);
  EXPECT_EQ(r.bytes[0x08], 0);
  std::memcpy(&q, r.bytes + 0x10, 8);
  EXPECT_EQ(q, ~std::uint64_t{0});
  std::memcpy(&q, r.bytes + 0x18, 8);
  EXPECT_EQ(q, ~std::uint64_t{0});
  std::memcpy(&q, r.bytes + 0x20, 8);
  EXPECT_EQ(q, 0x1122334455667788ULL);
  EXPECT_EQ(Float(r.bytes + 0x28), 0.5f);
  EXPECT_EQ(r.bytes[0x2C], 0);
}

TEST(RulesStateView, StartScoresAreTwoFloats) {
  std::uint8_t box[0x40];
  std::memset(box, 0xa5, sizeof box);
  const rules::RulesState r(box);
  r.SetStartScores(10.0f, 20.0f);
  EXPECT_EQ(r.StartScore(rules::Team::kBlue), 10.0f);
  EXPECT_EQ(r.StartScore(rules::Team::kOrange), 20.0f);
  EXPECT_EQ(box[0x08], 0xa5);
  EXPECT_EQ(rules::RulesState(nullptr).StartScore(rules::Team::kBlue), 0.0f);
  rules::RulesState(nullptr).SetStartScores(1.0f, 2.0f);  // dropped, no fault
}

class CaptureScoresInstalled : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(Hooking::Initialize());
    nevr_hook_guard::ResetForTest();
    sh::ResetForTest();
    cs::ResetForTest();
    g_logged.clear();
    SetBootLogSink(&CaptureLog);
    g_setter_calls = 0;
    std::memset(box_, 0xa5, sizeof box_);
    SetStarts(0.0f, 0.0f);
    mode_ = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, kModeImage, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
    point_ = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, kPointImage, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
    ASSERT_NE(mode_, nullptr);
    ASSERT_NE(point_, nullptr);
    for (std::size_t i = 0; i < 6; ++i) {
      std::vector<std::uint8_t> v = Pin(kPublishPins[i], sizeof cs::kPublishCB60);
      Emit(v, {0x48, 0x81, 0xc4, 0xa8, 0x00, 0x00, 0x00, 0xb8, 0x77, 0x00, 0x00, 0x00, 0xc3});
      Put(mode_, kPublishRvas[i], v);
    }
    Put(point_, 0x14780, Pin(cs::kBlueResetNode, sizeof cs::kBlueResetNode));
    Put(point_, 0x14E90, Pin(cs::kOrangeResetNode, sizeof cs::kOrangeResetNode));
    std::vector<std::uint8_t> d = Pin(cs::kDispatcherHead, sizeof cs::kDispatcherHead);
    Emit(d, {0x48, 0x3d});
    Imm32(d, static_cast<std::uint32_t>(cs::kBlueResetId));
    Emit(d, {0x0f, 0x84});
    Rel32(d, 0xB2D0, 0xB9C1);
    Emit(d, {0x48, 0x3d});
    Imm32(d, static_cast<std::uint32_t>(cs::kOrangeResetId));
    Emit(d, {0x0f, 0x84});
    Rel32(d, 0xB2D0, 0xB8DE);
    Emit(d, {0xb8, 0x01, 0x00, 0x00, 0x00, 0x48, 0x83, 0xc4, 0x28, 0xc3});
    Put(point_, 0xB2D0, d);
    Put(point_, 0xB9A4, Pin(cs::kDispatcherBlueReset, sizeof cs::kDispatcherBlueReset));
    Put(point_, 0xB8A4, Pin(cs::kDispatcherOrangeReset, sizeof cs::kDispatcherOrangeReset));
    std::vector<std::uint8_t> init = Pin(cs::kInitializerHead, sizeof cs::kInitializerHead);
    Emit(init, {0xe9});
    Rel32(init, 0x1CDE0, 0x1CE53);
    Put(point_, 0x1CDE0, init);
    Put(point_, 0x1CE53, Pin(cs::kInitializerOrangeWrite, sizeof cs::kInitializerOrangeWrite));
    Put(point_, 0x1CE5E, {0x48, 0x8b, 0x41, 0x68, 0x44, 0x89, 0x80, 0xf4, 0x00, 0x00, 0x00});  // the stock +0xF4 write
    std::vector<std::uint8_t> blue_write = Pin(cs::kInitializerBlueWrite, sizeof cs::kInitializerBlueWrite);
    Emit(blue_write, {0x48, 0x83, 0xc4, 0x28, 0xc3});
    Put(point_, 0x1CE69, blue_write);
    for (const auto& [rva, pin] : {std::pair{0xFA40u, cs::kBlueProgressNode}, std::pair{0x11180u, cs::kOrangeProgressNode}}) {
      std::vector<std::uint8_t> v = Pin(pin, sizeof cs::kBlueProgressNode);
      Emit(v, {0x48, 0x83, 0xc4, 0x58, 0xc3});
      Put(point_, rva, v);
    }
    std::vector<std::uint8_t> stub{0x48, 0xb8};  // movabs rax, Setter; jmp rax
    const std::uint64_t setter = reinterpret_cast<std::uint64_t>(&Setter);
    for (int i = 0; i < 8; ++i) stub.push_back(static_cast<std::uint8_t>(setter >> (8 * i)));
    Emit(stub, {0xff, 0xe0});
    Put(point_, cs::kProgressSetterRva, stub);

    std::memset(node_, 0xa5, sizeof node_);
    std::memset(context_, 0xa5, sizeof context_);
    std::memset(globals_, 0xa5, sizeof globals_);
    std::uint8_t* context = context_;
    std::memcpy(node_, &context, sizeof context);
    std::uint8_t* globals = globals_;
    std::memcpy(context_ + cs::kContextGlobals, &globals, sizeof globals);
  }
  void TearDown() override {
    for (std::uint32_t rva : kPublishRvas) MH_RemoveHook(mode_ + rva);
    for (std::uint32_t rva : {0x14780u, 0x14E90u, 0xB2D0u, 0x1CDE0u, 0xFA40u, 0x11180u}) MH_RemoveHook(point_ + rva);
    VirtualFree(mode_, 0, MEM_RELEASE);
    VirtualFree(point_, 0, MEM_RELEASE);
    sh::ResetForTest();
    cs::ResetForTest();
    SetBootLogSink(nullptr);
  }
  void SetStarts(float blue, float orange) {
    SetFloat(box_ + rules::kBlueStartScore, blue);
    SetFloat(box_ + rules::kOrangeStartScore, orange);
  }
  void SetId(std::uint64_t id) { std::memcpy(node_ + cs::kNodeId, &id, sizeof id); }
  void SetCharges(float v) {
    SetFloat(globals_ + cs::kBlueCharge, v);
    SetFloat(globals_ + cs::kOrangeCharge, v);
  }
  float Blue() const { return Float(globals_ + cs::kBlueCharge); }
  float Orange() const { return Float(globals_ + cs::kOrangeCharge); }
  std::uint64_t Call(std::uint8_t* base, std::uint32_t rva, void* arg) {
    return reinterpret_cast<std::uint64_t (*)(void*)>(base + rva)(arg);
  }
  void Load() {
    cs::Register(box_);
    sh::OnLoaded(cs::kModeScript, mode_);
    sh::OnLoaded(cs::kPointScript, point_);
  }
  std::uint8_t* mode_ = nullptr;
  std::uint8_t* point_ = nullptr;
  std::uint8_t box_[0x40];
  std::uint8_t node_[0x40];
  std::uint8_t context_[0x80];
  std::uint8_t globals_[0x120];
};

TEST_F(CaptureScoresInstalled, EachPublishNodeCopiesTheStartingScores) {
  Load();
  EXPECT_EQ(Count("script hooks installed script=6fa8cc9cd721a563"), 1);
  EXPECT_EQ(Count("hooks=6 slots=0 pins=0"), 1);
  for (std::size_t i = 0; i < 6; ++i) {
    SCOPED_TRACE(i);
    const std::int32_t blue = 10 + static_cast<std::int32_t>(i), red = 20 + static_cast<std::int32_t>(i);
    std::memcpy(globals_ + cs::kBlueScore, &blue, sizeof blue);
    std::memcpy(globals_ + cs::kRedScore, &red, sizeof red);
    EXPECT_EQ(Call(mode_, kPublishRvas[i], node_), 0x77u) << "the node runs as shipped after the copy";
    EXPECT_EQ(Float(box_ + rules::kBlueStartScore), static_cast<float>(blue));
    EXPECT_EQ(Float(box_ + rules::kOrangeStartScore), static_cast<float>(red));
  }
}

TEST_F(CaptureScoresInstalled, EachChangeOfTheScoresAndEachRoundStartIsLogged) {
  Load();
  const auto publish = [&](std::int32_t blue, std::int32_t red) {
    std::memcpy(globals_ + cs::kBlueScore, &blue, sizeof blue);
    std::memcpy(globals_ + cs::kRedScore, &red, sizeof red);
    Call(mode_, kPublishRvas[0], node_);
  };
  publish(0, 0);
  EXPECT_EQ(Count("capture scores: starting scores changed"), 0) << "no starting score: nothing logged";
  publish(10, 20);
  publish(10, 20);
  EXPECT_EQ(Count("capture scores: starting scores changed from blue=0 orange=0 to blue=10 orange=20 "
                  "script=6fa8cc9cd721a563"),
            1);
  publish(12, 20);
  EXPECT_EQ(Count("capture scores: starting scores changed from blue=10 orange=20 to blue=12 orange=20"), 1);
  publish(12, 3);
  EXPECT_EQ(Count("capture scores: starting scores changed from blue=12 orange=20 to blue=12 orange=3"), 1)
      << "orange alone changed";
  EXPECT_EQ(Count("capture scores: starting scores changed"), 3) << "one line a change, none a press that changes nothing";
  Call(point_, 0x1CDE0, context_);
  EXPECT_EQ(Count("capture scores: round starts with blue=12 orange=3 script=b7d10bcffcb2a780"), 1);
  SetStarts(0.0f, 5.0f);
  Call(point_, 0x1CDE0, context_);
  EXPECT_EQ(Count("capture scores: round starts with blue=0 orange=5"), 1) << "orange alone";
  SetStarts(0.0f, 0.0f);
  Call(point_, 0x1CDE0, context_);
  EXPECT_EQ(Count("capture scores: round starts"), 2) << "a round from the stock 0 logs nothing";
}

TEST_F(CaptureScoresInstalled, EverythingThatZeroesATeamsPointsLoadsItsStartingScore) {
  Load();
  EXPECT_EQ(Count("script hooks installed script=b7d10bcffcb2a780"), 1);
  EXPECT_EQ(Count("hooks=6 slots=0 pins=4"), 1);
  SetStarts(10.0f, 20.0f);
  SetCharges(5.0f);
  EXPECT_EQ(Call(point_, 0x14780, node_), 0u);
  EXPECT_EQ(Blue(), 10.0f) << "blue's reset node";
  EXPECT_EQ(Orange(), 5.0f);
  EXPECT_EQ(Call(point_, 0x14E90, node_), 0u);
  EXPECT_EQ(Orange(), 20.0f) << "orange's reset node";

  SetCharges(5.0f);
  SetId(cs::kBlueResetId);
  EXPECT_EQ(Call(point_, 0xB2D0, node_), 0u);
  EXPECT_EQ(Blue(), 10.0f) << "the dispatcher's inline blue reset";
  EXPECT_EQ(Orange(), 5.0f);
  SetId(cs::kOrangeResetId);
  EXPECT_EQ(Call(point_, 0xB2D0, node_), 0u);
  EXPECT_EQ(Orange(), 20.0f) << "the dispatcher's inline orange reset";
  SetCharges(5.0f);
  SetId(0x1234);
  EXPECT_EQ(Call(point_, 0xB2D0, node_), 1u) << "another node: the dispatcher's own answer";
  EXPECT_EQ(Blue(), 5.0f);
  EXPECT_EQ(Orange(), 5.0f);
  SetId((std::uint64_t{0xdead} << 32) | cs::kBlueResetId);  // blue's id in the low dword only
  EXPECT_EQ(Call(point_, 0xB2D0, node_), 1u);
  EXPECT_EQ(Blue(), 5.0f) << "the id is compared in full";

  SetCharges(5.0f);
  SetFloat(globals_ + 0xF4, 7.0f);
  Call(point_, 0x1CDE0, context_);
  EXPECT_EQ(Blue(), 10.0f) << "the initializer";
  EXPECT_EQ(Orange(), 20.0f);
  EXPECT_EQ(Float(globals_ + 0xF4), 0.0f) << "the initializer ran as shipped";
  EXPECT_EQ(globals_[cs::kBlueCharge + 4], 0xa5) << "one float written per charge";

  SetStarts(0.0f, 0.0f);
  SetCharges(5.0f);
  Call(point_, 0x14780, node_);
  Call(point_, 0x1CDE0, context_);
  EXPECT_EQ(Blue(), 0.0f) << "a mailbox of zeros is the stock 0";
  EXPECT_EQ(Orange(), 0.0f);
}

TEST_F(CaptureScoresInstalled, TheHudShowsTheStartingScoreBeforeATeamCharges) {
  Load();
  SetStarts(10.0f, 0.0f);
  EXPECT_EQ(Call(point_, 0xFA40, node_), 0x99u) << "the setter's answer, as the stock node returns it";
  EXPECT_EQ(g_setter_calls, 1);
  EXPECT_EQ(g_record_node, node_);
  EXPECT_EQ(Float(g_record.bytes + 0x28), 0.1f);
  std::uint64_t symbol = 0;
  std::memcpy(&symbol, g_record.bytes + 0x20, sizeof symbol);
  EXPECT_EQ(symbol, cs::kBlueProgressSymbol);
  // Orange's starting score is 0: the stock node runs, and sets 0 itself.
  EXPECT_EQ(Call(point_, 0x11180, node_), 0x99u);
  EXPECT_EQ(g_setter_calls, 2);
  EXPECT_EQ(Float(g_record.bytes + 0x28), 0.0f);
  std::memcpy(&symbol, g_record.bytes + 0x20, sizeof symbol);
  EXPECT_EQ(symbol, cs::kOrangeProgressSymbol) << "the stock node's own record";
  // The record the stock node fills and the one rebuilt here agree in every byte the stock node writes.
  const cs::ProgressRecord stock = g_record;
  SetStarts(0.0f, 1e-30f);
  Call(point_, 0x11180, node_);
  for (std::size_t off : {0x00u, 0x10u, 0x18u, 0x20u}) {
    EXPECT_EQ(std::memcmp(stock.bytes + off, g_record.bytes + off, 8), 0) << "offset " << off;
  }
  EXPECT_EQ(stock.bytes[0x08], g_record.bytes[0x08]);
  EXPECT_EQ(stock.bytes[0x2C], g_record.bytes[0x2C]);
}

TEST_F(CaptureScoresInstalled, APinWhoseBytesDifferRefusesTheCapturePointScript) {
  point_[0xB9CA + 2] = 0xF0;  // the inline blue reset writes orange's charge
  Load();
  EXPECT_EQ(Count("script hooks refused script=b7d10bcffcb2a780"), 1);
  EXPECT_EQ(Count("script hooks installed script=6fa8cc9cd721a563"), 1) << "the other script is its own";
  SetStarts(10.0f, 20.0f);
  SetCharges(5.0f);
  Call(point_, 0x14780, node_);
  EXPECT_EQ(Blue(), 0.0f) << "the capture point script runs stock";
}

TEST_F(CaptureScoresInstalled, StockAfterTheFree) {
  Load();
  sh::OnUnloading(cs::kPointScript, point_);
  sh::OnUnloading(cs::kModeScript, mode_);
  SetStarts(10.0f, 20.0f);
  SetCharges(5.0f);
  Call(point_, 0x14780, node_);
  EXPECT_EQ(Blue(), 0.0f);
  EXPECT_EQ(Call(point_, 0xFA40, node_), 0x99u);
  EXPECT_EQ(Float(g_record.bytes + 0x28), 0.0f) << "the stock HUD node";
}

TEST_F(CaptureScoresInstalled, RegisteringTwiceAddsTheRowsOnce) {
  cs::Register(box_);
  cs::Register(box_);
  sh::OnLoaded(cs::kPointScript, point_);
  EXPECT_EQ(Count("capture scores fix registered"), 1);
  EXPECT_EQ(Count("hooks=6 slots=0 pins=4"), 1);
}

}  // namespace
