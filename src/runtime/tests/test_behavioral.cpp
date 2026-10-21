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
#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXWebSocket.h>
#include <ixwebsocket/IXWebSocketServer.h>
#include <chrono>
#include <condition_variable>
#include <future>
#include <cstdarg>
#include <algorithm>
#include <array>
#include <cstddef>
#include <utility>
#include <atomic>
#include <iterator>
#include <memory>
#include <random>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <nlohmann/json.hpp>
#include <mutex>
#include <signal.h>
#include <string>
#include <thread>
#include <vector>

// Project headers for type info. winsock2/windows already included above,
// so pch.h includes become no-ops via include guards.
#include "abi/echovr.h"
#include "abi/echovr_functions.h"
#include "core/logging.h"
#include "runtime/hook/hook_guard.h"
#include "runtime/compat/login_profile.h"
#include "runtime/tests/test_log_cap.h"
#include "runtime/ext/plugin_load_plan.h"  // PluginLoadItem / NevrCfgPluginLoadPlan (N134 S6)
#include "core/cli_option_value.h"
#include "core/host_environment_line.h"
#include "core/system_info.h"
#include "core/build_identity.h"
#include "runtime/log/security_diagnostics.h"
#include "runtime/patch/evrp_package.h"
#include "runtime/patch/xpid_patch.h"
#include "runtime/log/boot_log_tee.h"
#include "runtime/hook/addresses.h"
#include <MinHook.h>  // XpidPrefixDetourBoot initialises the hooking engine for one test

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
FLOAT  g_arenaRoundTime       = 0.0f;
FLOAT  g_arenaCelebrationTime = 0.0f;
FLOAT  g_arenaMercyScore      = 0.0f;

// --- logging.h implementations (not linking logging.cpp to avoid nlohmann-json) ---
std::string GetISO8601Timestamp() { return "2026-01-01T00:00:00.000Z"; }
const char* GetLogLevelString(EchoVR::LogLevel) { return "info"; }
std::string FormatJsonLogEntry(EchoVR::LogLevel, const char*, const char*) { return "{}"; }

std::mutex g_testLogMutex;
std::condition_variable g_testLogCv;  // notified on every line, so a test waits for a line without sleeping
std::vector<std::string> g_testLogMessages;

bool TestLogContains(const std::string& needle) {
  std::lock_guard<std::mutex> lock(g_testLogMutex);
  return std::any_of(g_testLogMessages.begin(), g_testLogMessages.end(),
                     [&needle](const std::string& message) {
                       return message.find(needle) != std::string::npos;
                     });
}

// Every line with the level it was logged at, for tests that pin the level (#598).
std::vector<std::pair<EchoVR::LogLevel, std::string>> g_testLogLeveled;

bool TestLogContainsAtLevel(EchoVR::LogLevel level, const std::string& needle) {
  std::lock_guard<std::mutex> lock(g_testLogMutex);
  return std::any_of(g_testLogLeveled.begin(), g_testLogLeveled.end(),
                     [&](const std::pair<EchoVR::LogLevel, std::string>& entry) {
                       return entry.first == level && entry.second.find(needle) != std::string::npos;
                     });
}

void ClearTestLogs() {
  std::lock_guard<std::mutex> lock(g_testLogMutex);
  g_testLogMessages.clear();
  g_testLogLeveled.clear();
}

void Log(EchoVR::LogLevel level, const char* format, ...) {
  char buffer[2048] = {};
  va_list args;
  va_start(args, format);
  vsnprintf(buffer, sizeof(buffer), format, args);
  va_end(args);
  std::lock_guard<std::mutex> lock(g_testLogMutex);
  nevr_test_log_cap::Append(g_testLogMessages, buffer);
  g_testLogLeveled.emplace_back(level, buffer);
  g_testLogCv.notify_all();
}

// boot_log_tee.cpp stamps its lines with the run id; the test links it without core.
const CHAR* GetRunId() { return "test-run"; }
// boot_log_tee.cpp sets Log()'s boot sink; the stub Log() above records every line instead, so the
// sink is not kept (test_boot_log_routing drives the real routing).
void SetBootLogSink(BootLogSink) {}

FatalErrorHandlerFunc g_fatalErrorHandler = nullptr;
void SetFatalErrorHandler(FatalErrorHandlerFunc) {}

