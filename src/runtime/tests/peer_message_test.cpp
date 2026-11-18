// Tests for runtime/compat/peer_message.cpp (#2365): the PC half of nEVR's control messages on the game's broadcaster.
// peer_message_rules_test.cpp holds the codec and the rules; this holds what the half DECIDES with them: when it listens,
// who it greets, who it answers, which messages it refuses, and what it logs, against a fake game context, a fake
// broadcaster (BroadcasterListen and BroadcasterSend are the abi's own function pointers) and, for the client, a fake
// GetAuthorityPeer built from the stock prologue the half checks.
#include "runtime/compat/peer_message.h"

#include <winsock2.h>  // before windows.h: core/logging.h pulls it in
#include <windows.h>

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "abi/echovr.h"
#include "abi/echovr_functions.h"
#include "core/logging.h"
#include "runtime/compat/peer_message_rules.h"
#include "runtime/compat/peer_message_targets.h"
#include "runtime/lifecycle/cli.h"
#include "runtime/lifecycle/config.h"

// The two process globals the half reads (their definitions live in lifecycle/cli.cpp and lifecycle/config.cpp).
BOOL g_isServer = FALSE;
PVOID g_pGame = nullptr;

namespace nevr_combat {
// The server's stated features, set by each test (the real list is combat's).
std::vector<std::string> g_testFeatures;
std::vector<std::string> ServerFeatures() { return g_testFeatures; }
}  // namespace nevr_combat

namespace pm = nevr_peer_message;

