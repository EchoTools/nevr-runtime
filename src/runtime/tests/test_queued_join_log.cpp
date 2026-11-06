// #2163: the [NEVR.MATCH] records for a lobby session success and the join it queues.
//
// Two layers. The rules (queued_join_log_rules.h) are tested as functions. The hooks (patch/queued_join_log.cpp) are the
// production code, installed under MinHook into a fake game image: the four game functions sit at their real RVAs with
// their real prologue bytes, followed by code that undoes the prologue's stack effect and jumps to a C++ stand-in for the
// game's behaviour. The stand-ins read and write the lobby the way the disassembly does (status at +0x448, the queue
// buffer at +0x6e0 aligned to 8 with the id at +8, the lobby client at [netGame+0x40]). Install, PatchDetour,
// ValidatePrologue and the four detours are the production code; only the game is stood in for.
//
// NOT shown by these tests: that echovr.exe has these bytes and this layout on a live process. That is the run on a game
// copy.

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
#include "runtime/hook/hook_guard.h"
#include "runtime/patch/queued_join_log.h"
#include "runtime/patch/queued_join_log_rules.h"

namespace {

using nevr_lobby_frame::Guid;
using namespace nevr_queued_join_log;

constexpr std::uint64_t kImageBase = 0x140000000ULL;
constexpr std::uint64_t kSuccessCbVa = 0x14017DB90ULL;
constexpr std::uint64_t kLeaveVa = 0x140179600ULL;
constexpr std::uint64_t kSwitchToVa = 0x1401B7BB0ULL;
constexpr std::uint64_t kEndVa = 0x140162230ULL;
constexpr std::uint64_t kInvalidIdVa = 0x142100B20ULL;
constexpr std::size_t kImageBytes = 0x2200000;  // covers kInvalidIdVa + 16 (RVA 0x2100B20)
constexpr std::size_t kPageBytes = 0x1000;
constexpr std::size_t kLobbyBytes = 0x1000;

// The four real prologues, then the undo of their stack effect, then `movabs rax, imm64; jmp rax`.
constexpr std::array<std::uint8_t, 12> kSuccessPrologue = {0x4C, 0x8B, 0xDC, 0x55, 0x57, 0x48,
                                                           0x81, 0xEC, 0x88, 0x00, 0x00, 0x00};
constexpr std::array<std::uint8_t, 9> kSuccessUndo = {0x48, 0x81, 0xC4, 0x88, 0x00, 0x00, 0x00, 0x5F, 0x5D};
constexpr std::array<std::uint8_t, 8> kLeavePrologue = {0x40, 0x56, 0x48, 0x83, 0xEC, 0x20, 0x33, 0xD2};
constexpr std::array<std::uint8_t, 5> kLeaveUndo = {0x48, 0x83, 0xC4, 0x20, 0x5E};
constexpr std::array<std::uint8_t, 18> kSwitchPrologue = {0x48, 0x89, 0x5C, 0x24, 0x18, 0x48, 0x89, 0x74, 0x24,
                                                          0x20, 0x55, 0x57, 0x41, 0x54, 0x41, 0x56, 0x41, 0x57};
constexpr std::array<std::uint8_t, 8> kSwitchUndo = {0x41, 0x5F, 0x41, 0x5E, 0x41, 0x5C, 0x5F, 0x5D};

// CR15NetClientLobby::End (echovr.exe 0x140162230): PUSH RBX,RDI,R14; SUB RSP,0x30; XOR EDX,EDX; MOV [RSP+0x60],R15.
constexpr std::array<std::uint8_t, 16> kEndPrologue = {0x40, 0x53, 0x57, 0x41, 0x56, 0x48, 0x83, 0xEC,
                                                       0x30, 0x33, 0xD2, 0x4C, 0x89, 0x7C, 0x24, 0x60};
constexpr std::array<std::uint8_t, 8> kEndUndo = {0x48, 0x83, 0xC4, 0x30, 0x41, 0x5E, 0x5F, 0x5B};

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

const Guid kInvalid = {0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE};
const Guid kSession = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
const char* const kSessionText = "04030201-0605-0807-090A-0B0C0D0E0F10";

// The game's stand-ins. What the success callback does is set by the test: the game decides it from the game state, which
// the fixture does not model.
enum class Outcome { Queue, Join, Oversize };
Outcome g_outcome = Outcome::Join;
bool g_leaveStarts = true;
int g_successCalls = 0;
void* g_successLobby = nullptr;
void* g_successMsg = nullptr;
int g_leaveCalls = 0;
int g_switchCalls = 0;

std::uint8_t* QueueSlot(void* lobby) {
  // The game's alignment, from the disassembly: (-(lobby + 0x6e0)) & 7 added to lobby + 0x6e0.
  const auto base = reinterpret_cast<std::uintptr_t>(lobby) + 0x6E0;
  const std::uintptr_t pad = static_cast<std::uintptr_t>(-static_cast<std::intptr_t>(base)) & 7U;
  return reinterpret_cast<std::uint8_t*>(base + pad);
}

std::int32_t& Status(void* lobby) { return *reinterpret_cast<std::int32_t*>(static_cast<std::uint8_t*>(lobby) + 0x448); }

void FakeSuccessCb(void* lobby, void* msg) {
  ++g_successCalls;
  g_successLobby = lobby;
  g_successMsg = msg;
  if (!GateOpen(Status(lobby))) return;
  switch (g_outcome) {
    case Outcome::Queue: {
      const std::uint8_t* payload = nullptr;
      std::memcpy(&payload, static_cast<std::uint8_t*>(msg) + 0x10, sizeof(payload));
      std::memcpy(QueueSlot(lobby), payload, 0x48);
      Status(lobby) = 7;
      break;
    }
    case Outcome::Join:
      Status(lobby) = 8;
      break;
    case Outcome::Oversize:
      break;
  }
}

Guid g_invalidForFake = kInvalid;
std::uint8_t* g_imageForFake = nullptr;
int g_endCalls = 0;

using EndFn = void (*)(void*);
__attribute__((noinline)) void CallEnd(EndFn fn, void* lobby) { fn(lobby); }

// CR15NetClientLobby::End writes status 0 directly (echovr.exe 0x14016230f: MOV [RBX+0x448], R14D with R14D = 0); it does
// not go through SwitchTo.
void FakeEnd(void* lobby) {
  ++g_endCalls;
  Status(lobby) = 0;
}

// The game's "Leave For Match" (0x140179600): LeaveGame first (0x140179622), whose lobby End leaves the status at 0; then,
// when its own conditions allow, StartSessionJoin on the queue buffer: status 8, and the invalid id over the queued id.
// A join is started whether or not an id was queued.
void FakeLeave(void* netGame) {
  ++g_leaveCalls;
  void* lobby = nullptr;
  std::memcpy(&lobby, static_cast<std::uint8_t*>(netGame) + 0x40, sizeof(lobby));
  CallEnd(reinterpret_cast<EndFn>(g_imageForFake + (kEndVa - kImageBase)), lobby);
  if (!g_leaveStarts) return;
  Status(lobby) = 8;
  std::memcpy(QueueSlot(lobby) + 8, g_invalidForFake.data(), 16);
}

void FakeSwitchTo(void* lobby, std::int32_t status) {
  ++g_switchCalls;
  Status(lobby) = status;
}

struct Line {
  EchoVR::LogLevel level;
  std::string text;
};
std::vector<Line> g_logged;

void CaptureLog(EchoVR::LogLevel level, const CHAR* message) { g_logged.push_back({level, message}); }

int CountContaining(const std::string& needle) {
  int n = 0;
  for (const Line& line : g_logged) {
    if (line.text.find(needle) != std::string::npos) ++n;
  }
  return n;
}

const Line* FindContaining(const std::string& needle) {
  for (const Line& line : g_logged) {
    if (line.text.find(needle) != std::string::npos) return &line;
  }
  return nullptr;
}

using SuccessFn = void (*)(void*, void*);
using LeaveFn = void (*)(void*);
using SwitchFn = void (*)(void*, std::int32_t);

__attribute__((noinline)) void CallSuccess(SuccessFn fn, void* lobby, void* msg) { fn(lobby, msg); }
__attribute__((noinline)) void CallLeave(LeaveFn fn, void* netGame) { fn(netGame); }
__attribute__((noinline)) void CallSwitch(SwitchFn fn, void* lobby, std::int32_t status) { fn(lobby, status); }

class QueuedJoinLogHooks : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(Hooking::Initialize());
    nevr_hook_guard::ResetForTest();
    g_logged.clear();
    g_outcome = Outcome::Join;
    g_leaveStarts = true;
    g_successCalls = g_leaveCalls = g_switchCalls = g_endCalls = 0;
    g_successLobby = g_successMsg = nullptr;
    SetBootLogSink(&CaptureLog);

