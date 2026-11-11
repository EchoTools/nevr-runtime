// Gear carry-over measurement: the [NEVR.COMBAT] gear records.
//
// Two layers. The rules (combat/gear_trace_rules.h) are tested as functions. The hooks (combat/gear_trace.cpp) are the
// production code, installed under MinHook into a fake game image: the four hooked functions sit at their real VAs with
// their real prologue bytes, then code that undoes the prologue's stack effect and jumps to a C++ stand-in; the two
// readers the trace calls (ResolveConfigHash, the CJson float getter) are stand-ins at their real VAs; the startup call
// site (0x140c4e38c, `call 0x140c4eba0`) is a real call instruction, so the return-address classification is driven.
//
// NOT shown: that echovr.exe behaves like the stand-ins on a live process. That is the run on a game copy.

#include <gtest/gtest.h>

#include <winsock2.h>
#include <windows.h>

#include <MinHook.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "core/hooking.h"
#include "core/logging.h"
#include "runtime/combat/gear_trace.h"
#include "runtime/combat/gear_trace_rules.h"
#include "runtime/hook/hook_guard.h"

namespace {

using namespace nevr_gear_trace;

constexpr std::size_t kImageBytes = 0x2200000;  // covers 0x1420d3a70 (RVA 0x20d3a70)
constexpr std::uint64_t kGearReloadVa = 0x140C4EBA0, kInitVa = 0x140CFE650, kTeardownVa = 0x140D4CD70,
                        kUpdateVa = 0x140D49950, kResolveVa = 0x140D0B170, kGetFloatVa = 0x1405FCA80,
                        kStartupSiteVa = 0x140C4E38C, kTableVa = 0x1420D3A68, kTacmodListVa = 0x1420D3430;

const std::vector<std::uint8_t> kGearReloadPro = {0x48, 0x8B, 0xC4, 0x55, 0x48, 0x8D, 0xA8, 0xF8, 0xFE,
                                                  0xFF, 0xFF, 0x48, 0x81, 0xEC, 0x00, 0x02, 0x00, 0x00};
const std::vector<std::uint8_t> kGearReloadUndo = {0x48, 0x81, 0xC4, 0x00, 0x02, 0x00, 0x00, 0x5D};
const std::vector<std::uint8_t> kInitPro = {0x48, 0x89, 0x4C, 0x24, 0x08, 0x55, 0x53, 0x56, 0x57,
                                            0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57};
const std::vector<std::uint8_t> kInitUndo = {0x41, 0x5F, 0x41, 0x5E, 0x41, 0x5D, 0x41, 0x5C, 0x5F, 0x5E, 0x5B, 0x5D};
const std::vector<std::uint8_t> kTeardownPro = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C, 0x24, 0x18, 0x48, 0x89,
                                                0x74, 0x24, 0x20, 0x57, 0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x20};
const std::vector<std::uint8_t> kTeardownUndo = {0x48, 0x83, 0xC4, 0x20, 0x41, 0x5F, 0x41, 0x5E, 0x5F};
const std::vector<std::uint8_t> kUpdatePro = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x18,
                                              0x48, 0x89, 0x74, 0x24, 0x20, 0x57, 0x48, 0x83, 0xEC, 0x20};
const std::vector<std::uint8_t> kUpdateUndo = {0x48, 0x83, 0xC4, 0x20, 0x5F};

std::vector<std::uint8_t> Stub(const std::vector<std::uint8_t>& pro, const std::vector<std::uint8_t>& undo,
                               std::uintptr_t target) {
  std::vector<std::uint8_t> s = pro;
  s.insert(s.end(), undo.begin(), undo.end());
  s.push_back(0x48);
  s.push_back(0xB8);
  for (int i = 0; i < 8; ++i) s.push_back(static_cast<std::uint8_t>((target >> (8 * i)) & 0xFF));
  s.push_back(0xFF);
  s.push_back(0xE0);
  return s;
}

std::uint64_t Hash(const char* path) { return EchoVR::CSymbol64Hash(path); }

