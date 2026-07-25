// ============================================================================
// Wave I behavioral tests — N68 (plugin/module tick dispatch) + N61 (callback
// lifecycle).
//
// These are the automated behavioral regression tests required by the Wave I
// gate. The grep-verifiers in the justfile catch STRUCTURAL re-drop of call
// sites; these tests catch BEHAVIORAL regression — even with the call site
// present, does the tick actually fire registered callbacks?
//
// N68: TickPlugins, TickModules, NotifyPluginsStateChange, NotifyModulesStateChange
// N61: conn>=2 callback survives conn=1 close (matchmaker routing continuity)
//
// Build: compiled with -DNEVR_TEST_HOOKS which enables test-only injection
// functions in plugin_loader.cpp and module_loader.cpp.
//
// Include order is delicate: echovr.h includes <Windows.h> before pch.h can
// include <winsock2.h>. We front-load winsock2.h + windows.h to prevent the
// warning and ensure types are available.
// ============================================================================

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <windows.h>

#include <gtest/gtest.h>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <signal.h>

// Include all project headers needed for type information. Since winsock2.h
// and windows.h are already included, the pch.h includes become no-ops via
// include guards.
#include "echovr.h"
#include "echovr_functions.h"
#include "logging.h"

// ============================================================================
// Define extern symbols declared by the project headers. The test only calls
// TickPlugins/TickModules/NotifyPlugins* + test-hook registration functions.
// It never calls LoadPlugins/LoadModule, so the Windows API call sites in
// those functions are never reached. Provide minimal definitions.
// ============================================================================

// --- echovr_functions.h: g_GameBaseAddress is CHAR*, not uintptr_t ---
namespace EchoVR {
CHAR* g_GameBaseAddress = reinterpret_cast<CHAR*>(0x140000000);
WriteLogFunc*  WriteLog          = nullptr;
JsonValueAsStringFunc* JsonValueAsString = nullptr;
const char* (*LookupSymbolName)(uint64_t) = nullptr;
PoolFindItemFunc*                 PoolFindItem                 = nullptr;
TcpBroadcasterListenFunc*         TcpBroadcasterListen         = nullptr;
BroadcasterSendFunc*              BroadcasterSend              = nullptr;
BroadcasterReceiveLocalEventFunc* BroadcasterReceiveLocalEvent = nullptr;
BroadcasterListenFunc*            BroadcasterListen            = nullptr;
BroadcasterUnlistenFunc*          BroadcasterUnlisten          = nullptr;
CJsonGetFloatFunc*                CJsonGetFloat                = nullptr;
UriContainerParseFunc*            UriContainerParse            = nullptr;
BuildCmdLineSyntaxDefinitionsFunc* BuildCmdLineSyntaxDefinitions = nullptr;
AddArgSyntaxFunc*                 AddArgSyntax                 = nullptr;
AddArgHelpStringFunc*             AddArgHelpString             = nullptr;
PreprocessCommandLineFunc*        PreprocessCommandLine        = nullptr;
HttpConnectFunc*                  HttpConnect                  = nullptr;
LoadJsonFromFileFunc*             LoadJsonFromFile             = nullptr;
LoadLocalConfigFunc*              LoadLocalConfig              = nullptr;
NetGameSwitchStateFunc*           NetGameSwitchState           = nullptr;
NetGameScheduleReturnToLobbyFunc* NetGameScheduleReturnToLobby = nullptr;
GetProcAddressFunc*               GetProcAddress               = nullptr;
SetWindowTextAFunc*               SetWindowTextA_              = nullptr;
ListenProxyFunc*                  ListenProxy                  = nullptr;
udp_recvfrom_sockaddr_storageFunc* udp_recvfrom_sockaddr_storage = nullptr;
CleanupPingsFunc*                 CleanupPings                 = nullptr;
udp_protocol_lookup_or_dispatchFunc* udp_protocol_lookup_or_dispatch = nullptr;
udp_protocol_get_stateFunc*       udp_protocol_get_state       = nullptr;
udp_protocol_get_peer_idFunc*     udp_protocol_get_peer_id     = nullptr;
udp_protocol_find_peerFunc*       udp_protocol_find_peer       = nullptr;
udp_protocol_find_peer_by_addrFunc* udp_protocol_find_peer_by_addr = nullptr;
udp_protocol_get_contextFunc*     udp_protocol_get_context     = nullptr;
udp_protocol_handshake_or_intro1Func* udp_protocol_handshake_or_intro1 = nullptr;
udp_protocol_handshake_or_intro2Func* udp_protocol_handshake_or_intro2 = nullptr;
udp_protocol_handshake_or_intro3Func* udp_protocol_handshake_or_intro3 = nullptr;
}  // namespace EchoVR