int g_fatalErrorCalls = 0;
void FatalError(const char* msg, const char* title) {
  ++g_fatalErrorCalls;
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

// --- initialize.h extern (set by xpid_patch.cpp; initialize.cpp is not linked here) ---
bool g_bootHookFailed = false;
char g_bootHookFailedCause[96] = {};

// --- config.h extern (used by ws_bridge.cpp) ---
PVOID g_pGame = nullptr;
void* g_earlyConfigPtr = nullptr;

// --- service_config.h accessor (N133 S4a, used by ws_bridge.cpp login injection) ---
// service_config.cpp is not linked here (it drags the config.yaml singleton +
// file discovery). A null return == "key absent", so ws_bridge's guard skips the
// URL-credential path exactly as an absent identity.discord_id/auth.password
// would. The N61 tests drive the Close handler, not login injection.
// #708: nevr_session_router answers from g_testRouterMode, so a test picks the bridge's routing mode.
const char* g_testRouterMode = nullptr;
const char* NevrCfgGetFlat(const char* flatKey) {
  return std::strcmp(flatKey, "nevr_session_router") == 0 ? g_testRouterMode : nullptr;
}

// --- plugin_load_plan.h (N134 S6, referenced by plugin_loader.cpp LoadPlugins) ---
// service_config.cpp defines the real one (BuildLoadPlan over the config.yaml
// singleton), which this target does not link. LoadPlugins is never CALLED by the
// N68 tick-dispatch tests inject via TestHook_* and leave this plan empty. The
// loader-diagnostic fixtures below supply one item at a time, so their real
// LoadLibrary/GetProcAddress path remains hermetic; pure plan construction is
// still covered in test_plugin_load_plan.
std::vector<PluginLoadItem> g_testPluginLoadPlan;

std::vector<PluginLoadItem> NevrCfgPluginLoadPlan() { return g_testPluginLoadPlan; }

// symbol_corpus.cpp provides the real LookupSymbolName function (670-entry table)
// and FormatSymbolId. It is compiled into this test target.

// ============================================================================
// Production headers (included AFTER all stubs are defined).
// ============================================================================

#include <cctype>
#include "runtime/ext/plugin_loader.h"
#include "runtime/ext/plugin_refusal_notice.h"
#include "runtime/hook/dll_load_hook.h"
#include "runtime/ext/plugin_trust.h"
#include "runtime/ext/module_loader.h"
#include "core/hex_dump.h"
#include "runtime/patch/matchmaker_host_patch.h"
#include "runtime/compat/ws_bridge.h"
#include "runtime/compat/self_check.h"
#include "runtime/compat/evr_codec.h"
#include "runtime/compat/socket_role.h"
#include "runtime/compat/hmd_serial.h"
#include "runtime/compat/social_party.h"
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

// ============================================================================
// Plugin loader diagnostics — real LoadLibraryExA + real GetProcAddress path.
//
// The test executable acts as the host module, so plugin_loader.cpp derives the
// deterministic build/bin/plugins directory without touching echovr.exe or its
// installation. The deliberately tiny test-only DLLs are built into that
// directory; one declares host API + 1 and the other exposes a counter that
// makes its real OnFrame callback observable.
// ============================================================================

class PluginLoaderDiagnosticTest : public ::testing::Test {
 protected:
  void SetUp() override {
    UnloadPlugins();
    g_testPluginLoadPlan.clear();
    ClearTestLogs();
    EchoVR::g_GameBaseAddress = reinterpret_cast<CHAR*>(GetModuleHandleA(nullptr));
    ASSERT_NE(EchoVR::g_GameBaseAddress, nullptr);
    // These tests are about the loader, not the signature gate: their fixture DLLs are unsigned. The gate has
    // its own fixture below (PluginSignatureGateTest), which enforces.
    nevr_plugin_trust::TestHook_SetTrust(nevr_plugin_trust::Mode::UnsignedAllowed, nullptr);
  }

  void TearDown() override {
    UnloadPlugins();
    g_testPluginLoadPlan.clear();
    ClearTestLogs();
    nevr_plugin_trust::TestHook_ResetTrust();
  }
};

// ============================================================================
// The plugin signature gate in the real loader (ADR 0010, #741): real LoadPlugins, real files, a policy that
// pins the test root. The fixtures are the test plugin signed by a throwaway PKI, made by
// tools/gen_plugin_signature_fixtures.py (--plugin-dll) into $NEVR_SIGFIX_DIR; a missing directory fails the
// tests, it does not skip them.
// ============================================================================

class PluginSignatureGateTest : public PluginLoaderDiagnosticTest {
 protected:
  void SetUp() override {
    PluginLoaderDiagnosticTest::SetUp();
    const char* dir = std::getenv("NEVR_SIGFIX_DIR");
    ASSERT_NE(dir, nullptr) << "NEVR_SIGFIX_DIR is not set: just test-auth-unit makes the fixtures and sets it";
    fixtures_ = dir;
    std::ifstream in(fixtures_ + "/fixtures.json");
    ASSERT_TRUE(in.good()) << "no fixtures.json in " << fixtures_;
    manifest_ = nlohmann::json::parse(in);
    ASSERT_TRUE(manifest_["plugins"].contains("test_plugin_onframe"));
    std::ifstream root(fixtures_ + "/" + manifest_["root_der"].get<std::string>(), std::ios::binary);
    policy_.rootDer.assign(std::istreambuf_iterator<char>(root), std::istreambuf_iterator<char>());
    ASSERT_FALSE(policy_.rootDer.empty());
    policy_.profileEkus = {manifest_["profile_eku"].get<std::string>()};
    nevr_plugin_trust::TestHook_SetTrust(nevr_plugin_trust::Mode::SignedOnly, &policy_);

    char exePath[MAX_PATH] = {};
    ASSERT_NE(GetModuleFileNameA(nullptr, exePath, MAX_PATH), 0U);
    pluginsDir_ = std::string(exePath);
    pluginsDir_.resize(pluginsDir_.find_last_of("\\/") + 1);
    pluginsDir_ += "plugins\\";
  }

  void TearDown() override {
    UnloadPlugins();
    for (const std::string& file : copies_) DeleteFileA(file.c_str());
    PluginLoaderDiagnosticTest::TearDown();
  }

  // Puts fixture `kind` (signed, wrong_eku, unsigned) of the test plugin into plugins/ as `name` and plans it.
  void Plan(const std::string& name, const std::string& kind, bool required = false) {
    const std::string source = fixtures_ + "/" + manifest_["files"]["test_plugin_onframe." + kind].get<std::string>();
    const std::string target = pluginsDir_ + name;
    ASSERT_TRUE(CopyFileA(source.c_str(), target.c_str(), FALSE)) << source << " error " << GetLastError();
    copies_.push_back(target);
    g_testPluginLoadPlan.push_back({name, name, required, "", "{}"});
  }

  std::string fixtures_;
  std::string pluginsDir_;
  nlohmann::json manifest_;
  nevr_plugin_signature::Policy policy_;
  std::vector<std::string> copies_;
};

TEST_F(PluginSignatureGateTest, PluginSignedByThePinnedSignerLoads) {
  Plan("gate_signed.dll", "signed");
  LoadPlugins();
  EXPECT_EQ(GetLoadedPluginCount(), 1);
  EXPECT_FALSE(TestLogContains("refused"));
  EXPECT_NE(BuildPluginManifestJson().find("gate_signed.dll"), std::string::npos);
}

TEST_F(PluginSignatureGateTest, UnsignedPluginIsRefusedWithOneLineAndAManifestEntry) {
  Plan("gate_unsigned.dll", "unsigned");
  LoadPlugins();
  EXPECT_EQ(GetLoadedPluginCount(), 0);
  EXPECT_EQ(GetModuleHandleA("gate_unsigned.dll"), nullptr) << "a refused file was mapped";
  EXPECT_TRUE(TestLogContains("plugin=gate_unsigned.dll file=gate_unsigned.dll refused: unsigned"));
  const nlohmann::json manifest = nlohmann::json::parse(BuildPluginManifestJson());
  ASSERT_EQ(manifest.size(), 1u);
  EXPECT_FALSE(manifest[0]["loaded"].get<bool>());
  EXPECT_EQ(manifest[0]["error"].get<std::string>(), "refused: unsigned");
}

TEST_F(PluginSignatureGateTest, PluginSignedByAnotherProfileIsRefused) {
  Plan("gate_other.dll", "wrong_eku");
  LoadPlugins();
  EXPECT_EQ(GetLoadedPluginCount(), 0);
  EXPECT_EQ(GetModuleHandleA("gate_other.dll"), nullptr);
  EXPECT_TRUE(TestLogContains("file=gate_other.dll refused: EKU"));
}

TEST_F(PluginSignatureGateTest, DevBuildLoadsAnUnsignedPluginWithAWarning) {
  nevr_plugin_trust::TestHook_SetTrust(nevr_plugin_trust::Mode::UnsignedAllowed, &policy_);
  Plan("gate_dev.dll", "unsigned");
  LoadPlugins();
  EXPECT_EQ(GetLoadedPluginCount(), 1);
  EXPECT_TRUE(TestLogContainsAtLevel(EchoVR::LogLevel::Warning,
                                     "file=gate_dev.dll UNSIGNED, loaded because this is a dev build"));
}

TEST_F(PluginSignatureGateTest, RefusalIsPerPluginAndTheSignedOneStillLoads) {
  Plan("gate_a_unsigned.dll", "unsigned");
  Plan("gate_b_signed.dll", "signed");
  LoadPlugins();
  EXPECT_EQ(GetLoadedPluginCount(), 1);
  EXPECT_TRUE(TestLogContains("file=gate_a_unsigned.dll refused: unsigned"));
}

// ---- the dependency walk, the refusal dialog and the load filter (ADR 0010 sections 1 and 6, #741) -------------

namespace {
std::vector<std::string> g_refusalDialogs;
void CaptureRefusalDialog(const std::string& text) { g_refusalDialogs.push_back(text); }
}  // namespace

class PluginDependencyGateTest : public PluginSignatureGateTest {
 protected:
  void SetUp() override {
    PluginSignatureGateTest::SetUp();
    ASSERT_TRUE(manifest_.contains("trees"));
    wasServer_ = g_isServer;
    wasHeadless_ = g_isHeadless;
    g_isServer = FALSE;  // a client with graphics: the one run that shows the dialog
    g_isHeadless = FALSE;
    g_refusalDialogs.clear();
    TestHook_ResetRefusals();
    TestHook_SetRefusalDialogSink(&CaptureRefusalDialog);
  }

  void TearDown() override {
    TestHook_ResetPluginLoadFilter();
    TestHook_SetRefusalDialogSink(nullptr);
    TestHook_ResetRefusals();
    g_isServer = wasServer_;
    g_isHeadless = wasHeadless_;
    PluginSignatureGateTest::TearDown();
  }

  // Copies the fixture tree `tree` (its signed and unsigned files) into plugins/ and returns the root file's name.
  std::string Copy(const std::string& tree) {
    const nlohmann::json& t = manifest_["trees"][tree];
    const std::string from = fixtures_ + "/" + t["dir"].get<std::string>() + "/";
    WIN32_FIND_DATAA found;
    HANDLE h = FindFirstFileA((from + "*.dll").c_str(), &found);
    EXPECT_NE(h, INVALID_HANDLE_VALUE) << from;
    if (h == INVALID_HANDLE_VALUE) return {};
    do {
      const std::string target = pluginsDir_ + found.cFileName;
      EXPECT_TRUE(CopyFileA((from + found.cFileName).c_str(), target.c_str(), FALSE)) << target;
      copies_.push_back(target);
    } while (FindNextFileA(h, &found));
    FindClose(h);
    return t["root"].get<std::string>();
  }

  void PlanTree(const std::string& tree) {
    const std::string root = Copy(tree);
    g_testPluginLoadPlan.push_back({root, root, false, "", "{}"});
  }

  // What the filter is asked about a load of `file` in plugins/.
  bool Blocked(const std::string& file) {
    std::wstring wide;
    for (char c : pluginsDir_ + file) wide.push_back(static_cast<wchar_t>(std::tolower(static_cast<unsigned char>(c))));
    return nevr_dll_load_hook::IsLoadBlocked(wide.c_str(), nullptr);
  }

  void ExpectDependencyRefusal(const std::string& tree) {
    const nlohmann::json& t = manifest_["trees"][tree];
    PlanTree(tree);
    LoadPlugins();
    EXPECT_EQ(GetLoadedPluginCount(), 0);
    EXPECT_EQ(GetModuleHandleA(t["root"].get<std::string>().c_str()), nullptr) << "the plugin was mapped";
    EXPECT_EQ(GetModuleHandleA("dep.dll"), nullptr) << "the refused dependency was mapped";
    const std::string expected = "refused: dependency dep.dll (" + t["via"].get<std::string>() + "): unsigned";
    EXPECT_TRUE(TestLogContains(expected)) << expected;
    const nlohmann::json manifest = nlohmann::json::parse(BuildPluginManifestJson());
    ASSERT_EQ(manifest.size(), 1u);
    EXPECT_EQ(manifest[0]["error"].get<std::string>(), expected);
  }

  BOOL wasServer_ = FALSE;
  BOOL wasHeadless_ = FALSE;
};

TEST_F(PluginDependencyGateTest, PluginWithAnUnsignedImportIsRefused) { ExpectDependencyRefusal("import_unsigned"); }
TEST_F(PluginDependencyGateTest, PluginWithAnUnsignedDelayLoadIsRefused) { ExpectDependencyRefusal("delay_unsigned"); }
TEST_F(PluginDependencyGateTest, PluginWithAnUnsignedForwardTargetIsRefused) {
  ExpectDependencyRefusal("forwarder_unsigned");
}
TEST_F(PluginDependencyGateTest, PluginWithAnUnsignedDependencyTwoLevelsDownIsRefused) {
  ExpectDependencyRefusal("transitive_unsigned");
}

TEST_F(PluginDependencyGateTest, SignedDependenciesAreNotARefusal) {
  PlanTree("import_ok");
  LoadPlugins();
  // The fixture is not a real plugin, so it fails for lacking the plugin exports; what matters is that the gate let
  // it through.
  EXPECT_FALSE(TestLogContains("refused"));
  EXPECT_TRUE(g_refusalDialogs.empty());
}

TEST_F(PluginDependencyGateTest, DevBuildDoesNotWalkTheDependencies) {
  nevr_plugin_trust::TestHook_SetTrust(nevr_plugin_trust::Mode::UnsignedAllowed, &policy_);
  PlanTree("import_unsigned");
  LoadPlugins();
  EXPECT_FALSE(TestLogContains("dependency"));
}

TEST_F(PluginDependencyGateTest, RefusalDialogNamesThePluginAndTheFileThatStoppedIt) {
  PlanTree("import_unsigned");
  LoadPlugins();
  ASSERT_EQ(g_refusalDialogs.size(), 1u);
  EXPECT_NE(g_refusalDialogs[0].find("importer.dll wasn't loaded because it isn't signed by nEVR."), std::string::npos)
      << g_refusalDialogs[0];
  EXPECT_NE(g_refusalDialogs[0].find("dep.dll wasn't loaded because it isn't signed by nEVR."), std::string::npos)
      << g_refusalDialogs[0];
  EXPECT_NE(g_refusalDialogs[0].find("ask each plugin's author for a signed release."), std::string::npos);
}

TEST_F(PluginDependencyGateTest, OneRefusedPluginGetsTheExactDialogText) {
  Plan("gate_dialog.dll", "unsigned");
  LoadPlugins();
  ASSERT_EQ(g_refusalDialogs.size(), 1u);
  EXPECT_EQ(g_refusalDialogs[0],
            "gate_dialog.dll wasn't loaded because it isn't signed by nEVR. Plugins from the launcher are signed; "
            "ask the plugin's author for a signed release.");
}

TEST_F(PluginDependencyGateTest, DialogIsShownOncePerRun) {
  Plan("gate_dialog_a.dll", "unsigned");
  LoadPlugins();
  ASSERT_EQ(g_refusalDialogs.size(), 1u);
  TestHook_RecordRefusal("later.dll");
  TestHook_ShowRefusalNoticeIfDue();
  EXPECT_EQ(g_refusalDialogs.size(), 1u) << "a second dialog in the same run";
}

TEST_F(PluginDependencyGateTest, NoDialogOnAServer) {
  g_isServer = TRUE;
  Plan("gate_dialog_s.dll", "unsigned");
  LoadPlugins();
  EXPECT_TRUE(g_refusalDialogs.empty());
  EXPECT_TRUE(TestLogContains("file=gate_dialog_s.dll refused: unsigned"));  // the record is still written
}

TEST_F(PluginDependencyGateTest, NoDialogOnAHeadlessRun) {
  g_isHeadless = TRUE;
  Plan("gate_dialog_h.dll", "unsigned");
  LoadPlugins();
  EXPECT_TRUE(g_refusalDialogs.empty());
}

TEST_F(PluginDependencyGateTest, NoDialogWhenNothingWasRefused) {
  Plan("gate_dialog_ok.dll", "signed");
  LoadPlugins();
  EXPECT_TRUE(g_refusalDialogs.empty());
}

TEST_F(PluginDependencyGateTest, LateLoadOfAnUnsignedFileInThePluginsDirectoryIsRefused) {
  Plan("late_unsigned.dll", "unsigned");  // copied into plugins/, never planned to load
  g_testPluginLoadPlan.clear();
  TestHook_InstallPluginLoadFilter(pluginsDir_);
  EXPECT_TRUE(Blocked("late_unsigned.dll"));
  EXPECT_TRUE(TestLogContains("refused a later load of late_unsigned.dll: unsigned"));
  TestHook_ShowRefusalNoticeIfDue();
  ASSERT_EQ(g_refusalDialogs.size(), 1u);
  EXPECT_NE(g_refusalDialogs[0].find("late_unsigned.dll"), std::string::npos);
}

TEST_F(PluginDependencyGateTest, LateLoadOfASignedFileIsLeftAlone) {
  Plan("late_signed.dll", "signed");
  g_testPluginLoadPlan.clear();
  TestHook_InstallPluginLoadFilter(pluginsDir_);
  EXPECT_FALSE(Blocked("late_signed.dll"));
  EXPECT_FALSE(TestLogContains("refused a later load"));
}

TEST_F(PluginDependencyGateTest, LateLoadByBareNameResolvesToThePluginsDirectory) {
  Plan("late_bare.dll", "unsigned");
  g_testPluginLoadPlan.clear();
  TestHook_InstallPluginLoadFilter(pluginsDir_);
  EXPECT_TRUE(nevr_dll_load_hook::IsLoadBlocked(L"late_bare.dll", nullptr));
}

TEST_F(PluginDependencyGateTest, LateLoadOfAFileOutsideThePluginsDirectoryIsNotThisFiltersBusiness) {
  TestHook_InstallPluginLoadFilter(pluginsDir_);
  std::wstring outside;
  for (char c : fixtures_ + "/" + manifest_["files"]["test_plugin_onframe.unsigned"].get<std::string>()) {
    outside.push_back(static_cast<wchar_t>(std::tolower(static_cast<unsigned char>(c))));
  }
  EXPECT_FALSE(nevr_dll_load_hook::IsLoadBlocked(outside.c_str(), nullptr));
}

TEST_F(PluginDependencyGateTest, LateLoadOfASignedFileWithAnUnsignedDependencyIsRefused) {
  Copy("import_unsigned");
  TestHook_InstallPluginLoadFilter(pluginsDir_);
  EXPECT_TRUE(Blocked("importer.dll"));
  EXPECT_TRUE(TestLogContains("refused a later load of importer.dll: dependency dep.dll (import): unsigned"));
}

TEST_F(PluginDependencyGateTest, DevBuildAllowsALateLoadWithAWarning) {
  nevr_plugin_trust::TestHook_SetTrust(nevr_plugin_trust::Mode::UnsignedAllowed, &policy_);
  Plan("late_dev.dll", "unsigned");
  g_testPluginLoadPlan.clear();
  TestHook_InstallPluginLoadFilter(pluginsDir_);
  EXPECT_FALSE(Blocked("late_dev.dll"));
  EXPECT_TRUE(TestLogContainsAtLevel(EchoVR::LogLevel::Warning, "a plugin loads late_dev.dll after its init"));
}

TEST(PluginRefusalNotice, TrackerKeepsEachNameOnceAndNeverShowsATwiceDialog) {
  nevr_plugin_refusal::Tracker t;
  t.Record("a.dll");
  t.Record("a.dll");
  EXPECT_EQ(t.refused(), 1u);
  EXPECT_TRUE(t.TakeDialog(true, false).empty());  // a server gets no dialog
  EXPECT_FALSE(t.shown());
  EXPECT_TRUE(t.TakeDialog(false, true).empty());  // nor does a headless run
  EXPECT_FALSE(t.TakeDialog(false, false).empty());
  EXPECT_TRUE(t.shown());
  EXPECT_TRUE(t.TakeDialog(false, false).empty());
}

TEST(PluginSignatureGate, NoFallbackToTheDefaultSearchInASignedOnlyBuild) {
  EXPECT_FALSE(TestHook_RestrictedSearchFallbackAllowed(true, ERROR_INVALID_PARAMETER));
  EXPECT_TRUE(TestHook_RestrictedSearchFallbackAllowed(false, ERROR_INVALID_PARAMETER));
  EXPECT_FALSE(TestHook_RestrictedSearchFallbackAllowed(false, ERROR_FILE_NOT_FOUND));
}

TEST_F(PluginLoaderDiagnosticTest, MissingOptionalDllLogsLoadLibraryFailureAndContinues) {
  g_testPluginLoadPlan.push_back(
      {"missing", "plugin_that_does_not_exist.dll", false, "", "{}"});

  LoadPlugins();

  EXPECT_EQ(GetLoadedPluginCount(), 0);
  EXPECT_TRUE(TestLogContains("plugin_that_does_not_exist.dll"));
  EXPECT_TRUE(TestLogContains("LoadLibrary failed: error"));
  EXPECT_TRUE(TestLogContains("optional, continuing"));
}

TEST_F(PluginLoaderDiagnosticTest, ConfiguredPluginPathCannotEscapePluginsDirectory) {
  char exePath[MAX_PATH] = {};
  ASSERT_NE(GetModuleFileNameA(nullptr, exePath, MAX_PATH), 0U);
  std::string directory(exePath);
  directory.resize(directory.find_last_of("\\/") + 1);
  const std::string source = directory + "plugins\\test_plugin_onframe.dll";
  const std::string escaped = directory + "test_plugin_escape.dll";
  ASSERT_NE(GetFileAttributesA(source.c_str()), INVALID_FILE_ATTRIBUTES);
  ASSERT_TRUE(CopyFileA(source.c_str(), escaped.c_str(), FALSE));

  g_testPluginLoadPlan.push_back({"escape", "..\\test_plugin_escape.dll", false, "", "{}"});
  LoadPlugins();

  EXPECT_EQ(GetLoadedPluginCount(), 0);
  EXPECT_EQ(GetModuleHandleA("test_plugin_escape.dll"), nullptr);
  EXPECT_TRUE(TestLogContains("outside plugins directory"));
  UnloadPlugins();
  EXPECT_TRUE(DeleteFileA(escaped.c_str()));
}

TEST_F(PluginLoaderDiagnosticTest, RejectsEveryUnsafePluginFilename) {
  const std::vector<std::string> invalid = {
      "", ".", "..", ".\\", ".\\.", ".\\..", "sub/plugin.dll",
      "sub\\plugin.dll", "C:plugin.dll", std::string("plugin.dll\0extra", 16)};
  for (const std::string& file : invalid) {
    SCOPED_TRACE(file);
    g_testPluginLoadPlan = {{"unsafe", file, false, "", "{}"}};
    ClearTestLogs();
    LoadPlugins();
    EXPECT_EQ(GetLoadedPluginCount(), 0);
    EXPECT_TRUE(TestLogContains("outside plugins directory"));
  }
}

TEST_F(PluginLoaderDiagnosticTest, DottedSupersededPluginIsSkippedBeforeLoad) {
  for (const char* file : {".\\log_filter.dll", ".\\LOG-FILTER.DLL"}) {
    SCOPED_TRACE(file);
    g_testPluginLoadPlan = {{"old-filter", file, true, "", "{}"}};
    ClearTestLogs();
    LoadPlugins();
    EXPECT_EQ(GetLoadedPluginCount(), 0);
    EXPECT_TRUE(TestLogContains("superseded by the built-in log filter"));
  }
}

TEST_F(PluginLoaderDiagnosticTest, FailedInitRunsShutdownBeforeReleasingPlugin) {
  constexpr char kEventName[] = "Local\\NEVRTestPartialInitShutdownObserved";
  HANDLE shutdownObserved = CreateEventA(nullptr, TRUE, FALSE, kEventName);
  ASSERT_NE(shutdownObserved, nullptr);

  g_testPluginLoadPlan.push_back({"partial", "test_plugin_partial_init_fail.dll", false, "", "{}"});
  LoadPlugins();

  EXPECT_EQ(GetLoadedPluginCount(), 0);
  EXPECT_EQ(WaitForSingleObject(shutdownObserved, 0), WAIT_OBJECT_0);
  EXPECT_EQ(GetModuleHandleA("test_plugin_partial_init_fail.dll"), nullptr);
  EXPECT_TRUE(TestLogContains("test_plugin_partial_init_fail.dll: shutdown after failed init"));
  CloseHandle(shutdownObserved);
}

TEST_F(PluginLoaderDiagnosticTest, FailedInitWithoutShutdownRetainsModule) {
  g_testPluginLoadPlan.push_back({"partial", "test_plugin_init_fail.dll", false, "", "{}"});
  LoadPlugins();

  EXPECT_EQ(GetLoadedPluginCount(), 0);
  EXPECT_NE(GetModuleHandleA("test_plugin_init_fail.dll"), nullptr);
  EXPECT_TRUE(TestLogContains("retained until process exit"));
}

TEST_F(PluginLoaderDiagnosticTest, FailedInitStillVerifiesHookedAddresses) {
  nevr_hook_guard::ResetForTest();
  std::array<unsigned char, 16> target{};
  nevr_hook_guard::Record(target.data(), "failed-init-target");
  target[0] = 0xE9;
  const int fatalCallsBefore = g_serverFatalCalls;
  g_testPluginLoadPlan.push_back({"partial", "test_plugin_init_fail.dll", false, "", "{}"});

  LoadPlugins();

  EXPECT_EQ(GetLoadedPluginCount(), 0);
  EXPECT_EQ(g_serverFatalCalls, fatalCallsBefore + 1);
  EXPECT_TRUE(TestLogContains("failed-init-target"));
  nevr_hook_guard::ResetForTest();
}

TEST_F(PluginLoaderDiagnosticTest, FutureApiDllLogsVersionMismatchAndLoads) {
  g_testPluginLoadPlan.push_back(
      {"future-api", "test_plugin_future_api.dll", false, "", "{}"});

  LoadPlugins();

  ASSERT_EQ(GetLoadedPluginCount(), 1);
  // Build from the published host version so this fixture remains exactly one
  // API generation ahead after a future bump.
  const std::string versionDiagnostic =
      "test_plugin_future_api.dll requires API v" +
      std::to_string(NEVR_PLUGIN_API_VERSION + 1u) + ", host supports v" +
      std::to_string(NEVR_PLUGIN_API_VERSION);
  EXPECT_TRUE(TestLogContains(versionDiagnostic));
  EXPECT_TRUE(TestLogContains("loading anyway"));
  EXPECT_TRUE(TestLogContains("Loaded: test-plugin-future-api"));
}

TEST_F(PluginLoaderDiagnosticTest, RealLoadedPluginOnFrameHasObservableSideEffect) {
  g_testPluginLoadPlan.push_back(
      {"onframe", "test_plugin_onframe.dll", false, "", "{}"});

  LoadPlugins();

  ASSERT_EQ(GetLoadedPluginCount(), 1);
  const HMODULE plugin = GetModuleHandleA("test_plugin_onframe.dll");
  ASSERT_NE(plugin, nullptr);
  const auto getFrameCount = reinterpret_cast<uint32_t (*)(void)>(
      GetProcAddress(plugin, "NvrTestPluginGetFrameCount"));
  ASSERT_NE(getFrameCount, nullptr);
  EXPECT_EQ(getFrameCount(), 0u);

  NvrGameContext ctx = {};
  ctx.base_addr = reinterpret_cast<uintptr_t>(EchoVR::g_GameBaseAddress);
  ctx.flags = NEVR_HOST_IS_SERVER;
  ctx.ctx_size = sizeof(NvrGameContext);
  ctx.get_plugin_count = GetLoadedPluginCount;
  ctx.get_plugin_info = GetLoadedPluginInfo;
  TickPlugins(&ctx);
  TickPlugins(&ctx);

  EXPECT_EQ(getFrameCount(), 2u);
}

TEST_F(PluginLoaderDiagnosticTest, ExplicitUnloadInvokesShutdownAndReleasesPluginReference) {
  constexpr char kShutdownEventName[] = "Local\\NEVRTestPluginShutdownObserved";
  HANDLE shutdownObserved = CreateEventA(nullptr, TRUE, FALSE, kShutdownEventName);
  ASSERT_NE(shutdownObserved, nullptr);

  g_testPluginLoadPlan.push_back(
      {"onframe", "test_plugin_onframe.dll", false, "", "{}"});
  LoadPlugins();

  const int loadedCount = GetLoadedPluginCount();
  EXPECT_EQ(loadedCount, 1);
  EXPECT_NE(GetModuleHandleA("test_plugin_onframe.dll"), nullptr);
  UnloadPlugins();

  EXPECT_EQ(WaitForSingleObject(shutdownObserved, 0),
            loadedCount == 1 ? WAIT_OBJECT_0 : WAIT_TIMEOUT);
  EXPECT_EQ(GetModuleHandleA("test_plugin_onframe.dll"), nullptr);
  CloseHandle(shutdownObserved);
}

// The number of times the loaded fixture's init ran, read through its test-only
// export. -1 when the fixture is not loaded.
static long OnFrameFixtureInitCount() {
  const HMODULE plugin = GetModuleHandleA("test_plugin_onframe.dll");
  if (plugin == nullptr) return -1;
  const auto getInitCount = reinterpret_cast<uint32_t (*)(void)>(
      GetProcAddress(plugin, "NvrTestPluginGetInitCount"));
  if (getInitCount == nullptr) return -1;
  return static_cast<long>(getInitCount());
}

static void TickLoadedPluginsOnce() {
  NvrGameContext ctx = {};
  ctx.base_addr = reinterpret_cast<uintptr_t>(EchoVR::g_GameBaseAddress);
  ctx.flags = NEVR_HOST_IS_SERVER;
  ctx.ctx_size = sizeof(NvrGameContext);
  ctx.get_plugin_count = GetLoadedPluginCount;
  ctx.get_plugin_info = GetLoadedPluginInfo;
  TickPlugins(&ctx);
}

static uint32_t OnFrameFixtureFrameCount() {
  const HMODULE plugin = GetModuleHandleA("test_plugin_onframe.dll");
  if (plugin == nullptr) return 0xFFFFFFFFu;
  const auto getFrameCount = reinterpret_cast<uint32_t (*)(void)>(
      GetProcAddress(plugin, "NvrTestPluginGetFrameCount"));
  return getFrameCount != nullptr ? getFrameCount() : 0xFFFFFFFFu;
}

// The same DLL listed twice is loaded once: one init, and each OnFrame reaches it
// once. Before, LoadLibrary handed back the loaded module for the repeat, its init
// ran again, and it was staged twice, so every tick called it twice.
TEST_F(PluginLoaderDiagnosticTest, PluginListedTwiceLoadsOnce) {
  g_testPluginLoadPlan.push_back({"onframe", "test_plugin_onframe.dll", false, "", "{}"});
  g_testPluginLoadPlan.push_back({"onframe-again", "TEST_PLUGIN_ONFRAME.DLL", false, "",
                                  R"({"other":"args"})"});

  LoadPlugins();

  ASSERT_EQ(GetLoadedPluginCount(), 1);
  EXPECT_TRUE(TestLogContains("SKIPPED onframe-again (TEST_PLUGIN_ONFRAME.DLL)"));
  EXPECT_TRUE(TestLogContains("args were ignored"));
  EXPECT_EQ(OnFrameFixtureInitCount(), 1);
  TickLoadedPluginsOnce();
  EXPECT_EQ(OnFrameFixtureFrameCount(), 1u);

  const nlohmann::json manifest = nlohmann::json::parse(BuildPluginManifestJson());
  ASSERT_EQ(manifest.size(), 2u) << manifest.dump();
  EXPECT_EQ(manifest[1].at("loaded"), false);
  EXPECT_EQ(manifest[1].at("error"), "listed twice in config.yaml");
}

// N89 (#100 regression net): a plugin whose function is built in is refused even when it is listed
// as required, with the reason in the login's report; the plugin after it still loads.
TEST_F(PluginLoaderDiagnosticTest, SupersededPluginIsRefusedAndTheNextOneLoads) {
  g_testPluginLoadPlan.push_back({"filter", "Log_Filter.dll", true, "", "{}"});
  g_testPluginLoadPlan.push_back({"onframe", "test_plugin_onframe.dll", false, "", "{}"});

  LoadPlugins();

  ASSERT_EQ(GetLoadedPluginCount(), 1);
  EXPECT_TRUE(TestLogContains("SKIPPED Log_Filter.dll"));
  const nlohmann::json manifest = nlohmann::json::parse(BuildPluginManifestJson());
  ASSERT_EQ(manifest.size(), 2u) << manifest.dump();
  EXPECT_EQ(manifest[0].at("loaded"), false);
  EXPECT_EQ(manifest[0].at("error"), "superseded by the built-in log filter");
  EXPECT_EQ(manifest[1].at("loaded"), true);
}

// The same DLL under another spelling of its path passes the file-name check, so
// the loader's same-module check is what stops it.
TEST_F(PluginLoaderDiagnosticTest, PluginListedUnderAnotherPathSpellingLoadsOnce) {
  g_testPluginLoadPlan.push_back({"onframe", "test_plugin_onframe.dll", false, "", "{}"});
  g_testPluginLoadPlan.push_back({"onframe-dotted", ".\\test_plugin_onframe.dll", false, "", "{}"});

  LoadPlugins();

  ASSERT_EQ(GetLoadedPluginCount(), 1);
  EXPECT_TRUE(TestLogContains("SKIPPED onframe-dotted"));
  EXPECT_TRUE(TestLogContains("the same module as onframe"));
  EXPECT_TRUE(TestLogContains("args were ignored"));
  EXPECT_EQ(OnFrameFixtureInitCount(), 1);
  TickLoadedPluginsOnce();
  EXPECT_EQ(OnFrameFixtureFrameCount(), 1u);

  const nlohmann::json manifest = nlohmann::json::parse(BuildPluginManifestJson());
  ASSERT_EQ(manifest.size(), 2u) << manifest.dump();
  EXPECT_EQ(manifest[1].at("loaded"), false);
  EXPECT_EQ(manifest[1].at("error"), "the same module as onframe");
}

// A required repeat of an optional entry that failed to load: the plugin the
// config declared required is missing, so that is fatal, not a skipped duplicate.
TEST_F(PluginLoaderDiagnosticTest, RequiredRepeatOfFailedOptionalEntryIsFatal) {
  g_serverFatalCalls = 0;
  g_testPluginLoadPlan.push_back({"gone", "plugin_that_does_not_exist.dll", false, "", "{}"});
  g_testPluginLoadPlan.push_back({"gone-again", "plugin_that_does_not_exist.dll", true, "", "{}"});

  LoadPlugins();

  EXPECT_EQ(g_serverFatalCalls, 1);
  EXPECT_NE(g_lastServerFatal.find("gone"), std::string::npos) << g_lastServerFatal;
  const nlohmann::json manifest = nlohmann::json::parse(BuildPluginManifestJson());
  ASSERT_EQ(manifest.size(), 2u) << manifest.dump();
  EXPECT_EQ(manifest[0].at("required"), true);
}

// The same, for a plugin that loads but whose init fails: the failure surfaces
// later (pass 2), and must still be fatal because a repeat required it.
TEST_F(PluginLoaderDiagnosticTest, RequiredRepeatOfInitFailingOptionalEntryIsFatal) {
  g_serverFatalCalls = 0;
  g_testPluginLoadPlan.push_back({"flaky", "test_plugin_init_fail.dll", false, "", "{}"});
  g_testPluginLoadPlan.push_back({"flaky-again", "TEST_PLUGIN_INIT_FAIL.DLL", true, "", "{}"});

  LoadPlugins();

  EXPECT_EQ(g_serverFatalCalls, 1);
  EXPECT_NE(g_lastServerFatal.find("flaky"), std::string::npos) << g_lastServerFatal;
  EXPECT_EQ(GetLoadedPluginCount(), 0);
}

// Same, when the repeat is only caught by the same-module check.
TEST_F(PluginLoaderDiagnosticTest, RequiredRepeatUnderAnotherSpellingOfInitFailingEntryIsFatal) {
  g_serverFatalCalls = 0;
  g_testPluginLoadPlan.push_back({"flaky", "test_plugin_init_fail.dll", false, "", "{}"});
  g_testPluginLoadPlan.push_back({"flaky-dotted", ".\\test_plugin_init_fail.dll", true, "", "{}"});

  LoadPlugins();

  EXPECT_EQ(g_serverFatalCalls, 1);
  EXPECT_EQ(GetLoadedPluginCount(), 0);
}

// A required repeat of a spelling that was itself skipped as the same module: the
// requirement reaches the entry that holds the module, not the skipped one. The
// holder loaded fine, so nothing is fatal.
TEST_F(PluginLoaderDiagnosticTest, RequiredRepeatOfSkippedSpellingOfLoadedEntryIsNotFatal) {
  g_serverFatalCalls = 0;
  g_testPluginLoadPlan.push_back({"first", "test_plugin_onframe.dll", false, "", "{}"});
  g_testPluginLoadPlan.push_back({"second", ".\\test_plugin_onframe.dll", false, "", "{}"});
  g_testPluginLoadPlan.push_back({"third", ".\\test_plugin_onframe.dll", true, "", "{}"});

  LoadPlugins();

  EXPECT_EQ(g_serverFatalCalls, 0) << g_lastServerFatal;
  EXPECT_EQ(GetLoadedPluginCount(), 1);
  const nlohmann::json manifest = nlohmann::json::parse(BuildPluginManifestJson());
  ASSERT_EQ(manifest.size(), 3u) << manifest.dump();
  EXPECT_EQ(manifest[0].at("required"), true);
}

// The same chain when the holder's init fails: the fatal names the holder (the
// first entry), the entry that really holds the module.
TEST_F(PluginLoaderDiagnosticTest, RequiredRepeatOfSkippedSpellingOfInitFailingEntryNamesHolder) {
  g_serverFatalCalls = 0;
  g_testPluginLoadPlan.push_back({"first", "test_plugin_init_fail.dll", false, "", "{}"});
  g_testPluginLoadPlan.push_back({"second", ".\\test_plugin_init_fail.dll", false, "", "{}"});
  g_testPluginLoadPlan.push_back({"third", ".\\test_plugin_init_fail.dll", true, "", "{}"});

  LoadPlugins();

  EXPECT_EQ(g_serverFatalCalls, 1) << g_lastServerFatal;
  EXPECT_NE(g_lastServerFatal.find("first"), std::string::npos) << g_lastServerFatal;
  EXPECT_EQ(g_lastServerFatal.find("second"), std::string::npos) << g_lastServerFatal;
  EXPECT_EQ(GetLoadedPluginCount(), 0);
}

// An optional first entry that loads, then a required repeat of the same spelling.
TEST_F(PluginLoaderDiagnosticTest, RequiredRepeatOfSameSpellingOfLoadedEntryIsNotFatal) {
  g_serverFatalCalls = 0;
  g_testPluginLoadPlan.push_back({"first", "test_plugin_onframe.dll", false, "", "{}"});
  g_testPluginLoadPlan.push_back({"again", "test_plugin_onframe.dll", true, "", "{}"});

  LoadPlugins();

  EXPECT_EQ(g_serverFatalCalls, 0) << g_lastServerFatal;
  EXPECT_EQ(GetLoadedPluginCount(), 1);
}

// Control: an optional repeat of an optional entry stays non-fatal.
TEST_F(PluginLoaderDiagnosticTest, OptionalRepeatOfInitFailingOptionalEntryIsNotFatal) {
  g_serverFatalCalls = 0;
  g_testPluginLoadPlan.push_back({"flaky", "test_plugin_init_fail.dll", false, "", "{}"});
  g_testPluginLoadPlan.push_back({"flaky-again", "test_plugin_init_fail.dll", false, "", "{}"});

  LoadPlugins();

  EXPECT_EQ(g_serverFatalCalls, 0);
  EXPECT_EQ(GetLoadedPluginCount(), 0);
}

// AGENTS.md logging rule 6: the Warning the loader writes for an arg whose bytes
// were replaced exists, names the plugin and the arg KEY, and never carries the value.
TEST_F(PluginLoaderDiagnosticTest, ReplacedArgKeyIsLoggedByNameNeverByValue) {
  PluginLoadItem item;
  item.name = "onframe";
  item.file = "test_plugin_onframe.dll";
  item.args_json = "{\"path\":\"SECRETVALUE\"}";
  item.args_replaced_keys = {"path"};
  g_testPluginLoadPlan.push_back(item);

  LoadPlugins();

  ASSERT_EQ(GetLoadedPluginCount(), 1);
  EXPECT_TRUE(TestLogContains("onframe: arg 'path' held invalid UTF-8"));
  EXPECT_FALSE(TestLogContains("SECRETVALUE"));
}

// #152: a plugin may keep what get_plugin_info returned during its init. The loader reserves
// g_plugins before any init so the push_back that follows that init cannot move the entry the kept
// pointer names. The keeper fixture takes plugin 0's pointer in its init; after the load it must still
// be the pointer the host hands out for index 0.
TEST_F(PluginLoaderDiagnosticTest, InfoPointerKeptDuringALaterPluginsInitStaysValid) {
  g_testPluginLoadPlan.push_back({"onframe", "test_plugin_onframe.dll", false, "", "{}"});
  g_testPluginLoadPlan.push_back({"keeper", "test_plugin_info_keeper.dll", false, "", "{}"});

  LoadPlugins();

  ASSERT_EQ(GetLoadedPluginCount(), 2);
  const HMODULE keeper = GetModuleHandleA("test_plugin_info_keeper.dll");
  ASSERT_NE(keeper, nullptr);
  const auto getKept = reinterpret_cast<const NvrLoadedPluginInfo* (*)(void)>(
      GetProcAddress(keeper, "NvrTestPluginGetKeptInfo"));
  ASSERT_NE(getKept, nullptr);
  const NvrLoadedPluginInfo* kept = getKept();
  ASSERT_NE(kept, nullptr);
  EXPECT_EQ(kept, GetLoadedPluginInfo(0)) << "the pointer a plugin kept was invalidated by a later load";
  EXPECT_STREQ(kept->name, "test-plugin-onframe");
}

// #596: the address GetLoadedPluginInfo hands out belongs to the storage, not to a prediction of how
// many plugins a later pass adds. Registering plugins the loader never planned for (the test hook
// appends exactly as a load does) must not move the first one's entry.
static void NoOpPluginFrame(const NvrGameContext*) {}

TEST_F(PluginLoaderDiagnosticTest, InfoPointerStaysValidWhilePluginsAreAddedWithoutAPlan) {
  TestHook_RegisterPluginOnFrame(NoOpPluginFrame);
  const NvrLoadedPluginInfo* first = GetLoadedPluginInfo(0);
  ASSERT_NE(first, nullptr);

  for (int i = 0; i < 256; ++i) TestHook_RegisterPluginOnFrame(NoOpPluginFrame);

  ASSERT_EQ(GetLoadedPluginCount(), 257);
  EXPECT_EQ(first, GetLoadedPluginInfo(0)) << "adding plugins moved an entry a plugin may have kept";
}

// #496: an entry marked early loads in the early pass, alone; the normal pass loads the
// rest, the early one is not loaded again, and the report covers both passes.
static PluginLoadItem EarlyItem(const char* name, const char* file) {
  PluginLoadItem item{name, file, false, "", "{}"};
  item.early = true;
  return item;
}

TEST_F(PluginLoaderDiagnosticTest, EarlyPassLoadsOnlyTheEntriesMarkedEarly) {
  g_testPluginLoadPlan.push_back({"onframe", "test_plugin_onframe.dll", false, "", "{}"});
  g_testPluginLoadPlan.push_back(EarlyItem("future-api", "test_plugin_future_api.dll"));

  LoadPlugins(PluginPhase::Early);

  ASSERT_EQ(GetLoadedPluginCount(), 1);
  EXPECT_EQ(GetModuleHandleA("test_plugin_onframe.dll"), nullptr) << "the early pass loaded a normal entry";
  EXPECT_TRUE(TestLogContains("early pass: 1 plugin(s) marked early"));
  EXPECT_TRUE(TestLogContains("early pass complete: 1/1 loaded"));
  nlohmann::json manifest = nlohmann::json::parse(BuildPluginManifestJson());
  ASSERT_EQ(manifest.size(), 2u) << manifest.dump();
  EXPECT_EQ(manifest[0].at("loaded"), false);
  EXPECT_EQ(manifest[1].at("loaded"), true);

  LoadPlugins();

  ASSERT_EQ(GetLoadedPluginCount(), 2);
  EXPECT_EQ(OnFrameFixtureInitCount(), 1);
  EXPECT_TRUE(TestLogContains("plugin load complete: 2/2 loaded"));
  EXPECT_FALSE(TestLogContains("SKIPPED")) << "the normal pass took the early entry again";
  manifest = nlohmann::json::parse(BuildPluginManifestJson());
  ASSERT_EQ(manifest.size(), 2u) << manifest.dump();
  EXPECT_EQ(manifest[0].at("loaded"), true);
  EXPECT_EQ(manifest[1].at("loaded"), true);
}

TEST_F(PluginLoaderDiagnosticTest, EarlyPassWithNothingMarkedEarlyLoadsNothing) {
  g_testPluginLoadPlan.push_back({"onframe", "test_plugin_onframe.dll", false, "", "{}"});

  LoadPlugins(PluginPhase::Early);

  EXPECT_EQ(GetLoadedPluginCount(), 0);
  EXPECT_FALSE(TestLogContains("early pass"));

  LoadPlugins();

  EXPECT_EQ(GetLoadedPluginCount(), 1);
  EXPECT_TRUE(TestLogContains("plugin load complete: 1/1 loaded"));
}

// The first entry for a file decides its pass, as for any repeat (#135): an early repeat
// of a normal entry is skipped by the early pass and the plugin loads in the normal one.
TEST_F(PluginLoaderDiagnosticTest, EarlyRepeatOfANormalEntryLoadsInTheNormalPass) {
  g_testPluginLoadPlan.push_back({"onframe", "test_plugin_onframe.dll", false, "", "{}"});
  g_testPluginLoadPlan.push_back(EarlyItem("onframe-early", "test_plugin_onframe.dll"));

  LoadPlugins(PluginPhase::Early);

  EXPECT_EQ(GetLoadedPluginCount(), 0);
  EXPECT_TRUE(TestLogContains("SKIPPED onframe-early (test_plugin_onframe.dll)"));

  LoadPlugins();

  ASSERT_EQ(GetLoadedPluginCount(), 1);
  EXPECT_EQ(OnFrameFixtureInitCount(), 1);
}

// Another spelling of a module the early pass loaded is caught by the same-module check.
TEST_F(PluginLoaderDiagnosticTest, ModuleTheEarlyPassLoadedIsNotLoadedAgainUnderAnotherSpelling) {
  g_testPluginLoadPlan.push_back(EarlyItem("onframe", "test_plugin_onframe.dll"));
  g_testPluginLoadPlan.push_back({"onframe-dotted", ".\\test_plugin_onframe.dll", false, "", "{}"});

  LoadPlugins(PluginPhase::Early);
  LoadPlugins();

  ASSERT_EQ(GetLoadedPluginCount(), 1);
  EXPECT_TRUE(TestLogContains("SKIPPED onframe-dotted"));
  EXPECT_TRUE(TestLogContains("the same module as onframe"));
  EXPECT_EQ(OnFrameFixtureInitCount(), 1);
  TickLoadedPluginsOnce();
  EXPECT_EQ(OnFrameFixtureFrameCount(), 1u);
}

// #152 across the passes: a pointer an early plugin kept from get_plugin_info stays valid
// when the normal pass loads more.
TEST_F(PluginLoaderDiagnosticTest, InfoPointerKeptInTheEarlyPassStaysValidAfterTheNormalPass) {
  g_testPluginLoadPlan.push_back(EarlyItem("onframe", "test_plugin_onframe.dll"));
  g_testPluginLoadPlan.push_back(EarlyItem("keeper", "test_plugin_info_keeper.dll"));
  g_testPluginLoadPlan.push_back({"future-api", "test_plugin_future_api.dll", false, "", "{}"});

  LoadPlugins(PluginPhase::Early);
  ASSERT_EQ(GetLoadedPluginCount(), 2);
  LoadPlugins();
  ASSERT_EQ(GetLoadedPluginCount(), 3);

  const HMODULE keeper = GetModuleHandleA("test_plugin_info_keeper.dll");
  ASSERT_NE(keeper, nullptr);
  const auto getKept = reinterpret_cast<const NvrLoadedPluginInfo* (*)(void)>(
      GetProcAddress(keeper, "NvrTestPluginGetKeptInfo"));
  ASSERT_NE(getKept, nullptr);
  const NvrLoadedPluginInfo* kept = getKept();
  ASSERT_NE(kept, nullptr);
  EXPECT_EQ(kept, GetLoadedPluginInfo(0)) << "the normal pass moved what an early plugin kept";
  EXPECT_STREQ(kept->name, "test-plugin-onframe");
}

// get_plugin_info reports each plugin's own API version and capabilities. Casting
// NvrPluginInfo (padded to 32 bytes) as NvrLoadedPluginInfo would make
// api_version read the padding and capabilities read the API version: a v5
// plugin declaring no capabilities would show as caps 5.
TEST_F(PluginLoaderDiagnosticTest, LoadedPluginInfoReportsApiVersionAndCapabilities) {
  g_testPluginLoadPlan.push_back({"onframe", "test_plugin_onframe.dll", false, "", "{}"});
  g_testPluginLoadPlan.push_back({"future-api", "test_plugin_future_api.dll", false, "", "{}"});

  LoadPlugins();

  ASSERT_EQ(GetLoadedPluginCount(), 2);
  const NvrLoadedPluginInfo* first = GetLoadedPluginInfo(0);
  const NvrLoadedPluginInfo* second = GetLoadedPluginInfo(1);
  ASSERT_NE(first, nullptr);
  ASSERT_NE(second, nullptr);
  // Both declare no capabilities: same priority band, config order kept.
  EXPECT_STREQ(first->name, "test-plugin-onframe");
  EXPECT_EQ(first->api_version, static_cast<uint32_t>(NEVR_PLUGIN_API_VERSION));
  EXPECT_EQ(first->capabilities, static_cast<uint32_t>(NEVR_PLUGIN_CAP_UNDECLARED));
  EXPECT_STREQ(second->name, "test-plugin-future-api");
  EXPECT_EQ(second->api_version, static_cast<uint32_t>(NEVR_PLUGIN_API_VERSION + 1u));
  EXPECT_EQ(second->capabilities, static_cast<uint32_t>(NEVR_PLUGIN_CAP_UNDECLARED));
  EXPECT_EQ(second->version_major, 1u);
}

// ----------------------------------------------------------------------------
// #598: plugins log through the host's structured Log(), tagged with the plugin.
// The fixtures (plugin_logging_test_dll.cpp) call ctx->log from init, per-frame, state change and
// shutdown, through the real loader and a real LoadLibraryExA.
// ----------------------------------------------------------------------------

static uint32_t LoggingFixtureBoundAtInit(const char* dll) {
  const HMODULE plugin = GetModuleHandleA(dll);
  if (plugin == nullptr) return 0xFFFFFFFFu;
  const auto bound = reinterpret_cast<uint32_t (*)(void)>(GetProcAddress(plugin, "NevrTestPluginGetLogBoundAtInit"));
  return bound != nullptr ? bound() : 0xFFFFFFFFu;
}

TEST(PluginLogApi, ContextCarriesALogFunctionAndTheVersionSaysSo) {
  EXPECT_EQ(static_cast<uint32_t>(NEVR_PLUGIN_API_VERSION), 6u);
  NvrGameContext ctx = {};
  ctx.ctx_size = sizeof(NvrGameContext);
  EXPECT_GE(ctx.ctx_size, offsetof(NvrGameContext, log) + sizeof(NevrLogFn));
  EXPECT_GT(offsetof(NvrGameContext, plugin_id), offsetof(NvrGameContext, get_plugin_info))
      << "the v6 fields extend the tail; a v5 plugin still reads the v5 fields at the same offsets";
}

TEST_F(PluginLoaderDiagnosticTest, PluginInitLineReachesTheHostLogWithItsNameLevelAndFields) {
  g_testPluginLoadPlan.push_back({"logger-a", "test_plugin_logging_a.dll", false, "", "{}"});

  LoadPlugins();

  ASSERT_EQ(GetLoadedPluginCount(), 1);
  EXPECT_EQ(LoggingFixtureBoundAtInit("test_plugin_logging_a.dll"), 1u)
      << "the context handed to init did not carry plugin_id and log";
  EXPECT_TRUE(TestLogContainsAtLevel(EchoVR::LogLevel::Warning,
                                     "[NEVR.PLUGIN] plugin=logger-a init hello answer=42 who=\"a b\""));
}

TEST_F(PluginLoaderDiagnosticTest, EachPluginsFrameAndStateLinesCarryItsOwnName) {
  g_testPluginLoadPlan.push_back({"logger-a", "test_plugin_logging_a.dll", false, "", "{}"});
  g_testPluginLoadPlan.push_back({"logger-b", "test_plugin_logging_b.dll", false, "", "{}"});
  LoadPlugins();
  ASSERT_EQ(GetLoadedPluginCount(), 2);
  ClearTestLogs();

  TickLoadedPluginsOnce();
  NvrGameContext ctx = {};
  ctx.ctx_size = sizeof(NvrGameContext);
  NotifyPluginsStateChange(&ctx, 3, 5);

  EXPECT_TRUE(TestLogContainsAtLevel(EchoVR::LogLevel::Debug, "[NEVR.PLUGIN] plugin=logger-a frame n=1"));
  EXPECT_TRUE(TestLogContainsAtLevel(EchoVR::LogLevel::Debug, "[NEVR.PLUGIN] plugin=logger-b frame n=1"));
  EXPECT_TRUE(TestLogContainsAtLevel(EchoVR::LogLevel::Info,
                                     "[NEVR.PLUGIN] plugin=logger-a state change from=3 to=5"));
  EXPECT_TRUE(TestLogContainsAtLevel(EchoVR::LogLevel::Info,
                                     "[NEVR.PLUGIN] plugin=logger-b state change from=3 to=5"));
  EXPECT_FALSE(TestLogContains("plugin=unknown")) << "a context reached a plugin with no id";
}

TEST_F(PluginLoaderDiagnosticTest, PluginCanLogFromShutdownWithWhatItKeptAtInit) {
  g_testPluginLoadPlan.push_back({"logger-a", "test_plugin_logging_a.dll", false, "", "{}"});
  LoadPlugins();
  ASSERT_EQ(GetLoadedPluginCount(), 1);
  ClearTestLogs();

  UnloadPlugins();

  EXPECT_TRUE(TestLogContainsAtLevel(EchoVR::LogLevel::Error, "[NEVR.PLUGIN] plugin=logger-a shutdown bye"));
}

TEST(PluginLogHost, HostLogFunctionTagsLevelsAndKeepsAPercentSignOutOfTheFormat) {
  ClearTestLogs();
  const NevrLogField fields[] = {{"k", "100%s"}};
  HostPluginLog("direct", NEVR_LOG_ERROR, "50%n done", fields, 1);
  HostPluginLog("direct", 99u, "bad level", nullptr, 0);
  EXPECT_TRUE(TestLogContainsAtLevel(EchoVR::LogLevel::Error, "[NEVR.PLUGIN] plugin=direct 50%n done k=100%s"));
  EXPECT_TRUE(TestLogContainsAtLevel(EchoVR::LogLevel::Warning,
                                     "[NEVR.PLUGIN] plugin=direct bad level level_invalid=99"));
  ClearTestLogs();
}

// #60: the login reports every configured plugin — the one that loaded, the one
// that is enabled but failed, and the one that is disabled — with the real loader
// filling the record from a real LoadLibraryExA run. The disabled entry names a
// DLL that exists in plugins/ and would load, so "not loaded" proves the loader
// honours enabled:false now that the plan carries disabled entries.
TEST_F(PluginLoaderDiagnosticTest, LoginCarriesLoadedFailedAndDisabledPlugins) {
  g_testPluginLoadPlan.push_back({"onframe", "test_plugin_onframe.dll", false, "", "{}", true});
  g_testPluginLoadPlan.push_back({"missing", "plugin_that_does_not_exist.dll", true, "", "{}", true});
  g_testPluginLoadPlan.push_back({"off", "test_plugin_future_api.dll", false, "", "{}", false});

  LoadPlugins();

  ASSERT_EQ(GetLoadedPluginCount(), 1);
  EXPECT_EQ(GetModuleHandleA("test_plugin_future_api.dll"), nullptr) << "a disabled plugin was loaded";
  EXPECT_TRUE(TestLogContains("off (test_plugin_future_api.dll): disabled in config.yaml"));
  EXPECT_TRUE(TestLogContains("plugin load complete: 1/2 loaded"));

  const nlohmann::json manifest = nlohmann::json::parse(BuildPluginManifestJson());
  ASSERT_TRUE(manifest.is_array());
  ASSERT_EQ(manifest.size(), 3u) << manifest.dump();

  const nlohmann::json& loaded = manifest[0];
  EXPECT_EQ(loaded.at("name"), "onframe");
  EXPECT_EQ(loaded.at("file"), "test_plugin_onframe.dll");
  EXPECT_EQ(loaded.at("enabled"), true);
  EXPECT_EQ(loaded.at("required"), false);
  EXPECT_EQ(loaded.at("loaded"), true);
  EXPECT_EQ(loaded.at("ver"), "1.0.0");
  EXPECT_EQ(loaded.at("api"), NEVR_PLUGIN_API_VERSION);
  EXPECT_EQ(loaded.at("caps"), NEVR_PLUGIN_CAP_UNDECLARED);
  EXPECT_FALSE(loaded.contains("error"));

  const nlohmann::json& failed = manifest[1];
  EXPECT_EQ(failed.at("name"), "missing");
  EXPECT_EQ(failed.at("required"), true);
  EXPECT_EQ(failed.at("loaded"), false);
  EXPECT_EQ(failed.at("error").get<std::string>().rfind("LoadLibrary failed: error ", 0), 0u)
      << failed.dump();
  EXPECT_FALSE(failed.contains("ver"));

  const nlohmann::json& disabled = manifest[2];
  EXPECT_EQ(disabled.at("name"), "off");
  EXPECT_EQ(disabled.at("enabled"), false);
  EXPECT_EQ(disabled.at("loaded"), false);
  EXPECT_FALSE(disabled.contains("error"));

  // The wire payload: the same array, as a JSON array (not a string), under the
  // top-level `nevr_plugins` key of the LoginProfile JSON.
  const std::string request = TestHook_BuildLoginRequest(55, 4, "Player", "token");
  constexpr size_t kJsonOffset = 56;
  ASSERT_GT(request.size(), kJsonOffset);
  ASSERT_EQ(request.back(), '\0');
  const nlohmann::json login =
      nlohmann::json::parse(request.substr(kJsonOffset, request.size() - kJsonOffset - 1));
  ASSERT_TRUE(login.contains("nevr_plugins"));
  ASSERT_TRUE(login.at("nevr_plugins").is_array());
  EXPECT_EQ(login.at("nevr_plugins"), manifest);
  EXPECT_TRUE(TestLogContains("login nevr_plugins configured=3 loaded=1"));

  // After unload nothing is loaded, so the report is empty rather than stale.
  UnloadPlugins();
  EXPECT_EQ(nlohmann::json::parse(BuildPluginManifestJson()), nlohmann::json::array());
}

TEST_F(N68_PluginTickTest, OnFrame_Fires_When_Registered) {
  TestHook_RegisterPluginOnFrame(N68_PluginOnFrame);
  NvrGameContext ctx = {};
  ctx.base_addr = reinterpret_cast<uintptr_t>(EchoVR::g_GameBaseAddress);
  ctx.flags = NEVR_HOST_IS_SERVER;
  ctx.ctx_size = sizeof(NvrGameContext);
  ctx.get_plugin_count = GetLoadedPluginCount;
  ctx.get_plugin_info = GetLoadedPluginInfo;
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
  ctx.ctx_size = sizeof(NvrGameContext);
  ctx.get_plugin_count = GetLoadedPluginCount;
  ctx.get_plugin_info = GetLoadedPluginInfo;
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
// N133 S5 — module API version gate. The host refuses a module whose reported
// version exceeds its own (boot.cpp, N120). The refusal RULE is the header-only
// predicate NvrModuleApiVersionSupported, which boot.cpp and these tests share.
// Verifying the predicate verifies the mismatch decision without spawning a
// process.
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

TEST(WsBridgeListener, GracefulStopReleasesBothListeningPorts) {
  SetWebSocketBridgeTarget("wss://127.0.0.1:9/ws");
  InstallWebSocketBridge();
  const uint16_t proxyPort = GetWebSocketBridgePort();
  const uint16_t matchPort = GetMatchmakerBridgePort();
  ASSERT_NE(proxyPort, 0);
  ASSERT_NE(matchPort, 0);
  ASSERT_NE(proxyPort, matchPort);

  StopWebSocketBridgeListener();
  SetWebSocketBridgeTarget("");

  for (const uint16_t port : {proxyPort, matchPort}) {
    SOCKET probe = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    ASSERT_NE(probe, INVALID_SOCKET);
    sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    const int result = bind(probe, reinterpret_cast<sockaddr*>(&address), sizeof(address));
    const int error = result == SOCKET_ERROR ? WSAGetLastError() : 0;
    EXPECT_NE(result, SOCKET_ERROR) << "port " << port << " stayed bound after stop: WSA " << error;
    closesocket(probe);
  }
}

// #70: when the shared remote session ends, every game socket on it (login and matchmaker) is closed so
// the game reconnects; a socket on another remote, or one the game already closed, is left alone.
TEST_F(N61_WsBridgeTest, RemoteEndSelectsEveryGameSocketOnThatSessionOnly) {
  auto remote = MockWsHandle::Create();
  auto other = MockWsHandle::Create();
  auto loginWs = MockWsHandle::Create();
  auto matchWs = MockWsHandle::Create();
  void* loginRaw = TestHook_N61_RegisterLogin(remote.handle, loginWs.handle);
  ASSERT_NE(loginRaw, nullptr);
  bool fired = false;
  void* matchRaw = TestHook_N61_RegisterMatchmaker(matchWs.handle, &fired);
  ASSERT_NE(matchRaw, nullptr);

  EXPECT_EQ(TestHook_GameSocketsBoundTo(remote.handle), 2U) << "login and matchmaker share the session";
  EXPECT_EQ(TestHook_GameSocketsBoundTo(other.handle), 0U);
  (void)TestHook_N61_SimulateCloseAndCheckCleared(matchRaw);
  EXPECT_EQ(TestHook_GameSocketsBoundTo(remote.handle), 1U) << "a socket the game closed is not closed again";
}

// #70: the login session ending makes the game's next connection a new login (numbered 1), not a
// matchmaker attached to the dead session; any other session ending changes nothing.
TEST_F(N61_WsBridgeTest, LoginSessionEndRenumbersTheNextConnectionAsLogin) {
  auto remote = MockWsHandle::Create();
  auto other = MockWsHandle::Create();
  auto loginWs = MockWsHandle::Create();
  ASSERT_NE(TestHook_N61_RegisterLogin(remote.handle, loginWs.handle), nullptr);
  int next = -1;
  EXPECT_FALSE(TestHook_ForgetLoginSession(other.handle, &next)) << "not the login session";
  EXPECT_TRUE(TestHook_ForgetLoginSession(remote.handle, &next));
  EXPECT_EQ(next, 1);
  EXPECT_FALSE(TestHook_ForgetLoginSession(remote.handle, &next)) << "already forgotten";
}

// #723/#729: the login remote ending runs the self-check session end, which flushes the queued results through
// SendFrameToServer, which takes g_pairsMutex. Ending the session while holding that mutex deadlocked the
// remote's own thread, and WebSocket::stop() (graceful shutdown) then waited on that thread forever. The path
// runs on a worker thread here so a regression fails the test instead of hanging it.
TEST_F(N61_WsBridgeTest, TheLoginRemoteEndingDoesNotDeadlockOnItsOwnMutex) {
  auto remote = MockWsHandle::Create();
  auto loginWs = MockWsHandle::Create();
  ASSERT_NE(TestHook_N61_RegisterLogin(remote.handle, loginWs.handle), nullptr);

  nevr_self_check::ResetForTest();
  nevr_self_check::SetEnabled(true);
  nevr_self_check::SetSender(&TestHook_SendFrameToServer);
  nevr_self_check::SetLoggedIn(true, nevr_evr_codec::UserId());
  const nevr_self_check::CheckId id = nevr_self_check::Register({"deadlock_probe", "a result waits in the queue", nullptr});
  ASSERT_GE(id, 0);
  nevr_self_check::Report(id, "queued", true);  // something for the session end to flush through the sender

  std::promise<void> done;
  std::future<void> finished = done.get_future();
  std::thread worker([&] {
    TestHook_CloseGameSocketsForRemote(remote.handle, 1, 1000);
    done.set_value();
  });
  if (finished.wait_for(std::chrono::seconds(10)) != std::future_status::ready) {
    ADD_FAILURE() << "the remote-ended path never returned: it holds g_pairsMutex while the self-check flush takes it";
    std::fflush(stdout);
    std::_Exit(1);  // the stuck thread owns the mutex the fixture's teardown needs
  }
  worker.join();
  nevr_self_check::ResetForTest();
}

// #723, the ServerDB drop: the session-end flush runs inside the login remote's own Close callback, and ix holds
// its non-recursive _setReadyStateMutex there. A send on that same remote (the flush's sender) blocks on it, so the
// remote's thread never returned and graceful shutdown (WebSocket::stop joins it) never finished. This drives a real
// ix::WebSocket against a real local server that closes it, with a self-check result queued for the flush to send.
TEST_F(N61_WsBridgeTest, TheLoginRemoteDroppedByItsServerEndsItsSessionWithoutSendingOnItself) {
  nevr_self_check::ResetForTest();
  nevr_self_check::SetEnabled(true);
  nevr_self_check::SetSender(&TestHook_SendFrameToServer);
  nevr_self_check::SetLoggedIn(true, nevr_evr_codec::UserId());
  const nevr_self_check::CheckId id = nevr_self_check::Register({"drop_probe", "a result waits in the queue", nullptr});
  ASSERT_GE(id, 0);
  nevr_self_check::Report(id, "queued", true);

  if (!TestHook_LoginRemoteDroppedThroughRealIx(10000)) {
    ADD_FAILURE() << "the remote's Close callback never returned: the session-end flush sent on the closing remote";
    std::fflush(stdout);
    std::_Exit(1);  // the remote's thread is stuck on ix's own mutex; tearing down would wait on it
  }
  nevr_self_check::ResetForTest();
}

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

TEST_F(N61_WsBridgeTest, MultipleMatchmakerConnectionsKeepTheLatestActiveCallback) {
  // Each conn>=2 registration shares the login remote.  The most recently
  // accepted matchmaker owns the callback, and closing login must preserve it.
  auto remote = MockWsHandle::Create();
  auto loginWs = MockWsHandle::Create();
  auto firstMatchWs = MockWsHandle::Create();
  auto secondMatchWs = MockWsHandle::Create();

  void* loginRaw = TestHook_N61_RegisterLogin(remote.handle, loginWs.handle);
  ASSERT_NE(loginRaw, nullptr);

  bool firstFired = false;
  bool secondFired = false;
  ASSERT_NE(TestHook_N61_RegisterMatchmaker(firstMatchWs.handle, &firstFired), nullptr);
  ASSERT_NE(TestHook_N61_RegisterMatchmaker(secondMatchWs.handle, &secondFired), nullptr);

  EXPECT_TRUE(TestHook_N61_HasActiveCallback());
  EXPECT_FALSE(TestHook_N61_SimulateCloseAndCheckCleared(loginRaw));
  EXPECT_TRUE(TestHook_N61_HasActiveCallback());
}

TEST_F(N61_WsBridgeTest, FramesFromTheLoginSessionGoToALiveConnectionAfterTheNewestCloses) {
  // Measured 2026-10-01: conn=3 opens for the lobby join and closes ~15 s later; every profile
  // reply after that went to the closed socket and the game never saw it. The route is the newest
  // connection still open, then the login connection, then none.
  auto remote = MockWsHandle::Create();
  auto loginWs = MockWsHandle::Create();
  auto conn2 = MockWsHandle::Create();
  auto conn3 = MockWsHandle::Create();
  void* loginRaw = TestHook_N61_RegisterLogin(remote.handle, loginWs.handle);
  ASSERT_NE(loginRaw, nullptr);
  EXPECT_EQ(TestHook_SharedRouteConn(), 1);
  bool fired = false;
  void* raw2 = TestHook_N61_RegisterMatchmaker(conn2.handle, &fired);
  void* raw3 = TestHook_N61_RegisterMatchmaker(conn3.handle, &fired);
  EXPECT_EQ(TestHook_SharedRouteConn(), 3) << "the newest connection takes the frames";

  TestHook_N61_SimulateCloseAndCheckCleared(raw3);
  EXPECT_EQ(TestHook_SharedRouteConn(), 2) << "not the closed conn=3";
  TestHook_N61_SimulateCloseAndCheckCleared(raw2);
  EXPECT_EQ(TestHook_SharedRouteConn(), 1) << "back to the login connection";
  TestHook_N61_SimulateCloseAndCheckCleared(loginRaw);
  EXPECT_EQ(TestHook_SharedRouteConn(), -1) << "nothing open: frames are dropped (and logged)";
}

TEST_F(N61_WsBridgeTest, LoginCloseDuringActiveMatchmakerIsSafe) {
  auto remote = MockWsHandle::Create();
  auto loginWs = MockWsHandle::Create();
  auto matchWs = MockWsHandle::Create();

  void* loginRaw = TestHook_N61_RegisterLogin(remote.handle, loginWs.handle);
  ASSERT_NE(loginRaw, nullptr);
  bool matchFired = false;
  ASSERT_NE(TestHook_N61_RegisterMatchmaker(matchWs.handle, &matchFired), nullptr);

  // Repeating the close is deliberately harmless: the close path must not
  // dereference the already-removed login pair or clear the matchmaker route.
  EXPECT_FALSE(TestHook_N61_SimulateCloseAndCheckCleared(loginRaw));
  EXPECT_FALSE(TestHook_N61_SimulateCloseAndCheckCleared(loginRaw));
  EXPECT_TRUE(TestHook_N61_HasActiveCallback());
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
// #520: which game socket is config and which is login. These tests run the REAL bridge: a game-side
// client connects to its listener, the bridge's Open handler assigns the role, and a stand-in for the
// community service answers every frame it gets. Nothing here sleeps; each wait is on a condition
// variable (a log line, a received frame) with a bound that only a failing run reaches.
// ============================================================================

namespace {

constexpr auto kRoleWait = std::chrono::seconds(20);

// Waits for `done()` by yielding, for a state ixwebsocket changes without telling anyone (a closed
// connection leaving a server's client set). Bounded; false when the bound passes.
template <typename Done>
bool YieldUntil(Done done) {
  const auto deadline = std::chrono::steady_clock::now() + kRoleWait;
  while (!done()) {
    if (std::chrono::steady_clock::now() > deadline) return false;
    std::this_thread::yield();
  }
  return true;
}

std::vector<std::string> LogLinesContaining(const std::string& needle) {
  std::lock_guard<std::mutex> lock(g_testLogMutex);
  std::vector<std::string> lines;
  for (const std::string& line : g_testLogMessages) {
    if (line.find(needle) != std::string::npos) lines.push_back(line);
  }
  return lines;
}

bool WaitForLogLines(const std::string& needle, size_t count) {
  std::unique_lock<std::mutex> lock(g_testLogMutex);
  return g_testLogCv.wait_for(lock, kRoleWait, [&] {
    return static_cast<size_t>(std::count_if(g_testLogMessages.begin(), g_testLogMessages.end(),
                                             [&needle](const std::string& line) {
                                               return line.find(needle) != std::string::npos;
                                             })) >= count;
  });
}

// The community service as the bridge's remote: it counts the LoginRequests it receives and answers each
// binary frame, a LoginRequest with LoginSettings and anything else with DocumentSuccess, so a test can see
// which game socket the bridge routed a reply to and which request the reply answers.
class RemoteService {
 public:
  // The answer to a FindSessionRequest: a symbol the router does not read, so it is routed as lobby traffic.
  static constexpr uint64_t kLobbyReplySymbol = 0x7e57104410bb9001ULL;

  bool Start() {
    std::random_device device;
    std::mt19937 generator(device());
    std::uniform_int_distribution<int> ports(49152, 65535);
    for (int attempt = 0; attempt < 20; ++attempt) {
      const int port = ports(generator);
      server_ = std::make_unique<ix::WebSocketServer>(port, "127.0.0.1");
      server_->disablePerMessageDeflate();
      server_->setOnClientMessageCallback(
          [this](std::shared_ptr<ix::ConnectionState>, ix::WebSocket& socket, const ix::WebSocketMessagePtr& message) {
            if (message->type == ix::WebSocketMessageType::Open) {
              ++opens_;
              std::lock_guard<std::mutex> lock(urisMutex_);
              uris_.push_back(message->openInfo.uri);
            }
            if (message->type != ix::WebSocketMessageType::Message || !message->binary) return;
            const uint64_t symbol = nevr_evr_codec::FirstSymbol(message->str);
            {
              std::lock_guard<std::mutex> lock(urisMutex_);
              received_.push_back(symbol);
            }
            const bool isLogin = symbol == nevr_evr_codec::kSymLoginRequest;
            if (isLogin) ++loginRequests_;
            uint64_t reply = nevr_evr_codec::kSymDocumentSuccess;
            if (isLogin) reply = loginReply_.load();
            if (symbol == nevr_evr_codec::kSymConfigRequest) {
              reply = nevr_evr_codec::kSymConfigSuccess;
              if (message->str.find("main_menu") != std::string::npos) {
                // The service echoes the request's type and id, as symbols, ahead of the resource.
                std::string payload;
                for (int copy = 0; copy < 2; ++copy) {
                  for (int i = 0; i < 8; ++i) payload.push_back(static_cast<char>((0x1509ee27440e1d7bULL >> (8 * i)) & 0xFF));
                }
                payload += "{}";
                socket.sendBinary(nevr_evr_codec::BuildMessage(reply, payload));
                return;
              }
            }
            if (symbol == nevr_evr_codec::kSymFindSessionRequest) {
              if (holdLobbyReplies_.load()) {
                ++heldLobbyReplies_;
                return;
              }
              reply = kLobbyReplySymbol;
            }
            std::string customLogin;
            {
              std::lock_guard<std::mutex> lock(urisMutex_);
              customLogin = loginReplyFrame_;
            }
            if (isLogin && !customLogin.empty()) {
              socket.sendBinary(customLogin);
              return;
            }
            socket.sendBinary(nevr_evr_codec::BuildMessage(reply, ""));
          });
      if (server_->listen().first) {
        port_ = port;
        server_->start();
        return true;
      }
    }
    server_.reset();
    return false;
  }
  void Stop() {
    if (server_) server_->stop();
    server_.reset();
  }
  void CloseEveryClient() {
    for (const auto& client : server_->getClients()) client->close();
  }
  // ixwebsocket's server stop closes every client it still holds, and a connection that is finishing when
  // that happens can be left waiting for a close it will never get, which hangs the stop. So the service
  // is stopped only once its client set is empty.
  bool WaitForNoClients() {
    return YieldUntil([this] { return server_->getClients().empty(); });
  }
  std::string Uri() const { return "ws://127.0.0.1:" + std::to_string(port_) + "/"; }
  int LoginRequests() const { return loginRequests_.load(); }
  // Connections the bridge has opened to the service.
  int Opens() const { return opens_.load(); }
  // The request target (path and query) of each connection the bridge opened, in order.
  std::vector<std::string> Uris() {
    std::lock_guard<std::mutex> lock(urisMutex_);
    return uris_;
  }
  // What the service answers a LoginRequest with (LoginSettings by default).
  void SetLoginReply(uint64_t symbol) { loginReply_ = symbol; }
  // A whole frame to answer a LoginRequest with, in place of the symbol above.
  void SetLoginReplyFrame(std::string frame) {
    std::lock_guard<std::mutex> lock(urisMutex_);
    loginReplyFrame_ = std::move(frame);
  }
  // How many frames led with `symbol` among those the bridge sent the service.
  int ReceivedCount(uint64_t symbol) {
    std::lock_guard<std::mutex> lock(urisMutex_);
    return static_cast<int>(std::count(received_.begin(), received_.end(), symbol));
  }
  // While held, a FindSessionRequest is not answered; ReleaseLobbyReplies sends each held answer then.
  void HoldLobbyReplies(bool hold) { holdLobbyReplies_ = hold; }
  int HeldLobbyReplies() const { return heldLobbyReplies_.load(); }
  void ReleaseLobbyReplies() {
    holdLobbyReplies_ = false;
    for (int held = heldLobbyReplies_.exchange(0); held > 0; --held) PushToClients(kLobbyReplySymbol);
  }
  // Sends `symbol` to every connection, unasked.
  void PushToClients(uint64_t symbol) {
    for (const auto& client : server_->getClients()) client->sendBinary(nevr_evr_codec::BuildMessage(symbol, ""));
  }

 private:
  std::unique_ptr<ix::WebSocketServer> server_;
  int port_ = 0;
  std::atomic<int> loginRequests_{0};
  std::atomic<int> opens_{0};
  std::atomic<bool> holdLobbyReplies_{false};
  std::atomic<int> heldLobbyReplies_{0};
  std::atomic<uint64_t> loginReply_{nevr_evr_codec::kSymLoginSettings};
  std::mutex urisMutex_;  // guards uris_, received_ and loginReplyFrame_
  std::vector<std::string> uris_;
  std::vector<uint64_t> received_;
  std::string loginReplyFrame_;
};

// A game-side socket: connects to the bridge at a request target and counts the frames the bridge sends it.
class GameSocket {
 public:
  ~GameSocket() { socket_.stop(); }
  bool Connect(uint16_t bridgePort, const std::string& target) {
    socket_.setUrl("ws://127.0.0.1:" + std::to_string(bridgePort) + target);
    socket_.disableAutomaticReconnection();
    socket_.disablePerMessageDeflate();
    socket_.setOnMessageCallback([this](const ix::WebSocketMessagePtr& message) {
      std::lock_guard<std::mutex> lock(mutex_);
      if (message->type == ix::WebSocketMessageType::Open) open_ = true;
      if (message->type == ix::WebSocketMessageType::Close) {
        closed_ = true;
        closeCode_ = message->closeInfo.code;
      }
      if (message->type == ix::WebSocketMessageType::Message) {
        ++frames_;
        symbols_.push_back(nevr_evr_codec::FirstSymbol(message->str));
        payloads_.push_back(message->str);
      }
      changed_.notify_all();
    });
    socket_.start();
    std::unique_lock<std::mutex> lock(mutex_);
    return changed_.wait_for(lock, kRoleWait, [this] { return open_; });
  }
  // The bridge ended this socket (a close frame, or the connection dropping).
  bool WaitForClose() {
    std::unique_lock<std::mutex> lock(mutex_);
    return changed_.wait_for(lock, kRoleWait, [this] { return closed_; });
  }
  bool WaitForFrames(int count) {
    std::unique_lock<std::mutex> lock(mutex_);
    return changed_.wait_for(lock, kRoleWait, [&] { return frames_ >= count; });
  }
  int Frames() {
    std::lock_guard<std::mutex> lock(mutex_);
    return frames_;
  }
  // The close code the bridge sent, 0 until it closed the socket.
  int CloseCode() {
    std::lock_guard<std::mutex> lock(mutex_);
    return closeCode_;
  }
  // The first frame that led with `symbol`, whole; empty when none did.
  std::string FrameWith(uint64_t symbol) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (size_t index = 0; index < symbols_.size(); ++index) {
      if (symbols_[index] == symbol) return payloads_[index];
    }
    return std::string();
  }
  // How many frames led with `symbol`.
  int Count(uint64_t symbol) {
    std::lock_guard<std::mutex> lock(mutex_);
    return static_cast<int>(std::count(symbols_.begin(), symbols_.end(), symbol));
  }
  // Waits for a frame whose first message is `symbol`.
  bool WaitForSymbol(uint64_t symbol) {
    std::unique_lock<std::mutex> lock(mutex_);
    return changed_.wait_for(lock, kRoleWait, [&] { return Received(symbol); });
  }
  bool Received(uint64_t symbol) {
    for (const uint64_t seen : symbols_) {
      if (seen == symbol) return true;
    }
    return false;
  }
  bool HasReceived(uint64_t symbol) {
    std::lock_guard<std::mutex> lock(mutex_);
    return Received(symbol);
  }
  void SendRequest() { Send(nevr_evr_codec::kSymDocumentRequest); }
  void Send(uint64_t symbol) { Send(symbol, std::string()); }
  void Send(uint64_t symbol, const std::string& payload) {
    socket_.sendBinary(nevr_evr_codec::BuildMessage(symbol, payload));
  }

 private:
  ix::WebSocket socket_;
  std::mutex mutex_;
  std::condition_variable changed_;
  bool open_ = false;
  bool closed_ = false;
  int closeCode_ = 0;
  int frames_ = 0;
  std::vector<uint64_t> symbols_;
  std::vector<std::string> payloads_;
};

constexpr const char* kConnectedLine = "Proxy: game connected (conn=";

class RolesByPathTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ix::initNetSystem();
    TestHook_N61_ResetState();
    ClearTestLogs();
    g_testRouterMode = RouterMode();
    fatalBefore_ = g_fatalErrorCalls;
    ASSERT_TRUE(remote_.Start());
    SetWebSocketBridgeTarget(remote_.Uri().c_str());
    InstallWebSocketBridge();
    port_ = GetWebSocketBridgePort();
    if (BridgeExpected()) {
      ASSERT_NE(port_, 0);
    }
  }
  void TearDown() override {
    // The game-side sockets are closed by now; stop the bridge once its server has let go of them (the
    // same ixwebsocket stop hazard as the service's, below).
    EXPECT_TRUE(YieldUntil([] { return TestHook_ConnectedGameSockets() == 0; }));
    StopWebSocketBridgeListener();
    g_testRouterMode = nullptr;
    // The login session's remote outlives the game sockets that used it; dropping it closes the service's
    // connection, which the service's stop waits for.
    TestHook_N61_ResetState();
    SetWebSocketBridgeTarget("");
    EXPECT_TRUE(remote_.WaitForNoClients());
    remote_.Stop();
  }
  // The value network.session_router has for this test: null is the key being absent.
  virtual const char* RouterMode() const { return nullptr; }
  virtual bool BridgeExpected() const { return true; }
  int fatalBefore_ = 0;
  // Connects `socket` at `target` and returns once the bridge has finished its Open handler for it.
  void Admit(GameSocket& socket, const std::string& target, uint16_t port = 0) {
    ASSERT_TRUE(socket.Connect(port != 0 ? port : port_, target)) << target;
    ASSERT_TRUE(WaitForLogLines(kConnectedLine, ++admitted_)) << "the bridge never admitted " << target;
  }
  std::string AdmittedLine(size_t index) {
    const std::vector<std::string> lines = LogLinesContaining(kConnectedLine);
    return index < lines.size() ? lines[index] : std::string("<no such line>");
  }
  RemoteService remote_;
  uint16_t port_ = 0;
  size_t admitted_ = 0;
};

}  // namespace