// The game, stood in for: the stock values, the values in force, the table pointer.
std::map<std::uint64_t, float> g_stockValues;
std::map<std::uint64_t, float> g_values;
std::uint8_t* g_image = nullptr;
std::array<std::uint8_t, 0x40> g_table{};           // the CS's table header: entries at +0, count at +0x30
std::array<std::uint64_t, 3> g_tableEntry{};         // one entry: id, then the CJson (unused by the stand-in)
bool g_teardownRestoresStock = true;
std::array<std::uint8_t, 16> g_json{};

void* FakeResolve(std::int64_t id) {
  return static_cast<std::uint64_t>(id) == kWeaponSettingsId ? g_json.data() : nullptr;
}

float FakeGetFloat(void*, std::int64_t path, float fallback) {
  const auto it = g_values.find(static_cast<std::uint64_t>(path));
  return it == g_values.end() ? fallback : it->second;
}

void SetTable(bool present) {
  void* p = present ? g_table.data() : nullptr;
  std::memcpy(g_image + (kTableVa - 0x140000000ULL), &p, sizeof p);
}

void FakeGearReload() {}
std::uint64_t FakeInit(void*) {
  SetTable(true);
  return 0;
}
void FakeTeardown(void*) {
  SetTable(false);
  if (g_teardownRestoresStock) g_values = g_stockValues;
}
// The listener: the pushed values replace the entry's JSON (the stand-in reads a "heal required_charge" byte).
void FakeUpdate(void*, void* msg) {
  const char* text = nullptr;
  std::memcpy(&text, static_cast<std::uint8_t*>(msg) + 0x18, sizeof text);
  if (std::strstr(text, "\"required_charge\": 5") != nullptr) g_values[Hash("ability_properties|heal|required_charge")] = 5;
}

struct Line {
  EchoVR::LogLevel level;
  std::string text;
};
std::vector<Line> g_logged;
void CaptureLog(EchoVR::LogLevel level, const CHAR* message) { g_logged.push_back({level, message}); }

const Line* Last(const std::string& needle) {
  for (auto it = g_logged.rbegin(); it != g_logged.rend(); ++it)
    if (it->text.find(needle) != std::string::npos) return &*it;
  return nullptr;
}

using VoidFn = void (*)();
using CsFn = void (*)(void*);
using InitFn = std::uint64_t (*)(void*);
using UpdateFn = void (*)(void*, void*);
__attribute__((noinline)) void CallVoid(VoidFn f) { f(); }