    image_ = static_cast<std::uint8_t*>(
        VirtualAlloc(nullptr, kImageBytes, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
    ASSERT_NE(image_, nullptr) << "VirtualAlloc error=" << GetLastError();

    PutStub(kSuccessCbVa, MakeStub(kSuccessPrologue.data(), kSuccessPrologue.size(), kSuccessUndo.data(),
                                   kSuccessUndo.size(), reinterpret_cast<std::uintptr_t>(&FakeSuccessCb)));
    PutStub(kLeaveVa, MakeStub(kLeavePrologue.data(), kLeavePrologue.size(), kLeaveUndo.data(), kLeaveUndo.size(),
                               reinterpret_cast<std::uintptr_t>(&FakeLeave)));
    PutStub(kSwitchToVa, MakeStub(kSwitchPrologue.data(), kSwitchPrologue.size(), kSwitchUndo.data(),
                                  kSwitchUndo.size(), reinterpret_cast<std::uintptr_t>(&FakeSwitchTo)));
    PutStub(kEndVa, MakeStub(kEndPrologue.data(), kEndPrologue.size(), kEndUndo.data(), kEndUndo.size(),
                             reinterpret_cast<std::uintptr_t>(&FakeEnd)));
    g_imageForFake = image_;
    std::memcpy(At(kInvalidIdVa), kInvalid.data(), kInvalid.size());

    lobbyStore_.assign(kLobbyBytes + 16, 0);
    netGameStore_.assign(0x100, 0);
    payload_.assign(0x48, 0);
    msg_.assign(0x40, 0);
    SetLobbyOffset(0);
    std::memcpy(payload_.data() + 8, kSession.data(), kSession.size());
    const std::uint8_t* payloadPtr = payload_.data();
    std::memcpy(msg_.data() + 0x10, &payloadPtr, sizeof(payloadPtr));
    ClearQueue();
  }

  void TearDown() override {
    if (image_ != nullptr) {
      // Hooking::Detach only disables; MinHook keeps the entry, and the next allocation can land on the same address.
      MH_RemoveHook(At(kSuccessCbVa));
      MH_RemoveHook(At(kLeaveVa));
      MH_RemoveHook(At(kSwitchToVa));
      MH_RemoveHook(At(kEndVa));
      VirtualFree(image_, 0, MEM_RELEASE);
    }
    image_ = nullptr;
    SetBootLogSink(nullptr);
  }

  std::uintptr_t Base() const { return reinterpret_cast<std::uintptr_t>(image_); }
  std::uint8_t* At(std::uint64_t va) { return image_ + (va - kImageBase); }
  void PutStub(std::uint64_t va, const std::vector<std::uint8_t>& bytes) { std::memcpy(At(va), bytes.data(), bytes.size()); }

  // The lobby sits at an offset into its store so a test can give it an address that is not 8-aligned.
  void SetLobbyOffset(std::size_t offset) {
    lobby_ = lobbyStore_.data() + offset;
    void* lobby = lobby_;
    std::memcpy(netGameStore_.data() + 0x40, &lobby, sizeof(lobby));
  }
  void ClearQueue() { std::memcpy(QueueSlot(lobby_) + 8, kInvalid.data(), kInvalid.size()); }
  void Queued(const Guid& id) { std::memcpy(QueueSlot(lobby_) + 8, id.data(), id.size()); }

  void Success(std::int32_t statusAtArrival) {
    Status(lobby_) = statusAtArrival;
    CallSuccess(reinterpret_cast<SuccessFn>(At(kSuccessCbVa)), lobby_, msg_.data());
  }
  void Leave() { CallLeave(reinterpret_cast<LeaveFn>(At(kLeaveVa)), netGameStore_.data()); }
  void End() { CallEnd(reinterpret_cast<EndFn>(At(kEndVa)), lobby_); }
  void SwitchTo(std::int32_t status) { CallSwitch(reinterpret_cast<SwitchFn>(At(kSwitchToVa)), lobby_, status); }

  std::uint8_t* image_ = nullptr;
  std::vector<std::uint8_t> lobbyStore_;
  std::vector<std::uint8_t> netGameStore_;
  std::vector<std::uint8_t> payload_;
  std::vector<std::uint8_t> msg_;
  std::uint8_t* lobby_ = nullptr;
};

}  // namespace