namespace {

std::string P(std::uint64_t v) { return std::to_string(v); }

constexpr pm::Targets kT = pm::kPcTargets;
constexpr std::uint64_t kPollUs = 250000;          // the half's own step (peer_message.cpp kPollUs)
constexpr std::uint64_t kRetryUs = 5000000;        // a failed listen is tried again after this
constexpr std::uint64_t kFirstPollUs = kPollUs;    // the first Poll that passes the 250 ms gate
constexpr std::size_t kContextBytes = 0x9000;      // covers the game context's net game field
constexpr std::size_t kNetGameBytes = 0x3000;      // covers the net game's broadcaster and flags fields
constexpr std::size_t kBroadcasterBytes = 64;
constexpr std::size_t kFakeImageBytes = 0x1AC000;  // the image up to past the authority peer function's RVA
constexpr std::uint64_t kImageBaseVa = 0x140000000ULL;
constexpr std::uint16_t kListenOk = 7;
constexpr std::uint16_t kListenNone = 0xFFFF;
constexpr std::uint64_t kPeerA = 11;
constexpr std::uint64_t kPeerB = 12;
constexpr std::uint64_t kPeerOutside = 99;
constexpr std::uint64_t kAuthority = 5;
constexpr std::uint32_t kAcceptBound = pm::kAcceptPerWindow;
constexpr std::uint32_t kRefusalBound = pm::kRefusalLinesPerWindow;
constexpr std::uint32_t kListenWarnEvery = 12;  // peer_message.cpp: the first failure and every 12th after it
constexpr std::uint64_t kSeq = 42;
constexpr std::size_t kMachineCodeMax = 64;
constexpr std::size_t kSendsPerGreeting = 2;  // a greeted peer gets capabilities and a ping
constexpr std::size_t kTwoGreetings = 2 * kSendsPerGreeting;
constexpr std::uint64_t kHalfStepUs = kPollUs / 2;
constexpr std::uint64_t kTwoStepsUs = 2 * kPollUs;
constexpr int kListenWarnings = 2;       // the first failure and the 13th attempt
constexpr std::uint32_t kPastRefusalBound = 3;
constexpr std::uint32_t kPastAcceptBound = 2;
constexpr std::uint64_t kPeerSelf = 0xFFFFFFFFFFFFFFFCULL;  // the game's "this machine is the authority" answer (peer_message.cpp)

struct Send {
  std::uint64_t symbol = 0;
  std::uint64_t peer = 0;
  std::uint64_t dest = 0;
  std::string bytes;
};

std::vector<Send> g_sends;
int g_listenCalls = 0;
std::uint16_t g_listenResult = kListenOk;
std::uint64_t g_listenSymbol = 0;
BOOL g_listenReliable = FALSE;
void* g_proxyFunc = nullptr;
std::vector<std::string> g_lines;
std::uint64_t g_fakeAuthority = kAuthority;  // what the fake GetAuthorityPeer writes

UINT16 FakeListen(EchoVR::Broadcaster*, EchoVR::SymbolId id, BOOL reliable, VOID* px, BOOL) {
  ++g_listenCalls;
  g_listenSymbol = static_cast<std::uint64_t>(id);
  g_listenReliable = reliable;
  g_proxyFunc = px != nullptr ? static_cast<EchoVR::DelegateProxy*>(px)->proxyFunc : nullptr;
  return g_listenResult;
}

INT32 FakeSend(EchoVR::Broadcaster*, EchoVR::SymbolId id, INT32, VOID* item, UINT64 size, VOID*, UINT64, EchoVR::Peer peer,
               UINT64 dest, FLOAT, EchoVR::SymbolId) {
  Send s;
  s.symbol = static_cast<std::uint64_t>(id);
  s.peer = peer;
  s.dest = dest;
  s.bytes.assign(static_cast<const char*>(item), static_cast<std::size_t>(size));
  g_sends.push_back(std::move(s));
  return 1;
}

VOID CaptureLog(EchoVR::LogLevel, const CHAR* message) { g_lines.push_back(message); }

using ListenerFn = void (*)(void*, void*, const void*, std::uint64_t, std::uint64_t, std::uint64_t);

int Count(const std::string& needle) {
  int n = 0;
  for (const std::string& l : g_lines) n += l.find(needle) != std::string::npos ? 1 : 0;
  return n;
}

class PeerMessage : public ::testing::Test {
 protected:
  void SetUp() override {
    pm::ResetForTest();
    g_sends.clear();
    g_lines.clear();
    g_listenCalls = 0;
    g_listenResult = kListenOk;
    g_listenSymbol = 0;
    g_proxyFunc = nullptr;
    g_fakeAuthority = kAuthority;
    nevr_combat::g_testFeatures = {"echocombat"};
    g_isServer = FALSE;
    context_.assign(kContextBytes, 0);
    netGame_.assign(kNetGameBytes, 0);
    broadcaster_.assign(kBroadcasterBytes, 0);
    flagWord_ = 0;
    Write(context_.data(), kT.contextNetGame, netGame_.data());
    Write(netGame_.data(), kT.netGameBroadcaster, broadcaster_.data());
    Write(netGame_.data(), kT.netGameFlags, &flagWord_);
    g_pGame = context_.data();
    savedListen_ = EchoVR::BroadcasterListen;
    savedSend_ = EchoVR::BroadcasterSend;
    savedBase_ = EchoVR::g_GameBaseAddress;
    EchoVR::BroadcasterListen = &FakeListen;
    EchoVR::BroadcasterSend = &FakeSend;
    SetBootLogSink(&CaptureLog);
  }
  void TearDown() override {
    SetBootLogSink(nullptr);
    EchoVR::BroadcasterListen = savedListen_;
    EchoVR::BroadcasterSend = savedSend_;
    EchoVR::g_GameBaseAddress = savedBase_;
    g_pGame = nullptr;
    g_isServer = FALSE;
    if (image_ != nullptr) VirtualFree(image_, 0, MEM_RELEASE);
    image_ = nullptr;
    pm::ResetForTest();
  }

  static void Write(void* base, std::size_t offset, const void* pointer) {
    std::memcpy(static_cast<std::uint8_t*>(base) + offset, &pointer, sizeof(pointer));
  }