class GearTraceHooks : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(Hooking::Initialize());
    nevr_hook_guard::ResetForTest();
    ResetForTest();
    g_logged.clear();
    SetBootLogSink(&CaptureLog);
    image_ = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, kImageBytes, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
    ASSERT_NE(image_, nullptr);
    g_image = image_;
    Put(kGearReloadVa, Stub(kGearReloadPro, kGearReloadUndo, reinterpret_cast<std::uintptr_t>(&FakeGearReload)));
    Put(kInitVa, Stub(kInitPro, kInitUndo, reinterpret_cast<std::uintptr_t>(&FakeInit)));
    Put(kTeardownVa, Stub(kTeardownPro, kTeardownUndo, reinterpret_cast<std::uintptr_t>(&FakeTeardown)));
    Put(kUpdateVa, Stub(kUpdatePro, kUpdateUndo, reinterpret_cast<std::uintptr_t>(&FakeUpdate)));
    Put(kResolveVa, Stub({}, {}, reinterpret_cast<std::uintptr_t>(&FakeResolve)));
    Put(kGetFloatVa, Stub({}, {}, reinterpret_cast<std::uintptr_t>(&FakeGetFloat)));
    // The startup site: sub rsp,8 (so the call keeps the stack aligned, as the game's caller does), call rel32 to
    // 0x140c4eba0 at 0x140c4e38c (the return address is 0x140c4e391), add rsp,8, ret.
    const std::int32_t rel = static_cast<std::int32_t>(kGearReloadVa - (kStartupSiteVa + 5));
    std::vector<std::uint8_t> site = {0x48, 0x83, 0xEC, 0x08, 0xE8, 0, 0, 0, 0, 0x48, 0x83, 0xC4, 0x08, 0xC3};
    std::memcpy(site.data() + 5, &rel, sizeof rel);
    Put(kStartupSiteVa - 4, site);
    // The tac mod list: one entry named "heal".
    tacEntries_.assign(0x88, 0);
    std::memcpy(tacEntries_.data(), "heal", 4);
    tacList_.assign(0x20, 0);
    const std::uint8_t* data = tacEntries_.data();
    const std::uint64_t one = 1;
    std::memcpy(tacList_.data() + 8, &data, sizeof data);
    std::memcpy(tacList_.data() + 0x10, &one, sizeof one);
    const std::uint8_t* list = tacList_.data();
    std::memcpy(At(kTacmodListVa), &list, sizeof list);
    // The CS's table holds one entry, mp_weapon_settings.
    g_tableEntry = {kWeaponSettingsId, 0, 0};
    const void* entries = g_tableEntry.data();
    const std::uint64_t count = 1;
    std::memcpy(g_table.data(), &entries, sizeof entries);
    std::memcpy(g_table.data() + 0x30, &count, sizeof count);
    SetTable(false);
    g_stockValues = {{Hash("ability_properties|heal|required_charge"), 35.0F},
                     {Hash("grenade_properties|det|cooldown"), 35.0F}};
    g_values = g_stockValues;
    g_teardownRestoresStock = true;
    msg_.assign(0x28, 0);
  }

  void TearDown() override {
    for (std::uint64_t va : {kGearReloadVa, kInitVa, kTeardownVa, kUpdateVa}) MH_RemoveHook(At(va));
    SetBootLogSink(nullptr);
    VirtualFree(image_, 0, MEM_RELEASE);
    image_ = nullptr;
  }

  std::uint8_t* At(std::uint64_t va) { return image_ + (va - 0x140000000ULL); }
  void Put(std::uint64_t va, const std::vector<std::uint8_t>& bytes) { std::memcpy(At(va), bytes.data(), bytes.size()); }
  std::uintptr_t Base() const { return reinterpret_cast<std::uintptr_t>(image_); }

  void Startup() { CallVoid(reinterpret_cast<VoidFn>(At(kStartupSiteVa - 4))); }
  void Init() { reinterpret_cast<InitFn>(At(kInitVa))(nullptr); }
  void Teardown() { reinterpret_cast<CsFn>(At(kTeardownVa))(nullptr); }
  void Push(const std::string& json) {
    pushText_ = json;
    const char* t = pushText_.c_str();
    const std::uint64_t size = pushText_.size();
    const std::uint64_t* id = &kWeaponSettingsId;
    std::memcpy(msg_.data() + 0x10, &id, sizeof id);
    std::memcpy(msg_.data() + 0x18, &t, sizeof t);
    std::memcpy(msg_.data() + 0x20, &size, sizeof size);
    reinterpret_cast<UpdateFn>(At(kUpdateVa))(nullptr, msg_.data());
  }

  std::uint8_t* image_ = nullptr;
  std::vector<std::uint8_t> tacEntries_, tacList_, msg_;
  std::string pushText_;
};

}  // namespace

// ---- the rules ----

TEST(GearTraceRules, ThePathHashIsTheGamesCSymbol64) {
  // symcache_data.cpp: 0xa5feb0c610bf6133 is "ability_properties|sensor|ping_delay" (a path the game names).
  EXPECT_EQ(Hash("ability_properties|sensor|ping_delay"), 0xa5feb0c610bf6133ULL);
  EXPECT_EQ(Hash("mp_weapon_settings"), kWeaponSettingsId);
  EXPECT_EQ(kKeys[1].hash, Hash("ability_properties|heal|required_charge"));
}

TEST(GearTraceRules, TheCallerIsTheReturnAddress) {
  EXPECT_EQ(ClassifyCaller(0x140C4E391), Caller::kStartup);
  EXPECT_EQ(ClassifyCaller(0x1401B2DF4), Caller::kSettingsUpdate);
  EXPECT_EQ(ClassifyCaller(0x140000000), Caller::kOther);
}