// ---- the rules ----

TEST(QueuedJoinRules, ClassifyNamesWhatTheGameDid) {
  EXPECT_EQ(Classify(5, 7).action, Action::Queued);
  EXPECT_STREQ(Classify(5, 7).reason, "in_game_session");
  EXPECT_EQ(Classify(4, 8).action, Action::Joined);
  EXPECT_EQ(Classify(6, 8).action, Action::Joined);
  EXPECT_STREQ(Classify(6, 8).reason, "no_game_session");
  EXPECT_EQ(Classify(7, 7).action, Action::Ignored);
  EXPECT_STREQ(Classify(7, 7).reason, "already_queued");
  EXPECT_EQ(Classify(3, 3).action, Action::Ignored);
  EXPECT_STREQ(Classify(3, 3).reason, "status_not_4_5_6");
  EXPECT_EQ(Classify(8, 8).action, Action::Ignored);
  EXPECT_EQ(Classify(5, 5).action, Action::Ignored);
  EXPECT_STREQ(Classify(5, 5).reason, "message_too_large");
  EXPECT_EQ(Classify(5, 12).action, Action::Unclassified);
}

TEST(QueuedJoinRules, TheGateIsStatusFourToSix) {
  for (std::int32_t s = -2; s < 12; ++s) EXPECT_EQ(GateOpen(s), s >= 4 && s <= 6) << "status " << s;
}