  // A fake game image whose GetAuthorityPeer holds the stock prologue the half validates and then, in place of the rest of the
  // game's function, code that stores g_fakeAuthority through the out pointer and returns it, so a client can have an authority.
  void InstallFakeAuthorityPeer(bool stockPrologue) {
    image_ = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, kFakeImageBytes, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
    ASSERT_NE(image_, nullptr);
    std::uint8_t* fn = image_ + (kT.authorityPeer - kImageBaseVa);
    if (stockPrologue) {
      std::memcpy(fn, pm::kPcAuthorityPeerPrologue.data(), pm::kPcAuthorityPeerPrologue.size());
      // After the prologue (push rbx; sub rsp,20h; mov rax,[rcx+2da0h]; mov rbx,rdx):
      // The game's function loads the flags word through the pointer without a check (peer_message.cpp probes it first); so does this.
      const std::uint8_t loadFlags[] = {0x48, 0x8B, 0x00};  // mov rax,[rax]
      std::memcpy(fn + pm::kPcAuthorityPeerPrologue.size(), loadFlags, sizeof(loadFlags));
      const std::uint8_t tail[] = {0x48, 0xB9};  // mov rcx, imm64 (&g_fakeAuthority)
      std::uint8_t* p = fn + pm::kPcAuthorityPeerPrologue.size() + sizeof(loadFlags);
      std::memcpy(p, tail, sizeof(tail));
      p += sizeof(tail);
      const std::uintptr_t source = reinterpret_cast<std::uintptr_t>(&g_fakeAuthority);
      std::memcpy(p, &source, sizeof(source));
      p += sizeof(source);
      const std::uint8_t rest[] = {0x48, 0x8B, 0x09,        // mov rcx,[rcx]
                                   0x48, 0x89, 0x0B,        // mov [rbx],rcx
                                   0x48, 0x89, 0xD8,        // mov rax,rbx
                                   0x48, 0x83, 0xC4, 0x20,  // add rsp,20h
                                   0x5B,                    // pop rbx
                                   0xC3};                   // ret
      std::memcpy(p, rest, sizeof(rest));
      ASSERT_LT(static_cast<std::size_t>(p - fn) + sizeof(rest), kMachineCodeMax);
    }
    EchoVR::g_GameBaseAddress = reinterpret_cast<CHAR*>(image_);
  }

  void BecomeServer(const std::vector<std::uint64_t>& peers, const std::string& session) {
    g_isServer = TRUE;
    match_.valid = true;
    match_.session = session;
    match_.peers = peers;
    pm::SetMatchViewSource([this] { return match_; });
  }

  // The listener the half registered, called as the game's dispatch calls it.
  void Deliver(const std::string& payload, std::uint64_t sender) {
    ASSERT_NE(g_proxyFunc, nullptr) << "no listener was registered";
    reinterpret_cast<ListenerFn>(g_proxyFunc)(nullptr, nullptr, payload.data(), payload.size(), 0, sender);
  }

  std::vector<std::uint8_t> context_, netGame_, broadcaster_;
  std::uint64_t flagWord_ = 0;
  std::uint8_t* image_ = nullptr;
  pm::MatchView match_;
  EchoVR::BroadcasterListenFunc* savedListen_ = nullptr;
  EchoVR::BroadcasterSendFunc* savedSend_ = nullptr;
  CHAR* savedBase_ = nullptr;
};

pm::Decoded Dec(const std::string& bytes) { return pm::Decode(bytes.data(), bytes.size()); }

}  // namespace

TEST_F(PeerMessage, NothingHappensWithoutANetGameBroadcaster) {
  g_pGame = nullptr;
  pm::Poll(kFirstPollUs);
  EXPECT_EQ(g_listenCalls, 0);
  Write(netGame_.data(), kT.netGameBroadcaster, nullptr);
  g_pGame = context_.data();
  pm::Poll(kFirstPollUs + kPollUs);
  EXPECT_EQ(g_listenCalls, 0) << "a net game with no broadcaster is not listened on";
  EXPECT_TRUE(g_sends.empty());
}