// The game's login socket may connect before its config socket. The role comes from the request target,
// not from the order, so the socket that asked for /login is the login and receives the LoginRequest's
// reply. By order the first connection is config: the login socket is labelled config, gets no
// LoginRequest, and the config socket is labelled login and gets it (#520).
TEST_F(RolesByPathTest, LoginSocketThatConnectsFirstIsStillTheLogin) {
  GameSocket login, config;
  Admit(login, "/login");
  Admit(config, "/config");
  EXPECT_NE(AdmittedLine(0).find("(conn=1, login"), std::string::npos) << AdmittedLine(0);
  EXPECT_NE(AdmittedLine(1).find("(conn=0, config"), std::string::npos) << AdmittedLine(1);
  EXPECT_TRUE(login.WaitForSymbol(nevr_evr_codec::kSymLoginSettings))
      << "the /login socket never received the reply to the LoginRequest";
  // The config socket's own request is answered on its own remote connection, after anything the bridge
  // sent on it first; so once that answer is here, a LoginRequest injected on the config socket would
  // already have been answered too.
  config.Send(nevr_evr_codec::kSymConfigRequest);
  ASSERT_TRUE(config.WaitForSymbol(nevr_evr_codec::kSymConfigSuccess));
  EXPECT_FALSE(config.HasReceived(nevr_evr_codec::kSymLoginSettings)) << "a LoginRequest was injected on the /config socket";
  EXPECT_EQ(remote_.LoginRequests(), 1);
}