TEST(QueuedJoinRules, TheSuccessLineHasEveryField) {
  EXPECT_EQ(FormatSuccessLine(5, 7, kSession),
            std::string("[NEVR.MATCH] session success status_at_arrival=5 status_after=7 session=") + kSessionText +
                " action=queued reason=in_game_session (the join waits until the game leaves the current game session)");
}

TEST(QueuedJoinRules, ANilSessionReadsNone) {
  EXPECT_NE(FormatSuccessLine(3, 3, Guid{}).find("session=none "), std::string::npos);
}

// StartSessionJoin always switches to 8 and sends the request, so the status after Leave For Match says whether a join
// went out; whether an id was queued is a separate fact.
TEST(QueuedJoinRules, LeaveResultFollowsTheStatusAfterNotTheQueue) {
  EXPECT_EQ(ClassifyLeave(true, 8), LeaveResult::Started);
  EXPECT_EQ(ClassifyLeave(false, 8), LeaveResult::StartedNothingQueued);
  EXPECT_EQ(ClassifyLeave(true, 0), LeaveResult::NotStarted);
  EXPECT_EQ(ClassifyLeave(false, 0), LeaveResult::NotStarted);
  EXPECT_EQ(ClassifyLeave(true, 7), LeaveResult::NotStarted);
  EXPECT_STREQ(LeaveResultName(LeaveResult::StartedNothingQueued), "started_nothing_queued");
}

TEST(QueuedJoinRules, TheLeaveLineSaysWhatHappenedToTheJoin) {
  EXPECT_EQ(FormatLeaveLine(LeaveResult::NotStarted, kSession, 7, 0),
            std::string("[NEVR.MATCH] queued join acted on session=") + kSessionText +
                " result=not_started status_before=7 status_after=0 (the game left its session but sent no join "
                "request; the queued id stays in the buffer and a later Leave For Match could still send it)");
  EXPECT_NE(FormatLeaveLine(LeaveResult::StartedNothingQueued, Guid{}, 5, 8).find("a join request was sent with no queued id"),
            std::string::npos);
}

// A drop writes nothing to the queue buffer (only a started join overwrites the id), so the line says the id stays there.
TEST(QueuedJoinRules, TheDropLineSaysTheQueuedIdStaysInTheBuffer) {
  EXPECT_EQ(FormatDropLine(kSession, 0, "End"),
            std::string("[NEVR.MATCH] queued join dropped session=") + kSessionText +
                " status_to=0 by=End (no join request was sent for the queued session; its id stays in the buffer and a "
                "later Leave For Match could still send it)");
}