TEST_F(PeerMessage, ListensOnceOnTheMessageSymbolReliably) {
  pm::Poll(kFirstPollUs);
  pm::Poll(kFirstPollUs + kPollUs);
  pm::Poll(kFirstPollUs + kTwoStepsUs);
  EXPECT_EQ(g_listenCalls, 1) << "one registration per broadcaster";
  EXPECT_EQ(g_listenSymbol, pm::kMessageSymbol);
  EXPECT_TRUE(g_listenReliable);
  EXPECT_EQ(Count("listener registered role=client"), 1);
  EXPECT_NE(g_lines[0].find("handle=" + P(kListenOk)), std::string::npos);
}

TEST_F(PeerMessage, APollWithinTheStepIsIgnored) {
  BecomeServer({kPeerA}, "S1");
  pm::Poll(kFirstPollUs);
  ASSERT_EQ(g_sends.size(), kSendsPerGreeting);  // capabilities and ping to peer A
  match_.peers = {kPeerA, kPeerB};
  pm::Poll(kFirstPollUs + kHalfStepUs);
  EXPECT_EQ(g_sends.size(), kSendsPerGreeting) << "inside the 250 ms step nothing is read";
  pm::Poll(kFirstPollUs + kPollUs);
  EXPECT_EQ(g_sends.size(), kTwoGreetings) << "at the step the new peer is greeted";
}

TEST_F(PeerMessage, AFailedListenIsRetriedEveryFiveSecondsAndLoggedOnTheFirstAndEveryTwelfth) {
  g_listenResult = kListenNone;
  std::uint64_t now = kFirstPollUs;
  pm::Poll(now);
  EXPECT_EQ(g_listenCalls, 1);
  pm::Poll(now + kRetryUs - kPollUs);
  EXPECT_EQ(g_listenCalls, 1) << "not retried before the 5 s wait";
  for (std::uint32_t i = 1; i < kListenWarnEvery; ++i) {
    now += kRetryUs;
    pm::Poll(now);
  }
  EXPECT_EQ(g_listenCalls, static_cast<int>(kListenWarnEvery));
  EXPECT_EQ(Count("listener not registered"), 1) << "attempts 2 to 12 write nothing";
  now += kRetryUs;
  pm::Poll(now);
  EXPECT_EQ(g_listenCalls, 1 + static_cast<int>(kListenWarnEvery));
  EXPECT_EQ(Count("listener not registered"), kListenWarnings) << "the first failure and the 13th attempt";
  EXPECT_EQ(Count("listener registered"), 0);
  g_listenResult = kListenOk;
  now += kRetryUs;
  pm::Poll(now);
  EXPECT_EQ(Count("listener registered role=client"), 1);
  bool sawCount = false;
  for (const std::string& l : g_lines) sawCount = sawCount || (l.find("listener registered role=client") != std::string::npos && l.find("failures_before=" + P(1 + kListenWarnEvery)) != std::string::npos);
  EXPECT_TRUE(sawCount) << "the registration says how many attempts failed before it";
}

TEST_F(PeerMessage, TheServerGreetsEachAcceptedPeerOnceWithItsGearAndAPing) {
  BecomeServer({kPeerA, kPeerB}, "S1");
  pm::Poll(kFirstPollUs);
  ASSERT_EQ(g_sends.size(), kTwoGreetings);
  EXPECT_EQ(g_sends[0].peer, kPeerA);
  EXPECT_EQ(g_sends[1].peer, kPeerA);
  EXPECT_EQ(g_sends[kSendsPerGreeting].peer, kPeerB);
  EXPECT_EQ(g_sends[kSendsPerGreeting + 1].peer, kPeerB);
  for (const Send& s : g_sends) {
    EXPECT_EQ(s.symbol, pm::kMessageSymbol);
    EXPECT_EQ(s.dest, ~0ULL) << "reliable";
  }
  const pm::Decoded caps = Dec(g_sends[0].bytes);
  EXPECT_EQ(caps.message.kind, pm::Kind::kCapabilities);
  EXPECT_EQ(caps.message.gear, (std::vector<std::string>{"smoke", "buff", "point"}));
  EXPECT_EQ(Dec(g_sends[1].bytes).message.kind, pm::Kind::kPing);
  EXPECT_NE(Dec(g_sends[1].bytes).message.seq, Dec(g_sends[kSendsPerGreeting + 1].bytes).message.seq) << "each ping has its own sequence";

  g_sends.clear();
  pm::Poll(kFirstPollUs + kPollUs);
  EXPECT_TRUE(g_sends.empty()) << "a greeted peer is not greeted again in the session";
}

