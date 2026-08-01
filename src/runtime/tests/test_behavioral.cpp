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
#include "abi/echovr.h"
#include "abi/echovr_functions.h"
#include "core/logging.h"
#include "runtime/hook/hook_guard.h"
#include "core/system_info.h"

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

// N120. ServerFatal lives in crash_recovery.cpp, which this target does not
// compile (it drags in the VEH, the ExitProcess hooks and the whole crash path).
// Record the call instead of terminating, so the seam that makes the link work
// also makes the new behaviour observable: a test can assert that a condition
// DID reach ServerFatal, which a void stub could not.
int g_serverFatalCalls = 0;
std::string g_lastServerFatal;
void ServerFatal(const CHAR* format, ...) {
  char buf[1024];
  va_list args;
  va_start(args, format);
  vsnprintf(buf, sizeof(buf), format, args);
  va_end(args);
  g_serverFatalCalls++;
  g_lastServerFatal = buf;
  fprintf(stderr, "[TEST] ServerFatal: %s\n", buf);
}

// --- config.h extern (used by ws_bridge.cpp) ---
void* g_earlyConfigPtr = nullptr;

// --- service_config.h accessor (N133 S4a, used by ws_bridge.cpp login injection) ---
// service_config.cpp is not linked here (it drags the config.yaml singleton +
// file discovery). A null return == "key absent", so ws_bridge's guard skips the
// URL-credential path exactly as an absent identity.discord_id/auth.password
// would. The N61 tests drive the Close handler, not login injection.
const char* NevrCfgGetFlat(const char* /*flatKey*/) { return nullptr; }

// symbol_corpus.cpp provides the real LookupSymbolName function (670-entry table)
// and FormatSymbolId. It is compiled into this test target.

// ============================================================================
// Production headers (included AFTER all stubs are defined).
// ============================================================================

#include "runtime/ext/plugin_loader.h"
#include "runtime/ext/module_loader.h"
#include "runtime/compat/ws_bridge.h"
#include "runtime/hook/symbol_corpus.h"
#include "runtime/hook/addresses.h"

// WOULD-FAIL-IF (N68): delete TickPlugins iteration loop in plugin_loader.cpp.
// WOULD-FAIL-IF (N68-module): delete TickModules loop in module_loader.cpp.
// WOULD-FAIL-IF (N68-state): delete NotifyPluginsStateChange/NotifyModulesStateChange loops.
// WOULD-FAIL-IF (N61): delete matchmaker callback registration at conn>=2 in ws_bridge.cpp.
// WOULD-FAIL-IF (N60): delete mutex-unlock before stop() in ws_bridge Close handler.
// WOULD-FAIL-IF (N66): delete maxLen<=0||buf==nullptr guard in symbol_corpus.cpp FormatSymbolId.
// WOULD-FAIL-IF (N65): delete HEADLESS_GATE_TABLE entry or change HEADLESS_GATE_COUNT static_assert.
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
// N133 S5 — module API version gate. The loader refuses a module whose reported
// version exceeds the host's (module_loader.cpp LoadModule -> FatalError, N120).
// LoadModule itself is not unit-testable (real LoadLibraryExA + a process-killing
// FatalError), so the refusal RULE is factored into the header-only predicate
// NvrModuleApiVersionSupported, which the loader and these tests share. Verifying
// the predicate verifies the mismatch decision without spawning a process.
// ============================================================================

TEST(N133_ModuleApiVersion, HostVersionIsTwo) {
  // The bump this change makes: implicit v1 (pre-versioning) -> explicit v2 (adds
  // config_get to NvrModuleContext).
  EXPECT_EQ(static_cast<uint32_t>(NEVR_MODULE_API_VERSION), 2u);
}

TEST(N133_ModuleApiVersion, AcceptsEqualAndOlder) {
  // v1 (absent export) and v2 (our modules) both load — appended context fields an
  // older module does not know about are simply unused.
  EXPECT_TRUE(NvrModuleApiVersionSupported(1u));
  EXPECT_TRUE(NvrModuleApiVersionSupported(2u));
}