TEST(GearTraceRules, JoinAndLeaveAreNamedFromTheStates) {
  EXPECT_STREQ(SessionEvent(8, 9), "match_join");
  EXPECT_STREQ(SessionEvent(3, 5), "lobby_join");
  EXPECT_STREQ(SessionEvent(9, 5), "lobby_join");
  EXPECT_STREQ(SessionEvent(9, 7), "match_leave");
  EXPECT_STREQ(SessionEvent(5, 7), "state");
}

// ---- the hooks ----

TEST_F(GearTraceHooks, AllFourHooksInstall) {
  Install(Base());
  const Line* l = Last("gear trace hooks:");
  ASSERT_NE(l, nullptr);
  EXPECT_NE(l->text.find("4 of 4 installed"), std::string::npos) << l->text;
}

TEST_F(GearTraceHooks, StartupIsClassifiedAndItsValuesAreTheStock) {
  Install(Base());
  Startup();
  const Line* load = Last("gear tables loaded");
  ASSERT_NE(load, nullptr);
  EXPECT_NE(load->text.find("caller=startup"), std::string::npos) << load->text;
  EXPECT_NE(load->text.find("tacmods=1 (heal)"), std::string::npos) << load->text;
  const Line* v = Last("gear values at=startup");
  ASSERT_NE(v, nullptr);
  EXPECT_NE(v->text.find("heal.required_charge=35 (stock 35, equal)"), std::string::npos) << v->text;
  EXPECT_NE(v->text.find("heal.heal_duration=absent"), std::string::npos) << v->text;
  EXPECT_NE(v->text.find("source=base_resource"), std::string::npos) << v->text;
}

TEST_F(GearTraceHooks, APushIsLoggedWithWhatItChanged) {
  Install(Base());
  Startup();
  Init();
  ASSERT_NE(Last("gear balance table made"), nullptr);
  Push(R"({"ability_properties": {"heal": {"required_charge": 5}}, "zz": 1})");
  const Line* p = Last("gear balance push received");
  ASSERT_NE(p, nullptr);
  EXPECT_EQ(p->level, EchoVR::LogLevel::Info);
  EXPECT_NE(p->text.find("resource=mp_weapon_settings"), std::string::npos) << p->text;
  EXPECT_NE(p->text.find("sections=ability_properties other_sections=1"), std::string::npos) << p->text;
  EXPECT_NE(p->text.find("changed=1 (heal.required_charge 35->5)"), std::string::npos) << p->text;
  const Line* v = Last("gear values at=balance_push");
  ASSERT_NE(v, nullptr);
  EXPECT_NE(v->text.find("source=balance_table"), std::string::npos) << v->text;
  EXPECT_NE(v->text.find("heal.required_charge=5 (stock 35, differs"), std::string::npos) << v->text;
}

TEST_F(GearTraceHooks, AJoinAfterACleanTeardownReadsStock) {
  Install(Base());
  Startup();
  Init();
  Push(R"({"ability_properties": {"heal": {"required_charge": 5}}})");
  OnNetGameState(9, 7);
  Teardown();
  ASSERT_NE(Last("gear balance table freed"), nullptr);
  OnNetGameState(8, 9);
  const Line* j = Last("gear values at=match_join from=8 to=9");
  ASSERT_NE(j, nullptr);
  EXPECT_NE(j->text.find("heal.required_charge=35 (stock 35, equal)"), std::string::npos) << j->text;
  EXPECT_NE(j->text.find("differ_from_stock=0"), std::string::npos) << j->text;
}

TEST_F(GearTraceHooks, AJoinThatKeepsAPushedValueSaysItDiffersAndWhereItCameFrom) {
  g_teardownRestoresStock = false;
  Install(Base());
  Startup();
  Init();
  Push(R"({"ability_properties": {"heal": {"required_charge": 5}}})");
  Teardown();
  OnNetGameState(8, 9);
  const Line* j = Last("gear values at=match_join");
  ASSERT_NE(j, nullptr);
  EXPECT_NE(j->text.find("heal.required_charge=5 (stock 35, differs, last record 5)"), std::string::npos) << j->text;
  EXPECT_NE(j->text.find("differ_from_stock=1"), std::string::npos) << j->text;
}