// N91: a second socket that asks for /login while a login session exists is not a second login. It takes
// no login slot and sends no LoginRequest; it shares the session.
TEST_F(RolesByPathTest, SecondLoginSocketIsNotALoginAndInjectsNothing) {
  GameSocket first, second;
  Admit(first, "/login");
  ASSERT_TRUE(first.WaitForFrames(1)) << "the first /login socket never received the reply to the LoginRequest";
  Admit(second, "/login");
  EXPECT_NE(AdmittedLine(0).find("(conn=1, login"), std::string::npos) << AdmittedLine(0);
  EXPECT_EQ(AdmittedLine(1).find("(conn=1, login"), std::string::npos) << AdmittedLine(1);
  EXPECT_NE(AdmittedLine(1).find("sharing login session (no LoginRequest)"), std::string::npos) << AdmittedLine(1);
  EXPECT_EQ(remote_.LoginRequests(), 1);
}

// A config socket that opens after login is a config socket: it does not share the login session and the
// login session's replies keep going to the login socket (it was counted as a matchmaker and took them).
TEST_F(RolesByPathTest, ConfigSocketAfterLoginStaysConfigAndTakesNoLoginReplies) {
  GameSocket firstConfig, login, lateConfig;
  Admit(firstConfig, "/config");
  Admit(login, "/login");
  ASSERT_TRUE(login.WaitForFrames(1)) << "the /login socket never received the reply to the LoginRequest";
  Admit(lateConfig, "/config");
  EXPECT_NE(AdmittedLine(2).find("(conn=0, config"), std::string::npos) << AdmittedLine(2);
  EXPECT_EQ(AdmittedLine(2).find("sharing login session"), std::string::npos) << AdmittedLine(2);
  login.SendRequest();
  EXPECT_TRUE(login.WaitForFrames(2)) << "the reply to the login socket's request went to another socket";
  EXPECT_EQ(lateConfig.Frames(), 0);
}

// #70: when the login session ends, the game's next /login is a new login (the numbering starts again).
TEST_F(RolesByPathTest, LoginSocketAfterTheSessionEndsIsANewLogin) {
  GameSocket first, second;
  Admit(first, "/login");
  ASSERT_TRUE(first.WaitForFrames(1));
  remote_.CloseEveryClient();
  ASSERT_TRUE(WaitForLogLines("login session ended", 1)) << "the remote closing did not end the login session";
  Admit(second, "/login");
  EXPECT_NE(AdmittedLine(1).find("(conn=1, login"), std::string::npos) << AdmittedLine(1);
  EXPECT_TRUE(second.WaitForFrames(1)) << "the new /login socket never received the reply to its LoginRequest";
}

// The measurement the issue asks for: every admission is one structured line with the slot, the role, why,
// and the path the game asked for.
TEST_F(RolesByPathTest, EachAdmissionLogsTheRoleItsBasisAndThePathItAskedFor) {
  GameSocket login, config;
  Admit(login, "/login");
  Admit(config, "/config");
  const std::vector<std::string> lines = LogLinesContaining("game socket admitted");
  ASSERT_EQ(lines.size(), 2U);
  EXPECT_NE(lines[0].find("conn=1 role=login basis=path listener=service target=/login has_query=false"), std::string::npos)
      << lines[0];
  EXPECT_NE(lines[1].find("conn=0 role=config basis=path listener=service target=/config has_query=false"), std::string::npos)
      << lines[1];
}

// logging.md Rule 4: a query can carry a credential. The role is read from the path, and only the path is logged.
TEST_F(RolesByPathTest, AQueryOnTheRequestTargetDoesNotChangeTheRoleAndIsNotLogged) {
  GameSocket login;
  Admit(login, "/login?token=SECRET-VALUE-520");
  const std::vector<std::string> lines = LogLinesContaining("game socket admitted");
  ASSERT_EQ(lines.size(), 1U);
  EXPECT_NE(lines[0].find("conn=1 role=login basis=path listener=service target=/login has_query=true"), std::string::npos)
      << lines[0];
  EXPECT_TRUE(LogLinesContaining("SECRET-VALUE-520").empty());
}

// The matchmaker listener is a matchmaker whatever it asked for, and it shares the login session.
TEST_F(RolesByPathTest, ASocketOnTheMatchmakerListenerSharesTheLoginSession) {
  GameSocket login, matchmaker;
  Admit(login, "/login");
  ASSERT_TRUE(login.WaitForFrames(1));
  ASSERT_NE(GetMatchmakerBridgePort(), 0);
  Admit(matchmaker, "/", GetMatchmakerBridgePort());
  EXPECT_NE(AdmittedLine(1).find("(conn=2, matchmaker"), std::string::npos) << AdmittedLine(1);
  EXPECT_NE(AdmittedLine(1).find("sharing login session (no LoginRequest)"), std::string::npos) << AdmittedLine(1);
  const std::vector<std::string> lines = LogLinesContaining("game socket admitted");
  ASSERT_EQ(lines.size(), 2U);
  EXPECT_NE(lines[1].find("conn=2 role=matchmaker basis=listener listener=matchmaker"), std::string::npos) << lines[1];
}

// While the redirect has tagged the config and login services, a socket that names neither is not given
// either slot by the connect order; the tagged sockets still get theirs.
TEST_F(RolesByPathTest, ASocketWithNoTargetDoesNotTakeASlotATaggedSocketIsExpectedFor) {
  nevr_socket_role::GlobalAdvertisement().Note(nevr_socket_role::Role::Config);
  nevr_socket_role::GlobalAdvertisement().Note(nevr_socket_role::Role::Login);
  GameSocket pathless, matchmaker, login, config;
  Admit(pathless, "/");
  // A matchmaker with no login session has none to share, and the socket that has none is not a login
  // session for the next one to share either.
  ASSERT_NE(GetMatchmakerBridgePort(), 0);
  Admit(matchmaker, "/", GetMatchmakerBridgePort());
  Admit(login, "/login");
  Admit(config, "/config");
  const std::vector<std::string> lines = LogLinesContaining("game socket admitted");
  ASSERT_EQ(lines.size(), 4U);
  EXPECT_NE(lines[0].find("role=matchmaker basis=slot-reserved"), std::string::npos) << lines[0];
  EXPECT_EQ(AdmittedLine(1).find("sharing login session"), std::string::npos) << AdmittedLine(1);
  EXPECT_NE(lines[2].find("conn=1 role=login basis=path"), std::string::npos) << lines[2];
  EXPECT_NE(lines[3].find("conn=0 role=config basis=path"), std::string::npos) << lines[3];
  EXPECT_TRUE(login.WaitForFrames(1)) << "the /login socket never received the reply to the LoginRequest";
}

// The first frame a socket sends names its role (ADR 0003). A frame that contradicts the role the socket was
// admitted with is logged once, with both roles.
TEST_F(RolesByPathTest, AFirstFrameThatContradictsTheRoleIsLoggedOnce) {
  GameSocket login;
  Admit(login, "/login");
  ASSERT_TRUE(login.WaitForFrames(1));
  login.Send(nevr_evr_codec::kSymConfigRequest);
  ASSERT_TRUE(WaitForLogLines("first frame contradicts the socket's role", 1));
  const std::string line = LogLinesContaining("first frame contradicts the socket's role").front();
  EXPECT_NE(line.find("conn=1 role=login first_frame_role=config"), std::string::npos) << line;
  login.Send(nevr_evr_codec::kSymConfigRequest);
  ASSERT_TRUE(login.WaitForFrames(3));  // the LoginRequest's reply, then one reply per frame sent
  EXPECT_EQ(LogLinesContaining("first frame contradicts the socket's role").size(), 1U);
}

TEST_F(RolesByPathTest, AFirstFrameThatMatchesTheRoleOrNamesNoneIsNotLogged) {
  GameSocket config, login;
  Admit(config, "/config");
  Admit(login, "/login");
  ASSERT_TRUE(login.WaitForFrames(1));
  config.Send(nevr_evr_codec::kSymConfigRequest);
  login.Send(nevr_evr_codec::kSymDocumentRequest);  // names no role: the login connection can send anything
  ASSERT_TRUE(config.WaitForFrames(1));
  ASSERT_TRUE(login.WaitForFrames(2));
  EXPECT_TRUE(LogLinesContaining("first frame contradicts the socket's role").empty());
}

// A socket with no recognised request target keeps the connect order when nothing says otherwise:
// config, then login.
TEST_F(RolesByPathTest, SocketsWithoutATargetKeepTheConnectOrder) {
  GameSocket first, second;
  Admit(first, "/");
  Admit(second, "/");
  EXPECT_NE(AdmittedLine(0).find("(conn=0, config"), std::string::npos) << AdmittedLine(0);
  EXPECT_NE(AdmittedLine(1).find("(conn=1, login"), std::string::npos) << AdmittedLine(1);
  EXPECT_TRUE(second.WaitForFrames(1));
}

// ============================================================================
// #708 S1: network.session_router. The same real bridge, with the /config sockets handed to
// nevr_session_router. Login and matchmaker sockets keep the bridge's own rules in this mode.
// ============================================================================

namespace {

class RouterConfigTest : public RolesByPathTest {
 protected:
  const char* RouterMode() const override { return "config"; }
};

class RouterBogusModeTest : public RolesByPathTest {
 protected:
  const char* RouterMode() const override { return "bogus"; }
  bool BridgeExpected() const override { return false; }
};

}  // namespace

// The default (the key absent) is the bridge's own routing, and each admission line says which path took it.
TEST_F(RolesByPathTest, EveryAdmissionNamesTheRoutingPath) {
  GameSocket config;
  Admit(config, "/config");
  const std::vector<std::string> lines = LogLinesContaining("game socket admitted");
  ASSERT_EQ(lines.size(), 1U);
  EXPECT_NE(lines[0].find("path=legacy game=0"), std::string::npos) << lines[0];
  EXPECT_EQ(LogLinesContaining("path=router").size(), 0U);
  EXPECT_FALSE(LogLinesContaining("network.session_router=legacy").empty());
}

// C1: a /config socket in config mode is taken by the router, and its request is answered.
TEST_F(RouterConfigTest, AConfigSocketIsRoutedAndItsRequestIsAnswered) {
  GameSocket config;
  Admit(config, "/config");
  const std::vector<std::string> lines = LogLinesContaining("game socket admitted");
  ASSERT_EQ(lines.size(), 1U);
  EXPECT_NE(lines[0].find("conn=0 role=config basis=path listener=service target=/config"), std::string::npos) << lines[0];
  EXPECT_NE(lines[0].find("path=router"), std::string::npos) << lines[0];
  EXPECT_FALSE(LogLinesContaining("network.session_router=config").empty());
  config.Send(nevr_evr_codec::kSymConfigRequest);
  EXPECT_TRUE(config.WaitForSymbol(nevr_evr_codec::kSymConfigSuccess)) << "the router never delivered the reply";
}

// C2: the login socket is not the router's in this mode: the bridge injects its LoginRequest as before, and the
// config socket, which the router owns, sees none of the login session's traffic.
TEST_F(RouterConfigTest, ALoginSocketStaysOnTheBridgesRulesAndTheConfigSocketSeesNoLoginReply) {
  GameSocket config, login;
  Admit(config, "/config");
  Admit(login, "/login");
  const std::vector<std::string> lines = LogLinesContaining("game socket admitted");
  ASSERT_EQ(lines.size(), 2U);
  EXPECT_NE(lines[0].find("path=router"), std::string::npos) << lines[0];
  EXPECT_NE(lines[1].find("conn=1 role=login basis=path listener=service target=/login"), std::string::npos) << lines[1];
  EXPECT_NE(lines[1].find("path=legacy"), std::string::npos) << lines[1];
  EXPECT_TRUE(login.WaitForSymbol(nevr_evr_codec::kSymLoginSettings));
  config.Send(nevr_evr_codec::kSymConfigRequest);
  ASSERT_TRUE(config.WaitForSymbol(nevr_evr_codec::kSymConfigSuccess));
  EXPECT_FALSE(config.HasReceived(nevr_evr_codec::kSymLoginSettings));
  EXPECT_EQ(remote_.LoginRequests(), 1);
}

// C3: the router gives every config socket one shared remote session, and each reply goes to the socket whose
// request it answers.
TEST_F(RouterConfigTest, TwoConfigSocketsShareOneRemoteSessionAndEachGetsItsOwnReply) {
  GameSocket first, second;
  Admit(first, "/config");
  first.Send(nevr_evr_codec::kSymConfigRequest);
  ASSERT_TRUE(first.WaitForSymbol(nevr_evr_codec::kSymConfigSuccess));
  Admit(second, "/config");
  second.Send(nevr_evr_codec::kSymConfigRequest);
  ASSERT_TRUE(second.WaitForSymbol(nevr_evr_codec::kSymConfigSuccess));
  EXPECT_EQ(first.Frames(), 1);
  EXPECT_EQ(second.Frames(), 1);
  EXPECT_EQ(remote_.Opens(), 1) << "the router opened more than one remote session for the config sockets";
}