TEST_F(PeerMessage, AServerWithoutCombatOffersNoGear) {
  nevr_combat::g_testFeatures = {};
  BecomeServer({kPeerA}, "S1");
  pm::Poll(kFirstPollUs);
  ASSERT_EQ(g_sends.size(), kSendsPerGreeting);
  const pm::Decoded caps = Dec(g_sends[0].bytes);
  EXPECT_EQ(caps.message.kind, pm::Kind::kCapabilities);
  EXPECT_TRUE(caps.message.gear.empty());
}

TEST_F(PeerMessage, ANewSessionGreetsEveryoneAgainAndSaysSo) {
  BecomeServer({kPeerA}, "S1");
  pm::Poll(kFirstPollUs);
  g_sends.clear();
  match_.session = "S2";
  pm::Poll(kFirstPollUs + kPollUs);
  EXPECT_EQ(g_sends.size(), kSendsPerGreeting);
  EXPECT_EQ(Count("session changed from=S1 to=S2 greeted_peers_forgotten=1"), 1);
}

TEST_F(PeerMessage, AServerBetweenSessionsGreetsNobody) {
  BecomeServer({kPeerA}, "");
  pm::Poll(kFirstPollUs);
  EXPECT_TRUE(g_sends.empty());
  match_.valid = false;
  match_.session = "S1";
  pm::Poll(kFirstPollUs + kPollUs);
  EXPECT_TRUE(g_sends.empty()) << "no lobby, no greeting";
}

TEST_F(PeerMessage, TheServerSaysOnceWhetherItsBroadcasterIsTheLobbys) {
  BecomeServer({}, "S1");
  match_.lobbyBroadcaster = reinterpret_cast<std::uintptr_t>(broadcaster_.data());
  pm::Poll(kFirstPollUs);
  pm::Poll(kFirstPollUs + kPollUs);
  EXPECT_EQ(Count("server broadcaster same_as_lobby=yes"), 1);
  pm::ResetForTest();
  g_lines.clear();
  BecomeServer({}, "S1");
  match_.lobbyBroadcaster = reinterpret_cast<std::uintptr_t>(broadcaster_.data()) + 1;
  pm::Poll(kFirstPollUs);
  EXPECT_EQ(Count("server broadcaster same_as_lobby=no"), 1);
}

TEST_F(PeerMessage, ASendThatCannotGoIsLoggedAndNothingIsSent) {
  BecomeServer({kPeerA}, "S1");
  EchoVR::BroadcasterSend = nullptr;
  pm::Poll(kFirstPollUs);
  EXPECT_EQ(Count("not sent role=server peer=" + P(kPeerA)), static_cast<int>(kSendsPerGreeting));
  EXPECT_EQ(Count("reason=no_send_function"), static_cast<int>(kSendsPerGreeting));
  EXPECT_TRUE(g_sends.empty());
}

TEST_F(PeerMessage, TheServerAnswersAPingFromAMatchPeerWithAPongOfTheSameSequence) {
  BecomeServer({kPeerA}, "S1");
  pm::Poll(kFirstPollUs);
  g_sends.clear();
  Deliver(pm::EncodePing(kSeq), kPeerA);
  ASSERT_EQ(g_sends.size(), 1u);
  EXPECT_EQ(g_sends[0].peer, kPeerA);
  const pm::Decoded pong = Dec(g_sends[0].bytes);
  EXPECT_EQ(pong.message.kind, pm::Kind::kPong);
  EXPECT_EQ(pong.message.seq, kSeq);
  EXPECT_EQ(Count("ping received role=server peer=" + P(kPeerA) + " from=match_peer"), 1);
}