TEST(QueuedJoinRules, ADropIsLeavingSevenForAnythingButEight) {
  EXPECT_TRUE(IsDrop(7, 2));
  EXPECT_TRUE(IsDrop(7, 5));
  EXPECT_FALSE(IsDrop(7, 8));
  EXPECT_FALSE(IsDrop(7, 7));
  EXPECT_FALSE(IsDrop(5, 2));
}

// ---- the hooks ----

TEST_F(QueuedJoinLogHooks, ASuccessThatQueuesIsLoggedAsQueuedWithTheSessionAndBothStatuses) {
  nevr_queued_join_log::Install(Base());
  g_outcome = Outcome::Queue;
  Success(5);
  const Line* line = FindContaining("[NEVR.MATCH] session success");
  ASSERT_NE(line, nullptr);
  EXPECT_EQ(line->level, EchoVR::LogLevel::Info);
  EXPECT_EQ(line->text, FormatSuccessLine(5, 7, kSession));
  EXPECT_NE(line->text.find("status_at_arrival=5 status_after=7"), std::string::npos);
  EXPECT_NE(line->text.find(std::string("session=") + kSessionText), std::string::npos);
  EXPECT_NE(line->text.find("action=queued"), std::string::npos);
}

TEST_F(QueuedJoinLogHooks, ASuccessThatStartsTheJoinIsLoggedAsJoined) {
  nevr_queued_join_log::Install(Base());
  g_outcome = Outcome::Join;
  Success(4);
  EXPECT_EQ(CountContaining("status_at_arrival=4 status_after=8"), 1);
  EXPECT_EQ(CountContaining("action=joined"), 1);
}

TEST_F(QueuedJoinLogHooks, ASuccessWhileAlreadyQueuedIsLoggedAsIgnored) {
  nevr_queued_join_log::Install(Base());
  g_outcome = Outcome::Queue;
  Success(7);
  EXPECT_EQ(CountContaining("status_at_arrival=7 status_after=7"), 1);
  EXPECT_EQ(CountContaining("action=ignored reason=already_queued"), 1);
}

TEST_F(QueuedJoinLogHooks, ASuccessOutsideTheGateIsLoggedAsIgnored) {
  nevr_queued_join_log::Install(Base());
  Success(3);
  EXPECT_EQ(CountContaining("status_at_arrival=3 status_after=3"), 1);
  EXPECT_EQ(CountContaining("action=ignored reason=status_not_4_5_6"), 1);
}

TEST_F(QueuedJoinLogHooks, AMessageThatDoesNotFitLeavesTheStatusAndIsLoggedAsIgnored) {
  nevr_queued_join_log::Install(Base());
  g_outcome = Outcome::Oversize;
  Success(5);
  EXPECT_EQ(CountContaining("action=ignored reason=message_too_large"), 1);
}

TEST_F(QueuedJoinLogHooks, TheSuccessCallbackIsForwardedOnceWithItsArgumentsForEveryStatus) {
  nevr_queued_join_log::Install(Base());
  for (std::int32_t status : {2, 5, 7}) {
    g_successCalls = 0;
    g_outcome = Outcome::Join;
    Success(status);
    EXPECT_EQ(g_successCalls, 1) << "status " << status;
    EXPECT_EQ(g_successLobby, lobby_);
    EXPECT_EQ(g_successMsg, msg_.data());
  }
}

TEST_F(QueuedJoinLogHooks, ALobbyAtAnAddressThatIsNotEightAlignedReadsTheSameQueue) {
  nevr_queued_join_log::Install(Base());
  SetLobbyOffset(3);
  ClearQueue();
  Queued(kSession);
  SwitchTo(7);  // the status is read from the unaligned lobby too
  SwitchTo(2);
  EXPECT_EQ(CountContaining(std::string("queued join dropped session=") + kSessionText + " status_to=2"), 1);
}

TEST_F(QueuedJoinLogHooks, LeavingForAMatchThatStartsIsLoggedWithTheQueuedSession) {
  nevr_queued_join_log::Install(Base());
  Status(lobby_) = 7;
  Queued(kSession);
  g_leaveStarts = true;
  Leave();
  EXPECT_EQ(g_leaveCalls, 1);
  EXPECT_EQ(CountContaining(std::string("queued join acted on session=") + kSessionText +
                            " result=started status_before=7 status_after=8"),
            1);
}