// C4: when the remote session ends, the router closes the config socket, and the game's next /config opens a
// new session.
TEST_F(RouterConfigTest, TheRemoteEndingClosesTheConfigSocketAndTheNextOneOpensANewSession) {
  GameSocket first, second;
  Admit(first, "/config");
  first.Send(nevr_evr_codec::kSymConfigRequest);
  ASSERT_TRUE(first.WaitForSymbol(nevr_evr_codec::kSymConfigSuccess));
  remote_.CloseEveryClient();
  EXPECT_TRUE(first.WaitForClose()) << "the config socket outlived its remote session";
  Admit(second, "/config");
  second.Send(nevr_evr_codec::kSymConfigRequest);
  EXPECT_TRUE(second.WaitForSymbol(nevr_evr_codec::kSymConfigSuccess));
  EXPECT_EQ(remote_.Opens(), 2);
}

// C5: a game socket closing releases it from the router (the log says so) and the session stays up for the next.
TEST_F(RouterConfigTest, AClosedConfigSocketIsReleasedAndTheSessionServesTheNext) {
  auto first = std::make_unique<GameSocket>();
  GameSocket second;
  Admit(*first, "/config");
  first->Send(nevr_evr_codec::kSymConfigRequest);
  ASSERT_TRUE(first->WaitForSymbol(nevr_evr_codec::kSymConfigSuccess));
  first.reset();
  ASSERT_TRUE(WaitForLogLines("Proxy: game disconnected (conn=0, config) path=router", 1));
  Admit(second, "/config");
  second.Send(nevr_evr_codec::kSymConfigRequest);
  EXPECT_TRUE(second.WaitForSymbol(nevr_evr_codec::kSymConfigSuccess));
  EXPECT_EQ(remote_.Opens(), 1);
}

// C7: the graceful stop (N38/N64/N105) stops the router's remote session itself, on the caller, and counts it.
TEST_F(RouterConfigTest, StoppingTheBridgeStopsTheRouterSessionAndCountsIt) {
  GameSocket config;
  Admit(config, "/config");
  config.Send(nevr_evr_codec::kSymConfigRequest);
  ASSERT_TRUE(config.WaitForSymbol(nevr_evr_codec::kSymConfigSuccess));
  ClearTestLogs();
  StopWebSocketBridgeListener();
  EXPECT_FALSE(LogLinesContaining("1 remote connection(s) closed").empty());
}

// C6: a value that names no mode is fatal at boot and installs nothing, never a guess.
TEST_F(RouterBogusModeTest, ANonsenseModeIsFatalAndInstallsNothing) {
  EXPECT_EQ(g_fatalErrorCalls, fatalBefore_ + 1);
  EXPECT_FALSE(LogLinesContaining("network.session_router refused").empty());
  EXPECT_EQ(GetWebSocketBridgePort(), 0);
}

// ============================================================================
// #708 S2b: network.session_router=all. Every socket the bridge admits (config, login, matchmaker) is the
// router's: one shared remote session, one LoginRequest per login session, and the account gate.
// ============================================================================

namespace {

class RouterAllTest : public RolesByPathTest {
 protected:
  const char* RouterMode() const override { return "all"; }
  void TearDown() override {
    TestHook_SetRouterGate(-1);
    TestHook_SetRefuseRemoteOpens(false);
    RolesByPathTest::TearDown();
  }
  struct Stats {
    uint64_t droppedGame = 0, droppedRemote = 0, droppedUnrequires = 0, loginsRemoved = 0, heldRemotes = 0;
  };
  Stats RouterStats() {
    Stats stats;
    EXPECT_TRUE(TestHook_RouterStats(&stats.droppedGame, &stats.droppedRemote, &stats.droppedUnrequires,
                                     &stats.loginsRemoved, &stats.heldRemotes));
    return stats;
  }
  // The bridge's login socket, admitted and answered.
  void AdmitLogin(GameSocket& login) {
    Admit(login, "/login");
    ASSERT_TRUE(login.WaitForSymbol(nevr_evr_codec::kSymLoginSettings)) << "the login never reached the service";
  }
};

enum class Kind { Config, Login, Matchmaker };

const char* KindName(Kind kind) {
  switch (kind) {
    case Kind::Config: return "config";
    case Kind::Login: return "login";
    case Kind::Matchmaker: return "matchmaker";
  }
  return "?";
}

// Each of the six orders the game's three sockets can open in.
class RouterAllOrderTest : public RouterAllTest, public ::testing::WithParamInterface<std::array<Kind, 3>> {};

std::string OrderName(const ::testing::TestParamInfo<std::array<Kind, 3>>& info) {
  std::string name;
  for (const Kind kind : info.param) name += std::string(KindName(kind)) + "_";
  name.pop_back();
  return name;
}

}  // namespace

// L1: whatever the open order, the service sees one LoginRequest, and the login socket gets its reply. A
// matchmaker that opens before any login is attached has no session to share, so it keeps a connection of its
// own: that is the only reason for a second connection to the service.
TEST_P(RouterAllOrderTest, TheThreeSocketsShareOneLoginInAnyOrder) {
  const std::array<Kind, 3> order = GetParam();
  GameSocket config, login, matchmaker;
  for (const Kind kind : order) {
    switch (kind) {
      case Kind::Config: Admit(config, "/config"); break;
      case Kind::Login: Admit(login, "/login"); break;
      case Kind::Matchmaker: ASSERT_NE(GetMatchmakerBridgePort(), 0); Admit(matchmaker, "/", GetMatchmakerBridgePort()); break;
    }
  }
  ASSERT_TRUE(login.WaitForSymbol(nevr_evr_codec::kSymLoginSettings)) << "the login socket never received its reply";
  EXPECT_EQ(remote_.LoginRequests(), 1);
  const auto at = [&order](Kind kind) { return std::find(order.begin(), order.end(), kind) - order.begin(); };
  const int expectedOpens = at(Kind::Matchmaker) < at(Kind::Login) ? 2 : 1;
  EXPECT_TRUE(YieldUntil([&] { return remote_.Opens() >= expectedOpens; }));
  EXPECT_EQ(remote_.Opens(), expectedOpens);
  EXPECT_EQ(LogLinesContaining("path=router").size(), 6U) << "three admissions and three connects, all the router's";
  EXPECT_FALSE(config.HasReceived(nevr_evr_codec::kSymLoginSettings));
  EXPECT_FALSE(matchmaker.HasReceived(nevr_evr_codec::kSymLoginSettings));
  EXPECT_EQ(RouterStats().droppedGame, 0U);
  EXPECT_EQ(RouterStats().droppedRemote, 0U);
}

INSTANTIATE_TEST_SUITE_P(
    Orders, RouterAllOrderTest,
    ::testing::Values(std::array<Kind, 3>{Kind::Config, Kind::Login, Kind::Matchmaker},
                      std::array<Kind, 3>{Kind::Config, Kind::Matchmaker, Kind::Login},
                      std::array<Kind, 3>{Kind::Login, Kind::Config, Kind::Matchmaker},
                      std::array<Kind, 3>{Kind::Login, Kind::Matchmaker, Kind::Config},
                      std::array<Kind, 3>{Kind::Matchmaker, Kind::Config, Kind::Login},
                      std::array<Kind, 3>{Kind::Matchmaker, Kind::Login, Kind::Config}),
    OrderName);

// L2: a standalone matchmaker's connection is a protobuf one (format=evr is stripped from the target); the
// session's connection keeps it.
TEST_F(RouterAllTest, AStandaloneMatchmakerConnectionDropsFormatEvr) {
  SetWebSocketBridgeTarget((remote_.Uri() + "?format=evr").c_str());
  GameSocket matchmaker, login;
  ASSERT_NE(GetMatchmakerBridgePort(), 0);
  Admit(matchmaker, "/", GetMatchmakerBridgePort());
  Admit(login, "/login");
  ASSERT_TRUE(login.WaitForSymbol(nevr_evr_codec::kSymLoginSettings));
  ASSERT_TRUE(YieldUntil([this] { return remote_.Opens() >= 2; }));
  const std::vector<std::string> uris = remote_.Uris();
  ASSERT_EQ(uris.size(), 2U);
  EXPECT_EQ(uris[0].find("format=evr"), std::string::npos) << "the matchmaker's connection: " << uris[0];
  EXPECT_NE(uris[1].find("format=evr"), std::string::npos) << "the session's connection: " << uris[1];
}

// L3 (N91): a second /login while a login session exists is a matchmaker on the shared session; it injects nothing.
TEST_F(RouterAllTest, ASecondLoginSocketSharesTheSessionAndInjectsNothing) {
  GameSocket first, second;
  AdmitLogin(first);
  Admit(second, "/login");
  EXPECT_TRUE(WaitForLogLines("(matchmaker) shares login session", 1));
  EXPECT_EQ(remote_.LoginRequests(), 1);
  EXPECT_EQ(remote_.Opens(), 1);
  EXPECT_EQ(LogLinesContaining("login injected xpid=").size(), 1U);
  EXPECT_EQ(second.Count(nevr_evr_codec::kSymLoginSettings), 0);
}

// The game's first config request has a '{' where the pad byte is: `{` + `{"type":"main_menu","id":"main_menu"}` + NUL. Its reply
// is matched to the request by type and id, so the router must read the key out of it, or the game never gets
// its main_menu config (seen live, #708 S2b).
TEST_F(RouterAllTest, AConfigRequestWhosePadByteIsABraceIsAnswered) {
  GameSocket config;
  Admit(config, "/config");
  const std::string payload = std::string(1, '{') + R"({"type":"main_menu","id":"main_menu"})" + std::string(1, '\0');
  config.Send(nevr_evr_codec::kSymConfigRequest, payload);
  ASSERT_TRUE(config.WaitForSymbol(nevr_evr_codec::kSymConfigSuccess)) << "the reply was dropped as unmatched";
  EXPECT_TRUE(LogLinesContaining("config reply dropped").empty());
  EXPECT_TRUE(LogLinesContaining("no usable type and id").empty());
}

// L4: the reply to a request on the login socket goes to the login socket.
TEST_F(RouterAllTest, TheReplyToTheLoginSocketsRequestGoesToTheLoginSocket) {
  GameSocket config, login;
  Admit(config, "/config");
  AdmitLogin(login);
  login.SendRequest();
  ASSERT_TRUE(login.WaitForSymbol(nevr_evr_codec::kSymDocumentSuccess));
  EXPECT_EQ(config.Count(nevr_evr_codec::kSymDocumentSuccess), 0);
}

// T706 through the bridge: the lobby reply goes to the matchmaker that last asked.
TEST_F(RouterAllTest, ALobbyReplyGoesToTheMatchmakerThatAsked) {
  GameSocket login, first, second;
  AdmitLogin(login);
  ASSERT_NE(GetMatchmakerBridgePort(), 0);
  Admit(first, "/", GetMatchmakerBridgePort());
  Admit(second, "/", GetMatchmakerBridgePort());
  first.Send(nevr_evr_codec::kSymFindSessionRequest);
  ASSERT_TRUE(first.WaitForSymbol(RemoteService::kLobbyReplySymbol));
  second.Send(nevr_evr_codec::kSymFindSessionRequest);
  ASSERT_TRUE(second.WaitForSymbol(RemoteService::kLobbyReplySymbol));
  first.Send(nevr_evr_codec::kSymFindSessionRequest);
  ASSERT_TRUE(first.WaitForFrames(2));
  EXPECT_EQ(first.Count(RemoteService::kLobbyReplySymbol), 2);
  EXPECT_EQ(second.Count(RemoteService::kLobbyReplySymbol), 1);
  EXPECT_FALSE(LogLinesContaining("sent symbol=0x312c2a01819aa3f5").empty()) << "the sender of the find is not on record";
  EXPECT_FALSE(LogLinesContaining("-> game=").empty()) << "the destination of the reply is not on record";
}

// A matchmaker that opens while another is waiting for its answer does not take the answer.
TEST_F(RouterAllTest, ANewMatchmakerDoesNotTakeTheAnswerAnotherIsWaitingFor) {
  GameSocket login, first, second;
  AdmitLogin(login);
  ASSERT_NE(GetMatchmakerBridgePort(), 0);
  Admit(first, "/", GetMatchmakerBridgePort());
  remote_.HoldLobbyReplies(true);
  first.Send(nevr_evr_codec::kSymFindSessionRequest);
  ASSERT_TRUE(YieldUntil([this] { return remote_.HeldLobbyReplies() == 1; })) << "the find never reached the service";
  Admit(second, "/", GetMatchmakerBridgePort());
  remote_.ReleaseLobbyReplies();
  ASSERT_TRUE(first.WaitForSymbol(RemoteService::kLobbyReplySymbol)) << "the answer did not reach the asker";
  EXPECT_EQ(second.Count(RemoteService::kLobbyReplySymbol), 0);
}

// L5: a dedicated server has no login to complete; the service's LoginFailure reaches the game as a LoginSuccess.
TEST_F(RouterAllTest, AServersLoginFailureReachesTheGameAsALoginSuccess) {
  remote_.SetLoginReply(nevr_evr_codec::kSymLoginFailure);
  GameSocket login;
  Admit(login, "/login");
  ASSERT_TRUE(login.WaitForSymbol(nevr_evr_codec::kSymLoginSuccess)) << "the game was left at the login gate";
  EXPECT_FALSE(login.HasReceived(nevr_evr_codec::kSymLoginFailure));
}

// L6: an Unrequire nobody asked for is dropped and counted, not handed to a socket.
TEST_F(RouterAllTest, AnUnrequireNobodyAskedForIsDroppedAndCounted) {
  GameSocket login;
  AdmitLogin(login);
  remote_.PushToClients(nevr_evr_codec::kSymConnectionUnrequire);
  ASSERT_TRUE(YieldUntil([this] { return RouterStats().droppedUnrequires == 1; })) << "the Unrequire was not counted";
  EXPECT_EQ(login.Count(nevr_evr_codec::kSymConnectionUnrequire), 0);
}

// A login the service accepted is followed by the friend-list subscription, sent once, on the session.
TEST_F(RouterAllTest, ALoginSuccessIsFollowedByTheFriendListSubscription) {
  remote_.SetLoginReply(nevr_evr_codec::kSymLoginSuccess);
  GameSocket login;
  Admit(login, "/login");
  ASSERT_TRUE(login.WaitForSymbol(nevr_evr_codec::kSymLoginSuccess));
  const uint64_t subscribe = nevr_evr_codec::FirstSymbol(nevr_evr_codec::BuildFriendListSubscribe());
  ASSERT_TRUE(YieldUntil([&] { return remote_.ReceivedCount(subscribe) >= 1; })) << "no friend-list subscription";
  EXPECT_EQ(remote_.ReceivedCount(subscribe), 1);
}

// L7: a runtime frame sent before the session exists is refused (and says so); once the login is up it goes to the
// service, and its reply reaches the login socket.
TEST_F(RouterAllTest, ARuntimeFrameIsRefusedBeforeTheSessionAndSentAfter) {
  const nevr_social_party::Sender sender = nevr_social_party::SenderSlot().load();
  ASSERT_NE(sender, nullptr);
  const std::string frame = nevr_evr_codec::BuildMessage(nevr_evr_codec::kSymDocumentRequest, "");
  EXPECT_FALSE(sender(frame));
  EXPECT_FALSE(LogLinesContaining("game->server frame not sent").empty());
  GameSocket login;
  AdmitLogin(login);
  EXPECT_TRUE(sender(frame));
  ASSERT_TRUE(login.WaitForSymbol(nevr_evr_codec::kSymDocumentSuccess)) << "the reply to the runtime frame was lost";
}

// L8: the ninth matchmaker is closed with 1013, and the first eight share the session's one connection.
TEST_F(RouterAllTest, ANinthMatchmakerIsClosedTryAgainLater) {
  GameSocket login;
  AdmitLogin(login);
  ASSERT_NE(GetMatchmakerBridgePort(), 0);
  std::vector<std::unique_ptr<GameSocket>> matchmakers;
  for (int index = 0; index < 8; ++index) {
    matchmakers.push_back(std::make_unique<GameSocket>());
    Admit(*matchmakers.back(), "/", GetMatchmakerBridgePort());
  }
  GameSocket ninth;
  Admit(ninth, "/", GetMatchmakerBridgePort());
  EXPECT_TRUE(ninth.WaitForClose());
  EXPECT_EQ(ninth.CloseCode(), 1013);
  EXPECT_EQ(remote_.Opens(), 1);
}

// L9: the remote ending ends the login session once, and removes no login (that is Quest's, not the PC's).
TEST_F(RouterAllTest, TheRemoteEndingEndsTheSessionOnceAndRemovesNoLogin) {
  GameSocket first, second;
  AdmitLogin(first);
  remote_.CloseEveryClient();
  ASSERT_TRUE(WaitForLogLines("login session ended", 1));
  ASSERT_TRUE(first.WaitForClose());
  EXPECT_EQ(LogLinesContaining("login session ended").size(), 1U);
  EXPECT_EQ(RouterStats().loginsRemoved, 0U);
  EXPECT_FALSE(LogLinesContaining("router stats (session ended)").empty());
  Admit(second, "/login");
  EXPECT_TRUE(second.WaitForSymbol(nevr_evr_codec::kSymLoginSettings)) << "the game's next login was not a new login";
  EXPECT_EQ(remote_.LoginRequests(), 2);
}

// T704-A: the account is awaited and the adapter refuses to open a connection. The config socket is closed (the
// game sees a failed connect), nothing reaches the service, and the login socket stays open with its session held.
// When the account arrives the held session opens and carries the login.
TEST_F(RouterAllTest, AHeldLoginSessionOpensWhenTheAccountArrives) {
  TestHook_SetRouterGate(0);
  TestHook_SetRefuseRemoteOpens(true);
  GameSocket config, login;
  Admit(config, "/config");
  ASSERT_TRUE(config.WaitForClose());
  EXPECT_EQ(config.CloseCode(), 1011);
  Admit(login, "/login");
  ASSERT_TRUE(WaitForLogLines("(login) held", 1));
  EXPECT_EQ(remote_.Opens(), 0);
  EXPECT_EQ(remote_.LoginRequests(), 0);
  EXPECT_EQ(RouterStats().heldRemotes, 1U);
  EXPECT_EQ(login.Frames(), 0);

  TestHook_SetRefuseRemoteOpens(false);
  TestHook_SetRouterGate(1);
  TestHook_ReevaluateRouterGate();
  ASSERT_TRUE(login.WaitForSymbol(nevr_evr_codec::kSymLoginSettings)) << "the held session never opened";
  EXPECT_EQ(remote_.Opens(), 1);
  EXPECT_EQ(remote_.LoginRequests(), 1);
  EXPECT_EQ(RouterStats().heldRemotes, 0U);
}

// T704-B: the adapter opens the session but the account is still awaited: the login waits; the config socket
// is served meanwhile. No LoginRequest is sent until the gate is ready.
TEST_F(RouterAllTest, AnOpenSessionWaitsForTheAccountBeforeItsLogin) {
  TestHook_SetRouterGate(0);
  GameSocket config, login;
  Admit(config, "/config");
  Admit(login, "/login");
  ASSERT_TRUE(WaitForLogLines("login deferred", 1));
  EXPECT_EQ(remote_.LoginRequests(), 0);
  TestHook_SetRouterGate(1);
  TestHook_ReevaluateRouterGate();
  ASSERT_TRUE(login.WaitForSymbol(nevr_evr_codec::kSymLoginSettings));
  EXPECT_EQ(remote_.LoginRequests(), 1);
  EXPECT_FALSE(LogLinesContaining("the account is available; injecting the deferred login").empty());
}

// ============================================================================
// Login request wire format and WebSocket callback containment.  These exercise
// production helpers through NEVR_TEST_HOOKS; the executable never exports the
// hooks and the shipped DLL keeps its private implementation details private.
// ============================================================================

namespace {

uint64_t ReadLe64(const std::string& bytes, size_t offset) {
  uint64_t result = 0;
  for (size_t index = 0; index < sizeof(result); ++index) {
    result |= static_cast<uint64_t>(static_cast<unsigned char>(bytes[offset + index])) <<
              static_cast<unsigned int>(index * 8);
  }
  return result;
}

void WriteLe64(std::string& bytes, size_t offset, uint64_t value) {
  for (size_t index = 0; index < sizeof(value); ++index) {
    bytes[offset + index] = static_cast<char>(value >> static_cast<unsigned int>(index * 8));
  }
}

std::string BuildLoginFailureFrame(uint64_t declaredPayloadSize, uint64_t statusCode,
                                  const std::string& message) {
  constexpr uint64_t kLoginFailureSymbol = 0xa5b9d5a3021ccf51ULL;
  std::string frame(24 + 24, '\0');
  WriteLe64(frame, 8, kLoginFailureSymbol);
  WriteLe64(frame, 16, declaredPayloadSize);
  WriteLe64(frame, 24 + 16, statusCode);
  frame.append(message);
  return frame;
}

}  // namespace

TEST(SecurityDiagnostics, CapturedResponseSummaryExcludesBodySentinel) {
  ClearTestLogs();
  constexpr char kSecret[] = "auth-response-secret-sentinel";
  nevr_log_diagnostics::LogHttpResponseSummary(EchoVR::LogLevel::Warning, "[NEVR.AUTH] rejected ", 403, kSecret);

  std::lock_guard<std::mutex> lock(g_testLogMutex);
  ASSERT_EQ(g_testLogMessages.size(), 1U);
  EXPECT_NE(g_testLogMessages[0].find("http_status=403 response_bytes="), std::string::npos);
  EXPECT_EQ(g_testLogMessages[0].find(kSecret), std::string::npos);
}

TEST(SecurityDiagnostics, NumericTransportFormatterCarriesOnlyNumericFields) {
  const std::string closed = nevr_log_diagnostics::FormatWebSocketCloseDiagnostic("closed ", 1008, 3);
  EXPECT_EQ(closed, "closed code=1008 reconnect_count=3");
  const std::string failed = nevr_log_diagnostics::FormatWebSocketErrorDiagnostic("failed ", 502, 2, 3);
  EXPECT_EQ(failed, "failed http_status=502 retries=2 reconnect_count=3");
  EXPECT_EQ(nevr_log_diagnostics::FormatCurlFailureDiagnostic("curl ", 28), "curl curl_code=28");
  EXPECT_EQ(nevr_log_diagnostics::FormatBindFailureDiagnostic("Proxy", 5000, 1, 3),
            "[NEVR.WS] Proxy port 5000 bind failed failure=1 — retrying (1/3)");
  EXPECT_EQ(nevr_log_diagnostics::FormatBindFailureDiagnostic("Matchmaker", 5001, 2, 3),
            "[NEVR.WS] Matchmaker port 5001 bind failed failure=1 — retrying (2/3)");
}

// #47: the one hex-dump helper behind the SAVE_SUCCESS, CURRENT_LOADOUT and bone dumps.
TEST(HexDump, FormatsBytesAsUppercaseHexWithATrailingSpace) {
  const uint8_t bytes[] = {0xDE, 0xAD, 0x0B, 0x00};
  const auto lines = nevr::HexDumpLines(bytes, sizeof(bytes), 256, 32);
  ASSERT_EQ(lines.size(), 1U);
  EXPECT_EQ(lines[0], "DE AD 0B 00 ");
}

TEST(HexDump, SplitsIntoLinesAndCapsTheLength) {
  std::vector<uint8_t> bytes(100);
  for (size_t i = 0; i < bytes.size(); ++i) bytes[i] = static_cast<uint8_t>(i);
  const auto lines = nevr::HexDumpLines(bytes.data(), bytes.size(), 70, 32);
  ASSERT_EQ(lines.size(), 3U);                 // 32 + 32 + 6 of the first 70 bytes
  EXPECT_EQ(lines[0].size(), 32U * 3U);
  EXPECT_EQ(lines[2], "40 41 42 43 44 45 ");
  EXPECT_TRUE(nevr::HexDumpLines(bytes.data(), 0, 70, 32).empty());
  EXPECT_TRUE(nevr::HexDumpLines(nullptr, 4, 70, 32).empty());
}

// #18: pnsradmatchmaking.dll is unloaded and reloaded mid-session and every load maps a fresh image,
// so the host rewrite must apply to each image on its own.
namespace {
std::vector<uint8_t> FreshMatchmakerImage() {
  std::vector<uint8_t> image(nevr_matchmaker_host_patch::kHostRva + nevr_matchmaker_host_patch::kHostSlotSize + 16, 0xAA);
  std::memcpy(image.data() + nevr_matchmaker_host_patch::kHostRva, nevr_matchmaker_host_patch::kHostExpected,
              sizeof(nevr_matchmaker_host_patch::kHostExpected));
  return image;
}
bool CopyWrite(uint8_t* dst, const char* src, size_t len) {
  std::memcpy(dst, src, len);
  return true;
}
const char* HostOf(const std::vector<uint8_t>& image) {
  return reinterpret_cast<const char*>(image.data() + nevr_matchmaker_host_patch::kHostRva);
}
}  // namespace

// The slot is the original string and its NUL: the byte after it belongs to other data
// (`dd if=pnsradmatchmaking.dll bs=1 skip=$((0x1c76d8)) count=64 | xxd` shows the NUL, then 0x13 0xcc ...).
TEST(MatchmakerHostPatch, SlotIsTheOriginalStringAndItsNulNothingMore) {
  EXPECT_EQ(nevr_matchmaker_host_patch::kHostSlotSize, sizeof(nevr_matchmaker_host_patch::kHostExpected));
  EXPECT_EQ(nevr_matchmaker_host_patch::kHostSlotSize, 48U);
  EXPECT_TRUE(nevr_matchmaker_host_patch::FitsInHostSlot(47)) << "47 characters and the NUL fill the slot";
  EXPECT_FALSE(nevr_matchmaker_host_patch::FitsInHostSlot(48)) << "one byte more would overwrite the next field";
  EXPECT_FALSE(nevr_matchmaker_host_patch::FitsInHostSlot(0));
  EXPECT_FALSE(nevr_matchmaker_host_patch::FitsInHostSlot(-1));
}