TEST_F(PeerMessage, TheServerRefusesAPeerOutsideTheMatchAndACapabilitiesMessage) {
  BecomeServer({kPeerA}, "S1");
  pm::Poll(kFirstPollUs);
  g_sends.clear();
  Deliver(pm::EncodePing(kSeq), kPeerOutside);
  EXPECT_EQ(Count("message refused role=server peer=" + P(kPeerOutside)), 1);
  EXPECT_EQ(Count("reason=wrong_sender"), 1);
  Deliver(pm::EncodeCapabilities({"smoke"}), kPeerA);
  EXPECT_EQ(Count("reason=not_for_server"), 1);
  EXPECT_TRUE(g_sends.empty()) << "a refused message is never answered";
}

TEST_F(PeerMessage, AMessageThatDoesNotDecodeIsRefusedWithItsReason) {
  BecomeServer({kPeerA}, "S1");
  pm::Poll(kFirstPollUs);
  g_sends.clear();
  Deliver(std::string(), kPeerA);
  Deliver("not an envelope", kPeerA);
  EXPECT_EQ(Count("reason=empty"), 1);
  EXPECT_EQ(Count("reason=undecodable"), 1);
  EXPECT_TRUE(g_sends.empty());
}

TEST_F(PeerMessage, RefusalLinesAreBoundedPerWindow) {
  BecomeServer({kPeerA}, "S1");
  pm::Poll(kFirstPollUs);
  for (std::uint32_t i = 0; i < kRefusalBound + kPastRefusalBound; ++i) Deliver("not an envelope", kPeerA);
  EXPECT_EQ(Count("message refused"), static_cast<int>(kRefusalBound));
}

TEST_F(PeerMessage, APeerPastItsAcceptBoundIsDroppedWithoutAReplyAndCountedOnTheNextTakenLine) {
  BecomeServer({kPeerA}, "S1");
  pm::Poll(kFirstPollUs);
  g_sends.clear();
  for (std::uint32_t i = 0; i < kAcceptBound + kPastAcceptBound; ++i) Deliver(pm::EncodePing(kSeq), kPeerA);
  EXPECT_EQ(g_sends.size(), static_cast<std::size_t>(kAcceptBound)) << "past the bound a ping is not answered";
  EXPECT_EQ(Count("ping received"), static_cast<int>(kAcceptBound));
  // Another peer has its own window.
  match_.peers = {kPeerA, kPeerB};
  Deliver(pm::EncodePing(kSeq), kPeerB);
  EXPECT_EQ(g_sends.size(), static_cast<std::size_t>(kAcceptBound) + 1);
}

TEST_F(PeerMessage, AClientTakesNothingWhileItHasNoAuthorityPeer) {
  InstallFakeAuthorityPeer(true);
  g_fakeAuthority = ~0ULL;  // the game's "no server" answer
  pm::Poll(kFirstPollUs);
  Deliver(pm::EncodePing(kSeq), kAuthority);
  EXPECT_EQ(Count("reason=wrong_sender"), 1);
  EXPECT_TRUE(g_sends.empty());
  EXPECT_EQ(Count("authority peer changed"), 0) << "no authority before and after: no change";
}

TEST_F(PeerMessage, AClientThatIsItsOwnAuthorityTakesNothing) {
  InstallFakeAuthorityPeer(true);
  g_fakeAuthority = kPeerSelf;
  pm::Poll(kFirstPollUs);
  EXPECT_EQ(Count("authority peer changed"), 0);
  Deliver(pm::EncodePing(kSeq), kPeerSelf);
  EXPECT_EQ(Count("reason=wrong_sender"), 1);
  EXPECT_TRUE(g_sends.empty());
}

