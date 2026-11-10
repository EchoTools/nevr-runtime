// The lobby settings hook (#2333, runtime/patch/lobby_settings_hook.h) under MinHook against a fake game image: the
// callback at 0x14017de60 carries the game's own prologue, the hook reads the message's text and size where the game
// reads them (msg+0x18, msg+0x20), sets the client's gate and forwards the call unchanged; a changed prologue is a
// Warning and no hook.
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
#include "runtime/compat/server_features.h"
#include "runtime/hook/hook_guard.h"
#include "runtime/patch/lobby_settings_hook.h"

namespace {

constexpr std::uint64_t kImageBase = 0x140000000ULL;
constexpr std::uint64_t kCallbackVa = 0x14017DE60ULL;
constexpr std::size_t kImageBytes = 0x200000;  // covers RVA 0x17DE60 and the stub

// MOV [RSP+8],RBX; PUSH RDI; SUB RSP,0x30 (echovr.exe 0x14017de60), then its undo, then a jump to the fake.
constexpr std::array<std::uint8_t, 10> kPrologue = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x30};
constexpr std::array<std::uint8_t, 10> kUndo = {0x48, 0x83, 0xC4, 0x30, 0x5F, 0x48, 0x8B, 0x5C, 0x24, 0x08};

std::vector<std::uint8_t> MakeStub(const std::uint8_t* prologue, std::size_t prologueLen, const std::uint8_t* undo,
                                   std::size_t undoLen, std::uintptr_t target) {
  std::vector<std::uint8_t> stub(prologue, prologue + prologueLen);
  stub.insert(stub.end(), undo, undo + undoLen);
  stub.push_back(0x48);  // movabs rax, imm64
  stub.push_back(0xB8);
  for (int i = 0; i < 8; ++i) stub.push_back(static_cast<std::uint8_t>((target >> (8 * i)) & 0xFF));
  stub.push_back(0xFF);  // jmp rax
  stub.push_back(0xE0);
  return stub;
}

int g_calls = 0;
void* g_netGame = nullptr;
void* g_msg = nullptr;
void FakeCallback(void* netGame, void* msg) {
  ++g_calls;
  g_netGame = netGame;
  g_msg = msg;
}

struct Line {
  EchoVR::LogLevel level;
  std::string text;
};
std::vector<Line> g_logged;
void CaptureLog(EchoVR::LogLevel level, const CHAR* message) { g_logged.push_back({level, message}); }

const Line* FindContaining(const std::string& needle) {
  for (const Line& line : g_logged) {
    if (line.text.find(needle) != std::string::npos) return &line;
  }
  return nullptr;
}

using CallbackFn = void (*)(void*, void*);
__attribute__((noinline)) void CallCallback(CallbackFn fn, void* netGame, void* msg) { fn(netGame, msg); }

class LobbySettingsHook : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(Hooking::Initialize());
    nevr_hook_guard::ResetForTest();
    g_logged.clear();
    g_calls = 0;
    g_netGame = g_msg = nullptr;
    nevr_server_features::Client().Clear();
    SetBootLogSink(&CaptureLog);
    image_ = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, kImageBytes, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
    ASSERT_NE(image_, nullptr) << "VirtualAlloc error=" << GetLastError();
    msg_.assign(0x40, 0);
  }

  void TearDown() override {
    if (image_ != nullptr) {
      MH_RemoveHook(image_ + (kCallbackVa - kImageBase));
      VirtualFree(image_, 0, MEM_RELEASE);
    }
    image_ = nullptr;
    SetBootLogSink(nullptr);
    nevr_server_features::Client().Clear();
  }

  void PutCallback(const std::uint8_t* prologue, std::size_t len, const std::uint8_t* undo = kUndo.data(),
                   std::size_t undoLen = kUndo.size()) {
    const std::vector<std::uint8_t> stub =
        MakeStub(prologue, len, undo, undoLen, reinterpret_cast<std::uintptr_t>(&FakeCallback));
    std::memcpy(image_ + (kCallbackVa - kImageBase), stub.data(), stub.size());
  }

  void Receive(const std::string& text, std::uint64_t size) {
    text_ = text;
    const char* p = text_.data();
    std::memcpy(msg_.data() + 0x18, &p, sizeof(p));
    std::memcpy(msg_.data() + 0x20, &size, sizeof(size));
    CallCallback(reinterpret_cast<CallbackFn>(image_ + (kCallbackVa - kImageBase)), &netGame_, msg_.data());
  }
  void Receive(const std::string& text) { Receive(text, text.size()); }

  std::uint8_t* image_ = nullptr;
  std::vector<std::uint8_t> msg_;
  std::string text_;
  int netGame_ = 0;
};