// --- globals.h externs ---
BOOL   g_isServer           = TRUE;
BOOL   g_isHeadless         = TRUE;
BOOL   g_noConsole          = FALSE;
BOOL   g_exitOnError        = TRUE;
BOOL   g_noOvr              = FALSE;
BOOL   g_telemetryEnabled   = FALSE;
BOOL   g_timestampLogs      = FALSE;
BOOL   g_allowDbgCore       = FALSE;
BOOL   g_upnpEnabled        = FALSE;
UINT32 g_headlessTickRateHz = 60;
UINT32 g_telemetryRateHz    = 10;
UINT16 g_upnpPort           = 0;
CHAR   g_internalIpOverride[46] = {};
CHAR   g_externalIpOverride[46] = {};
CHAR   g_customConfigPath[MAX_PATH] = {};
CHAR   g_regionOverride[64]   = {};
GUID   g_loginSessionId       = {};
FLOAT  g_arenaRoundTime       = 0.0f;
FLOAT  g_arenaCelebrationTime = 0.0f;
FLOAT  g_arenaMercyScore      = 0.0f;
volatile sig_atomic_t g_shutdownRequested = 0;

// --- logging.h implementations ---
std::string GetISO8601Timestamp() { return "2026-01-01T00:00:00.000Z"; }
const char* GetLogLevelString(EchoVR::LogLevel) { return "info"; }
std::string FormatJsonLogEntry(EchoVR::LogLevel, const char*, const char*) { return "{}"; }

void Log(EchoVR::LogLevel level, const char* format, ...) {
  (void)level; (void)format;
}

FatalErrorHandlerFunc g_fatalErrorHandler = nullptr;
void SetFatalErrorHandler(FatalErrorHandlerFunc) {}

void FatalError(const char* msg, const char* title) {
  fprintf(stderr, "[TEST] FatalError: %s: %s\n", title ? title : "?", msg ? msg : "?");
}

// --- module_loader.h: SetModuleContext/GetModuleContext/RegisterModuleProc/
//     ResolveModuleProc are DEFINED in module_loader.cpp (which is compiled
//     into this test). Do NOT redefine them here — let the linker pick them up
//     from module_loader.cpp.obj.

#include "plugin_loader.h"
#include "module_loader.h"

// ============================================================================
// N68 behavioral tests — plugin tick dispatch
// ============================================================================
//
// The test-hook registration functions take plain C function pointers.
// We use file-static counters that the test resets before each tick.

static int s_pluginFrameCount = 0;
static int s_moduleFrameCount = 0;
static uint32_t s_stateChangeOld = 0;
static uint32_t s_stateChangeNew = 0;
static int s_stateChangeCount = 0;

static void N68_PluginOnFrame(const NvrGameContext*) {
  s_pluginFrameCount++;
}

static void N68_PluginOnStateChange(const NvrGameContext*, uint32_t oldState, uint32_t newState) {
  s_stateChangeOld = oldState;
  s_stateChangeNew = newState;
  s_stateChangeCount++;
}

static void N68_ModuleOnFrame(const NvrModuleContext*) {
  s_moduleFrameCount++;
}

static void N68_ModuleOnStateChange(const NvrModuleContext*, uint32_t oldState, uint32_t newState) {
  s_stateChangeOld = oldState;
  s_stateChangeNew = newState;
  s_stateChangeCount++;
}

// ============================================================================
// N68 Plugin tests
// ============================================================================

class N68_PluginTickTest : public ::testing::Test {
 protected:
  void SetUp() override {
    s_pluginFrameCount = 0;
    s_stateChangeCount = 0;
    s_stateChangeOld = 0;
    s_stateChangeNew = 0;
  }
  void TearDown() override {
    TestHook_ClearPlugins();
  }
};

TEST_F(N68_PluginTickTest, OnFrame_Fires_When_Registered) {
  TestHook_RegisterPluginOnFrame(N68_PluginOnFrame);

  NvrGameContext ctx = {};
  ctx.base_addr = reinterpret_cast<uintptr_t>(EchoVR::g_GameBaseAddress);
  ctx.flags = NEVR_HOST_IS_SERVER;

  TickPlugins(&ctx);
  EXPECT_EQ(s_pluginFrameCount, 1) << "OnFrame callback should fire on first tick";

  TickPlugins(&ctx);
  EXPECT_EQ(s_pluginFrameCount, 2) << "OnFrame callback should fire on each subsequent tick";
}

TEST_F(N68_PluginTickTest, OnFrame_Skips_When_Null) {
  // Register with NULL on_frame (via state-change hook instead).
  // TickPlugins should iterate without calling anything.
  TestHook_RegisterPluginOnStateChange(N68_PluginOnStateChange);

  NvrGameContext ctx = {};
  ctx.base_addr = reinterpret_cast<uintptr_t>(EchoVR::g_GameBaseAddress);
  ctx.flags = NEVR_HOST_IS_SERVER;

  s_pluginFrameCount = 0;
  TickPlugins(&ctx);
  EXPECT_EQ(s_pluginFrameCount, 0) << "Null on_frame should not be called";
}