TEST_F(PeerMessage, AClientTakesMessagesFromTheAuthorityPeerOnly) {
  InstallFakeAuthorityPeer(true);
  pm::Poll(kFirstPollUs);
  EXPECT_EQ(Count("authority peer changed from=-1 to=" + P(kAuthority)), 1);
  pm::Poll(kFirstPollUs + kPollUs);
  EXPECT_EQ(Count("authority peer changed"), 1) << "logged on change only";

  Deliver(pm::EncodePing(kSeq), kPeerA);
  EXPECT_EQ(Count("message refused role=client peer=" + P(kPeerA)), 1);
  EXPECT_TRUE(g_sends.empty());

  Deliver(pm::EncodePing(kSeq), kAuthority);
  ASSERT_EQ(g_sends.size(), 1u);
  EXPECT_EQ(g_sends[0].peer, kAuthority);
  EXPECT_EQ(Dec(g_sends[0].bytes).message.kind, pm::Kind::kPong);
  EXPECT_EQ(Count("ping received role=client peer=" + P(kAuthority) + " from=authority"), 1);

  g_fakeAuthority = kPeerA;
  pm::Poll(kFirstPollUs + kTwoStepsUs);
  EXPECT_EQ(Count("authority peer changed from=" + P(kAuthority) + " to=" + P(kPeerA)), 1);
}

TEST_F(PeerMessage, AClientAnswersTheServersCapabilitiesWithAPingAndTimesThePong) {
  InstallFakeAuthorityPeer(true);
  pm::Poll(kFirstPollUs);
  Deliver(pm::EncodeCapabilities({"smoke", "medigun"}), kAuthority);
  EXPECT_EQ(Count("capabilities received role=client peer=" + P(kAuthority) + " from=authority"), 1);
  EXPECT_EQ(Count("gear=smoke,medigun"), 1);
  ASSERT_EQ(g_sends.size(), 1u);
  const pm::Decoded ping = Dec(g_sends[0].bytes);
  ASSERT_EQ(ping.message.kind, pm::Kind::kPing);
  EXPECT_EQ(g_sends[0].peer, kAuthority);

  g_lines.clear();
  Deliver(pm::EncodePong(ping.message.seq), kAuthority);
  EXPECT_EQ(Count("pong received role=client peer=" + P(kAuthority)), 1);
  EXPECT_EQ(Count("no ping of this seq is outstanding"), 0) << "the pong answers the ping that was sent";
  EXPECT_EQ(Count("rtt_ms=-1"), 0);
  Deliver(pm::EncodePong(ping.message.seq), kAuthority);
  EXPECT_EQ(Count("rtt_ms=-1"), 1) << "a second pong of the same sequence has no ping outstanding";
  EXPECT_EQ(Count("no ping of this seq is outstanding"), 1);
}

TEST_F(PeerMessage, AClientWhoseAuthorityLookupHasANonStockPrologueUsesNoneAndTakesNothing) {
  InstallFakeAuthorityPeer(false);
  pm::Poll(kFirstPollUs);
  EXPECT_EQ(Count("authority peer lookup not used"), 1);
  EXPECT_EQ(Count("reason=prologue_mismatch"), 1);
  pm::Poll(kFirstPollUs + kPollUs);
  EXPECT_EQ(Count("authority peer lookup not used"), 1) << "the prologue is checked once";
  Deliver(pm::EncodePing(kSeq), kAuthority);
  EXPECT_EQ(Count("reason=wrong_sender"), 1);
  EXPECT_TRUE(g_sends.empty());
}

TEST_F(PeerMessage, AClientBeforeMultiplayerHasNoAuthorityWhenTheFlagsPointerIsUnset) {
  InstallFakeAuthorityPeer(true);
  Write(netGame_.data(), kT.netGameFlags, nullptr);  // GetAuthorityPeer would dereference it: the half probes first
  pm::Poll(kFirstPollUs);
  EXPECT_EQ(Count("authority peer changed"), 0);
  Deliver(pm::EncodePing(kSeq), kAuthority);
  EXPECT_EQ(Count("reason=wrong_sender"), 1);
}