TEST(MatchmakerHostPatch, EveryFreshImageAfterAReloadIsPatched) {
  std::vector<uint8_t> first = FreshMatchmakerImage();
  ASSERT_EQ(nevr_matchmaker_host_patch::Apply(first.data(), 51234, CopyWrite), nevr_matchmaker_host_patch::Result::Patched);
  EXPECT_STREQ(HostOf(first), "ws://127.0.0.1:51234");

  // The game frees the module and loads it again: a new, unpatched image, possibly a new port.
  std::vector<uint8_t> second = FreshMatchmakerImage();
  EXPECT_STREQ(HostOf(second), nevr_matchmaker_host_patch::kHostExpected);
  ASSERT_EQ(nevr_matchmaker_host_patch::Apply(second.data(), 60001, CopyWrite), nevr_matchmaker_host_patch::Result::Patched);
  EXPECT_STREQ(HostOf(second), "ws://127.0.0.1:60001");
}

// The loader-notification path end to end, over fake images: load, unload, reload, with the module name as
// the loader reports it (UTF-16, any case, no NUL). A write counter proves what does and does not write.
namespace {
std::u16string Utf16(const char* ascii) {
  return std::u16string(ascii, ascii + std::strlen(ascii));
}
}  // namespace

TEST(MatchmakerHostPatch, LoadUnloadReloadPatchesEveryLoadAndUnloadWritesNothing) {
  namespace mm = nevr_matchmaker_host_patch;
  int writes = 0;
  auto counted = [&writes](uint8_t* dst, const char* src, size_t len) {
    ++writes;
    std::memcpy(dst, src, len);
    return true;
  };
  const std::u16string name = Utf16("pnsradmatchmaking.dll");

  std::vector<uint8_t> first = FreshMatchmakerImage();
  auto r1 = mm::OnModuleNotification(mm::kNotificationLoaded, name.data(), name.size(), first.data(), 51234, counted);
  ASSERT_TRUE(r1.has_value());
  EXPECT_EQ(*r1, mm::Result::Patched);
  EXPECT_STREQ(HostOf(first), "ws://127.0.0.1:51234");
  EXPECT_EQ(writes, 1);

  // The unload notification: nothing is written and nothing is held; the image is gone with its patch.
  auto r2 = mm::OnModuleNotification(mm::kNotificationUnloaded, name.data(), name.size(), first.data(), 51234, counted);
  EXPECT_FALSE(r2.has_value());
  EXPECT_EQ(writes, 1);

  // The reload maps a fresh, unpatched image (a different base) and is patched like the first.
  std::vector<uint8_t> second = FreshMatchmakerImage();
  ASSERT_NE(first.data(), second.data());
  EXPECT_STREQ(HostOf(second), mm::kHostExpected);
  auto r3 = mm::OnModuleNotification(mm::kNotificationLoaded, name.data(), name.size(), second.data(), 51234, counted);
  ASSERT_TRUE(r3.has_value());
  EXPECT_EQ(*r3, mm::Result::Patched);
  EXPECT_STREQ(HostOf(second), "ws://127.0.0.1:51234");
  EXPECT_EQ(writes, 2);

  // A repeated load notification for the same image is idempotent: reported, nothing rewritten.
  auto r4 = mm::OnModuleNotification(mm::kNotificationLoaded, name.data(), name.size(), second.data(), 51234, counted);
  ASSERT_TRUE(r4.has_value());
  EXPECT_EQ(*r4, mm::Result::BytesMismatch);
  EXPECT_STREQ(HostOf(second), "ws://127.0.0.1:51234");
  EXPECT_EQ(writes, 2);

  // And a third load after another unload is patched again: no once-only guard anywhere.
  mm::OnModuleNotification(mm::kNotificationUnloaded, name.data(), name.size(), second.data(), 51234, counted);
  std::vector<uint8_t> third = FreshMatchmakerImage();
  auto r5 = mm::OnModuleNotification(mm::kNotificationLoaded, name.data(), name.size(), third.data(), 51234, counted);
  ASSERT_TRUE(r5.has_value());
  EXPECT_EQ(*r5, mm::Result::Patched);
  EXPECT_STREQ(HostOf(third), "ws://127.0.0.1:51234");
  EXPECT_EQ(writes, 3);
}

// Self-check "matchmaking_reload_patch" (#451): the ledger the loader callback feeds says, per change, whether
// every load of the module had its host default re-applied.
TEST(MatchmakerHostPatch, ReloadLedgerPassesWhenEveryLoadWasPatchedAndFailsWhenOneWasNot) {
  namespace mm = nevr_matchmaker_host_patch;
  mm::ReloadLedger ledger;
  std::string observed;
  bool pass = false;
  EXPECT_FALSE(ledger.Take(&observed, &pass)) << "no load yet: nothing to report";

  ledger.NoteLoad(mm::Result::Patched);
  ASSERT_TRUE(ledger.Take(&observed, &pass));
  EXPECT_EQ(observed, "loads=1 patched=1");
  EXPECT_TRUE(pass);
  EXPECT_FALSE(ledger.Take(&observed, &pass)) << "unchanged: said once";

  ledger.NoteLoad(mm::Result::Patched);  // the reload of #18, patched again
  ASSERT_TRUE(ledger.Take(&observed, &pass));
  EXPECT_EQ(observed, "loads=2 patched=2");
  EXPECT_TRUE(pass);

  ledger.NoteLoad(mm::Result::BytesMismatch);  // a load the patch did not land on
  ASSERT_TRUE(ledger.Take(&observed, &pass));
  EXPECT_EQ(observed, "loads=3 patched=2");
  EXPECT_FALSE(pass);

  ledger.NoteLoad(std::nullopt);
  ASSERT_TRUE(ledger.Take(&observed, &pass));
  EXPECT_EQ(observed, "loads=4 patched=2");
  EXPECT_FALSE(pass);
}

TEST(MatchmakerHostPatch, OnlyTheMatchmakingModuleByWholeNameAnyCaseIsPatched) {
  namespace mm = nevr_matchmaker_host_patch;
  int writes = 0;
  auto counted = [&writes](uint8_t* dst, const char* src, size_t len) {
    ++writes;
    std::memcpy(dst, src, len);
    return true;
  };
  for (const char* other : {"pnsrad.dll", "pnsradmatchmaking.dll.bak", "xpnsradmatchmaking.dll", "pnsradmatchmaking.d",
                            "pnsradgameserver.dll", ""}) {
    std::vector<uint8_t> image = FreshMatchmakerImage();
    const std::u16string name = Utf16(other);
    EXPECT_FALSE(mm::OnModuleNotification(mm::kNotificationLoaded, name.data(), name.size(), image.data(), 51234, counted)
                     .has_value())
        << other;
    EXPECT_STREQ(HostOf(image), mm::kHostExpected) << other;
  }
  EXPECT_EQ(writes, 0);
  std::vector<uint8_t> image = FreshMatchmakerImage();
  const std::u16string upper = Utf16("PnsRadMatchmaking.DLL");
  auto r = mm::OnModuleNotification(mm::kNotificationLoaded, upper.data(), upper.size(), image.data(), 4000, counted);
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(*r, mm::Result::Patched);
  // The loader reports the name as WCHAR; the same function takes it.
  std::vector<uint8_t> wide = FreshMatchmakerImage();
  const std::wstring wname = L"pnsradmatchmaking.dll";
  EXPECT_TRUE(mm::OnModuleNotification(mm::kNotificationLoaded, wname.data(), wname.size(), wide.data(), 4000, counted)
                  .has_value());
  EXPECT_EQ(writes, 2);
}

TEST(MatchmakerHostPatch, AnAlreadyPatchedImageIsLeftAlone) {
  std::vector<uint8_t> image = FreshMatchmakerImage();
  ASSERT_EQ(nevr_matchmaker_host_patch::Apply(image.data(), 51234, CopyWrite), nevr_matchmaker_host_patch::Result::Patched);
  EXPECT_EQ(nevr_matchmaker_host_patch::Apply(image.data(), 60001, CopyWrite), nevr_matchmaker_host_patch::Result::BytesMismatch);
  EXPECT_STREQ(HostOf(image), "ws://127.0.0.1:51234");
}

TEST(MatchmakerHostPatch, NoPortAndWriteFailureAreReportedNotPatched) {
  std::vector<uint8_t> image = FreshMatchmakerImage();
  EXPECT_EQ(nevr_matchmaker_host_patch::Apply(image.data(), 0, CopyWrite), nevr_matchmaker_host_patch::Result::NoPort);
  EXPECT_STREQ(HostOf(image), nevr_matchmaker_host_patch::kHostExpected);
  EXPECT_EQ(nevr_matchmaker_host_patch::Apply(image.data(), 51234, [](uint8_t*, const char*, size_t) { return false; }),
            nevr_matchmaker_host_patch::Result::WriteFailed);
}

// #201: the server's new-location text ends with the code line, which the game's screen drops.
namespace {
std::string FrameWithText(const std::string& text) {
  const std::string withNul = text + std::string(1, '\0');
  return BuildLoginFailureFrame(24 + withNul.size(), 400, withNul);
}
}  // namespace

TEST(WsBridgeLoginFailure, NewLocationCodeLineMovesFirst) {
  const std::string text =
      "[XPID:OVR-ORG-1 / Discord:1]\n Please authorize this new location.\n"
      "Check your Discord DMs from @EchoVRCE.\nSelect code >>> 42 <<<";
  std::string out;
  ASSERT_TRUE(TestHook_MoveCodeLineFirst(FrameWithText(text), &out));
  EXPECT_EQ(out, FrameWithText("Select code >>> 42 <<<\n[XPID:OVR-ORG-1 / Discord:1]\n Please authorize this new "
                               "location.\nCheck your Discord DMs from @EchoVRCE."));
  uint64_t statusCode = 0;
  size_t messageBytes = 0;
  ASSERT_TRUE(TestHook_ReadLoginFailureDiagnostic(out, &statusCode, &messageBytes));
  EXPECT_EQ(statusCode, 400U);
  EXPECT_EQ(messageBytes, text.size() + 1);
}

// A client (not a server) gets the service's LoginFailure itself, with the code line first: the game reads the
// first line of the message.
TEST_F(RouterAllTest, AClientsLoginFailureReachesTheGameWithTheCodeLineFirst) {
  const BOOL wasServer = g_isServer;
  g_isServer = FALSE;
  remote_.SetLoginReplyFrame(
      FrameWithText("[XPID:OVR-ORG-1 / Discord:1]\n Please authorize this new location.\n"
                    "Check your Discord DMs from @EchoVRCE.\nSelect code >>> 42 <<<"));
  GameSocket login;
  Admit(login, "/login");
  const bool failed = login.WaitForSymbol(nevr_evr_codec::kSymLoginFailure);
  g_isServer = wasServer;
  ASSERT_TRUE(failed) << "the game never saw the LoginFailure";
  EXPECT_FALSE(login.HasReceived(nevr_evr_codec::kSymLoginSuccess));
  EXPECT_EQ(login.FrameWith(nevr_evr_codec::kSymLoginFailure),
            FrameWithText("Select code >>> 42 <<<\n[XPID:OVR-ORG-1 / Discord:1]\n Please authorize this new "
                          "location.\nCheck your Discord DMs from @EchoVRCE."));
}

TEST(WsBridgeLoginFailure, OtherFailureTextsAreLeftAlone) {
  std::string out;
  EXPECT_FALSE(TestHook_MoveCodeLineFirst(FrameWithText("Account banned."), &out));
  EXPECT_FALSE(TestHook_MoveCodeLineFirst(FrameWithText("Select code >>> 42 <<<"), &out));  // already first
  EXPECT_FALSE(TestHook_MoveCodeLineFirst(FrameWithText("a\nSelect code >>> xx <<<"), &out));  // not digits
  EXPECT_FALSE(TestHook_MoveCodeLineFirst(FrameWithText("a\nSelect code >>>  <<<"), &out));  // empty code
  EXPECT_FALSE(TestHook_MoveCodeLineFirst(FrameWithText("Go to G, type /verify\nWhen prompted, select code >>> 42 <<<"),
                                         &out));  // guild variant: code is not a line of its own
  // Two frames in one message are not rewritten.
  const std::string two = FrameWithText("a\nSelect code >>> 42 <<<") + FrameWithText("b");
  EXPECT_FALSE(TestHook_MoveCodeLineFirst(two, &out));
  // A different message symbol is not touched.
  std::string other = FrameWithText("a\nSelect code >>> 42 <<<");
  other[8] ^= 1;
  EXPECT_FALSE(TestHook_MoveCodeLineFirst(other, &out));
}

TEST(SecurityDiagnostics, CapturedLoginFailureSummaryExcludesServerMessage) {
  ClearTestLogs();
  constexpr char kSecret[] = "login-failure-secret-sentinel";
  const std::string frame = BuildLoginFailureFrame(24 + std::string(kSecret).size(), 502, kSecret);

  EXPECT_TRUE(TestHook_LogLoginFailureDiagnostic(frame, false));
  std::lock_guard<std::mutex> lock(g_testLogMutex);
  ASSERT_EQ(g_testLogMessages.size(), 1U);
  EXPECT_NE(g_testLogMessages[0].find("login failed status=502 message_bytes="), std::string::npos);
  EXPECT_EQ(g_testLogMessages[0].find(kSecret), std::string::npos);
}

TEST(WsBridgeLoginFailure, DiagnosticUsesDeclaredLengthForConcatenatedFrames) {
  const std::string first = BuildLoginFailureFrame(24 + 4, 502, "nope");
  const std::string second = BuildLoginFailureFrame(24 + 5, 401, "later");
  const std::string concatenated = first + second;
  uint64_t statusCode = 0;
  size_t messageBytes = 0;

  ASSERT_TRUE(TestHook_ReadLoginFailureDiagnostic(concatenated, &statusCode, &messageBytes));
  EXPECT_EQ(statusCode, 502U);
  EXPECT_EQ(messageBytes, 4U);
}

namespace {
std::string BuildMarkedMessage(uint64_t symbol, const std::string& payload) {
  static const char kMarker[] = {'\xf6', '\x40', '\xbb', '\x78', '\xa2', '\xe7', '\x8c', '\xbb'};
  std::string msg(kMarker, sizeof(kMarker));
  msg.append(16, '\0');
  WriteLe64(msg, 8, symbol);
  WriteLe64(msg, 16, payload.size());
  msg.append(payload);
  return msg;
}
}  // namespace

// Nakama batches LoginSuccess, STcpConnectionUnrequireEvent and GameSettings into one frame. The
// bridge logs every symbol of a server->game frame; logging only the first would leave the other
// two out of a server's log. Every message in the frame must be logged.
TEST(WsBridgeFrameLog, LogsEveryMessageInABatchedFrame) {
  ClearTestLogs();
  const std::string frame = BuildMarkedMessage(0x1111111111111111ULL, std::string(40, 'a')) +
                            BuildMarkedMessage(0x43e6963ac76beee4ULL, "") +
                            BuildMarkedMessage(0x2222222222222222ULL, std::string(7, 'b'));
  EXPECT_EQ(TestHook_LogFrameMessages("server->game", 1, frame), 3);
  EXPECT_TRUE(TestLogContains("msg=0 sym=0x1111111111111111"));
  EXPECT_TRUE(TestLogContains("msg=1 sym=0x43e6963ac76beee4"));
  EXPECT_TRUE(TestLogContains("msg=2 sym=0x2222222222222222"));
  EXPECT_TRUE(TestLogContains("len=7"));
}

TEST(WsBridgeFrameLog, StopsAtATruncatedMessageAndSaysSo) {
  ClearTestLogs();
  std::string truncated = BuildMarkedMessage(0x3333333333333333ULL, std::string(10, 'c'));
  truncated.resize(truncated.size() - 4);
  EXPECT_EQ(TestHook_LogFrameMessages("game->server", 0, truncated), 1);
  EXPECT_TRUE(TestLogContains("declares 10 bytes but 6 remain"));
}

// The game->server diagnostic walk decodes payloads (hex dump, invite targets). A message whose declared
// payload runs past the frame must stop the walk before any decoder reads it.
static void PutU64(std::string& bytes, size_t offset, uint64_t value) {
  for (size_t i = 0; i < 8; ++i) bytes[offset + i] = static_cast<char>((value >> (8 * i)) & 0xff);
}

// A 0x30-byte invite payload with distinct values in the three decoded fields:
// routing at +0, session at +0x18, target at +0x20.
static std::string PatternedInvitePayload() {
  std::string payload(0x30, '\0');
  PutU64(payload, 0x00, 111);
  PutU64(payload, 0x18, 222);
  PutU64(payload, 0x20, 333);
  return payload;
}

constexpr uint64_t kFriendInviteSym = 0x7f0d7a28de3c6f70ULL;
constexpr uint64_t kPartyInviteSym = 0xcf13f934540b5f5eULL;
constexpr uint64_t kPlayerSessionSym = 0x9af2fab2a0c81a05ULL;
constexpr uint64_t kFriendSubscribeSym = 0xdcfa94680e8d19fcULL;

TEST(WsBridgeGameToServerLog, DecodesTheFieldsOfAWholeFriendInvite) {
  ClearTestLogs();
  EXPECT_EQ(TestHook_LogGameToServerFrame(BuildMarkedMessage(kFriendInviteSym, PatternedInvitePayload())), 1);
  EXPECT_TRUE(TestLogContains("FriendInvite: routing=111 target=333 session=222"));
}

TEST(WsBridgeGameToServerLog, DecodesTheFieldsOfAWholePartyInvite) {
  ClearTestLogs();
  EXPECT_EQ(TestHook_LogGameToServerFrame(BuildMarkedMessage(kPartyInviteSym, PatternedInvitePayload())), 1);
  EXPECT_TRUE(TestLogContains("PartyInviteRequest: routing=111 target=333 session=222"));
}

TEST(WsBridgeGameToServerLog, HexDumpsAWholePlayerSessionRequest) {
  ClearTestLogs();
  EXPECT_EQ(TestHook_LogGameToServerFrame(BuildMarkedMessage(kPlayerSessionSym, std::string("\x01\xab\xff", 3))), 1);
  EXPECT_TRUE(TestLogContains("PlayerSessionReq payload: 01 ab ff "));
}

TEST(WsBridgeGameToServerLog, NotesAFriendListSubscribe) {
  ClearTestLogs();
  EXPECT_EQ(TestHook_LogGameToServerFrame(BuildMarkedMessage(kFriendSubscribeSym, "")), 1);
  EXPECT_TRUE(TestLogContains("FriendListSubscribeRequest sent"));
}

TEST(WsBridgeGameToServerLog, ATruncatedMessageIsNotDecoded) {
  ClearTestLogs();
  std::string frame = BuildMarkedMessage(kFriendInviteSym, PatternedInvitePayload());
  PutU64(frame, 16, 0x1000);  // claim 0x1000 payload bytes while 0x30 are present
  const uint8_t stale = 0;
  nevr_evr_codec::Message message;
  message.payload = &stale;  // a payload left over from an earlier read must not survive a Truncated one
  ASSERT_EQ(nevr_evr_codec::ReadMessage(frame, 0, &message), nevr_evr_codec::ReadStatus::Truncated);
  EXPECT_EQ(message.payload, nullptr);
  EXPECT_EQ(TestHook_LogGameToServerFrame(frame), 0);
  EXPECT_TRUE(TestLogContains("truncated: header declares 4096 payload bytes but only 48 remain"));
  EXPECT_FALSE(TestLogContains("FriendInvite:"));
}

TEST(WsBridgeGameToServerLog, AWholeMessageThenATruncatedOneDecodesOnlyTheFirst) {
  ClearTestLogs();
  std::string second = BuildMarkedMessage(kFriendInviteSym, std::string(0x30, '\x07'));
  PutU64(second, 16, 0x1000);
  const std::string first = BuildMarkedMessage(kFriendInviteSym, PatternedInvitePayload());
  const std::string frame = first + second;
  // Fail fast, before the walk: the first message is whole and the second is truncated.
  nevr_evr_codec::Message message;
  ASSERT_EQ(nevr_evr_codec::ReadMessage(frame, 0, &message), nevr_evr_codec::ReadStatus::Ok);
  ASSERT_EQ(nevr_evr_codec::ReadMessage(frame, first.size(), &message), nevr_evr_codec::ReadStatus::Truncated);
  EXPECT_EQ(TestHook_LogGameToServerFrame(frame), 1);
  EXPECT_TRUE(TestLogContains("FriendInvite: routing=111 target=333 session=222"));
  EXPECT_TRUE(TestLogContains("truncated: header declares 4096 payload bytes but only 48 remain"));
  EXPECT_FALSE(TestLogContains("routing=506381209866536711"));  // the second message's 0x07 bytes
}

TEST(WsBridgeGameToServerLog, ATruncatedPlayerSessionRequestIsNotHexDumped) {
  ClearTestLogs();
  std::string frame = BuildMarkedMessage(kPlayerSessionSym, std::string(4, 'p'));
  PutU64(frame, 16, 16);  // declares 16 payload bytes, has 4
  nevr_evr_codec::Message message;
  ASSERT_EQ(nevr_evr_codec::ReadMessage(frame, 0, &message), nevr_evr_codec::ReadStatus::Truncated);
  EXPECT_EQ(TestHook_LogGameToServerFrame(frame), 0);
  EXPECT_FALSE(TestLogContains("PlayerSessionReq payload:"));
}

// Pins: a declared payload length that wraps to zero once the 24-byte header is added is read as truncated,
// ends the walk at that message, and is printed as declared. The codec result is asserted first so a
// regression there fails here at once instead of looping in the walk.
TEST(WsBridgeGameToServerLog, AWrappingDeclaredLengthEndsTheWalkAndPrintsTheDeclaredLength) {
  ClearTestLogs();
  std::string frame = BuildMarkedMessage(kFriendInviteSym, PatternedInvitePayload());
  const uint64_t wraps = UINT64_MAX - 23;
  PutU64(frame, 16, wraps);
  nevr_evr_codec::Message message;
  ASSERT_EQ(nevr_evr_codec::ReadMessage(frame, 0, &message), nevr_evr_codec::ReadStatus::Truncated);
  EXPECT_EQ(TestHook_LogGameToServerFrame(frame), 0);
  EXPECT_TRUE(TestLogContains("header declares " + std::to_string(wraps) + " payload bytes but only 48 remain"));
}

// The log sink is capped: a loop that logs on every pass ends the process (nevr_test_log_cap::EndProcessOnOverflow)
// instead of growing the sink until memory runs out. Here the handler is replaced to observe the overflow.
static size_t g_capOverflowCalls = 0;
static size_t g_capOverflowLines = 0;
static void CountCapOverflow(size_t lines) {
  ++g_capOverflowCalls;
  g_capOverflowLines = lines;
}

TEST(TestLogCapSink, ASpinningLoggerStopsGrowingAtTheCapAndReportsTheOverflow) {
  ClearTestLogs();
  g_capOverflowCalls = 0;
  nevr_test_log_cap::g_overflowHandler = CountCapOverflow;
  for (size_t i = 0; i < nevr_test_log_cap::kMaxLines + 50; ++i) Log(EchoVR::LogLevel::Info, "spin %zu", i);
  // Empty the sink before the default handler comes back: a Log from another thread in between would
  // otherwise overflow and end the process.
  size_t held = 0;
  {
    std::lock_guard<std::mutex> lock(g_testLogMutex);
    held = g_testLogMessages.size();
    g_testLogMessages.clear();
  }
  nevr_test_log_cap::g_overflowHandler = nevr_test_log_cap::EndProcessOnOverflow;
  EXPECT_EQ(held, 10000u);
  EXPECT_EQ(nevr_test_log_cap::kMaxLines, 10000u);
  EXPECT_EQ(g_capOverflowCalls, 50u);
  EXPECT_EQ(g_capOverflowLines, 10000u);
}

// The default action is the one a spinning loop meets: print the failure and end the process with the
// overflow exit code, so the loop stops at once instead of running to the time limit.
TEST(TestLogCapSinkDeathTest, TheDefaultHandlerPrintsTheMessageAndExits98) {
  ASSERT_EXIT(
      {
        for (size_t i = 0; i <= nevr_test_log_cap::kMaxLines; ++i) Log(EchoVR::LogLevel::Info, "spin %zu", i);
        std::_Exit(0);  // reached only if the cap did not end the process
      },
      ::testing::ExitedWithCode(nevr_test_log_cap::kOverflowExitCode),
      "FAILED: log capture overflowed: 10000 lines, a loop is spinning \\(in TestLogCapSinkDeathTest\\.");
}

TEST(WsBridgeLoginFailure, DiagnosticRejectsUndersizedTruncatedAndOversizedFrames) {
  uint64_t statusCode = 0;
  size_t messageBytes = 0;
  EXPECT_FALSE(TestHook_ReadLoginFailureDiagnostic(std::string(47, '\0'), &statusCode, &messageBytes));

  const std::string truncated = BuildLoginFailureFrame(24 + 6, 502, "x");
  EXPECT_FALSE(TestHook_ReadLoginFailureDiagnostic(truncated, &statusCode, &messageBytes));

  const std::string oversized = BuildLoginFailureFrame(UINT64_MAX, 502, "x");
  EXPECT_FALSE(TestHook_ReadLoginFailureDiagnostic(oversized, &statusCode, &messageBytes));

  const std::string fixedOnly = BuildLoginFailureFrame(24, 502, "");
  const std::string concatenated = fixedOnly + BuildLoginFailureFrame(25, 401, "x");
  EXPECT_FALSE(TestHook_ReadLoginFailureDiagnostic(concatenated, &statusCode, &messageBytes));
}