TEST_F(LobbySettingsHook, AServerThatStatesEchoCombatOpensTheGateAndTheGameStillGetsTheCall) {
  PutCallback(kPrologue.data(), kPrologue.size());
  nevr_lobby_settings_hook::Install(reinterpret_cast<std::uintptr_t>(image_));
  ASSERT_NE(FindContaining("[NEVR.PATCH] hooked name=LobbySettingsCB va=0x14017DE60"), nullptr);

  Receive(R"({"parties_unlocked":true,"nevr_server":{"features":["echocombat","echocombat_medigun"]}})");
  EXPECT_EQ(g_calls, 1);
  EXPECT_EQ(g_netGame, &netGame_);
  EXPECT_EQ(g_msg, msg_.data());
  EXPECT_TRUE(nevr_server_features::Client().Current().echoCombat);
  EXPECT_TRUE(nevr_server_features::Client().Current().medigun);
  const Line* line = FindContaining("[NEVR.PATCH] lobby settings received statement=stated features=echocombat,echocombat_medigun");
  ASSERT_NE(line, nullptr);
  EXPECT_EQ(line->level, EchoVR::LogLevel::Info);
  EXPECT_NE(line->text.find("changed=yes (EchoCombat tac mods shown, MEDIGUN shown on this server)"), std::string::npos)
      << line->text;
}

TEST_F(LobbySettingsHook, AStockServersDocumentClosesTheGate) {
  PutCallback(kPrologue.data(), kPrologue.size());
  nevr_lobby_settings_hook::Install(reinterpret_cast<std::uintptr_t>(image_));
  Receive(R"({"nevr_server":{"features":["echocombat"]}})");
  ASSERT_TRUE(nevr_server_features::Client().Current().echoCombat);
  g_logged.clear();
  Receive(R"({"parties_unlocked":true})");
  EXPECT_EQ(g_calls, 2);
  EXPECT_FALSE(nevr_server_features::Client().Current().echoCombat);
  EXPECT_NE(FindContaining("statement=not_stated features=none dropped=0"), nullptr);
}

// The game passes the size; the text has no terminator. A size past what the relay accepts is not read.
TEST_F(LobbySettingsHook, ReadsTheSizeTheGameGivesAndRefusesAHugeOne) {
  PutCallback(kPrologue.data(), kPrologue.size());
  nevr_lobby_settings_hook::Install(reinterpret_cast<std::uintptr_t>(image_));
  const std::string doc = R"({"nevr_server":{"features":["echocombat"]}})";
  Receive(doc + "trailing bytes the game does not count", doc.size());
  EXPECT_TRUE(nevr_server_features::Client().Current().echoCombat);
  g_logged.clear();
  Receive(doc, 2ULL * 1024ULL * 1024ULL);
  EXPECT_EQ(g_calls, 2);
  EXPECT_FALSE(nevr_server_features::Client().Current().echoCombat);
  const Line* line = FindContaining("statement=malformed");
  ASSERT_NE(line, nullptr);
  EXPECT_EQ(line->level, EchoVR::LogLevel::Warning);
}

TEST_F(LobbySettingsHook, AChangedPrologueIsAWarningAndNoHook) {
  std::array<std::uint8_t, 10> other = kPrologue;
  other[9] = 0x40;  // SUB RSP,0x40: another build
  std::array<std::uint8_t, 10> otherUndo = kUndo;
  otherUndo[3] = 0x40;  // ADD RSP,0x40
  PutCallback(other.data(), other.size(), otherUndo.data(), otherUndo.size());
  nevr_lobby_settings_hook::Install(reinterpret_cast<std::uintptr_t>(image_));
  const Line* line = FindContaining("[NEVR.PATCH] hook skipped name=LobbySettingsCB va=0x14017DE60 reason=prologue_mismatch "
                                    "expected=48895c2408574883ec30 actual=48895c2408574883ec40");
  ASSERT_NE(line, nullptr);
  EXPECT_EQ(line->level, EchoVR::LogLevel::Warning);
  Receive(R"({"nevr_server":{"features":["echocombat"]}})");
  EXPECT_EQ(g_calls, 1);
  EXPECT_FALSE(nevr_server_features::Client().Current().echoCombat);
}

}  // namespace