TEST(N133_ModuleApiVersion, RefusesNewer) {
  // A module built against a newer ABI expects context fields this host does not
  // set -> refused (the loader turns this false into a FatalError).
  EXPECT_FALSE(NvrModuleApiVersionSupported(3u));
  EXPECT_FALSE(NvrModuleApiVersionSupported(999u));
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

// WOULD-FAIL-IF: delete the mutex-unlock before stop() in the production
// Close handler — g_pairsMutex would still be held, try_lock would fail.
TEST_F(N61_WsBridgeTest, N60_StopCalledOutsideMutex) {
  auto remote = MockWsHandle::Create();
  auto gameWs = MockWsHandle::Create();
  void* gameRaw = TestHook_N61_RegisterLogin(remote.handle, gameWs.handle);
  ASSERT_NE(gameRaw, nullptr);

  // Pre-condition: mutex is free.
  EXPECT_TRUE(TestHook_N60_IsMutexFree());

  // Drive the real Close handler — locks, snapshots, unlocks, then calls stop().
  TestHook_N61_SimulateCloseAndCheckCleared(gameRaw);

  // Post-condition: mutex is free. If stop() were inside the lock, this fails.
  EXPECT_TRUE(TestHook_N60_IsMutexFree())
      << "N60: mutex must be free after Close — stop() was called inside the lock";
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

// ============================================================================
// N84 — HookGuard: detect a second detour on an address gamepatches owns
//
// Production-linked: these drive the REAL HookGuard from hook_guard.cpp, not a
// model of it. The guard exists because gamepatches links extern/minhook while
// plugins link the vcpkg minhook port — separate static copies with separate
// private hook tables, so neither library can report that the other already
// owns an address. A third-party plugin is a DLL we never compile, so the only
// detection that reaches it is byte-level: snapshot after our install, re-check
// after each plugin loads.
//
// A page of executable-ish memory stands in for a hooked function. That is
// exactly what the guard reads in production — it never interprets the bytes,
// only compares them.
// ============================================================================

namespace {

// VirtualAlloc'd scratch that outlives each test body.
struct GuardScratch {
    void* page;
    GuardScratch() {
        page = VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    }
    ~GuardScratch() {
        if (page) VirtualFree(page, 0, MEM_RELEASE);
    }
};

}  // namespace

TEST(N84_HookGuard, UnchangedBytes_NoMismatch) {
    HookGuard::ResetForTest();
    GuardScratch s;
    ASSERT_NE(s.page, nullptr);
    memset(s.page, 0x90, 32);  // NOPs stand in for an untouched prologue

    HookGuard::Record(s.page, "N84_unchanged");
    EXPECT_EQ(HookGuard::VerifyAll("test:unchanged"), 0)
        << "guard reported a mismatch on bytes nothing modified";
}

TEST(N84_HookGuard, OverwrittenBytes_Detected) {
    HookGuard::ResetForTest();
    GuardScratch s;
    ASSERT_NE(s.page, nullptr);
    memset(s.page, 0x90, 32);

    HookGuard::Record(s.page, "N84_overwritten");
    ASSERT_EQ(HookGuard::VerifyAll("test:baseline"), 0)
        << "precondition: freshly recorded bytes must match";

    // Simulate a second MinHook instance writing its own JMP rel32 over ours.
    unsigned char* p = static_cast<unsigned char*>(s.page);
    p[0] = 0xE9;  // JMP rel32
    p[1] = 0x11;
    p[2] = 0x22;
    p[3] = 0x33;
    p[4] = 0x44;

    EXPECT_EQ(HookGuard::VerifyAll("test:overwritten"), 1)
        << "guard did NOT detect a foreign detour overwriting a recorded address";
}

TEST(N84_HookGuard, NullTarget_Ignored) {
    HookGuard::ResetForTest();
    const int before = HookGuard::RecordedCount();
    HookGuard::Record(nullptr, "N84_null");
    EXPECT_EQ(HookGuard::RecordedCount(), before)
        << "null target was recorded; a bad call site would occupy a guard slot";
}

TEST(N84_HookGuard, UnreadableTarget_Ignored) {
    HookGuard::ResetForTest();
    const int before = HookGuard::RecordedCount();
    // Reserved-but-not-committed: readable-looking pointer, unreadable memory.
    void* reserved = VirtualAlloc(nullptr, 4096, MEM_RESERVE, PAGE_NOACCESS);
    ASSERT_NE(reserved, nullptr);
    HookGuard::Record(reserved, "N84_unreadable");
    EXPECT_EQ(HookGuard::RecordedCount(), before)
        << "uncommitted memory was recorded; VerifyAll would fault reading it";
    VirtualFree(reserved, 0, MEM_RELEASE);
}

// WOULD-FAIL-IF (N84): delete the memcmp in HookGuard::VerifyAll (hook_guard.cpp)
//   -> OverwrittenBytes_Detected fails: a foreign detour goes unreported.
// WOULD-FAIL-IF (N84-record): delete the Readable() check in HookGuard::Record
//   -> UnreadableTarget_Ignored fails, and production VerifyAll faults on the
//      uncommitted page instead of skipping it.
// WOULD-FAIL-IF (N84-wiring): delete HookGuard::VerifyAll(filename) from
//   plugin_loader.cpp -> not caught here (call-site wiring), caught by the
//   `just verify` grep sensor instead.


// ============================================================================
// N112 — SystemInfo must MEASURE, not invent.
//
// The login payload's system_info block was string literals: "cpu":"Wine",
// 4 physical cores, 8 logical, 16384 MB. It read like telemetry and was not.
// These tests exist because a plausible constant and a real reading are
// indistinguishable downstream — the only way to tell them apart is here,
// where we can check the value against the machine actually running the test.
// ============================================================================

TEST(SystemInfo, ReportsRealCpuAndMemory) {
    const SystemInfo::Host& h = SystemInfo::Get();

    EXPECT_GT(h.logical_cores, 0u) << "logical core count was never measured";
    EXPECT_GT(h.memory_total_mb, 0u) << "physical memory was never measured";
    EXPECT_LE(h.memory_used_mb, h.memory_total_mb) << "used exceeds total — derivation is wrong";

    // The old fabricated tuple, guarded as a set. Any single value could
    // legitimately match on some machine; all of them matching means the
    // literals came back.
    const bool all_old_literals = (h.physical_cores == 4 && h.logical_cores == 8 &&
                                   h.memory_total_mb == 16384 && h.memory_used_mb == 8192);
    EXPECT_FALSE(all_old_literals)
        << "every field equals the pre-N112 hardcoded tuple (4/8/16384/8192) — "
           "the fabricated constants are back";

    // physical <= logical whenever both are known. 0 means "could not
    // determine", which is a permitted answer and deliberately not an error.
    if (h.physical_cores > 0) {
        EXPECT_LE(h.physical_cores, h.logical_cores)
            << "more physical cores than logical — the relation table was misparsed";
    }
    std::printf("[system_info] cpu='%s' cores=%u/%u mem=%llu/%llu MB wine='%s' host='%s' build=%u\n",
                h.cpu_brand.c_str(), h.physical_cores, h.logical_cores,
                (unsigned long long)h.memory_used_mb, (unsigned long long)h.memory_total_mb,
                h.wine_version.c_str(), h.wine_host_os.c_str(), h.os_build);
}

TEST(SystemInfo, WineDetectionIsTheVersionString) {
    const SystemInfo::Host& h = SystemInfo::Get();
    // IsWine() must be exactly "we got a version from ntdll", with no second
    // source of truth that could disagree with the string we transmit.
    EXPECT_EQ(h.IsWine(), !h.wine_version.empty());
}

TEST(SystemInfo, IsCachedNotRemeasured) {
    // Callers may hit this on a login path; the probes (CPUID,
    // GetLogicalProcessorInformation) are not free. Same object every call.
    EXPECT_EQ(&SystemInfo::Get(), &SystemInfo::Get());
}

// WOULD-FAIL-IF (N112): restore the literals in ws_bridge.cpp's system_info
//   block -> not caught here (that is a format string, not a value this test
//   can reach). ReportsRealCpuAndMemory catches the case where SystemInfo
//   itself starts returning the old tuple; the `just verify` sensor catches
//   the format string.