TEST_F(QueuedJoinLogHooks, LeavingForAMatchThatTheGameHoldsBackIsLoggedAsNotStarted) {
  nevr_queued_join_log::Install(Base());
  Status(lobby_) = 7;
  Queued(kSession);
  g_leaveStarts = false;
  Leave();
  EXPECT_EQ(CountContaining(std::string("queued join acted on session=") + kSessionText +
                            " result=not_started status_before=7 status_after=0"),
            1);
  EXPECT_EQ(CountContaining("still queued"), 0) << "End has already taken the status to 0, so the join is no longer waiting at status 7";
  EXPECT_EQ(g_endCalls, 1) << "LeaveGame ran the lobby's End before the game's conditions were checked";
}

// StartSessionJoin runs whether or not an id was queued (it always switches to 8 and sends the request), so a Leave For
// Match with an empty queue is a join request for the invalid id, not "nothing happened".
TEST_F(QueuedJoinLogHooks, LeavingForAMatchWithNothingQueuedStillStartsAJoinAndSaysSo) {
  nevr_queued_join_log::Install(Base());
  Status(lobby_) = 5;
  g_leaveStarts = true;
  Leave();
  EXPECT_EQ(CountContaining("queued join acted on session=none result=started_nothing_queued status_before=5 "
                            "status_after=8"),
            1);
  EXPECT_EQ(CountContaining("result=nothing_queued"), 0);
}

TEST_F(QueuedJoinLogHooks, AQueuedJoinThatTheStatusLeavesIsLoggedAsDropped) {
  nevr_queued_join_log::Install(Base());
  Status(lobby_) = 7;
  Queued(kSession);
  SwitchTo(2);
  EXPECT_EQ(g_switchCalls, 1);
  EXPECT_EQ(Status(lobby_), 2);
  EXPECT_EQ(CountContaining(std::string("[NEVR.MATCH] queued join dropped session=") + kSessionText +
                            " status_to=2 by=SwitchTo"),
            1);
}

// CR15NetClientLobby::End writes status 0 directly, so a queued join that End ends never passes through SwitchTo.
TEST_F(QueuedJoinLogHooks, AQueuedJoinThatEndTakesToStatusZeroIsLoggedAsDropped) {
  nevr_queued_join_log::Install(Base());
  Status(lobby_) = 7;
  Queued(kSession);
  End();
  EXPECT_EQ(g_endCalls, 1) << "the original End runs";
  EXPECT_EQ(Status(lobby_), 0);
  const Line* line = FindContaining("[NEVR.MATCH] queued join dropped");
  ASSERT_NE(line, nullptr);
  EXPECT_EQ(line->level, EchoVR::LogLevel::Info);
  EXPECT_NE(line->text.find(std::string("session=") + kSessionText), std::string::npos);
  EXPECT_NE(line->text.find("status_to=0 by=End"), std::string::npos);
  EXPECT_EQ(CountContaining("[NEVR.MATCH] queued join dropped"), 1);
}

// The End that Leave For Match's own LeaveGame runs is the join being acted on, not a drop: one acted-on record only.
TEST_F(QueuedJoinLogHooks, TheEndInsideLeaveForMatchIsNotADrop) {
  nevr_queued_join_log::Install(Base());
  Status(lobby_) = 7;
  Queued(kSession);
  g_leaveStarts = true;
  Leave();
  EXPECT_EQ(g_endCalls, 1);
  EXPECT_EQ(CountContaining("[NEVR.MATCH] queued join dropped"), 0);
  EXPECT_EQ(CountContaining("result=started status_before=7 status_after=8"), 1);
}

// After Leave For Match returns, an End outside it is a drop again (the in-hook mark does not stay set).
TEST_F(QueuedJoinLogHooks, AnEndAfterLeaveForMatchHasReturnedIsADropAgain) {
  nevr_queued_join_log::Install(Base());
  Status(lobby_) = 7;
  Queued(kSession);
  g_leaveStarts = false;
  Leave();
  Status(lobby_) = 7;
  End();
  EXPECT_EQ(CountContaining("[NEVR.MATCH] queued join dropped"), 1);
}

TEST_F(QueuedJoinLogHooks, AnEndAtAnyOtherStatusWritesNothing) {
  nevr_queued_join_log::Install(Base());
  for (std::int32_t status : {0, 3, 5, 8}) {
    Status(lobby_) = status;
    End();
  }
  EXPECT_EQ(g_endCalls, 4);
  EXPECT_EQ(CountContaining("[NEVR.MATCH] queued join dropped"), 0);
}