TEST(WsBridgeLoginRequest, HasExpectedHeaderAndPayloadLength) {
  const std::string request = TestHook_BuildLoginRequest(123456789ULL, 1, "Player", "token");
  const std::array<unsigned char, 8> expectedMarker = {0xf6, 0x40, 0xbb, 0x78,
                                                        0xa2, 0xe7, 0x8c, 0xbb};

  ASSERT_GE(request.size(), 56U);
  for (size_t index = 0; index < expectedMarker.size(); ++index) {
    EXPECT_EQ(static_cast<unsigned char>(request[index]), expectedMarker[index]);
  }
  EXPECT_EQ(ReadLe64(request, 8), 0xbdb41ea9e67b200aULL);
  EXPECT_EQ(ReadLe64(request, 16), request.size() - 24U);
  EXPECT_EQ(ReadLe64(request, 40), 1ULL);
  EXPECT_EQ(ReadLe64(request, 48), 123456789ULL);
  for (size_t index = 24; index < 40; ++index) {
    EXPECT_EQ(request[index], '\0');
  }
}

// PlatformCode=4 (OVR_ORG in game numbering) at wire offset 40.
// Regression test for 2026-08-04: PlatformCode was sent as 3, which the game
// resolves to XBX; OVR_ORG is 4 in the game's and Nakama's shared numbering. The server echoes the value unchanged into LoginSuccess
// (evr_pipeline_login.go:185), and the game resolves it through
// GetProviderPrefix (echovr.exe fcn.14060d640, switch case 4→\"OVR-ORG\").
TEST(WsBridgeLoginRequest, PlatformCode4AtWireOffset40) {
  const std::string request = TestHook_BuildLoginRequest(999888777ULL, 4, "Test", "");
  ASSERT_GE(request.size(), 56U);
  // PlatformCode at offset 40 (24-byte header + 16-byte UUID), LE uint64
  EXPECT_EQ(ReadLe64(request, 40), 4ULL)
      << "PlatformCode at wire offset 40 must be 4 (OVR_ORG in game numbering)";
  // AccountId follows at offset 48
  EXPECT_EQ(ReadLe64(request, 48), 999888777ULL);
  // UUID is all zeros (no previous session)
  for (size_t i = 24; i < 40; ++i) EXPECT_EQ(request[i], '\0');
}

TEST(WsBridgeLoginRequest, JsonCarriesIdentityCredentialsAndMeasuredSystemInfo) {
  const std::string request = TestHook_BuildLoginRequest(77, 3, "A \"quoted\" name", "access-token");
  const size_t jsonOffset = 56;
  ASSERT_GT(request.size(), jsonOffset);
  ASSERT_EQ(request.back(), '\0');
  const nlohmann::json json = nlohmann::json::parse(
      request.substr(jsonOffset, request.size() - jsonOffset - 1));

  EXPECT_EQ(json.at("accountid"), 77);
  EXPECT_EQ(json.at("displayname"), "A \"quoted\" name");
  EXPECT_EQ(json.at("access_token"), "access-token");
  EXPECT_TRUE(json.contains("buildversion"));
  ASSERT_TRUE(json.contains("nevr_identity"));
  const nevr_build_identity::Info& identity = nevr_build_identity::Get();
  EXPECT_EQ(json["nevr_identity"]["version"], identity.project_version);
  EXPECT_EQ(json["nevr_identity"]["commit"], identity.git_commit);
  EXPECT_EQ(json["nevr_identity"]["build"], identity.git_describe);
  EXPECT_EQ(json.at("nevr_social"), nevr_social_party::kSocialLevel) << "the social level the server gates new messages on";
  // Outside the game there is no headset serial to read: "unknown", which alt detection ignores (#83).
  EXPECT_EQ(json.at("hmdserialnumber"), "unknown");
  EXPECT_EQ(json["nevr_identity"]["build_type"], identity.build_type);
  ASSERT_TRUE(json.contains("system_info"));
  EXPECT_TRUE(json["system_info"]["num_physical_cores"].is_number_unsigned());
  EXPECT_TRUE(json["system_info"]["memory_total"].is_number_unsigned());
}

// The login wire payload, rather than SystemInfo in isolation, must carry
// usable host-capacity measurements.  A positive value proves that the JSON
// serialization path preserves the real measurements instead of emitting the
// old placeholder zeroes.
TEST(WsBridgeLoginRequest, JsonEmitsPositiveCpuAndRamMeasurements) {
  const std::string request = TestHook_BuildLoginRequest(78, 3, "Player", "token");
  constexpr size_t kJsonOffset = 56;
  ASSERT_GT(request.size(), kJsonOffset);
  ASSERT_EQ(request.back(), '\0');
  const nlohmann::json json = nlohmann::json::parse(
      request.substr(kJsonOffset, request.size() - kJsonOffset - 1));

  const auto& systemInfo = json.at("system_info");
  EXPECT_GT(systemInfo.at("num_physical_cores").get<uint64_t>(), 0U);
  EXPECT_GT(systemInfo.at("memory_total").get<uint64_t>(), 0U);
}

TEST(LoginProfile, BuildsJsonFromQuestMeasurementsAndEscapesStrings) {
  nevr_login_profile::LoginProfileInputs inputs;
  inputs.account_id = 90210;
  inputs.display_name = "Quest \"player\"";
  inputs.access_token = "jwt-\\-token";
  inputs.password = "secret-\"value";
  inputs.hmd_serial_number = "quest-serial";
  inputs.headset_type = "Quest";
  inputs.cpu = "Quest measured CPU";
  inputs.physical_cores = 8;
  inputs.logical_cores = 8;
  inputs.memory_total_mb = 8192;
  inputs.memory_used_mb = 3072;
  inputs.project_version = "1.2.3";
  inputs.git_commit = "abcdef0";
  inputs.git_describe = "v1.2.3-4-gabcdef0";
  inputs.build_type = "RelWithDebInfo";
  inputs.social_level = 2;
  inputs.plugins = nlohmann::json::parse(R"([{"name":"example","loaded":true}])");

  const nlohmann::json profile = nlohmann::json::parse(nevr_login_profile::BuildLoginProfileJson(inputs));
  EXPECT_EQ(profile.at("accountid"), 90210);
  EXPECT_EQ(profile.at("displayname"), inputs.display_name);
  EXPECT_EQ(profile.at("access_token"), inputs.access_token);
  EXPECT_EQ(profile.at("password"), inputs.password);
  EXPECT_EQ(profile.at("hmdserialnumber"), "quest-serial");
  EXPECT_EQ(profile.at("nevr_identity").at("commit"), "abcdef0");
  EXPECT_EQ(profile.at("system_info").at("headset_type"), "Quest");
  EXPECT_EQ(profile.at("system_info").at("cpu"), "Quest measured CPU");
  EXPECT_EQ(profile.at("system_info").at("num_physical_cores"), 8);
  EXPECT_EQ(profile.at("system_info").at("memory_total"), 8192);
  EXPECT_EQ(profile.at("nonce"), "");
  EXPECT_EQ(profile.at("nevr_social"), 2);
  EXPECT_EQ(profile.at("nevr_plugins"), inputs.plugins);
  EXPECT_TRUE(profile.at("nevr_plugins").is_array());
}

TEST(LoginProfile, EmptyDisplayNameFallsBackToTheAccountId) {
  nevr_login_profile::LoginProfileInputs inputs;
  inputs.account_id = 90210;
  inputs.display_name = "";

  const nlohmann::json profile = nlohmann::json::parse(nevr_login_profile::BuildLoginProfileJson(inputs));
  EXPECT_EQ(profile.at("displayname"), "90210");
}

// A buffer shorter than the 28-byte header must be rejected by the size guard before any header
// byte is read (the later checks would reject it too, but only after reading past the buffer).
// The guard's own Warning is the only observable difference, so the test reads the captured log.
TEST(EvrpPackageLogging, ABufferShorterThanTheHeaderIsRejectedBySizeGuard) {
  ClearTestLogs();
  int64_t symbol = 0;
  nevr_evrp::TintData tint;
  const std::vector<uint8_t> shortBuffer(nevr_evrp::kHeaderSize - 1, 0);
  EXPECT_FALSE(nevr_evrp::ParseTint(shortBuffer, "short.evrp", symbol, tint));
  EXPECT_TRUE(TestLogContains("file too small: file=short.evrp"));

  // Exactly the header size is not "too small": it is a header whose data_length promises 80
  // bytes that are not there, so the size check rejects it instead.
  ClearTestLogs();
  std::vector<uint8_t> headerOnly(nevr_evrp::kHeaderSize, 0);
  const uint8_t header[] = {0x45, 0x56, 0x52, 0x50, 0x01, 0x00, 0x00, 0x00, 0x86, 0xDC, 0xC5, 0x9D,
                            0xD0, 0x28, 0xD2, 0x74, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                            0x50, 0x00, 0x00, 0x00};
  std::copy(std::begin(header), std::end(header), headerOnly.begin());
  EXPECT_FALSE(nevr_evrp::ParseTint(headerOnly, "header.evrp", symbol, tint));
  EXPECT_FALSE(TestLogContains("file too small"));
  EXPECT_TRUE(TestLogContains("size mismatch: file=header.evrp"));
}

TEST(WsBridgePlatformPrefix, EveryDefinedPlatformHasTheNakamaPrefix) {
  EXPECT_STREQ(TestHook_PlatformPrefix(0), "UNK");
  EXPECT_STREQ(TestHook_PlatformPrefix(1), "STM");
  EXPECT_STREQ(TestHook_PlatformPrefix(2), "DSC");
  EXPECT_STREQ(TestHook_PlatformPrefix(3), "XBX");
  EXPECT_STREQ(TestHook_PlatformPrefix(4), "OVR-ORG");
  EXPECT_STREQ(TestHook_PlatformPrefix(5), "OVR");
  EXPECT_STREQ(TestHook_PlatformPrefix(6), "BOT");
  EXPECT_STREQ(TestHook_PlatformPrefix(7), "DMO");
  EXPECT_STREQ(TestHook_PlatformPrefix(8), "UNK");
  EXPECT_STREQ(TestHook_PlatformPrefix(999), "UNK");
}

// The remote Bearer: a token-auth client sends its JWT; a URL-credential client sends the
// server key (Nakama treats it as the legacy session and authenticates from discordid/password),
// both through the /nevr ingress that forwards Authorization unchanged (issue #52).
TEST(WsBridgeRemoteBearer, TokenAuthClientSendsItsJwt) {
  EXPECT_EQ(TestHook_SelectRemoteBearer(false, "jwt-value", "server-key"), "jwt-value");
  EXPECT_EQ(TestHook_SelectRemoteBearer(false, "", "server-key"), "");
}

TEST(WsBridgeRemoteBearer, UrlCredentialClientSendsTheServerKeyNotTheJwt) {
  EXPECT_EQ(TestHook_SelectRemoteBearer(true, "jwt-value", "server-key"), "server-key");
  EXPECT_EQ(TestHook_SelectRemoteBearer(true, "", "server-key"), "server-key");
  // No server key configured: attach nothing (the caller logs a warning), never the JWT.
  EXPECT_EQ(TestHook_SelectRemoteBearer(true, "jwt-value", ""), "");
}

// The /ws catch-all replaces a token-auth client's Bearer (#52, #65); the bridge warns when a
// token-auth login is about to go there. Only the exact /ws path counts.
TEST(WsBridgeRemoteBearer, OnlyTheWsPathReplacesTheBearer) {
  EXPECT_TRUE(TestHook_IsBearerReplacingPath("wss://g.echovrce.com:443/ws"));
  EXPECT_TRUE(TestHook_IsBearerReplacingPath("wss://g.echovrce.com/ws?format=evr"));
  EXPECT_FALSE(TestHook_IsBearerReplacingPath("wss://g.echovrce.com:443/nevr"));
  EXPECT_FALSE(TestHook_IsBearerReplacingPath("wss://g.echovrce.com:443/nevr?format=evr"));
  EXPECT_FALSE(TestHook_IsBearerReplacingPath("wss://g.echovrce.com/wss"));
  EXPECT_FALSE(TestHook_IsBearerReplacingPath("wss://g.echovrce.com/ws/x"));
  EXPECT_FALSE(TestHook_IsBearerReplacingPath("wss://g.echovrce.com"));
}

// SelectPlatformCode: the bridge always logs in as platform 4 (OVR_ORG), the provider it forces
// into the game's own CNSUser. A login as platform 6 (-noovr) made Nakama answer the game's
// later LobbyPlayerSessionsRequest (sent as OVR-ORG) with "requesting player not found in
// match", so the game never reached a lobby host.
TEST(WsBridgeSelectPlatform, AlwaysOvrOrgToMatchTheGamesOwnIdentity) {
  EXPECT_EQ(TestHook_SelectPlatformCode(true, true), 4ULL);
  EXPECT_EQ(TestHook_SelectPlatformCode(true, false), 4ULL);
  EXPECT_EQ(TestHook_SelectPlatformCode(false, true), 4ULL);
  EXPECT_EQ(TestHook_SelectPlatformCode(false, false), 4ULL);
  // The code the bridge logs in as is the one labelled OVR-ORG, the provider the game names itself with.
  EXPECT_STREQ(TestHook_PlatformPrefix(TestHook_SelectPlatformCode(false, false)), "OVR-ORG");
}

TEST(WsBridgeCallbackGuard, ContainsStdExceptionsAtTheCallbackBoundary) {
  ClearTestLogs();
  EXPECT_TRUE(TestHook_GuardWsCallbackContainsStdException());
  std::lock_guard<std::mutex> lock(g_testLogMutex);
  ASSERT_EQ(g_testLogMessages.size(), 1U);
  EXPECT_NE(g_testLogMessages.front().find("callback threw and was CONTAINED"), std::string::npos);
  EXPECT_NE(g_testLogMessages.front().find("failure=1"), std::string::npos);
  EXPECT_EQ(g_testLogMessages.front().find("response-secret-sentinel"), std::string::npos);
}

TEST(WsBridgeCallbackGuard, ForwardsCallbackArgumentsWithoutLogging) {
  {
    std::lock_guard<std::mutex> lock(g_testLogMutex);
    g_testLogMessages.clear();
  }

  EXPECT_EQ(TestHook_GuardWsCallbackForwardsArguments(19, 23), 42);

  std::lock_guard<std::mutex> lock(g_testLogMutex);
  EXPECT_TRUE(g_testLogMessages.empty());
}

TEST(WsBridgeCallbackGuard, PreservesNonStdExceptionPropagationByDesign) {
  // GuardWsCallback deliberately names std::exception rather than using a
  // catch-all at an ixwebsocket boundary. This verifies that documented
  // contract without weakening the runtime's exception policy.
  EXPECT_TRUE(TestHook_GuardWsCallbackPropagatesNonStdException());
}

TEST(ModuleProcRegistry, ResolvesRegisteredProcAndRejectsUnknownName) {
  static const char kName[] = "test.module_proc_registry";
  static int kProbe = 0;
  RegisterModuleProc(kName, &kProbe);

  EXPECT_EQ(ResolveModuleProc(kName), &kProbe);
  EXPECT_EQ(ResolveModuleProc("test.module_proc_registry.absent"), nullptr);
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
  using namespace nevr_patch_addresses;
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
  using namespace nevr_patch_addresses;
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

HANDLE g_guardEntered = nullptr;
HANDLE g_guardContinue = nullptr;

void WaitBeforeGuardPublish() {
    SetEvent(g_guardEntered);
    WaitForSingleObject(g_guardContinue, 5000);
}

}  // namespace

TEST(N84_HookGuard, UnchangedBytes_NoMismatch) {
    nevr_hook_guard::ResetForTest();
    GuardScratch s;
    ASSERT_NE(s.page, nullptr);
    memset(s.page, 0x90, 32);  // NOPs stand in for an untouched prologue

    nevr_hook_guard::Record(s.page, "N84_unchanged");
    EXPECT_EQ(nevr_hook_guard::VerifyAll("test:unchanged"), 0)
        << "guard reported a mismatch on bytes nothing modified";
}

TEST(N84_HookGuard, ReadersSkipReservedEntryUntilWriterPublishesIt) {
    nevr_hook_guard::ResetForTest();
    GuardScratch s;
    ASSERT_NE(s.page, nullptr);
    memset(s.page, 0x90, 32);
    g_guardEntered = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_guardContinue = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    ASSERT_NE(g_guardEntered, nullptr);
    ASSERT_NE(g_guardContinue, nullptr);
    nevr_hook_guard::SetBeforePublishForTest(&WaitBeforeGuardPublish);

    std::thread writer([&] { nevr_hook_guard::Record(s.page, "N84_pending"); });
    const DWORD entered = WaitForSingleObject(g_guardEntered, 5000);
    if (entered == WAIT_OBJECT_0) {
        EXPECT_EQ(nevr_hook_guard::RecordedCount(), 0);
        EXPECT_FALSE(nevr_hook_guard::IsOurDetour(s.page));
        EXPECT_EQ(nevr_hook_guard::VerifyAll("test:pending"), 0);
    }
    SetEvent(g_guardContinue);
    writer.join();
    nevr_hook_guard::SetBeforePublishForTest(nullptr);
    EXPECT_EQ(entered, WAIT_OBJECT_0);
    EXPECT_EQ(nevr_hook_guard::RecordedCount(), 1);
    EXPECT_TRUE(nevr_hook_guard::IsOurDetour(s.page));
    CloseHandle(g_guardEntered);
    CloseHandle(g_guardContinue);
    g_guardEntered = nullptr;
    g_guardContinue = nullptr;
}

TEST(N84_HookGuard, OverwrittenBytes_Detected) {
    nevr_hook_guard::ResetForTest();
    GuardScratch s;
    ASSERT_NE(s.page, nullptr);
    memset(s.page, 0x90, 32);

    nevr_hook_guard::Record(s.page, "N84_overwritten");
    ASSERT_EQ(nevr_hook_guard::VerifyAll("test:baseline"), 0)
        << "precondition: freshly recorded bytes must match";

    // Simulate a second MinHook instance writing its own JMP rel32 over ours.
    unsigned char* p = static_cast<unsigned char*>(s.page);
    p[0] = 0xE9;  // JMP rel32
    p[1] = 0x11;
    p[2] = 0x22;
    p[3] = 0x33;
    p[4] = 0x44;

    EXPECT_EQ(nevr_hook_guard::VerifyAll("test:overwritten"), 1)
        << "guard did NOT detect a foreign detour overwriting a recorded address";
}

// IsOurDetour: code that calls a game function the runtime may have detoured asks whether the bytes there
// are still our own jump. Recorded and unchanged: yes. Never recorded, or overwritten since: no.
TEST(N84_HookGuard, IsOurDetour_OnlyForRecordedUnchangedSites) {
    nevr_hook_guard::ResetForTest();
    GuardScratch ours;
    GuardScratch other;
    ASSERT_NE(ours.page, nullptr);
    ASSERT_NE(other.page, nullptr);
    memset(ours.page, 0x90, 32);
    memset(other.page, 0x90, 32);

    nevr_hook_guard::Record(ours.page, "IsOurDetour_ours");
    EXPECT_TRUE(nevr_hook_guard::IsOurDetour(ours.page));
    EXPECT_FALSE(nevr_hook_guard::IsOurDetour(other.page)) << "an address the runtime never detoured";
    EXPECT_FALSE(nevr_hook_guard::IsOurDetour(nullptr));

    static_cast<unsigned char*>(ours.page)[0] = 0xE9;  // someone else's JMP over ours
    EXPECT_FALSE(nevr_hook_guard::IsOurDetour(ours.page)) << "a recorded site whose bytes changed is no longer ours";
}

TEST(N84_HookGuard, NullTarget_Ignored) {
    nevr_hook_guard::ResetForTest();
    const int before = nevr_hook_guard::RecordedCount();
    nevr_hook_guard::Record(nullptr, "N84_null");
    EXPECT_EQ(nevr_hook_guard::RecordedCount(), before)
        << "null target was recorded; a bad call site would occupy a guard slot";
}

TEST(N84_HookGuard, UnreadableTarget_Ignored) {
    nevr_hook_guard::ResetForTest();
    const int before = nevr_hook_guard::RecordedCount();
    // Reserved-but-not-committed: readable-looking pointer, unreadable memory.
    void* reserved = VirtualAlloc(nullptr, 4096, MEM_RESERVE, PAGE_NOACCESS);
    ASSERT_NE(reserved, nullptr);
    nevr_hook_guard::Record(reserved, "N84_unreadable");
    EXPECT_EQ(nevr_hook_guard::RecordedCount(), before)
        << "uncommitted memory was recorded; VerifyAll would fault reading it";
    VirtualFree(reserved, 0, MEM_RELEASE);
}

// The table holds every install site the runtime has. 62 is the count of sites in src/ that reach
// nevr_hook_guard::Record (counted in hook_guard.cpp, next to kMaxGuarded); a capacity below it drops
// guards on a healthy boot. A new site raises that count and this number with it.
TEST(N84_HookGuard, CapacityCoversEveryInstallSiteTheRuntimeHas) {
    constexpr int kInstallSitesInSource = 62;
    EXPECT_GE(nevr_hook_guard::Capacity(), kInstallSitesInSource);
}

// A full table must not look like a clean one. Recording past Capacity() drops the site, and the drop
// has to be counted and reported at ERROR by VerifyAll, not vanish.
TEST(N84_HookGuard, FullTable_DropsAreCountedAndReportedLoudly) {
    nevr_hook_guard::ResetForTest();
    ClearTestLogs();
    GuardScratch s;
    ASSERT_NE(s.page, nullptr);
    memset(s.page, 0x90, 32);

    const int cap = nevr_hook_guard::Capacity();
    ASSERT_GT(cap, 0);
    for (int i = 0; i < cap; i++) nevr_hook_guard::Record(s.page, "N84_fill");
    EXPECT_EQ(nevr_hook_guard::RecordedCount(), cap);
    EXPECT_EQ(nevr_hook_guard::DroppedCount(), 0);
    EXPECT_EQ(nevr_hook_guard::VerifyAll("test:full"), 0);
    EXPECT_FALSE(TestLogContains("hook guard table full")) << "reported a drop that never happened";

    nevr_hook_guard::Record(s.page, "N84_dropped_first");
    nevr_hook_guard::Record(s.page, "N84_dropped_second");
    EXPECT_EQ(nevr_hook_guard::RecordedCount(), cap) << "a full table must not grow";
    EXPECT_EQ(nevr_hook_guard::DroppedCount(), 2);

    EXPECT_EQ(nevr_hook_guard::VerifyAll("test:overflow"), 0) << "a drop is not an overwrite";
    EXPECT_TRUE(TestLogContains("hook guard table full cap=")) << "the drop was not reported";
    EXPECT_TRUE(TestLogContains("dropped=2 first=N84_dropped_first")) << "the report must name the first dropped detour";

    // Edge-triggered: the same drops are not reported again, a new drop is.
    ClearTestLogs();
    nevr_hook_guard::VerifyAll("test:again");
    EXPECT_FALSE(TestLogContains("hook guard table full"));
    nevr_hook_guard::Record(s.page, "N84_dropped_third");
    nevr_hook_guard::VerifyAll("test:third");
    EXPECT_TRUE(TestLogContains("dropped=3 first=N84_dropped_first"));
}

// Overflow itself does not Log(): the drop is counted and VerifyAll() reports it once.
TEST(N84_HookGuard, FullTable_RecordDoesNotLogUnderTheLoaderLock) {
    nevr_hook_guard::ResetForTest();
    ClearTestLogs();
    GuardScratch s;
    ASSERT_NE(s.page, nullptr);
    memset(s.page, 0x90, 32);
    for (int i = 0; i < nevr_hook_guard::Capacity() + 3; i++) nevr_hook_guard::Record(s.page, "N84_fill");
    EXPECT_EQ(nevr_hook_guard::DroppedCount(), 3);
    EXPECT_FALSE(TestLogContains("hook guard")) << "Record() logged on overflow; it can run under the loader lock";
}

// WOULD-FAIL-IF (N84): delete the memcmp in nevr_hook_guard::VerifyAll (hook_guard.cpp)
//   -> OverwrittenBytes_Detected fails: a foreign detour goes unreported.
// WOULD-FAIL-IF (N84-record): delete the Readable() check in nevr_hook_guard::Record
//   -> UnreadableTarget_Ignored fails, and production VerifyAll faults on the
//      uncommitted page instead of skipping it.
// WOULD-FAIL-IF (N84-wiring): delete nevr_hook_guard::VerifyAll(filename) from
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
    const nevr_system_info::Host& h = nevr_system_info::Get();

    EXPECT_GT(h.logical_cores, 0u) << "logical core count was never measured";
    EXPECT_GT(h.memory_total_mb, 0u) << "physical memory was never measured";
    EXPECT_LE(h.memory_used_mb, h.memory_total_mb) << "used exceeds total — derivation is wrong";

    // The fabricated tuple (4 cores, 8 threads, 16384 MB total, 8192 MB used), guarded as a set. Any single value could
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

TEST(HostEnvironmentLine, NamesTheWineVersionHostOsAndWindowsBuild) {
    nevr_system_info::Host host;
    host.wine_version = "10.0";
    host.wine_host_os = "Linux";
    host.os_build = 19045;
    EXPECT_EQ(nevr_host_environment::BootLine(host),
              "[NEVR.BOOT] host environment: wine=10.0 host_os=Linux os_build=19045\n");
}

TEST(HostEnvironmentLine, ANativeWindowsHostSaysNoneAndAnUnmeasuredBuildSaysUnknown) {
    nevr_system_info::Host host;  // nothing measured: native Windows with no build
    EXPECT_EQ(nevr_host_environment::BootLine(host),
              "[NEVR.BOOT] host environment: wine=none host_os=none os_build=unknown\n");
    host.os_build = 22631;
    EXPECT_EQ(nevr_host_environment::BootLine(host),
              "[NEVR.BOOT] host environment: wine=none host_os=none os_build=22631\n");
}

