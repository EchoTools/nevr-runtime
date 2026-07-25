// ============================================================================
// Wave I behavioral tests
// N68: plugin/module tick dispatch (production-linked via NEVR_TEST_HOOKS)
// N61: ws_bridge callback lifecycle (production-linked via NEVR_TEST_HOOKS)
// N66: FormatSymbolId guard logic (production-linked via symbol_corpus.cpp)
//
// Build: compiled with -DNEVR_TEST_HOOKS. Links plugin_loader.cpp,
// module_loader.cpp, ws_bridge.cpp, and symbol_corpus.cpp directly.
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

// Project headers for type info. winsock2/windows already included above,
// so pch.h includes become no-ops via include guards.
#include "echovr.h"
#include "echovr_functions.h"
#include "logging.h"

// ============================================================================
// Stubs for extern symbols declared by project headers but not provided by
// any translation unit linked into this test.
// ============================================================================

// --- echovr_functions.h function pointers (all null — never called by tests) ---
namespace EchoVR {
CHAR* g_GameBaseAddress = reinterpret_cast<CHAR*>(0x140000000);
WriteLogFunc*  WriteLog          = nullptr;
JsonValueAsStringFunc* JsonValueAsString = nullptr;
PoolFindItemFunc*   PoolFindItem   = nullptr;
TcpBroadcasterListenFunc* TcpBroadcasterListen = nullptr;
BroadcasterSendFunc* BroadcasterSend = nullptr;
BroadcasterReceiveLocalEventFunc* BroadcasterReceiveLocalEvent = nullptr;
BroadcasterListenFunc* BroadcasterListen = nullptr;
BroadcasterUnlistenFunc* BroadcasterUnlisten = nullptr;
CJsonGetFloatFunc* CJsonGetFloat = nullptr;
UriContainerParseFunc* UriContainerParse = nullptr;
BuildCmdLineSyntaxDefinitionsFunc* BuildCmdLineSyntaxDefinitions = nullptr;
AddArgSyntaxFunc* AddArgSyntax = nullptr;
AddArgHelpStringFunc* AddArgHelpString = nullptr;
PreprocessCommandLineFunc* PreprocessCommandLine = nullptr;
HttpConnectFunc* HttpConnect = nullptr;
LoadJsonFromFileFunc* LoadJsonFromFile = nullptr;
LoadLocalConfigFunc* LoadLocalConfig = nullptr;
NetGameSwitchStateFunc* NetGameSwitchState = nullptr;
NetGameScheduleReturnToLobbyFunc* NetGameScheduleReturnToLobby = nullptr;
GetProcAddressFunc* GetProcAddress = nullptr;
SetWindowTextAFunc* SetWindowTextA_ = nullptr;
ListenProxyFunc* ListenProxy = nullptr;
udp_recvfrom_sockaddr_storageFunc* udp_recvfrom_sockaddr_storage = nullptr;
CleanupPingsFunc* CleanupPings = nullptr;
udp_protocol_lookup_or_dispatchFunc* udp_protocol_lookup_or_dispatch = nullptr;
udp_protocol_get_stateFunc* udp_protocol_get_state = nullptr;
udp_protocol_get_peer_idFunc* udp_protocol_get_peer_id = nullptr;
udp_protocol_find_peerFunc* udp_protocol_find_peer = nullptr;
udp_protocol_find_peer_by_addrFunc* udp_protocol_find_peer_by_addr = nullptr;
udp_protocol_get_contextFunc* udp_protocol_get_context = nullptr;
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

// --- logging.h implementations (not linking logging.cpp to avoid nlohmann-json) ---
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

// --- config.h extern (used by ws_bridge.cpp) ---
void* g_earlyConfigPtr = nullptr;

// symbol_corpus.cpp provides the real LookupSymbolName function (670-entry table)
// and FormatSymbolId. It is compiled into this test target.

// ============================================================================
// Production headers (included AFTER all stubs are defined).
// ============================================================================

#include "plugin_loader.h"
#include "module_loader.h"
#include "ws_bridge.h"
#include "symbol_corpus.h"
#include "patch_addresses.h"

// ============================================================================
// N68 behavioral tests — plugin tick dispatch
// ============================================================================

static int s_pluginFrameCount = 0;
static int s_moduleFrameCount = 0;
static uint32_t s_stateChangeOld = 0;
static uint32_t s_stateChangeNew = 0;
static int s_stateChangeCount = 0;

static void N68_PluginOnFrame(const NvrGameContext*) { s_pluginFrameCount++; }
static void N68_PluginOnStateChange(const NvrGameContext*, uint32_t oldState, uint32_t newState) {
  s_stateChangeOld = oldState; s_stateChangeNew = newState; s_stateChangeCount++;
}
static void N68_ModuleOnFrame(const NvrModuleContext*) { s_moduleFrameCount++; }
static void N68_ModuleOnStateChange(const NvrModuleContext*, uint32_t oldState, uint32_t newState) {
  s_stateChangeOld = oldState; s_stateChangeNew = newState; s_stateChangeCount++;
}

class N68_PluginTickTest : public ::testing::Test {
 protected:
  void SetUp() override { s_pluginFrameCount = 0; s_stateChangeCount = 0; s_stateChangeOld = s_stateChangeNew = 0; }
  void TearDown() override { TestHook_ClearPlugins(); }
};

TEST_F(N68_PluginTickTest, OnFrame_Fires_When_Registered) {
  TestHook_RegisterPluginOnFrame(N68_PluginOnFrame);
  NvrGameContext ctx = {};
  ctx.base_addr = reinterpret_cast<uintptr_t>(EchoVR::g_GameBaseAddress);
  ctx.flags = NEVR_HOST_IS_SERVER;
  TickPlugins(&ctx);
  EXPECT_EQ(s_pluginFrameCount, 1);
  TickPlugins(&ctx);
  EXPECT_EQ(s_pluginFrameCount, 2);
}

TEST_F(N68_PluginTickTest, OnFrame_Skips_When_Null) {
  TestHook_RegisterPluginOnStateChange(N68_PluginOnStateChange);
  NvrGameContext ctx = {};
  ctx.base_addr = reinterpret_cast<uintptr_t>(EchoVR::g_GameBaseAddress);
  ctx.flags = NEVR_HOST_IS_SERVER;
  s_pluginFrameCount = 0;
  TickPlugins(&ctx);
  EXPECT_EQ(s_pluginFrameCount, 0);
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
  EXPECT_EQ(s_stateChangeCount, 2);
}

class N68_ModuleTickTest : public ::testing::Test {
 protected:
  void SetUp() override { s_moduleFrameCount = 0; s_stateChangeCount = 0; s_stateChangeOld = s_stateChangeNew = 0; }
  void TearDown() override { TestHook_ClearModules(); }
};

TEST_F(N68_ModuleTickTest, OnFrame_Fires_When_Registered) {
  TestHook_RegisterModuleOnFrame(N68_ModuleOnFrame);
  NvrModuleContext mctx = {};
  mctx.base_addr = reinterpret_cast<uintptr_t>(EchoVR::g_GameBaseAddress);
  mctx.flags = NEVR_MODULE_HOST_IS_SERVER;
  TickModules(&mctx);
  EXPECT_EQ(s_moduleFrameCount, 1);
  TickModules(&mctx);
  EXPECT_EQ(s_moduleFrameCount, 2);
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
// N61 behavioral tests — production-linked via ws_bridge.cpp test hooks
// ============================================================================
//
// The test creates real ix::WebSocket objects (never connected — serve as
// opaque handles) and drives the production Close handler through test hooks.
// This is the N68 pattern applied to ws_bridge: the production code IS the
// code under test.
//
// N61 regression: conn>=2 (matchmaker) registers its own callback on the
// shared remote (N61 fix). When login closes, the Close handler must NOT
// clear that callback — clearing kills matchmaker routing.

#include <memory>

// Wrap raw handles in RAII to ensure cleanup.
struct MockWsHandle {
  void* handle = nullptr;
  ~MockWsHandle() { if (handle) TestHook_N61_DestroyMockWs(handle); }
  void* raw() { return TestHook_N61_GetRawWsPtr(handle); }
  static MockWsHandle Create() { MockWsHandle h; h.handle = TestHook_N61_CreateMockWs(); return h; }
};

class N61_WsBridgeTest : public ::testing::Test {
 protected:
  void SetUp() override {
    TestHook_N61_ResetState();
  }
  void TearDown() override {
    TestHook_N61_ResetState();
  }
};

TEST_F(N61_WsBridgeTest, CallbackSurvivesLoginClose) {
  // Scenario:
  //   1. Login connects → callback_A on shared remote.
  //   2. Matchmaker connects → callback_B on shared remote (N61 fix).
  //   3. Login closes.
  // EXPECTED: shared remote callback survives (matchmaker routing intact).

  auto remote = MockWsHandle::Create();
  auto loginWs = MockWsHandle::Create();
  auto matchWs = MockWsHandle::Create();

  void* loginRaw = TestHook_N61_RegisterLogin(remote.handle, loginWs.handle);
  ASSERT_NE(loginRaw, nullptr);

  bool matchFired = false;
  void* matchRaw = TestHook_N61_RegisterMatchmaker(matchWs.handle, &matchFired);
  ASSERT_NE(matchRaw, nullptr);

  // Verify setup: shared remote has a callback.
  EXPECT_TRUE(TestHook_N61_HasActiveCallback());

  // Close login (this drives the REAL production Close handler).
  bool callbackCleared = TestHook_N61_SimulateCloseAndCheckCleared(loginRaw);

  EXPECT_FALSE(callbackCleared)
      << "N61: production Close handler must NOT clear shared callback "
      << "when a matchmaker connection is still active";
}

TEST_F(N61_WsBridgeTest, CallbackClearedWhenNoMatchmaker) {
  // When login closes and NO matchmaker is sharing, the callback SHOULD be
  // cleared (prevents UAF on freed ProxyPair — the N54 fix).

  auto remote = MockWsHandle::Create();
  auto loginWs = MockWsHandle::Create();

  void* loginRaw = TestHook_N61_RegisterLogin(remote.handle, loginWs.handle);
  ASSERT_NE(loginRaw, nullptr);

  EXPECT_TRUE(TestHook_N61_HasActiveCallback());

  bool callbackCleared = TestHook_N61_SimulateCloseAndCheckCleared(loginRaw);

  EXPECT_TRUE(callbackCleared)
      << "Callback should be cleared when no connections remain (UAF prevention)";
}

// ============================================================================
// N66 behavioral tests — FormatSymbolId guard logic (production-linked)
// ============================================================================
// These call the REAL EchoVR::FormatSymbolId from symbol_corpus.cpp, which is
// compiled into this test target. The guard at the top of the function
// (maxLen <= 0 || buf == nullptr) is the code under test.
// ============================================================================

TEST(N66_FormatSymbolId, NegativeMaxLen_ReturnsZero) {
  char buf[64];
  int result = EchoVR::FormatSymbolId(buf, -1, 0x1234);
  EXPECT_EQ(result, 0) << "Negative maxLen must return 0 (guard prevents buffer overflow)";
}

TEST(N66_FormatSymbolId, ZeroMaxLen_ReturnsZero) {
  char buf[64];
  int result = EchoVR::FormatSymbolId(buf, 0, 0x1234);
  EXPECT_EQ(result, 0) << "Zero maxLen must return 0";
}

TEST(N66_FormatSymbolId, NullBuffer_ReturnsZero) {
  int result = EchoVR::FormatSymbolId(nullptr, 64, 0x1234);
  EXPECT_EQ(result, 0) << "Null buffer must return 0 (prevents null deref)";
}

TEST(N66_FormatSymbolId, NullBufferAndNegativeMaxLen_ReturnsZero) {
  int result = EchoVR::FormatSymbolId(nullptr, -5, 0x1234);
  EXPECT_EQ(result, 0) << "Both guard conditions met — must return 0";
}

TEST(N66_FormatSymbolId, ValidInput_WritesHexString) {
  char buf[64] = {};
  int result = EchoVR::FormatSymbolId(buf, sizeof(buf), 0xABCD1234);
  EXPECT_GT(result, 0) << "Valid input must write output";
  EXPECT_GT(result, 2);  // at least "0x" prefix
  // The hash 0xABCD1234 is not in the symbol corpus, so output is hex.
  // Just verify it's non-empty and null-terminated.
  EXPECT_NE(buf[0], '\0') << "Buffer must be written";
  EXPECT_EQ(buf[result], '\0') << "snprintf null-terminated at result position";
}

TEST(N66_FormatSymbolId, ValidInput_DoesNotOverflow) {
  char buf[8] = {};  // small buffer
  int result = EchoVR::FormatSymbolId(buf, sizeof(buf), 0xABCD1234567890ABULL);
  // snprintf truncates — result is what WOULD have been written.
  // The guard ensures we don't pass a negative/zero maxLen.
  // Verify we didn't write past the buffer.
  EXPECT_GT(result, 0);
  // buf[0..6] written, buf[7] is null terminator.
  EXPECT_EQ(buf[sizeof(buf) - 1], '\0') << "Buffer must be null-terminated within bounds";
}

// ============================================================================
// N65 behavioral verification — gate count from production table
// ============================================================================
// mode_patches.cpp iterates HEADLESS_GATE_TABLE to install gates. The table is
// the single source of truth — gate install CANNOT drift from it. The test
// reads HEADLESS_GATE_COUNT which follows mechanically.
// ============================================================================

TEST(N65_GateCount, DerivedFromProductionTable) {
  using namespace PatchAddresses;
  EXPECT_EQ(HEADLESS_GATE_COUNT, 5)
      << "Gate count must match the 5 entries in HEADLESS_GATE_TABLE";
  // Verify table entries are distinct and have valid metadata.
  for (int i = 0; i < HEADLESS_GATE_COUNT; i++) {
    EXPECT_TRUE(HEADLESS_GATE_TABLE[i].expected_opcode == 0x74 ||
                HEADLESS_GATE_TABLE[i].expected_opcode == 0x75)
        << "Gate " << i << " has invalid expected opcode";
    EXPECT_NE(HEADLESS_GATE_TABLE[i].description, nullptr)
        << "Gate " << i << " has null description";
    for (int j = i + 1; j < HEADLESS_GATE_COUNT; j++) {
      EXPECT_NE(HEADLESS_GATE_TABLE[i].rva, HEADLESS_GATE_TABLE[j].rva)
          << "Gate RVAs must be distinct";
    }
  }
}

TEST(N65_GateCount, AllGatesInCodeRange) {
  using namespace PatchAddresses;
  for (int i = 0; i < HEADLESS_GATE_COUNT; i++) {
    EXPECT_GT(HEADLESS_GATE_TABLE[i].rva, 0x100000u)
        << "Gate " << i << " RVA below .text section";
    EXPECT_LT(HEADLESS_GATE_TABLE[i].rva, 0x2231000u)
        << "Gate " << i << " RVA above image extent";
  }
}