TEST_F(QueuedJoinLogHooks, OtherStatusChangesWriteNothingAndAreForwarded) {
  nevr_queued_join_log::Install(Base());
  Status(lobby_) = 7;
  Queued(kSession);
  SwitchTo(8);  // the queued join starting is the acted-on record, not a drop
  Status(lobby_) = 5;
  SwitchTo(2);
  EXPECT_EQ(g_switchCalls, 2);
  EXPECT_EQ(CountContaining("[NEVR.MATCH] queued join dropped"), 0);
}

TEST_F(QueuedJoinLogHooks, InstallSaysWhatItHookedAndNothingElse) {
  nevr_queued_join_log::Install(Base());
  EXPECT_EQ(CountContaining("[NEVR.MATCH] hooked name=LobbySessionSuccessCB va=0x14017DB90"), 1);
  EXPECT_EQ(CountContaining("[NEVR.MATCH] hooked name=LeaveForMatch va=0x140179600"), 1);
  EXPECT_EQ(CountContaining("[NEVR.MATCH] hooked name=NetClientLobbySwitchTo va=0x1401B7BB0"), 1);
  EXPECT_EQ(CountContaining("[NEVR.MATCH] hooked name=NetClientLobbyEnd va=0x140162230"), 1);
  EXPECT_EQ(CountContaining("reason="), 0);
}

TEST_F(QueuedJoinLogHooks, AChangedPrologueSkipsThatHookAndSaysWhatTheBytesWere) {
  std::uint8_t* target = At(kSuccessCbVa);
  target[2] = 0xD4;  // MOV R11,RSP becomes MOV R10,RSP: the callback is not the one the hook was written against, and still runs
  nevr_queued_join_log::Install(Base());
  EXPECT_EQ(CountContaining("hooked name=LobbySessionSuccessCB"), 0);
  EXPECT_EQ(CountContaining("expected=4c8bdc55574881ec88000000 actual=4c8bd455574881ec88000000"), 1);
  const Line* skipped = FindContaining("hook skipped name=LobbySessionSuccessCB");
  ASSERT_NE(skipped, nullptr);
  EXPECT_EQ(skipped->level, EchoVR::LogLevel::Warning);
  EXPECT_EQ(CountContaining("hooked name=LeaveForMatch"), 1);
  Success(5);
  EXPECT_EQ(CountContaining("[NEVR.MATCH] session success"), 0);
  EXPECT_EQ(g_successCalls, 1) << "the stock callback still runs";
}

TEST_F(QueuedJoinLogHooks, AnUnreadableInvalidIdKeepsTheSuccessRecordAndSkipsTheOtherTwoHooks) {
  const std::uint64_t page = kInvalidIdVa & ~static_cast<std::uint64_t>(kPageBytes - 1);
  ASSERT_TRUE(VirtualFree(At(page), kPageBytes, MEM_DECOMMIT)) << "VirtualFree error=" << GetLastError();
  nevr_queued_join_log::Install(Base());
  EXPECT_EQ(CountContaining("hooked name=LobbySessionSuccessCB"), 1);
  EXPECT_EQ(CountContaining("hooked name=LeaveForMatch"), 0);
  EXPECT_EQ(CountContaining("hooked name=NetClientLobbySwitchTo"), 0);
  EXPECT_EQ(CountContaining("hooked name=NetClientLobbyEnd"), 0);
  EXPECT_EQ(CountContaining("reason=invalid_id_unreadable va=0x142100B20"), 1);
}

TEST_F(QueuedJoinLogHooks, AChangedEndPrologueSkipsOnlyTheEndHookAndSaysWhatTheBytesWere) {
  At(kEndVa)[3] = 0x42;  // PUSH R14 (41 56) becomes 42 56: a different function, and the stub still runs
  nevr_queued_join_log::Install(Base());
  EXPECT_EQ(CountContaining("hooked name=NetClientLobbyEnd"), 0);
  EXPECT_EQ(CountContaining("hook skipped name=NetClientLobbyEnd va=0x140162230 reason=prologue_mismatch "
                            "expected=40535741564883ec3033d24c897c2460 actual=40535742564883ec3033d24c897c2460"),
            1);
  EXPECT_EQ(CountContaining("hooked name=NetClientLobbySwitchTo"), 1);
}