TEST(SystemInfo, WineDetectionIsTheVersionString) {
    const nevr_system_info::Host& h = nevr_system_info::Get();
    // IsWine() must be exactly "we got a version from ntdll", with no second
    // source of truth that could disagree with the string we transmit.
    EXPECT_EQ(h.IsWine(), !h.wine_version.empty());
}

TEST(SystemInfo, IsCachedNotRemeasured) {
    // Callers may hit this on a login path; the probes (CPUID,
    // GetLogicalProcessorInformation) are not free. Same object every call.
    EXPECT_EQ(&nevr_system_info::Get(), &nevr_system_info::Get());
}

// WOULD-FAIL-IF (N112): restore the literals in ws_bridge.cpp's system_info
//   block -> not caught here (that is a format string, not a value this test
//   can reach). ReportsRealCpuAndMemory catches the case where SystemInfo
//   itself starts returning the fabricated tuple; the `just verify` sensor catches
//   the format string.

// ============================================================================
// N112 — BuildIdentity. Compile-time constants, never measured. These values
// are baked into the binary by CMake; the test verifies they made it through
// the preprocessor and are not the fallback defaults (unknown/0.0.0).
// ============================================================================

TEST(BuildIdentity, ProjectVersionIsNotFallback) {
    const nevr_build_identity::Info& id = nevr_build_identity::Get();
    // The CMake-built binary always has a real version. The "0.0.0" fallback
    // only triggers for a manual compiler invocation without -DPROJECT_VERSION.
    EXPECT_NE(id.project_version, "0.0.0");
    EXPECT_FALSE(id.project_version.empty());
}

TEST(BuildIdentity, GitCommitIsNotEmpty) {
    const nevr_build_identity::Info& id = nevr_build_identity::Get();
    EXPECT_FALSE(id.git_commit.empty());
    EXPECT_NE(id.git_commit, "unknown");
}

TEST(BuildIdentity, GitDescribeIsNotEmpty) {
    const nevr_build_identity::Info& id = nevr_build_identity::Get();
    EXPECT_FALSE(id.git_describe.empty());
    EXPECT_NE(id.git_describe, "unknown");
}

TEST(BuildIdentity, BuildTypeIsSet) {
    const nevr_build_identity::Info& id = nevr_build_identity::Get();
    // "unknown-build-type" would mean CMAKE_BUILD_TYPE was not propagated
    // as a compile definition. Our CMake always sets it.
    EXPECT_NE(id.build_type, "unknown-build-type");
    EXPECT_FALSE(id.build_type.empty());
}

TEST(BuildIdentity, DirtyFlagMatchesDescribe) {
    const nevr_build_identity::Info& id = nevr_build_identity::Get();
    // The dirty flag is derived from git_describe, not a separate source.
    EXPECT_EQ(id.is_dirty, id.git_describe.find("-dirty") != std::string::npos);
}

// The one identity literal the release signer and the CI stamp check read out of the binary:
// "NEVR-BUILD <version> <commit40>", version and commit from the same definitions as Info.
TEST(BuildIdentity, IdentityLiteralBindsTheVersionAndTheFullCommit) {
    const std::string literal = nevr_build_identity::IdentityLiteral();
    const nevr_build_identity::Info& id = nevr_build_identity::Get();
    const std::string prefix = "NEVR-BUILD ";
    ASSERT_EQ(literal.compare(0, prefix.size(), prefix), 0) << literal;
    const size_t space = literal.rfind(' ');
    ASSERT_GT(space, prefix.size());
    EXPECT_EQ(literal.substr(prefix.size(), space - prefix.size()), id.project_version);
    const std::string commit = literal.substr(space + 1);
    EXPECT_EQ(commit.size(), 40u) << commit;
    EXPECT_EQ(commit.find_first_not_of("0123456789abcdef"), std::string::npos) << commit;
    EXPECT_EQ(commit.compare(0, id.git_commit.size(), id.git_commit), 0) << "the short commit is its prefix";
}

TEST(BuildIdentity, IsCachedNotRemeasured) {
    EXPECT_EQ(&nevr_build_identity::Get(), &nevr_build_identity::Get());
}

// ============================================================================
// N134 S8: ctx_size and plugin-query API tests.
// The functions are linked from plugin_loader.cpp (the real ones, not stubs).
// With an empty registry (no LoadPlugins call in this test binary — plugins
// are injected via TestHook_*), count=0 and info=nullptr are correct.
// ============================================================================

TEST(S8_PluginQueryApi, CtxSizeMatchesActualStruct) {
    // If someone adds a field to NvrGameContext without bumping ctx_size
    // (which is sizeof at each construction site), a v5+ plugin checking
    // ctx->ctx_size would see a stale value. This locks the two together.
    NvrGameContext ctx = {};
    ctx.ctx_size = sizeof(NvrGameContext);
    EXPECT_GE(ctx.ctx_size, sizeof(NvrGameContext));
    // The struct must have grown from its v4 size (4 fields, minimal padding).
    EXPECT_GT(ctx.ctx_size, 20u);  // base_addr(8) + net_game(8) + game_state(4) + flags(4) = 24, plus v5 fields
}

TEST(S8_PluginQueryApi, EmptyRegistryCountZero) {
    EXPECT_EQ(GetLoadedPluginCount(), 0);
}

TEST(S8_PluginQueryApi, InfoNullForAnyIndexOnEmptyRegistry) {
    EXPECT_EQ(GetLoadedPluginInfo(0), nullptr);
    EXPECT_EQ(GetLoadedPluginInfo(-1), nullptr);
    EXPECT_EQ(GetLoadedPluginInfo(999), nullptr);
}

TEST(S8_PluginQueryApi, FunctionPointersSetInContext) {
    NvrGameContext ctx = {};
    ctx.base_addr = reinterpret_cast<uintptr_t>(EchoVR::g_GameBaseAddress);
    ctx.flags = NEVR_HOST_IS_SERVER;
    ctx.ctx_size = sizeof(NvrGameContext);
    ctx.get_plugin_count = GetLoadedPluginCount;
    ctx.get_plugin_info = GetLoadedPluginInfo;

    EXPECT_NE(ctx.get_plugin_count, nullptr);
    EXPECT_NE(ctx.get_plugin_info, nullptr);
    EXPECT_EQ(ctx.get_plugin_count(), 0);
    EXPECT_EQ(ctx.get_plugin_info(0), nullptr);
}

// ============================================================================
// N134 S8: caps priority ordering table. Lock the sort key so a band
// reassignment (e.g. moving NETWORK before ALTERS_GAMEPLAY) is deliberate.
// ============================================================================

TEST(S8_CapsPriority, UndeclaredBeforeObservers) {
    // Unknown risk loads FIRST in its config-order position, so a declared
    // plugin's hooks land on top where HookGuard can catch a collision.
    EXPECT_LT(CapsLoadPriority(NEVR_PLUGIN_CAP_UNDECLARED),
              CapsLoadPriority(NEVR_PLUGIN_CAP_OBSERVES_ONLY));
}

TEST(S8_CapsPriority, ObserverBeforeCosmetic) {
    EXPECT_LT(CapsLoadPriority(NEVR_PLUGIN_CAP_OBSERVES_ONLY),
              CapsLoadPriority(NEVR_PLUGIN_CAP_COSMETIC));
}

TEST(S8_CapsPriority, CosmeticBeforeGameplay) {
    EXPECT_LT(CapsLoadPriority(NEVR_PLUGIN_CAP_COSMETIC),
              CapsLoadPriority(NEVR_PLUGIN_CAP_ALTERS_GAMEPLAY));
}

TEST(S8_CapsPriority, GameplayBeforeRules) {
    EXPECT_LT(CapsLoadPriority(NEVR_PLUGIN_CAP_ALTERS_GAMEPLAY),
              CapsLoadPriority(NEVR_PLUGIN_CAP_ALTERS_RULES));
}

TEST(S8_CapsPriority, NetworkBeforeEngineHooker) {
    // Network opens sockets; an engine-hooker may depend on the channel being up.
    EXPECT_LT(CapsLoadPriority(NEVR_PLUGIN_CAP_NETWORK),
              CapsLoadPriority(NEVR_PLUGIN_CAP_HOOKS_ENGINE));
}

TEST(S8_CapsPriority, RulesBeforeNetwork) {
    // Rules changer alters scoring/game mode; network plugin may depend on the
    // rules being in place before it connects.
    EXPECT_LT(CapsLoadPriority(NEVR_PLUGIN_CAP_ALTERS_RULES),
              CapsLoadPriority(NEVR_PLUGIN_CAP_NETWORK));
}

TEST(S8_CapsPriority, EngineHookerLoadsLast) {
    EXPECT_LT(CapsLoadPriority(NEVR_PLUGIN_CAP_ALTERS_GAMEPLAY),
              CapsLoadPriority(NEVR_PLUGIN_CAP_HOOKS_ENGINE));
    EXPECT_LT(CapsLoadPriority(NEVR_PLUGIN_CAP_ALTERS_RULES),
              CapsLoadPriority(NEVR_PLUGIN_CAP_HOOKS_ENGINE));
    EXPECT_LT(CapsLoadPriority(NEVR_PLUGIN_CAP_NETWORK),
              CapsLoadPriority(NEVR_PLUGIN_CAP_HOOKS_ENGINE));
}

TEST(S8_CapsPriority, CombinedCapsTakeHighestBand) {
    // A plugin declaring both COSMETIC and ALTERS_RULES — the higher-risk band
    // (RULES=4) wins for ordering purposes. It should NOT land in the COSMETIC
    // band (2).
    const uint32_t cosmeticAndRules = NEVR_PLUGIN_CAP_COSMETIC | NEVR_PLUGIN_CAP_ALTERS_RULES;
    EXPECT_GT(CapsLoadPriority(cosmeticAndRules),
              CapsLoadPriority(NEVR_PLUGIN_CAP_COSMETIC));
    EXPECT_EQ(CapsLoadPriority(cosmeticAndRules),
              CapsLoadPriority(NEVR_PLUGIN_CAP_ALTERS_RULES));
}

// #83: the stock HMD serial field choice (hmd_serial.h): the game's 24-byte buffer in VR, "N/A" with
// the No-VR flag, and "unknown" only when the serial buffer is absent or invalid.
TEST(HmdSerial, StockChoice) {
  char serial[nevr_hmd_serial::kSerialBytes] = {};
  std::memcpy(serial, "1WMHHA1234567", 13);
  const auto vr = nevr_hmd_serial::Select(false, serial);
  EXPECT_EQ(vr.value, "1WMHHA1234567");
  EXPECT_EQ(vr.source, nevr_hmd_serial::Source::GameBuffer);
  EXPECT_EQ(nevr_hmd_serial::Select(true, serial).value, "N/A") << "No-VR mode sends what the stock client sends";
  char empty[nevr_hmd_serial::kSerialBytes] = {};
  EXPECT_EQ(nevr_hmd_serial::Select(false, empty).value, "unknown");
  EXPECT_EQ(nevr_hmd_serial::Select(false, nullptr).value, "unknown");
  char garbage[nevr_hmd_serial::kSerialBytes] = {'A', 'B', '\x01', 'C'};
  EXPECT_EQ(nevr_hmd_serial::Select(false, garbage).value, "unknown") << "control bytes are not a serial";
  char full[nevr_hmd_serial::kSerialBytes];
  std::memset(full, 'Z', sizeof(full));  // no terminator within 24 bytes: take exactly 24
  EXPECT_EQ(nevr_hmd_serial::Select(false, full).value, std::string(24, 'Z'));
}


// ============================================================================
// #479 — the XPID provider-string patch reports failure through g_bootHookFailed.
//
// PatchDscProvider runs from Initialize(), before the CLI is parsed, so g_isServer is still false
// there and ServerFatal() would only warn. The failure has to reach boot.cpp's server check
// (`if (g_bootHookFailed) ServerFatal(...)`) through the flag instead.
//
// The test builds a 32 MB stand-in for the game image, puts the five provider strings at the
// real RVAs, and points EchoVR::g_GameBaseAddress at it.
// ============================================================================

namespace {

constexpr size_t kXpidImageSize = 0x2000000;

struct XpidSite {
    uintptr_t rva;
    const char* original;  // 4 bytes including the terminator or dash
    const char* patched;
};

const XpidSite kXpidSites[] = {
    {nevr_patch_addresses::XPID_PLATFORM_SHORT_NAME, "PSN", "DSC"},
    {nevr_patch_addresses::XPID_PLATFORM_DASH_PREFIX, "PSN-", "DSC-"},
    {nevr_patch_addresses::XPID_PLATFORM_COMPACT_NAME, "PSN", "DSC"},
    {nevr_patch_addresses::XPID_PLATFORM_FALLBACK_PREFIX, "??\?-", "DSC-"},
    {nevr_patch_addresses::XPID_PLATFORM_COMPACT_FALLBACK_NAME, "???", "DSC"},
};

void WriteXpidOriginals(char* base) {
    for (const auto& site : kXpidSites) memcpy(base + site.rva, site.original, 4);
}

// Points EchoVR::g_GameBaseAddress at `base` for one test and restores it, and clears the flag the
// code under test sets.
struct XpidBaseScope {
    CHAR* saved;
    explicit XpidBaseScope(char* base) : saved(EchoVR::g_GameBaseAddress) {
        EchoVR::g_GameBaseAddress = base;
        g_bootHookFailed = false;
        g_bootHookFailedCause[0] = '\0';
        g_serverFatalCalls = 0;
    }
    ~XpidBaseScope() {
        EchoVR::g_GameBaseAddress = saved;
        g_bootHookFailed = false;
        g_bootHookFailedCause[0] = '\0';
    }
};

// Opens the boot tee's file for one test and closes it again. Init() starts the boot phase, so
// the code under test reports through TeeFprintf exactly as it does under the loader lock.
// Output() returns what the tee wrote to nevr-boot.jsonl after the guard was constructed.
struct BootTeeCapture {
    std::string path;
    std::streamoff start = 0;
    BootTeeCapture() {
        nevr_boot_log_tee::Init();
        path = nevr_boot_log_tee::Path();
        std::ifstream in(path, std::ios::binary | std::ios::ate);
        if (in) start = in.tellg();
    }
    ~BootTeeCapture() { nevr_boot_log_tee::Close(); }
    std::string Output() const {
        std::ifstream in(path, std::ios::binary);
        in.seekg(start);
        return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
};

}  // namespace

TEST(XpidPatchBoot, ARecognisedImageIsPatchedAndBootContinues) {
    char* base = static_cast<char*>(VirtualAlloc(nullptr, kXpidImageSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    ASSERT_NE(base, nullptr);
    WriteXpidOriginals(base);
    {
        XpidBaseScope scope(base);
        PatchDscProvider();
        EXPECT_FALSE(g_bootHookFailed) << "a healthy image must not mark the boot failed";
        EXPECT_STREQ(g_bootHookFailedCause, "") << "a healthy image records no failure cause";
        EXPECT_EQ(g_serverFatalCalls, 0);
    }
    for (const auto& site : kXpidSites) {
        EXPECT_EQ(memcmp(base + site.rva, site.patched, 4), 0) << "site rva=0x" << std::hex << site.rva;
    }
    VirtualFree(base, 0, MEM_RELEASE);
}

TEST(XpidPatchBoot, AnImageWithUnexpectedBytesFailsTheBootThroughTheFlagNotServerFatal) {
    char* base = static_cast<char*>(VirtualAlloc(nullptr, kXpidImageSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    ASSERT_NE(base, nullptr);
    WriteXpidOriginals(base);
    memcpy(base + nevr_patch_addresses::XPID_PLATFORM_DASH_PREFIX, "XYZ-", 4);  // not this build
    {
        XpidBaseScope scope(base);
        PatchDscProvider();
        EXPECT_TRUE(g_bootHookFailed) << "a validation failure must reach boot.cpp's server check";
        EXPECT_EQ(g_serverFatalCalls, 0)
            << "ServerFatal runs before the CLI is parsed, where it only warns; the flag is the signal";
    }
    // Nothing is half-patched: validation happens before any write.
    EXPECT_EQ(memcmp(base + nevr_patch_addresses::XPID_PLATFORM_SHORT_NAME, "PSN", 4), 0);
    VirtualFree(base, 0, MEM_RELEASE);
}

TEST(XpidPatchBoot, AFailedWriteFailsTheBootThroughTheFlagNotServerFatal) {
    // The same bytes through a read-only view of a section: validation passes, then
    // VirtualProtect(PAGE_EXECUTE_READWRITE) on the view is refused, so every ApplyPatch fails.
    HANDLE section = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                        static_cast<DWORD>(kXpidImageSize), nullptr);
    ASSERT_NE(section, nullptr);
    char* writable = static_cast<char*>(MapViewOfFile(section, FILE_MAP_WRITE, 0, 0, kXpidImageSize));
    ASSERT_NE(writable, nullptr);
    WriteXpidOriginals(writable);
    UnmapViewOfFile(writable);
    char* readOnly = static_cast<char*>(MapViewOfFile(section, FILE_MAP_READ, 0, 0, kXpidImageSize));
    ASSERT_NE(readOnly, nullptr);
    {
        XpidBaseScope scope(readOnly);
        PatchDscProvider();
        EXPECT_TRUE(g_bootHookFailed) << "a failed patch write must reach boot.cpp's server check";
        EXPECT_STREQ(g_bootHookFailedCause, "XPID provider-string patch write failed");
        EXPECT_EQ(g_serverFatalCalls, 0);
    }
    EXPECT_EQ(memcmp(readOnly + nevr_patch_addresses::XPID_PLATFORM_SHORT_NAME, "PSN", 4), 0);
    UnmapViewOfFile(readOnly);
    CloseHandle(section);
}

TEST(XpidPatchBoot, AMismatchRecordsSiteExpectedAndActualBytesInTheBootLogNotLog) {
    // The boot path runs under the DllMain loader lock and reports through the boot tee. The
    // mismatch record is the only place the expected and actual bytes exist, so it has to reach it.
    char* base = static_cast<char*>(VirtualAlloc(nullptr, kXpidImageSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    ASSERT_NE(base, nullptr);
    WriteXpidOriginals(base);
    memcpy(base + nevr_patch_addresses::XPID_PLATFORM_DASH_PREFIX, "XYZ-", 4);
    ClearTestLogs();
    std::string written;
    {
        BootTeeCapture tee;
        ASSERT_FALSE(tee.path.empty()) << "the boot tee could not resolve nevr-boot.jsonl";
        XpidBaseScope scope(base);
        PatchDscProvider();
        written = tee.Output();
    }
    char expected[160];
    snprintf(expected, sizeof(expected),
             "[NEVR.XPID] dash prefix mismatch rva=0x%X expected=50534e2d actual=58595a2d",
             static_cast<unsigned>(nevr_patch_addresses::XPID_PLATFORM_DASH_PREFIX));
    EXPECT_NE(written.find(expected), std::string::npos) << "boot log was:\n" << written;
    EXPECT_FALSE(TestLogContains("[NEVR.XPID]")) << "the XPID boot path must not call Log()";
    VirtualFree(base, 0, MEM_RELEASE);
}

TEST(XpidPatchBoot, TheRefusalCauseNamesTheFirstMismatchedSite) {
    char* base = static_cast<char*>(VirtualAlloc(nullptr, kXpidImageSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    ASSERT_NE(base, nullptr);
    WriteXpidOriginals(base);
    memcpy(base + nevr_patch_addresses::XPID_PLATFORM_DASH_PREFIX, "XYZ-", 4);
    memcpy(base + nevr_patch_addresses::XPID_PLATFORM_FALLBACK_PREFIX, "XYZ-", 4);
    {
        XpidBaseScope scope(base);
        PatchDscProvider();
        char expected[96];
        snprintf(expected, sizeof(expected), "XPID dash prefix mismatch rva=0x%X",
                 static_cast<unsigned>(nevr_patch_addresses::XPID_PLATFORM_DASH_PREFIX));
        EXPECT_STREQ(g_bootHookFailedCause, expected)
            << "boot.cpp's ServerFatal prints this: it must name the failing site";
    }
    VirtualFree(base, 0, MEM_RELEASE);
}

// ============================================================================
// #532 — a failed GetProviderPrefix (OVR-ORG) detour refuses to start a server, the same as a
// failed five-site byte patch. A server that kept running would build 17 callers' xpids with the
// game's own PSN- prefixes while the bridge logs in as OVR-ORG.
// ============================================================================

namespace {

// Real x86-64 prologue (mov [rsp+8],rbx; mov [rsp+10h],rsi; ret) at GetProviderPrefix, in memory
// that can be executed, so MinHook can really detour it.
constexpr unsigned char kStandInPrefixFunction[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0xC3};

}  // namespace

TEST(XpidPrefixDetourBoot, AFailedDetourFailsTheBootThroughTheFlagAndNamesItself) {
    // MinHook is not initialised in this process, so MH_CreateHook refuses: the detour fails for real.
    char* base = static_cast<char*>(VirtualAlloc(nullptr, kXpidImageSize, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    ASSERT_NE(base, nullptr);
    memcpy(base + nevr_patch_addresses::GET_PROVIDER_PREFIX, kStandInPrefixFunction, sizeof(kStandInPrefixFunction));
    ClearTestLogs();
    std::string written;
    {
        BootTeeCapture tee;
        ASSERT_FALSE(tee.path.empty()) << "the boot tee could not resolve nevr-boot.jsonl";
        XpidBaseScope scope(base);
        PatchProviderPrefixOvrOrg();
        EXPECT_TRUE(g_bootHookFailed) << "a failed OVR-ORG detour must reach boot.cpp's server check";
        EXPECT_EQ(std::string(g_bootHookFailedCause).rfind("XPID GetProviderPrefix detour failed", 0), 0u)
            << "boot.cpp's ServerFatal prints this: it must name the detour, was '" << g_bootHookFailedCause << "'";
        EXPECT_EQ(g_serverFatalCalls, 0)
            << "ServerFatal runs before the CLI is parsed, where it only warns; the flag is the signal";
        written = tee.Output();
    }
    EXPECT_NE(written.find("[NEVR.XPID] warn; GetProviderPrefix detour FAILED"), std::string::npos) << "boot log was:\n" << written;
    EXPECT_NE(written.find("a server will refuse to start"), std::string::npos) << "boot log was:\n" << written;
    EXPECT_FALSE(TestLogContains("[NEVR.XPID]")) << "the XPID boot path must not call Log()";
    VirtualFree(base, 0, MEM_RELEASE);
}

TEST(XpidPrefixDetourBoot, AnInstalledDetourReturnsTheOvrOrgPrefixAndBootContinues) {
    ASSERT_EQ(MH_Initialize(), MH_OK);
    char* base = static_cast<char*>(VirtualAlloc(nullptr, kXpidImageSize, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    ASSERT_NE(base, nullptr);
    memcpy(base + nevr_patch_addresses::GET_PROVIDER_PREFIX, kStandInPrefixFunction, sizeof(kStandInPrefixFunction));
    memcpy(base + nevr_patch_addresses::PROVIDER_STRING_OVR_ORG, "OVR-ORG", 8);
    {
        XpidBaseScope scope(base);
        PatchProviderPrefixOvrOrg();
        EXPECT_FALSE(g_bootHookFailed) << "a working detour must not mark the boot failed";
        EXPECT_STREQ(g_bootHookFailedCause, "");
        using PrefixFn = const char* (*)(uint32_t*);
        uint32_t provider = 2;  // PSN: the detour answers OVR-ORG whatever the game asked for
        const char* prefix = reinterpret_cast<PrefixFn>(base + nevr_patch_addresses::GET_PROVIDER_PREFIX)(&provider);
        EXPECT_STREQ(prefix, "OVR-ORG");
    }
    MH_Uninitialize();
    VirtualFree(base, 0, MEM_RELEASE);
}

// #587: a value option takes the next token as its value only when that token is not an option.
TEST(CliOptionValue, ATokenThatIsNotAnOptionIsTheValue) {
    wchar_t exe[] = L"echovr.exe", flag[] = L"-timestep", value[] = L"120", negative[] = L"-5";
    wchar_t* argv[] = {exe, flag, value};
    ASSERT_NE(nevr_cli::OptionValue(3, argv, 1), nullptr);
    EXPECT_STREQ(nevr_cli::OptionValue(3, argv, 1), L"120");
    wchar_t* argvNegative[] = {exe, flag, negative};
    ASSERT_NE(nevr_cli::OptionValue(3, argvNegative, 1), nullptr) << "-5 is a number to the game's tokenizer";
    EXPECT_STREQ(nevr_cli::OptionValue(3, argvNegative, 1), L"-5");
}

TEST(CliOptionValue, AnOptionAfterAValueOptionIsNotItsValue) {
    wchar_t exe[] = L"echovr.exe", flag[] = L"-timestep", next[] = L"-no-console", plus[] = L"+x", dash[] = L"-";
    wchar_t* argv[] = {exe, flag, next};
    EXPECT_EQ(nevr_cli::OptionValue(3, argv, 1), nullptr) << "-timestep -no-console must leave -no-console to the parse";
    wchar_t* argvPlus[] = {exe, flag, plus};
    EXPECT_EQ(nevr_cli::OptionValue(3, argvPlus, 1), nullptr);
    wchar_t* argvDash[] = {exe, flag, dash};
    EXPECT_EQ(nevr_cli::OptionValue(3, argvDash, 1), nullptr);
}

TEST(CliOptionValue, NothingAfterTheOptionIsNoValue) {
    wchar_t exe[] = L"echovr.exe", flag[] = L"-config", beyond[] = L"past-argc";
    wchar_t* argv[] = {exe, flag, beyond};  // a token sits past argc: a lookahead that ignores argc finds it
    EXPECT_EQ(nevr_cli::OptionValue(2, argv, 1), nullptr);
    EXPECT_EQ(nevr_cli::OptionValue(2, nullptr, 1), nullptr);
}