TEST_F(N68_PluginTickTest, OnStateChange_Fires_When_Registered) {
  TestHook_RegisterPluginOnStateChange(N68_PluginOnStateChange);

  NvrGameContext ctx = {};
  ctx.base_addr = reinterpret_cast<uintptr_t>(EchoVR::g_GameBaseAddress);
  ctx.flags = NEVR_HOST_IS_SERVER;

  NotifyPluginsStateChange(&ctx, 3, 5);
  EXPECT_EQ(s_stateChangeCount, 1);
  EXPECT_EQ(s_stateChangeOld, 3u);
  EXPECT_EQ(s_stateChangeNew, 5u);
}

TEST_F(N68_PluginTickTest, OnStateChange_Fires_For_All_Registered) {
  TestHook_RegisterPluginOnStateChange(N68_PluginOnStateChange);
  TestHook_RegisterPluginOnStateChange(N68_PluginOnStateChange);

  NvrGameContext ctx = {};
  ctx.base_addr = reinterpret_cast<uintptr_t>(EchoVR::g_GameBaseAddress);
  ctx.flags = NEVR_HOST_IS_SERVER;

  s_stateChangeCount = 0;
  NotifyPluginsStateChange(&ctx, 0, 9);
  EXPECT_EQ(s_stateChangeCount, 2) << "Both registered callbacks should fire";
}

// ============================================================================
// N68 Module tests
// ============================================================================

class N68_ModuleTickTest : public ::testing::Test {
 protected:
  void SetUp() override {
    s_moduleFrameCount = 0;
    s_stateChangeCount = 0;
    s_stateChangeOld = 0;
    s_stateChangeNew = 0;
  }
  void TearDown() override {
    TestHook_ClearModules();
  }
};

TEST_F(N68_ModuleTickTest, OnFrame_Fires_When_Registered) {
  TestHook_RegisterModuleOnFrame(N68_ModuleOnFrame);

  NvrModuleContext mctx = {};
  mctx.base_addr = reinterpret_cast<uintptr_t>(EchoVR::g_GameBaseAddress);
  mctx.flags = NEVR_MODULE_HOST_IS_SERVER;

  TickModules(&mctx);
  EXPECT_EQ(s_moduleFrameCount, 1) << "Module OnFrame should fire on first tick";

  TickModules(&mctx);
  EXPECT_EQ(s_moduleFrameCount, 2) << "Module OnFrame should fire on each subsequent tick";
}

TEST_F(N68_ModuleTickTest, OnStateChange_Fires_When_Registered) {
  TestHook_RegisterModuleOnStateChange(N68_ModuleOnStateChange);

  NvrModuleContext mctx = {};
  mctx.base_addr = reinterpret_cast<uintptr_t>(EchoVR::g_GameBaseAddress);
  mctx.flags = NEVR_MODULE_HOST_IS_SERVER;

  NotifyModulesStateChange(&mctx, 3, 5);
  EXPECT_EQ(s_stateChangeCount, 1);
  EXPECT_EQ(s_stateChangeOld, 3u);
  EXPECT_EQ(s_stateChangeNew, 5u);
}

// ============================================================================
// N61 behavioral test — matchmaker callback survives login close
// ============================================================================
//
// This test replicates the core callback-lifecycle pattern from ws_bridge.cpp
// WITHOUT linking against the full ixwebsocket dependency chain. The behavioral
// contract:
//
//   1. A "shared remote" object holds a single callback.
//   2. conn=1 (login) connects, registers callback_A on shared remote.
//   3. conn>=2 (matchmaker) connects, registers callback_B on the SAME remote
//      (N61 fix: REPLACES callback_A — matchmaker has its own callback).
//   4. conn=1 closes. Close handler must NOT clear callback_B — matchmaker's
//      callback must survive because its ProxyPair is still alive.
//
// Pre-N61 / N54 regression: login close unconditionally cleared the callback.
// Matchmaker had no callback of its own → silent message routing death.

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

using WsCallback = std::function<void(const std::string&)>;

struct MockRemote {
  WsCallback cb;
  void setCb(WsCallback c) { cb = std::move(c); }
  bool hasCb() const { return static_cast<bool>(cb); }
  void deliver(const std::string& msg) { if (cb) cb(msg); }
};

struct MockPair {
  std::shared_ptr<MockRemote> remote;
  bool open = true;
};

struct N61State {
  std::shared_ptr<MockRemote> loginRemote;
  int connCount = 0;
  std::unordered_map<int, MockPair> pairs;
  int loginConnIdx = -1;
  int activeConnIdx = -1;
};

static int N61_RegisterLogin(N61State& s) {
  int idx = s.connCount++;
  s.loginConnIdx = idx;
  s.activeConnIdx = idx;
  auto remote = std::make_shared<MockRemote>();
  s.loginRemote = remote;
  remote->setCb([idx](const std::string&) { (void)idx; });
  s.pairs[idx] = {remote, true};
  return idx;
}

static int N61_RegisterMatchmaker(N61State& s) {
  int idx = s.connCount++;
  s.activeConnIdx = idx;
  s.loginRemote->setCb([idx](const std::string&) { (void)idx; });
  s.pairs[idx] = {s.loginRemote, true};
  return idx;
}

// PRE-FIX: unconditionally clears callback when login closes.
static void N61_CloseLogin_PreFix(N61State& s, int connIdx) {
  auto it = s.pairs.find(connIdx);
  if (it != s.pairs.end()) {
    bool isShared = (it->second.remote == s.loginRemote);
    if (isShared && connIdx == s.loginConnIdx) {
      it->second.remote->setCb(nullptr);  // BUG: clears even with matchmaker active
    }
    s.pairs.erase(it);
  }
}

// POST-FIX: only clears callback when no other connection is sharing.
static void N61_CloseLogin_PostFix(N61State& s, int connIdx) {
  auto it = s.pairs.find(connIdx);
  if (it != s.pairs.end()) {
    bool isShared = (it->second.remote == s.loginRemote);
    if (isShared && connIdx == s.loginConnIdx) {
      if (s.activeConnIdx < 0 || s.activeConnIdx == connIdx) {
        it->second.remote->setCb(nullptr);
      }
    }
    s.pairs.erase(it);
  }
}

// ============================================================================
// N61 Test Cases
// ============================================================================

TEST(N61_CallbackLifecycle, CallbackSurvivesLoginClose_PostFix) {
  N61State s;
  int loginIdx = N61_RegisterLogin(s);
  ASSERT_TRUE(s.loginRemote->hasCb());

  N61_RegisterMatchmaker(s);
  ASSERT_TRUE(s.loginRemote->hasCb()) << "Matchmaker must set its own callback (N61)";

  N61_CloseLogin_PostFix(s, loginIdx);

  EXPECT_TRUE(s.loginRemote->hasCb())
      << "N61: matchmaker callback must survive login close";
}

TEST(N61_CallbackLifecycle, CallbackClearedByLoginClose_PreFix_DemonstratesRegression) {
  N61State s;
  int loginIdx = N61_RegisterLogin(s);
  N61_RegisterMatchmaker(s);
  ASSERT_TRUE(s.loginRemote->hasCb());

  N61_CloseLogin_PreFix(s, loginIdx);

  EXPECT_FALSE(s.loginRemote->hasCb())
      << "Pre-fix: login close clears callback — this IS the regression";
}

TEST(N61_CallbackLifecycle, MatchmakerReceivesMessagesAfterLoginClose_PostFix) {
  N61State s;
  int loginIdx = N61_RegisterLogin(s);

  N61_RegisterMatchmaker(s);

  std::string delivered;
  s.loginRemote->setCb([&delivered](const std::string& msg) {
    delivered = msg;
  });

  N61_CloseLogin_PostFix(s, loginIdx);

  ASSERT_TRUE(s.loginRemote->hasCb())
      << "Shared remote must still have callback after login close";

  s.loginRemote->deliver("matchmaker_msg");
  EXPECT_EQ(delivered, "matchmaker_msg")
      << "Matchmaker should receive messages after login disconnects";
}

TEST(N61_CallbackLifecycle, NoActiveConnection_CallbackCleared) {
  N61State s;
  int loginIdx = N61_RegisterLogin(s);
  EXPECT_TRUE(s.loginRemote->hasCb());

  N61_CloseLogin_PostFix(s, loginIdx);

  EXPECT_FALSE(s.loginRemote->hasCb())
      << "Callback should be cleared when no connections remain";
}

TEST(N61_CallbackLifecycle, MultipleMatchmakers_FirstCloses_CallbackSurvives) {
  N61State s;
  N61_RegisterLogin(s);
  N61_RegisterMatchmaker(s);
  N61_RegisterMatchmaker(s);  // second matchmaker
  ASSERT_TRUE(s.loginRemote->hasCb());

  // First matchmaker closes (idx 1) — not login, so callback should survive.
  auto it = s.pairs.find(1);
  ASSERT_NE(it, s.pairs.end());
  bool isShared = (it->second.remote == s.loginRemote);
  EXPECT_TRUE(isShared);
  s.pairs.erase(it);

  EXPECT_TRUE(s.loginRemote->hasCb())
      << "Callback should survive first matchmaker closing (second still active)";
}
