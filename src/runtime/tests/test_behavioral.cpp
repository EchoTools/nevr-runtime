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
#include <shellapi.h>

#include <gtest/gtest.h>
#include <ixwebsocket/IXConnectionState.h>
#include <ixwebsocket/IXSocket.h>
#include <ixwebsocket/IXSocketServer.h>
#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXWebSocket.h>
#include <ixwebsocket/IXWebSocketServer.h>
#include <chrono>
#include <condition_variable>
#include <future>
#include <cstdarg>
#include <cstring>
#include <algorithm>
#include <array>
#include <cstddef>
#include <utility>
#include <atomic>
#include <iterator>
#include <filesystem>
#include <memory>
#include <random>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <nlohmann/json.hpp>
#include <mutex>
#include <signal.h>
#include <new>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

// Project headers for type info. winsock2/windows already included above,
// so pch.h includes become no-ops via include guards.
#include "abi/echovr.h"
#include "abi/echovr_functions.h"
#include "abi/game_level.h"
#include "nevr/symbol_hash.h"
#include "runtime/log/symcache.h"
#include "runtime/patch/ui_texture_mips_rules.h"
#include "runtime/hook/symbol_corpus.h"
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
Peer_LookupByAddrOrIDFunc* Peer_LookupByAddrOrID = nullptr;
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

// --- logging.h implementations (logging.cpp is not linked into this test) ---
const char* GetLogLevelString(EchoVR::LogLevel) { return "info"; }

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

// The park point for the flush tests (see Log).
std::atomic<bool> g_flushPark{false};
std::promise<void>* g_flushParked = nullptr;
std::shared_future<void> g_flushRelease;

void Log(EchoVR::LogLevel level, const char* format, ...) {
  char buffer[2048] = {};
  va_list args;
  va_start(args, format);
  vsnprintf(buffer, sizeof(buffer), format, args);
  va_end(args);
  // Park the logging thread on the "login injected" line (HandleRemoteOpen, between its first batch of
  // sends and its re-lock) until the test releases it.
  if (g_flushPark.load() && std::strstr(buffer, "login injected") != nullptr && g_flushPark.exchange(false)) {
    g_flushParked->set_value();
    g_flushRelease.wait();
  }
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
// A WebSocket worker thread can reach ServerFatal while the test thread waits on the count, so the count and the
// cause are written together under one mutex and read as a snapshot.
std::mutex g_serverFatalMutex;
int g_serverFatalCallsValue = 0;
std::string g_lastServerFatalValue;
int ServerFatalCalls() {
  std::lock_guard<std::mutex> lock(g_serverFatalMutex);
  return g_serverFatalCallsValue;
}
std::string LastServerFatal() {
  std::lock_guard<std::mutex> lock(g_serverFatalMutex);
  return g_lastServerFatalValue;
}
void ResetServerFatal() {
  std::lock_guard<std::mutex> lock(g_serverFatalMutex);
  g_serverFatalCallsValue = 0;
  g_lastServerFatalValue.clear();
}
void ServerFatal(const CHAR* format, ...) {
  char buf[1024];
  va_list args;
  va_start(args, format);
  vsnprintf(buf, sizeof(buf), format, args);
  va_end(args);
  {
    std::lock_guard<std::mutex> lock(g_serverFatalMutex);
    g_lastServerFatalValue = buf;
    g_serverFatalCallsValue++;
  }
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
// #1082: nevr_discord_id answers from g_testDiscordId (the config fallback of the login identity), so a login test
// has an account that is not 0 (the local user's UUID is the zero UUID for account 0).
const char* g_testDiscordId = nullptr;
// The synthetic account the login tests feed through the config fallback; its decimal text is made from the number
// so the source holds no 18-digit string (the N20 sensor in `just verify` reads one as an account id).
constexpr uint64_t kTestAccountId = 123456789012345678ULL;
const std::string kTestAccountIdText = std::to_string(kTestAccountId);
// #1254: nevr_password answers from g_testPassword; a server needs it and the discord id to log in.
const char* g_testPassword = nullptr;
const char* NevrCfgGetFlat(const char* flatKey) {
  if (std::strcmp(flatKey, "nevr_session_router") == 0) return g_testRouterMode;
  if (std::strcmp(flatKey, "nevr_discord_id") == 0) return g_testDiscordId;
  if (std::strcmp(flatKey, "nevr_password") == 0) return g_testPassword;
  return nullptr;
}
// #1254: the config the stop line names; service_config.cpp is not linked here.
const char* NevrCfgSource() { return "test config"; }

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
#include "runtime/patch/asset_registry.h"
#include "runtime/ext/module_loader.h"
#include "core/hex_dump.h"
#include "core/nevr_root.h"
#include "runtime/patch/matchmaker_host_patch.h"
#include "runtime/compat/ws_bridge.h"
#include "runtime/compat/self_check.h"
#include "runtime/compat/evr_codec.h"
#include "runtime/compat/socket_role.h"
#include "runtime/compat/hmd_serial.h"
#include "runtime/compat/social_party.h"
#include "runtime/tests/lobby_record_golden.h"
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

// The plugin directory of a run is <root>\\plugins, the root under %LOCALAPPDATA% (#856, ADR 0012), never the folder of the game
// executable. A test gives the process a LOCALAPPDATA of its own, a folder it removes afterwards, and starts that root's
// plugins\\ with the plugins the build put next to the test executable (the fixture DLLs the loader tests plan by name).
class TestNevrRoot {
 public:
  TestNevrRoot() {
    char exePath[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    std::string exeDir(exePath);
    exeDir.resize(exeDir.find_last_of("\\/") + 1);
    builtPluginsDir_ = exeDir + "plugins\\";
    static std::atomic<unsigned> counter{0};
    localAppData_ = exeDir + "nevr-test-localappdata-" + std::to_string(GetCurrentProcessId()) + "-" +
                    std::to_string(counter.fetch_add(1));
    char saved[MAX_PATH * 2] = {};
    const DWORD n = GetEnvironmentVariableA("LOCALAPPDATA", saved, static_cast<DWORD>(sizeof(saved)));
    hadLocalAppData_ = n != 0 && n < sizeof(saved);
    if (hadLocalAppData_) savedLocalAppData_.assign(saved, n);
    SetEnvironmentVariableA("LOCALAPPDATA", localAppData_.c_str());
    pluginsDir_ = nevr_root::PluginsDir(nevr_root::RootA()) + "\\";
    std::error_code ec;
    std::filesystem::create_directories(pluginsDir_, ec);
    for (const auto& entry : std::filesystem::directory_iterator(builtPluginsDir_, ec)) {
      if (entry.is_regular_file(ec)) {
        std::filesystem::copy_file(entry.path(), std::filesystem::path(pluginsDir_) / entry.path().filename(),
                                   std::filesystem::copy_options::overwrite_existing, ec);
      }
    }
  }
  ~TestNevrRoot() {
    if (hadLocalAppData_) {
      SetEnvironmentVariableA("LOCALAPPDATA", savedLocalAppData_.c_str());
    } else {
      SetEnvironmentVariableA("LOCALAPPDATA", nullptr);
    }
    std::error_code ec;
    std::filesystem::remove_all(localAppData_, ec);
  }
  TestNevrRoot(const TestNevrRoot&) = delete;
  TestNevrRoot& operator=(const TestNevrRoot&) = delete;

  // <root>\\plugins\\, where the loader looks.
  const std::string& PluginsDir() const { return pluginsDir_; }
  // <folder of the test executable>\\plugins\\: where the build puts the fixture DLLs, and where the loader used to look.
  const std::string& BuiltPluginsDir() const { return builtPluginsDir_; }

 private:
  std::string localAppData_, pluginsDir_, builtPluginsDir_, savedLocalAppData_;
  bool hadLocalAppData_ = false;
};

class PluginLoaderDiagnosticTest : public ::testing::Test {
 protected:
  void SetUp() override {
    UnloadPlugins();
    g_testPluginLoadPlan.clear();
    ClearTestLogs();
    EchoVR::g_GameBaseAddress = reinterpret_cast<CHAR*>(GetModuleHandleA(nullptr));
    ASSERT_NE(EchoVR::g_GameBaseAddress, nullptr);
    nevrRoot_ = std::make_unique<TestNevrRoot>();
    // These tests are about the loader, not the signature gate: their fixture DLLs are unsigned. The gate has
    // its own fixture below (PluginSignatureGateTest), which enforces.
    nevr_plugin_trust::TestHook_SetTrust(nevr_plugin_trust::Mode::UnsignedAllowed, nullptr);
  }

  void TearDown() override {
    UnloadPlugins();
    g_testPluginLoadPlan.clear();
    ClearTestLogs();
    nevr_plugin_trust::TestHook_ResetTrust();
    nevrRoot_.reset();
  }

  std::unique_ptr<TestNevrRoot> nevrRoot_;
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

    pluginsDir_ = nevrRoot_->PluginsDir();
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

TEST_F(PluginSignatureGateTest, UnsignedAllowedRunLoadsAnUnsignedPluginWithAWarning) {
  nevr_plugin_trust::TestHook_SetTrust(nevr_plugin_trust::Mode::UnsignedAllowed, &policy_);
  Plan("gate_dev.dll", "unsigned");
  LoadPlugins();
  EXPECT_EQ(GetLoadedPluginCount(), 1);
  EXPECT_TRUE(TestLogContainsAtLevel(EchoVR::LogLevel::Warning,
                                     "file=gate_dev.dll UNSIGNED, loaded because plugin signatures are not required in this run "
                                     "(plugin trust mode unsigned-allowed; start with -require-signed-plugins to refuse it)"));
}

TEST_F(PluginSignatureGateTest, RefusalIsPerPluginAndTheSignedOneStillLoads) {
  Plan("gate_a_unsigned.dll", "unsigned");
  Plan("gate_b_signed.dll", "signed");
  LoadPlugins();
  EXPECT_EQ(GetLoadedPluginCount(), 1);
  EXPECT_TRUE(TestLogContains("file=gate_a_unsigned.dll refused: unsigned"));
}

// ---- plugins load from the nevr root (ADR 0012, #856) ------------------------------------------------------------

class PluginsFromTheRootTest : public PluginSignatureGateTest {
 protected:
  void TearDown() override {
    TestHook_ResetPluginLoadFilter();
    PluginSignatureGateTest::TearDown();
  }

  // A signed copy of the fixture plugin as `name` in the folder the loader used to read, beside the game executable.
  std::string PlaceBesideTheGame(const std::string& name, const std::string& kind) {
    const std::string source = fixtures_ + "/" + manifest_["files"]["test_plugin_onframe." + kind].get<std::string>();
    std::error_code ec;
    std::filesystem::create_directories(nevrRoot_->BuiltPluginsDir(), ec);
    const std::string target = nevrRoot_->BuiltPluginsDir() + name;
    EXPECT_TRUE(CopyFileA(source.c_str(), target.c_str(), FALSE)) << source << " error " << GetLastError();
    copies_.push_back(target);
    return target;
  }

  static std::wstring LowerWide(const std::string& path) {
    std::wstring wide;
    for (char c : path) wide.push_back(static_cast<wchar_t>(std::tolower(static_cast<unsigned char>(c))));
    return wide;
  }
};

TEST_F(PluginsFromTheRootTest, TheLoaderReadsAndNamesThePluginFolderOfTheNevrRoot) {
  Plan("root_signed.dll", "signed");
  LoadPlugins();
  EXPECT_EQ(GetLoadedPluginCount(), 1);
  EXPECT_TRUE(TestLogContains("loading in list order from " + pluginsDir_)) << pluginsDir_;
  EXPECT_NE(pluginsDir_.find("\\nevr\\echovr\\plugins\\"), std::string::npos) << pluginsDir_;
}

TEST_F(PluginsFromTheRootTest, ASignedPluginBesideTheGameExecutableIsNotLoaded) {
  PlaceBesideTheGame("beside_signed.dll", "signed");
  g_testPluginLoadPlan.push_back({"beside_signed.dll", "beside_signed.dll", false, "", "{}"});
  LoadPlugins();
  EXPECT_EQ(GetLoadedPluginCount(), 0) << "the game folder is not a plugin folder";
  EXPECT_EQ(GetModuleHandleA("beside_signed.dll"), nullptr);
  EXPECT_TRUE(TestLogContains("beside_signed.dll")) << "the entry is reported as failed to load";
}

TEST_F(PluginsFromTheRootTest, TheLateLoadFilterGuardsTheRootsPluginFolderAndNotTheGameFolder) {
  Plan("filter_signed.dll", "signed");
  LoadPlugins();
  ASSERT_EQ(GetLoadedPluginCount(), 1);
  Plan("late_in_root.dll", "unsigned");
  const std::string beside = PlaceBesideTheGame("late_beside.dll", "unsigned");
  EXPECT_TRUE(nevr_dll_load_hook::IsLoadBlocked(LowerWide(pluginsDir_ + "late_in_root.dll").c_str(), nullptr))
      << "an unsigned DLL loaded late from the nevr root's plugin folder is refused";
  EXPECT_FALSE(nevr_dll_load_hook::IsLoadBlocked(LowerWide(beside).c_str(), nullptr))
      << "the game folder's plugins\\ is no longer the plugin directory";
}

TEST_F(PluginsFromTheRootTest, WithoutLocalAppDataNoPluginLoadsAndTheGameFolderIsNeverSearched) {
  PlaceBesideTheGame("beside_nolocal.dll", "signed");
  SetEnvironmentVariableA("LOCALAPPDATA", nullptr);
  g_testPluginLoadPlan.push_back({"beside_nolocal.dll", "beside_nolocal.dll", false, "", "{}"});
  LoadPlugins();
  EXPECT_EQ(GetLoadedPluginCount(), 0);
  EXPECT_EQ(GetModuleHandleA("beside_nolocal.dll"), nullptr);
  EXPECT_TRUE(TestLogContainsAtLevel(EchoVR::LogLevel::Error, "the plugin directory cannot be resolved"));
  const nlohmann::json manifest = nlohmann::json::parse(BuildPluginManifestJson());
  ASSERT_EQ(manifest.size(), 1u);
  EXPECT_FALSE(manifest[0]["loaded"].get<bool>());
  EXPECT_NE(manifest[0]["error"].get<std::string>().find("plugin directory cannot be resolved"), std::string::npos);
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

TEST_F(PluginDependencyGateTest, UnsignedAllowedRunDoesNotWalkTheDependencies) {
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
}

// After LoadPlugins has finished nothing else would tell the player: the refusal shows the notice itself, once.
// The test never calls the dialog hook; production does not either.
TEST_F(PluginDependencyGateTest, ALateRefusalAfterLoadPluginsShowsTheNoticeItself) {
  Plan("late_notice_signed.dll", "signed");
  LoadPlugins();  // loads it, installs the filter, shows its own (empty) notice
  ASSERT_EQ(GetLoadedPluginCount(), 1);
  ASSERT_TRUE(g_refusalDialogs.empty());
  Plan("late_notice.dll", "unsigned");
  g_testPluginLoadPlan.pop_back();  // copied into plugins/, never planned to load
  EXPECT_TRUE(Blocked("late_notice.dll"));
  ASSERT_EQ(g_refusalDialogs.size(), 1u) << "the player was not told";
  EXPECT_NE(g_refusalDialogs[0].find("late_notice.dll"), std::string::npos);
  Plan("late_notice_two.dll", "unsigned");
  g_testPluginLoadPlan.pop_back();
  EXPECT_TRUE(Blocked("late_notice_two.dll"));
  EXPECT_EQ(g_refusalDialogs.size(), 1u) << "one dialog per run";
}

// A refusal made while LoadPlugins still runs is shown by LoadPlugins's own notice, not before.
TEST_F(PluginDependencyGateTest, ARefusalBeforeLoadPluginsEndsWaitsForItsNotice) {
  Plan("early_notice.dll", "unsigned");
  g_testPluginLoadPlan.clear();
  TestHook_InstallPluginLoadFilter(pluginsDir_);
  EXPECT_TRUE(Blocked("early_notice.dll"));
  EXPECT_TRUE(g_refusalDialogs.empty());
}

// LoadLibrary appends ".dll" to a name with no extension, so a late load of "<plugins>\late_noext" maps
// late_noext.dll.
TEST_F(PluginDependencyGateTest, ALateLoadNamedWithoutAnExtensionIsRefused) {
  Plan("late_noext.dll", "unsigned");
  g_testPluginLoadPlan.clear();
  TestHook_InstallPluginLoadFilter(pluginsDir_);
  EXPECT_TRUE(Blocked("late_noext"));
  EXPECT_TRUE(TestLogContains("refused a later load of late_noext.dll: unsigned"));
}

TEST_F(PluginDependencyGateTest, ALateLoadByBareNameWithoutAnExtensionIsRefused) {
  Plan("late_bare_noext.dll", "unsigned");
  g_testPluginLoadPlan.clear();
  TestHook_InstallPluginLoadFilter(pluginsDir_);
  EXPECT_TRUE(nevr_dll_load_hook::IsLoadBlocked(L"late_bare_noext", nullptr));
}

// A relative path with a directory part: the loader finds it from the application directory even when the
// current directory is somewhere else.
class CurrentDirectoryScope {
 public:
  explicit CurrentDirectoryScope(const wchar_t* to) {
    wchar_t buf[MAX_PATH * 2] = {};
    const DWORD n = GetCurrentDirectoryW(static_cast<DWORD>(std::size(buf)), buf);
    if (n != 0 && n < std::size(buf)) saved_.assign(buf, n);
    SetCurrentDirectoryW(to);
  }
  ~CurrentDirectoryScope() {
    if (!saved_.empty()) SetCurrentDirectoryW(saved_.c_str());
  }

 private:
  std::wstring saved_;
};

// The loader appends a relative path to every directory it searches (System32, the Windows directory, PATH, the
// SetDllDirectory directories, each of them through links), so no list of candidates is complete: a "..", an
// ancestor of plugins\ and a junction in a search directory each reach it. In a signed-only build every relative
// path with a directory part, "\" or "/", is refused, whatever it leads to.
static const wchar_t* const kRelativeWithADirectoryPart[] = {
    L"plugins\\late_rel.dll",           L"plugins/late_rel.dll",       L".\\late_cwd.dll",
    L"./late_cwd.dll",                  L"..\\..\\no-such-dir\\late_up.dll",
    L".\\..\\x.dll",                    L"./../x.dll",                 L"sub\\..\\..\\x.dll",
    L"..\\x.dll",                       L"../x.dll",                   L"no-such-dir\\a..b.dll",
    L"work\\ancestor\\of\\plugins\\k.dll", L"a/b.dll",
};

TEST_F(PluginDependencyGateTest, ARelativePathWithADirectoryPartIsRefusedWhateverItLeadsTo) {
  Plan("late_rel.dll", "unsigned");
  g_testPluginLoadPlan.clear();
  TestHook_InstallPluginLoadFilter(pluginsDir_);
  wchar_t temp[MAX_PATH] = {};
  ASSERT_GT(GetTempPathW(MAX_PATH, temp), 0u);
  CurrentDirectoryScope cwd(temp);
  for (const wchar_t* spelling : kRelativeWithADirectoryPart) {
    SCOPED_TRACE(std::string(spelling, spelling + wcslen(spelling)));
    EXPECT_TRUE(nevr_dll_load_hook::IsLoadBlocked(spelling, nullptr));
  }
  EXPECT_TRUE(TestLogContains("a relative path with a directory part cannot be checked"));
}

// A name with no directory part, and a full path, keep the rules they had: a bare name is the plugins directory's
// when that directory holds it; a file that is not there is the real load's to report.
TEST_F(PluginDependencyGateTest, ABareNameAndAFullPathKeepTheirOwnRules) {
  Plan("late_bare.dll", "unsigned");
  g_testPluginLoadPlan.clear();
  TestHook_InstallPluginLoadFilter(pluginsDir_);
  EXPECT_TRUE(nevr_dll_load_hook::IsLoadBlocked(L"late_bare.dll", nullptr));
  EXPECT_TRUE(TestLogContains("refused a later load of late_bare.dll: unsigned"));
  std::wstring full = std::wstring(pluginsDir_.begin(), pluginsDir_.end()) + L"late_bare.dll";
  for (wchar_t& c : full) c = static_cast<wchar_t>(towlower(c));  // the hook hands the filter a lower-cased path
  EXPECT_TRUE(nevr_dll_load_hook::IsLoadBlocked(full.c_str(), nullptr));
  EXPECT_FALSE(nevr_dll_load_hook::IsLoadBlocked(L"no-such-file.dll", nullptr));
}

TEST_F(PluginDependencyGateTest, ADevBuildLetsARelativePathWithADirectoryPartThrough) {
  nevr_plugin_trust::TestHook_SetTrust(nevr_plugin_trust::Mode::UnsignedAllowed, &policy_);
  TestHook_InstallPluginLoadFilter(pluginsDir_);
  for (const wchar_t* spelling : kRelativeWithADirectoryPart) {
    SCOPED_TRACE(std::string(spelling, spelling + wcslen(spelling)));
    EXPECT_FALSE(nevr_dll_load_hook::IsLoadBlocked(spelling, nullptr));
  }
}

TEST_F(PluginDependencyGateTest, ARelativePathThatCannotBeResolvedIsRefusedNotLetThrough) {
  TestHook_InstallPluginLoadFilter(pluginsDir_);
  std::wstring tooLong;
  for (int i = 0; i < 700; ++i) tooLong += L"a\\";
  tooLong += L"x.dll";
  EXPECT_TRUE(nevr_dll_load_hook::IsLoadBlocked(tooLong.c_str(), nullptr));
  EXPECT_TRUE(TestLogContains("a relative path with a directory part cannot be checked"));
}

TEST_F(PluginDependencyGateTest, ADevBuildLetsAnUnresolvableRelativePathThrough) {
  nevr_plugin_trust::TestHook_SetTrust(nevr_plugin_trust::Mode::UnsignedAllowed, &policy_);
  TestHook_InstallPluginLoadFilter(pluginsDir_);
  std::wstring tooLong;
  for (int i = 0; i < 700; ++i) tooLong += L"a\\";
  tooLong += L"x.dll";
  EXPECT_FALSE(nevr_dll_load_hook::IsLoadBlocked(tooLong.c_str(), nullptr));
}

// The game under a path with a character the ANSI code page spells as a byte above 0x7F. The loader's directory is
// an ANSI string; widening it byte by byte makes a directory no path ever starts with, and the filter checked
// nothing.
TEST_F(PluginDependencyGateTest, ALateLoadUnderANonAsciiGamePathIsRefused) {
  std::wstring wideDir(pluginsDir_.begin(), pluginsDir_.end());
  const std::wstring root = wideDir.substr(0, wideDir.size() - std::wstring(L"plugins\\").size()) + L"Nevr_T\u0161\\";
  const std::wstring plugins = root + L"plugins\\";
  CreateDirectoryW(root.c_str(), nullptr);
  ASSERT_TRUE(CreateDirectoryW(plugins.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS) << GetLastError();
  BOOL usedDefault = FALSE;
  char narrow[MAX_PATH] = {};
  ASSERT_GT(WideCharToMultiByte(CP_ACP, 0, plugins.c_str(), -1, narrow, MAX_PATH, nullptr, &usedDefault), 0);
  ASSERT_FALSE(usedDefault) << "the ANSI code page cannot spell U+0161: this form is not under test";
  bool hasHighByte = false;
  for (const char* c = narrow; *c != '\0'; ++c) hasHighByte = hasHighByte || static_cast<unsigned char>(*c) > 0x7F;
  ASSERT_TRUE(hasHighByte) << "the spelled path has no byte above 0x7F: this form is not under test";
  const std::string source = fixtures_ + "/" + manifest_["files"]["test_plugin_onframe.unsigned"].get<std::string>();
  const std::wstring target = plugins + L"late_na.dll";
  ASSERT_TRUE(CopyFileW(std::wstring(source.begin(), source.end()).c_str(), target.c_str(), FALSE)) << GetLastError();
  TestHook_InstallPluginLoadFilter(narrow);
  std::wstring lowered = target;
  for (wchar_t& c : lowered) c = (c >= L'A' && c <= L'Z') ? static_cast<wchar_t>(c + 32) : c;
  const bool blocked = nevr_dll_load_hook::IsLoadBlocked(lowered.c_str(), nullptr);
  // The ANSI hooks hand the filter the path through the code page: the same wide spelling.
  const std::wstring viaHook = nevr_dll_load_hook::AnsiPathToLowerWide(narrow);
  TestHook_ResetPluginLoadFilter();
  DeleteFileW(target.c_str());
  RemoveDirectoryW(plugins.c_str());
  RemoveDirectoryW(root.c_str());
  EXPECT_TRUE(blocked);
  std::wstring expected = plugins;
  for (wchar_t& c : expected) c = (c >= L'A' && c <= L'Z') ? static_cast<wchar_t>(c + 32) : c;
  EXPECT_EQ(viaHook, expected);
}

// A plugin's own DllMain loading another file runs inside the loader's own load of that plugin, with the loader
// lock held. The file it loads is not the one the loader checked.
TEST_F(PluginDependencyGateTest, ALoadMadeFromAStagedPluginsDllMainIsChecked) {
  Plan("dllmain_victim.dll", "unsigned");
  g_testPluginLoadPlan.clear();
  PlanTree("dllmain_load");
  const std::wstring victim = std::wstring(pluginsDir_.begin(), pluginsDir_.end()) + L"dllmain_victim.dll";
  ASSERT_TRUE(SetEnvironmentVariableW(L"NEVR_TEST_DLLMAIN_LOAD", victim.c_str()));
  // The plugin's LoadLibraryW reaches the filter only through the real hooks.
  ASSERT_EQ(MH_Initialize(), MH_OK);
  const int hooked = nevr_dll_load_hook::Install();
  if (hooked == 4) {
    LoadPlugins();
    // The ANSI entry points reach the filter through the same hooks, with the path in the ANSI code page.
    const std::string victimNarrow = pluginsDir_ + "dllmain_victim.dll";
    SetLastError(0);
    EXPECT_EQ(LoadLibraryA(victimNarrow.c_str()), nullptr) << "LoadLibraryA mapped an unsigned file in plugins/";
    EXPECT_EQ(GetLastError(), static_cast<DWORD>(ERROR_MOD_NOT_FOUND));
    EXPECT_EQ(LoadLibraryExA(victimNarrow.c_str(), nullptr, 0), nullptr) << "LoadLibraryExA mapped an unsigned file";
  }
  nevr_dll_load_hook::Shutdown();  // also clears the registered filters
  MH_Uninitialize();
  nevr_hook_guard::ResetForTest();
  TestHook_ForgetLoadFilterRegistration();
  ASSERT_EQ(hooked, 4) << "the LoadLibrary hooks did not install";
  SetEnvironmentVariableW(L"NEVR_TEST_DLLMAIN_LOAD", nullptr);
  EXPECT_EQ(GetModuleHandleA("dllmain_victim.dll"), nullptr) << "a file loaded from a DllMain was mapped unchecked";
  ASSERT_EQ(g_refusalDialogs.size(), 1u) << "the refusal reached no notice";
  EXPECT_NE(g_refusalDialogs[0].find("dllmain_victim.dll"), std::string::npos);
}

// #768: a signed plugin whose one import is a System32 library (hid.dll). Nothing in the plugins directory names it,
// so the check accepts the tree; the loader searches the plugin's own directory before System32 for a name that is
// not already loaded, so an unsigned stand-in put there after the check is the one that maps. The stand-in says it
// was loaded by setting NEVR_TEST_DECOY_LOADED.
std::string g_decoyFrom;
std::string g_decoyTo;

void PlantTheDecoyInThePluginsDirectory() { CopyFileA(g_decoyFrom.c_str(), g_decoyTo.c_str(), FALSE); }

class PluginSystemDependencyTest : public PluginDependencyGateTest {
 protected:
  void SetUp() override {
    PluginDependencyGateTest::SetUp();
    ReleaseTheFixtureModules();
    ASSERT_EQ(GetModuleHandleA("hid.dll"), nullptr) << "hid.dll is already loaded in this process: nothing to plant against";
    const nlohmann::json& t = manifest_["trees"]["import_system"];
    g_decoyFrom = fixtures_ + "/" + t["decoy"].get<std::string>();
    g_decoyTo = pluginsDir_ + "hid.dll";
    SetEnvironmentVariableA("NEVR_TEST_DECOY_LOADED", nullptr);
  }

  void TearDown() override {
    TestHook_SetAfterDependenciesChecked(nullptr);
    ReleaseTheFixtureModules();
    SetEnvironmentVariableA("NEVR_TEST_DECOY_LOADED", nullptr);
    PluginDependencyGateTest::TearDown();
  }

  static void ReleaseTheFixtureModules() {
    for (const char* name : {"sysimporter.dll", "hid.dll"}) {
      while (HMODULE m = GetModuleHandleA(name)) {
        if (!FreeLibrary(m)) break;
      }
    }
  }

  static bool TheDecoyRan() { return GetEnvironmentVariableA("NEVR_TEST_DECOY_LOADED", nullptr, 0) != 0; }
};

TEST_F(PluginSystemDependencyTest, AStandInPlantedForASystemImportAfterTheCheckIsNotTheOneThatLoads) {
  PlanTree("import_system");
  TestHook_SetAfterDependenciesChecked(&PlantTheDecoyInThePluginsDirectory);
  LoadPlugins();
  TestHook_SetAfterDependenciesChecked(nullptr);
  copies_.push_back(g_decoyTo);  // removed at teardown, unless it was mapped
  EXPECT_FALSE(TheDecoyRan()) << "the stand-in planted after the check was loaded with the plugin";
  EXPECT_FALSE(TestLogContains("LoadLibrary failed")) << "the plugin did not load";
  EXPECT_FALSE(TestLogContains("refused"));
  EXPECT_TRUE(TestLogContains("plugin=sysimporter.dll file=sysimporter.dll 1 system dependencies loaded from the system directory first"));
  // The fixture is not a real plugin, so it is unloaded for lacking the plugin exports; the pin goes with it.
  EXPECT_EQ(GetModuleHandleA("sysimporter.dll"), nullptr);
  EXPECT_EQ(GetModuleHandleA("hid.dll"), nullptr) << "the pin outlived the plugin it was taken for";
}

TEST_F(PluginSystemDependencyTest, ASystemLibraryThatCannotBePinnedRefusesTheStagedPlugin) {
  PlanTree("import_system");
  TestHook_FailPinning(true);
  LoadPlugins();
  TestHook_FailPinning(false);
  EXPECT_EQ(GetLoadedPluginCount(), 0);
  EXPECT_EQ(GetModuleHandleA("sysimporter.dll"), nullptr) << "the plugin was mapped";
  const std::string expected = "refused: dependency hid.dll cannot be loaded from the system directory to pin it: error 5";
  EXPECT_TRUE(TestLogContains(expected));
  const nlohmann::json manifest = nlohmann::json::parse(BuildPluginManifestJson());
  ASSERT_EQ(manifest.size(), 1u);
  EXPECT_EQ(manifest[0]["error"].get<std::string>(), expected);
  EXPECT_FALSE(g_refusalDialogs.empty());
}

TEST_F(PluginSystemDependencyTest, ASystemLibraryThatCannotBePinnedRefusesALaterLoad) {
  Copy("import_system");
  g_testPluginLoadPlan.clear();
  TestHook_InstallPluginLoadFilter(pluginsDir_);
  TestHook_FailPinning(true);
  const bool blocked = Blocked("sysimporter.dll");
  TestHook_FailPinning(false);
  EXPECT_TRUE(blocked);
  EXPECT_TRUE(TestLogContains("refused a later load of sysimporter.dll: dependency hid.dll cannot be loaded from the system directory to pin it: error 5"));
}

TEST_F(PluginSystemDependencyTest, ALaterLoadsPinStaysLoadedAfterTheFilterIsReset) {
  Copy("import_system");
  g_testPluginLoadPlan.clear();
  TestHook_InstallPluginLoadFilter(pluginsDir_);
  EXPECT_FALSE(Blocked("sysimporter.dll"));
  EXPECT_NE(GetModuleHandleA("hid.dll"), nullptr) << "the filter did not pin hid.dll";
  TestHook_ResetPluginLoadFilter();  // drops the filter's verified entries
  EXPECT_NE(GetModuleHandleA("hid.dll"), nullptr) << "the pin was released with the entry, before the load it protects";
}

TEST_F(PluginSystemDependencyTest, AnUnsignedAllowedRunPinsNothing) {
  nevr_plugin_trust::TestHook_SetTrust(nevr_plugin_trust::Mode::UnsignedAllowed, &policy_);
  Copy("import_system");
  g_testPluginLoadPlan.clear();
  TestHook_InstallPluginLoadFilter(pluginsDir_);
  EXPECT_FALSE(Blocked("sysimporter.dll"));
  EXPECT_EQ(GetModuleHandleA("hid.dll"), nullptr) << "a run that does not require signatures loaded a library for the check";
}

TEST_F(PluginSystemDependencyTest, AStandInPlantedForASystemImportAfterALaterLoadsCheckIsNotTheOneThatLoads) {
  Copy("import_system");
  g_testPluginLoadPlan.clear();
  const std::string plugin = pluginsDir_ + "sysimporter.dll";
  // The plugin's own LoadLibrary reaches the filter only through the real hooks.
  ASSERT_EQ(MH_Initialize(), MH_OK);
  const int hooked = nevr_dll_load_hook::Install();
  if (hooked == 4) {
    TestHook_InstallPluginLoadFilter(pluginsDir_);
    TestHook_SetAfterDependenciesChecked(&PlantTheDecoyInThePluginsDirectory);
    SetLastError(0);
    HMODULE loaded = LoadLibraryExA(plugin.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    TestHook_SetAfterDependenciesChecked(nullptr);
    EXPECT_NE(loaded, nullptr) << "the checked plugin did not load: error " << GetLastError();
  }
  nevr_dll_load_hook::Shutdown();  // also clears the registered filters
  MH_Uninitialize();
  nevr_hook_guard::ResetForTest();
  TestHook_ForgetLoadFilterRegistration();
  ASSERT_EQ(hooked, 4) << "the LoadLibrary hooks did not install";
  copies_.push_back(g_decoyTo);
  EXPECT_FALSE(TheDecoyRan()) << "the stand-in planted after the filter's check was loaded with the plugin";
}

// #769: the loader's own load of a staged plugin is not checked twice, because the loader checked the file first. That
// exemption is for the loader's call. The plugin's DllMain runs inside the same call, with the loader lock held, and
// a load it makes is the plugin's own: it has to be checked like any other, even by the path being loaded.
class PluginDllMainLoadTest : public PluginDependencyGateTest {
 protected:
  void TearDown() override {
    SetEnvironmentVariableW(L"NEVR_TEST_DLLMAIN_LOAD", nullptr);
    // The fixture is not a real plugin, so it is released here: a file that stays mapped cannot be copied over by the
    // next test.
    while (HMODULE m = GetModuleHandleA("dllmain_loader.dll")) {
      if (!FreeLibrary(m)) break;
    }
    PluginDependencyGateTest::TearDown();
  }

  // Runs LoadPlugins with the real LoadLibrary hooks installed, as the game has them; false if they did not install.
  bool LoadPluginsWithTheRealHooks(size_t* checked = nullptr) {
    EXPECT_EQ(MH_Initialize(), MH_OK);
    const int hooked = nevr_dll_load_hook::Install();
    if (hooked == 4) {
      LoadPlugins();
      if (checked != nullptr) *checked = TestHook_LoadFilterVerifiedCount();
    }
    nevr_dll_load_hook::Shutdown();  // also clears the registered filters
    MH_Uninitialize();
    nevr_hook_guard::ResetForTest();
    TestHook_ForgetLoadFilterRegistration();
    EXPECT_EQ(hooked, 4) << "the LoadLibrary hooks did not install";
    return hooked == 4;
  }
};

TEST_F(PluginDllMainLoadTest, ALoadOfThePluginsOwnPathFromItsDllMainIsChecked) {
  PlanTree("dllmain_load");
  const std::wstring self = std::wstring(pluginsDir_.begin(), pluginsDir_.end()) + L"dllmain_loader.dll";
  ASSERT_TRUE(SetEnvironmentVariableW(L"NEVR_TEST_DLLMAIN_LOAD", self.c_str()));
  size_t checked = 0;
  ASSERT_TRUE(LoadPluginsWithTheRealHooks(&checked));
  EXPECT_EQ(checked, 1u) << "a load of the plugin's own path from its DllMain was not checked";
}

TEST_F(PluginDllMainLoadTest, TheLoadersOwnLoadOfAStagedPluginIsStillNotCheckedTwice) {
  PlanTree("dllmain_load");  // its DllMain loads nothing: NEVR_TEST_DLLMAIN_LOAD is not set
  size_t checked = 99;
  ASSERT_TRUE(LoadPluginsWithTheRealHooks(&checked));
  EXPECT_EQ(checked, 0u) << "the loader's own load of the plugin it checked was checked again";
  EXPECT_FALSE(TestLogContains("refused"));
  EXPECT_FALSE(TestLogContains("LoadLibrary failed")) << "the plugin did not load";
}

// The path the filter resolved is opened a moment later; a name pointed elsewhere in between must not be verified
// as the file that was resolved (heldPath != full). The hook turns the resolved name into a link to a signed file.
std::wstring g_linkTarget;

void PointTheResolvedNameAtTheSignedFile(const std::wstring& resolved) {
  DeleteFileW(resolved.c_str());
  CreateSymbolicLinkW(resolved.c_str(), g_linkTarget.c_str(), 0);
}

TEST_F(PluginDependencyGateTest, LateLoadOfANameRepointedBetweenResolveAndOpenIsRefused) {
  Plan("late_swap.dll", "unsigned");  // the name the late load asks for
  Plan("late_swap_target.dll", "signed");  // what the name is pointed at in between
  g_testPluginLoadPlan.clear();
  g_linkTarget.assign(pluginsDir_.begin(), pluginsDir_.end());
  g_linkTarget += L"late_swap_target.dll";
  TestHook_InstallPluginLoadFilter(pluginsDir_);
  TestHook_SetBetweenResolveAndOpen(&PointTheResolvedNameAtTheSignedFile);
  const bool blocked = Blocked("late_swap.dll");
  TestHook_SetBetweenResolveAndOpen(nullptr);
  EXPECT_TRUE(blocked);
  EXPECT_TRUE(TestLogContains("the file changed between resolving its path and opening it"));
}

// #767: the staged plugin's path is opened to check it and named again to load it. The held handle stops a write to
// the file; it does not stop the name, when it goes through a link, from leading somewhere else by the time of the
// load. The load has to be of the file that was checked. The entry names a link to the signed plugin; between the
// check and the load the link is pointed at an unsigned file that is no plugin at all.
std::wstring g_stagedLink;
std::wstring g_stagedLinkTarget;

void PointTheStagedLinkAtTheUnsignedFile(const std::wstring&) {
  DeleteFileW(g_stagedLink.c_str());
  CreateSymbolicLinkW(g_stagedLink.c_str(), g_stagedLinkTarget.c_str(), 0);
}

TEST_F(PluginDependencyGateTest, ALinkRepointedBetweenTheCheckAndTheLoadDoesNotChangeTheFileThatLoads) {
  Plan("staged_real.dll", "signed");
  g_testPluginLoadPlan.clear();
  const std::string evil = pluginsDir_ + "staged_evil.dll";
  ASSERT_TRUE(CopyFileA((fixtures_ + "/" + manifest_["files"]["nevr-runtime.unsigned"].get<std::string>()).c_str(),
                        evil.c_str(), FALSE))
      << GetLastError();
  copies_.push_back(evil);
  const std::string link = pluginsDir_ + "staged_link.dll";
  g_stagedLink.assign(link.begin(), link.end());
  g_stagedLinkTarget.assign(pluginsDir_.begin(), pluginsDir_.end());
  g_stagedLinkTarget += L"staged_real.dll";
  ASSERT_TRUE(CreateSymbolicLinkW(g_stagedLink.c_str(), g_stagedLinkTarget.c_str(), 0)) << GetLastError();
  copies_.push_back(link);
  g_stagedLinkTarget.assign(evil.begin(), evil.end());  // where the hook points the link
  g_testPluginLoadPlan.push_back({"staged_link.dll", "staged_link.dll", false, "", "{}"});

  TestHook_SetBetweenCheckAndLoad(&PointTheStagedLinkAtTheUnsignedFile);
  LoadPlugins();
  TestHook_SetBetweenCheckAndLoad(nullptr);

  EXPECT_EQ(GetModuleHandleA("staged_evil.dll"), nullptr) << "the unsigned file was mapped";
  const nlohmann::json manifest = nlohmann::json::parse(BuildPluginManifestJson());
  ASSERT_EQ(manifest.size(), 1u);
  EXPECT_TRUE(manifest[0]["loaded"].get<bool>()) << "the plugin that was checked did not load: " << manifest.dump();
  EXPECT_EQ(GetLoadedPluginCount(), 1) << "the file behind the link at the load was not the file that was checked";
  EXPECT_TRUE(TestLogContains("plugin=staged_link.dll file=staged_link.dll is loaded from "))
      << "a path that leads somewhere else than its name is not said";
}

// #1177: a late load the filter accepted is made by the hook with the caller's own text. The filter held the file it
// checked; a link in that text, pointed somewhere else after the check, leads the real load to a file that was
// never checked. The load has to be of the file that was checked. late_link.dll names the signed late_real.dll;
// after the check the link is pointed at the unsigned late_evil.dll, which is no plugin (no NvrPluginGetInfo).
std::wstring g_lateLink;
std::wstring g_lateLinkTarget;

void PointTheLateLinkAtTheUnsignedFile(const std::wstring&) {
  DeleteFileW(g_lateLink.c_str());
  CreateSymbolicLinkW(g_lateLink.c_str(), g_lateLinkTarget.c_str(), 0);
}

class PluginLateLoadTest : public PluginDependencyGateTest {
 protected:
  void TearDown() override {
    TestHook_SetAfterLateLoadAccepted(nullptr);
    TestHook_FailNamingTheHeldFile(false);
    for (const char* name : {"late_link.dll", "late_real.dll", "late_evil.dll"}) {
      while (HMODULE m = GetModuleHandleA(name)) {
        if (!FreeLibrary(m)) break;
      }
    }
    PluginDependencyGateTest::TearDown();
  }

  // late_link.dll -> late_real.dll (signed plugin); late_evil.dll is the file the seam points the link at.
  void ArrangeTheLink() {
    Plan("late_real.dll", "signed");
    g_testPluginLoadPlan.clear();
    evil_ = pluginsDir_ + "late_evil.dll";
    ASSERT_TRUE(CopyFileA((fixtures_ + "/" + manifest_["files"]["nevr-runtime.unsigned"].get<std::string>()).c_str(),
                          evil_.c_str(), FALSE))
        << GetLastError();
    copies_.push_back(evil_);
    link_ = pluginsDir_ + "late_link.dll";
    g_lateLink.assign(link_.begin(), link_.end());
    g_lateLinkTarget.assign(pluginsDir_.begin(), pluginsDir_.end());
    g_lateLinkTarget += L"late_real.dll";
    ASSERT_TRUE(CreateSymbolicLinkW(g_lateLink.c_str(), g_lateLinkTarget.c_str(), 0)) << GetLastError();
    copies_.push_back(link_);
    g_lateLinkTarget.assign(evil_.begin(), evil_.end());  // where the seam points the link
  }

  // Runs `body` with the four LoadLibrary hooks and the plugin filter live, then takes them down.
  template <typename Body>
  void WithTheRealHooks(Body body) {
    ASSERT_EQ(MH_Initialize(), MH_OK);
    const int hooked = nevr_dll_load_hook::Install();
    if (hooked == 4) {
      TestHook_InstallPluginLoadFilter(pluginsDir_);
      body();
    }
    nevr_dll_load_hook::Shutdown();  // also clears the registered filters
    MH_Uninitialize();
    nevr_hook_guard::ResetForTest();
    TestHook_ForgetLoadFilterRegistration();
    ASSERT_EQ(hooked, 4) << "the LoadLibrary hooks did not install";
  }

  std::string evil_;
  std::string link_;
};

TEST_F(PluginLateLoadTest, ALinkRepointedAfterTheLateCheckDoesNotChangeTheFileThatLoads) {
  ArrangeTheLink();
  WithTheRealHooks([&] {
    TestHook_SetAfterLateLoadAccepted(&PointTheLateLinkAtTheUnsignedFile);
    const HMODULE m = LoadLibraryW(g_lateLink.c_str());
    TestHook_SetAfterLateLoadAccepted(nullptr);
    ASSERT_NE(m, nullptr) << GetLastError();
    EXPECT_NE(GetProcAddress(m, "NvrPluginGetInfo"), nullptr)
        << "the file behind the link at the load was not the file that was checked";
    EXPECT_TRUE(TestLogContains("loads the file it checked")) << "a load that leads somewhere else than its name is not said";
  });
}

TEST_F(PluginLateLoadTest, AFileAlreadyCheckedIsLoadedAsCheckedWhenItsLinkIsRepointedLater) {
  ArrangeTheLink();
  WithTheRealHooks([&] {
    const HMODULE first = LoadLibraryW(g_lateLink.c_str());  // verifies and remembers the file
    ASSERT_NE(first, nullptr) << GetLastError();
    ASSERT_TRUE(FreeLibrary(first));
    TestHook_SetAfterLateLoadAccepted(&PointTheLateLinkAtTheUnsignedFile);
    const HMODULE second = LoadLibraryW(g_lateLink.c_str());  // the remembered file is accepted again
    TestHook_SetAfterLateLoadAccepted(nullptr);
    ASSERT_NE(second, nullptr) << GetLastError();
    EXPECT_NE(GetProcAddress(second, "NvrPluginGetInfo"), nullptr)
        << "a remembered file's second load went to the file behind the link";
  });
}

TEST_F(PluginLateLoadTest, TheAnsiLoadsAreLoadedAsCheckedToo) {
  ArrangeTheLink();
  WithTheRealHooks([&] {
    TestHook_SetAfterLateLoadAccepted(&PointTheLateLinkAtTheUnsignedFile);
    const HMODULE m = LoadLibraryExA(link_.c_str(), nullptr, 0);
    TestHook_SetAfterLateLoadAccepted(nullptr);
    ASSERT_NE(m, nullptr) << GetLastError();
    EXPECT_NE(GetProcAddress(m, "NvrPluginGetInfo"), nullptr) << "LoadLibraryExA loaded the file behind the link";
    ASSERT_TRUE(FreeLibrary(m));
    DeleteFileW(g_lateLink.c_str());  // the link is back at the signed file for the next variant
    g_lateLinkTarget.assign(pluginsDir_.begin(), pluginsDir_.end());
    g_lateLinkTarget += L"late_real.dll";
    ASSERT_TRUE(CreateSymbolicLinkW(g_lateLink.c_str(), g_lateLinkTarget.c_str(), 0)) << GetLastError();
    g_lateLinkTarget.assign(evil_.begin(), evil_.end());
    TestHook_SetAfterLateLoadAccepted(&PointTheLateLinkAtTheUnsignedFile);
    const HMODULE a = LoadLibraryA(link_.c_str());
    TestHook_SetAfterLateLoadAccepted(nullptr);
    ASSERT_NE(a, nullptr) << GetLastError();
    EXPECT_NE(GetProcAddress(a, "NvrPluginGetInfo"), nullptr) << "LoadLibraryA loaded the file behind the link";
  });
}

TEST_F(PluginLateLoadTest, ASignedLateLoadWithNoLinkLoadsAsBefore) {
  ArrangeTheLink();
  WithTheRealHooks([&] {
    const std::string real = pluginsDir_ + "late_real.dll";
    const HMODULE m = LoadLibraryA(real.c_str());
    ASSERT_NE(m, nullptr) << GetLastError();
    EXPECT_NE(GetProcAddress(m, "NvrPluginGetInfo"), nullptr);
    EXPECT_FALSE(TestLogContains("loads the file it checked")) << "a load whose name is its file was said to be rewritten";
  });
}

TEST_F(PluginLateLoadTest, AnUnsignedAllowedRunLoadsWhatTheCallerNamed) {
  nevr_plugin_trust::TestHook_SetTrust(nevr_plugin_trust::Mode::UnsignedAllowed, &policy_);
  ArrangeTheLink();
  WithTheRealHooks([&] {
    TestHook_SetAfterLateLoadAccepted(&PointTheLateLinkAtTheUnsignedFile);
    const HMODULE m = LoadLibraryW(g_lateLink.c_str());
    TestHook_SetAfterLateLoadAccepted(nullptr);
    ASSERT_NE(m, nullptr) << GetLastError();
    EXPECT_EQ(GetProcAddress(m, "NvrPluginGetInfo"), nullptr) << "a run that allows unsigned plugins rewrote a load";
  });
}

TEST_F(PluginLateLoadTest, AFileTheFilterCannotNameIsRefusedInASignedOnlyRun) {
  Plan("late_real.dll", "signed");
  g_testPluginLoadPlan.clear();
  TestHook_InstallPluginLoadFilter(pluginsDir_);
  TestHook_FailNamingTheHeldFile(true);
  EXPECT_TRUE(Blocked("late_real.dll"));
  EXPECT_TRUE(TestLogContains("cannot name the file it checked"));
}

TEST_F(PluginLateLoadTest, AFileTheFilterCannotNameIsLoadedWhenSignaturesAreNotRequired) {
  nevr_plugin_trust::TestHook_SetTrust(nevr_plugin_trust::Mode::UnsignedAllowed, &policy_);
  Plan("late_real.dll", "signed");
  g_testPluginLoadPlan.clear();
  TestHook_InstallPluginLoadFilter(pluginsDir_);
  TestHook_FailNamingTheHeldFile(true);
  EXPECT_FALSE(Blocked("late_real.dll"));
}

// A bare name is left to the OS search: the plugins directory may not be on it, so naming the plugins file would load
// a file the OS would not have chosen.
TEST_F(PluginLateLoadTest, ABareNameIsNotRewritten) {
  ArrangeTheLink();
  TestHook_InstallPluginLoadFilter(pluginsDir_);
  ASSERT_FALSE(nevr_dll_load_hook::IsLoadBlocked(L"late_link.dll", nullptr));
  std::wstring path;
  EXPECT_FALSE(nevr_dll_load_hook::RewriteLoad(L"late_link.dll", &path)) << "a bare name was given a path";
}

// What the filter noted is for the one load it just accepted: the text it was asked about, once.
TEST_F(PluginLateLoadTest, TheNotedFileAnswersOnlyTheTextTheFilterAcceptedAndOnlyOnce) {
  ArrangeTheLink();
  TestHook_InstallPluginLoadFilter(pluginsDir_);
  std::wstring link(link_.begin(), link_.end());
  for (wchar_t& c : link) c = (c >= L'A' && c <= L'Z') ? static_cast<wchar_t>(c + 32) : c;
  std::wstring path;

  ASSERT_FALSE(nevr_dll_load_hook::IsLoadBlocked(link.c_str(), nullptr));
  EXPECT_FALSE(nevr_dll_load_hook::RewriteLoad(L"c:\\other\\late_link.dll", &path)) << "another text was answered";
  EXPECT_TRUE(nevr_dll_load_hook::RewriteLoad(link.c_str(), &path));
  EXPECT_FALSE(path.empty());
  EXPECT_FALSE(nevr_dll_load_hook::RewriteLoad(link.c_str(), &path)) << "the noted file was used twice";

  // A later filter call that notes nothing clears what an earlier one noted.
  ASSERT_FALSE(nevr_dll_load_hook::IsLoadBlocked(link.c_str(), nullptr));
  ASSERT_FALSE(nevr_dll_load_hook::IsLoadBlocked(L"c:\\windows\\system32\\kernel32.dll", nullptr));
  EXPECT_FALSE(nevr_dll_load_hook::RewriteLoad(link.c_str(), &path)) << "a noted file outlived the next filter call";
}

TEST_F(PluginDependencyGateTest, APluginReachedByItsOwnNameIsNotSaidToBeLoadedFromElsewhere) {
  Plan("staged_plain.dll", "signed");
  LoadPlugins();
  EXPECT_EQ(GetLoadedPluginCount(), 1);
  EXPECT_FALSE(TestLogContains("is loaded from "));
}

// The OS can fail to name the file behind a handle. A signed-only run then has no path it can tie to the check.
TEST_F(PluginDependencyGateTest, AHeldFileThatCannotBeNamedIsRefusedInASignedOnlyRun) {
  Plan("unnamed.dll", "signed");
  TestHook_FailNamingTheHeldFile(true);
  LoadPlugins();
  TestHook_FailNamingTheHeldFile(false);
  EXPECT_EQ(GetLoadedPluginCount(), 0);
  EXPECT_EQ(GetModuleHandleA("unnamed.dll"), nullptr) << "a plugin whose checked file has no name was mapped";
  EXPECT_TRUE(TestLogContains("plugin=unnamed.dll file=unnamed.dll refused: cannot name the file it checked: error 5"));
  const nlohmann::json manifest = nlohmann::json::parse(BuildPluginManifestJson());
  ASSERT_EQ(manifest.size(), 1u);
  EXPECT_EQ(manifest[0]["error"].get<std::string>(), "refused: cannot name the file it checked: error 5");
}

TEST_F(PluginDependencyGateTest, AHeldFileThatCannotBeNamedLoadsByItsConfiguredPathWhenSignaturesAreNotRequired) {
  nevr_plugin_trust::TestHook_SetTrust(nevr_plugin_trust::Mode::UnsignedAllowed, &policy_);
  Plan("unnamed_dev.dll", "unsigned");
  TestHook_FailNamingTheHeldFile(true);
  LoadPlugins();
  TestHook_FailNamingTheHeldFile(false);
  EXPECT_EQ(GetLoadedPluginCount(), 1);
  EXPECT_TRUE(TestLogContainsAtLevel(EchoVR::LogLevel::Warning,
                                     "cannot name the file it checked: error 5; loading the configured path, which is "
                                     "not tied to the file checked (plugin trust mode unsigned-allowed)"));
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

// The same unsigned file in plugins/, named four ways that all resolve to it but do not spell its directory the way
// the loader does. A filter that compares the text of the path lets each of them load unchecked.
class PluginLateLoadAliasTest : public PluginDependencyGateTest {
 protected:
  void SetUp() override {
    PluginDependencyGateTest::SetUp();
    Plan("alias_target_unsigned.dll", "unsigned");  // copied into plugins/, never planned to load
    g_testPluginLoadPlan.clear();
    plain_ = pluginsDir_ + "alias_target_unsigned.dll";
    TestHook_InstallPluginLoadFilter(pluginsDir_);
  }

  static std::wstring Lower(const std::string& narrow) {
    std::wstring wide;
    for (char c : narrow) wide.push_back(static_cast<wchar_t>(std::tolower(static_cast<unsigned char>(c))));
    return wide;
  }

  void ExpectRefused(const std::wstring& spelled) {
    EXPECT_TRUE(nevr_dll_load_hook::IsLoadBlocked(spelled.c_str(), nullptr)) << "not refused: " << std::string(spelled.begin(), spelled.end());
    EXPECT_TRUE(TestLogContains("refused a later load of alias_target_unsigned.dll: unsigned"));
  }

  std::string plain_;
};

TEST_F(PluginLateLoadAliasTest, TheOrdinarySpellingIsRefused) { ExpectRefused(Lower(plain_)); }

TEST_F(PluginLateLoadAliasTest, AnEightDotThreeShortNameIsRefused) {
  char shortPath[MAX_PATH] = {};
  const DWORD n = GetShortPathNameA(plain_.c_str(), shortPath, MAX_PATH);
  ASSERT_GT(n, 0u) << "GetShortPathName failed: " << GetLastError();
  ASSERT_NE(std::string(shortPath), plain_) << "no 8.3 name was made for the file: this form is not under test";
  ExpectRefused(Lower(shortPath));
}

TEST_F(PluginLateLoadAliasTest, AQuestionMarkPrefixIsRefused) { ExpectRefused(Lower("\\\\?\\" + plain_)); }
TEST_F(PluginLateLoadAliasTest, ATrailingDotIsRefused) { ExpectRefused(Lower(plain_ + ".")); }
TEST_F(PluginLateLoadAliasTest, ATrailingSpaceIsRefused) { ExpectRefused(Lower(plain_ + " ")); }

TEST_F(PluginLateLoadAliasTest, ADotDotDetourThroughThePluginsDirectoryIsRefused) {
  ExpectRefused(Lower(pluginsDir_ + "..\\plugins\\alias_target_unsigned.dll"));
}

TEST_F(PluginLateLoadAliasTest, AFileThatIsNotInThePluginsDirectoryIsStillNotThisFiltersBusiness) {
  EXPECT_FALSE(nevr_dll_load_hook::IsLoadBlocked(Lower("\\\\?\\" + fixtures_ + "\\fixtures.json").c_str(), nullptr));
}

TEST_F(PluginDependencyGateTest, LateLoadOfASignedFileWithAnUnsignedDependencyIsRefused) {
  Copy("import_unsigned");
  TestHook_InstallPluginLoadFilter(pluginsDir_);
  EXPECT_TRUE(Blocked("importer.dll"));
  EXPECT_TRUE(TestLogContains("refused a later load of importer.dll: dependency dep.dll (import): unsigned"));
}

TEST_F(PluginDependencyGateTest, UnsignedAllowedRunAllowsALateLoadWithAWarning) {
  nevr_plugin_trust::TestHook_SetTrust(nevr_plugin_trust::Mode::UnsignedAllowed, &policy_);
  Plan("late_dev.dll", "unsigned");
  g_testPluginLoadPlan.clear();
  TestHook_InstallPluginLoadFilter(pluginsDir_);
  EXPECT_FALSE(Blocked("late_dev.dll"));
  EXPECT_TRUE(TestLogContainsAtLevel(EchoVR::LogLevel::Warning, "a plugin loads late_dev.dll after its init"));
  EXPECT_TRUE(TestLogContains("allowed because plugin signatures are not required in this run (plugin trust mode "
                              "unsigned-allowed)"));
}

// ---- the run's mode: unsigned plugins load by default, -require-signed-plugins refuses them (#924) ----------------
//
// These tests do not install a mode with TestHook_SetTrust: the mode is the one a run gets, from the command line
// through nevr_plugin_trust::ApplyCommandLine, the function PreflightRuntimeBootstrap calls before the first
// LoadPlugins. The policy is the production policy; the plugin is the unsigned test plugin the build puts in
// plugins/. This test executable is built without NEVR_ALLOW_UNSIGNED_PLUGINS, which the first test asserts.

namespace {
int TestLogCount(const std::string& needle) {
  std::lock_guard<std::mutex> lock(g_testLogMutex);
  return static_cast<int>(std::count_if(g_testLogMessages.begin(), g_testLogMessages.end(),
                                        [&needle](const std::string& m) { return m.find(needle) != std::string::npos; }));
}

std::vector<std::string> g_runModeDialogs;
void CaptureRunModeDialog(const std::string& text) { g_runModeDialogs.push_back(text); }

PluginLoadItem RunModeEarlyItem(const char* name, const char* file) {
  PluginLoadItem item{name, file, false, "", "{}"};
  item.early = true;
  return item;
}

// ApplyCommandLine over a command line given as text: `wide` is GetCommandLineW()'s string and `ansi` is
// GetCommandLineA()'s, the line the game tokenizes. Given separately so a byte the wide line cannot spell the same
// way in every code page (0x96) is tested as the game would see it.
void ApplyLine(const std::wstring& wide, const std::string& ansi) {
  nevr_plugin_trust::ApplyCommandLine(wide.c_str(), ansi.c_str());
}

// The same text for both readings; the text must be ASCII.
void ApplyLine(const std::wstring& wide) {
  std::string ansi;
  for (wchar_t c : wide) ansi.push_back(static_cast<char>(c));
  ApplyLine(wide, ansi);
}

// ApplyLine over arguments joined with a space (none of them holds a space or a quote).
void ApplyArgs(std::initializer_list<const wchar_t*> args) {
  std::wstring line;
  for (const wchar_t* arg : args) {
    if (!line.empty()) line += L' ';
    line += arg;
  }
  ApplyLine(line);
}

// The read-fault seam of nevr_plugin_trust::ApplyCommandLine: at one stage the hook throws a std::exception or reports
// that the call there failed.
enum class InjectedFault { None, ThrowBadAlloc, ThrowLengthError, CallFails };
nevr_plugin_trust::ReadStage g_faultStage = nevr_plugin_trust::ReadStage::GameTokens;
InjectedFault g_fault = InjectedFault::None;

bool InjectAtStage(nevr_plugin_trust::ReadStage stage) {
  if (stage != g_faultStage) return false;
  switch (g_fault) {
    case InjectedFault::ThrowBadAlloc:
      throw std::bad_alloc();
    case InjectedFault::ThrowLengthError:
      throw std::length_error("injected");
    case InjectedFault::CallFails:
      return true;
    case InjectedFault::None:
      break;
  }
  return false;
}

void ArmReadFault(nevr_plugin_trust::ReadStage stage, InjectedFault fault) {
  g_faultStage = stage;
  g_fault = fault;
  nevr_plugin_trust::TestHook_SetReadFault(&InjectAtStage);
}
}  // namespace

class PluginRunModeTest : public ::testing::Test {
 protected:
  void SetUp() override {
    UnloadPlugins();
    g_testPluginLoadPlan.clear();
    ClearTestLogs();
    EchoVR::g_GameBaseAddress = reinterpret_cast<CHAR*>(GetModuleHandleA(nullptr));
    ASSERT_NE(EchoVR::g_GameBaseAddress, nullptr);
    nevr_plugin_trust::TestHook_ResetTrust();  // the mode a run gets, not one a test installs
    wasServer_ = g_isServer;
    wasHeadless_ = g_isHeadless;
    g_isServer = FALSE;  // a client with graphics: the one run that shows the dialog
    g_isHeadless = FALSE;
    g_runModeDialogs.clear();
    TestHook_ResetRefusals();
    TestHook_SetRefusalDialogSink(&CaptureRunModeDialog);
    ResetServerFatal();
    nevrRoot_ = std::make_unique<TestNevrRoot>();
    pluginsDir_ = nevrRoot_->PluginsDir();
  }

  void TearDown() override {
    UnloadPlugins();
    for (const std::string& file : copies_) DeleteFileA(file.c_str());
    g_testPluginLoadPlan.clear();
    ClearTestLogs();
    TestHook_ResetPluginLoadFilter();
    TestHook_SetRefusalDialogSink(nullptr);
    TestHook_ResetRefusals();
    nevr_plugin_trust::TestHook_ResetTrust();
    g_isServer = wasServer_;
    g_isHeadless = wasHeadless_;
    nevrRoot_.reset();
  }

  // The unsigned test plugin, as `name` in plugins/ (a copy, so a late load has a file of its own to ask about).
  void CopyUnsignedPlugin(const std::string& name) {
    const std::string target = pluginsDir_ + name;
    ASSERT_TRUE(CopyFileA((pluginsDir_ + "test_plugin_onframe.dll").c_str(), target.c_str(), FALSE))
        << target << " error " << GetLastError();
    copies_.push_back(target);
  }

  bool Blocked(const std::string& file) {
    std::wstring wide;
    for (char c : pluginsDir_ + file) wide.push_back(static_cast<wchar_t>(std::tolower(static_cast<unsigned char>(c))));
    return nevr_dll_load_hook::IsLoadBlocked(wide.c_str(), nullptr);
  }

  std::unique_ptr<TestNevrRoot> nevrRoot_;
  std::string pluginsDir_;
  std::vector<std::string> copies_;
  BOOL wasServer_ = FALSE;
  BOOL wasHeadless_ = FALSE;
};

TEST_F(PluginRunModeTest, ThisIsAReleaseConfiguredBuild) {
  EXPECT_FALSE(nevr_plugin_trust::IsDevBuild());
  EXPECT_STREQ(nevr_plugin_trust::BuildWord(), "release");
  EXPECT_STREQ(nevr_plugin_trust::ModeLiteral(), "NEVR-PLUGIN-TRUST release");
}

TEST_F(PluginRunModeTest, AnUnsignedPluginLoadsWhenTheCommandLineHasNoArgument) {
  ApplyArgs({L"echovr.exe", L"-windowed"});
  g_testPluginLoadPlan.push_back({"onframe", "test_plugin_onframe.dll", false, "", "{}"});

  LoadPlugins();

  EXPECT_EQ(GetLoadedPluginCount(), 1);
  EXPECT_NE(GetModuleHandleA("test_plugin_onframe.dll"), nullptr);
  const nlohmann::json manifest = nlohmann::json::parse(BuildPluginManifestJson());
  ASSERT_EQ(manifest.size(), 1u);
  EXPECT_TRUE(manifest[0]["loaded"].get<bool>());
  EXPECT_TRUE(TestLogContainsAtLevel(EchoVR::LogLevel::Info,
                                     "[NEVR.PLUGIN] plugin trust: mode=unsigned-allowed source=default build=release "
                                     "literal=\"NEVR-PLUGIN-TRUST release\""));
  EXPECT_TRUE(TestLogContainsAtLevel(EchoVR::LogLevel::Warning,
                                     "plugin=onframe file=test_plugin_onframe.dll UNSIGNED, loaded because plugin "
                                     "signatures are not required in this run (plugin trust mode unsigned-allowed; "
                                     "start with -require-signed-plugins to refuse it)"));
  EXPECT_FALSE(TestLogContains("refused"));
  EXPECT_TRUE(g_runModeDialogs.empty());
}

TEST_F(PluginRunModeTest, TheSamePluginIsRefusedWithRequireSignedPlugins) {
  ApplyArgs({L"echovr.exe", L"-windowed", L"-require-signed-plugins"});
  g_testPluginLoadPlan.push_back({"onframe", "test_plugin_onframe.dll", false, "", "{}"});

  LoadPlugins();

  EXPECT_EQ(GetLoadedPluginCount(), 0);
  EXPECT_EQ(GetModuleHandleA("test_plugin_onframe.dll"), nullptr) << "a refused file was mapped";
  EXPECT_TRUE(TestLogContainsAtLevel(EchoVR::LogLevel::Info,
                                     "[NEVR.PLUGIN] plugin trust: mode=signed-only source=-require-signed-plugins "
                                     "build=release literal=\"NEVR-PLUGIN-TRUST release\""));
  EXPECT_TRUE(TestLogContains("plugin=onframe file=test_plugin_onframe.dll refused: unsigned"));
  const nlohmann::json manifest = nlohmann::json::parse(BuildPluginManifestJson());
  ASSERT_EQ(manifest.size(), 1u);
  EXPECT_FALSE(manifest[0]["loaded"].get<bool>());
  EXPECT_EQ(manifest[0]["error"].get<std::string>(), "refused: unsigned");
  ASSERT_EQ(g_runModeDialogs.size(), 1u) << "a client with graphics gets the refusal dialog, as before";
  EXPECT_NE(g_runModeDialogs[0].find("test_plugin_onframe.dll wasn't loaded because it isn't signed by nEVR."),
            std::string::npos)
      << g_runModeDialogs[0];
}

TEST_F(PluginRunModeTest, ARefusedRequiredPluginIsFatalOnAServerAndShowsNoDialog) {
  ApplyArgs({L"echovr.exe", L"-server", L"-require-signed-plugins"});
  g_isServer = TRUE;
  g_testPluginLoadPlan.push_back({"onframe", "test_plugin_onframe.dll", true, "", "{}"});

  LoadPlugins();

  EXPECT_EQ(GetLoadedPluginCount(), 0);
  EXPECT_EQ(ServerFatalCalls(), 1) << LastServerFatal();
  EXPECT_NE(LastServerFatal().find("refused: unsigned"), std::string::npos) << LastServerFatal();
  EXPECT_TRUE(g_runModeDialogs.empty());
}

TEST_F(PluginRunModeTest, ARequiredPluginThatIsUnsignedIsNotFatalWhenSignaturesAreNotRequired) {
  ApplyArgs({L"echovr.exe", L"-server"});
  g_isServer = TRUE;
  g_testPluginLoadPlan.push_back({"onframe", "test_plugin_onframe.dll", true, "", "{}"});

  LoadPlugins();

  EXPECT_EQ(GetLoadedPluginCount(), 1);
  EXPECT_EQ(ServerFatalCalls(), 0) << LastServerFatal();
}

TEST_F(PluginRunModeTest, TheEarlyPassAndTheNormalPassReadTheSameMode) {
  ApplyArgs({L"echovr.exe", L"-require-signed-plugins"});
  g_testPluginLoadPlan.push_back(RunModeEarlyItem("early-one", "test_plugin_onframe.dll"));

  LoadPlugins(PluginPhase::Early);
  EXPECT_EQ(GetLoadedPluginCount(), 0) << "the early pass loaded an unsigned plugin in a signed-only run";
  EXPECT_TRUE(TestLogContains("plugin=early-one file=test_plugin_onframe.dll refused: unsigned"));

  LoadPlugins();
  EXPECT_EQ(GetLoadedPluginCount(), 0);
  EXPECT_EQ(TestLogCount("plugin trust: mode=signed-only"), 1) << "the mode is stated once per run, not once per pass";
}

TEST_F(PluginRunModeTest, TheEarlyPassLoadsAnUnsignedPluginWhenSignaturesAreNotRequired) {
  ApplyArgs({L"echovr.exe"});
  g_testPluginLoadPlan.push_back(RunModeEarlyItem("early-one", "test_plugin_onframe.dll"));

  LoadPlugins(PluginPhase::Early);
  EXPECT_EQ(GetLoadedPluginCount(), 1);
  EXPECT_TRUE(TestLogContains("plugin trust: mode=unsigned-allowed source=default"));
}

TEST_F(PluginRunModeTest, TheModeIsLoggedEvenWhenNoPluginIsConfigured) {
  ApplyArgs({L"echovr.exe", L"-require-signed-plugins"});

  LoadPlugins();

  EXPECT_TRUE(TestLogContainsAtLevel(EchoVR::LogLevel::Info, "[NEVR.PLUGIN] plugin trust: mode=signed-only"));
  EXPECT_TRUE(TestLogContains("no plugins configured in config.yaml"));
}

TEST_F(PluginRunModeTest, ALaterLoadFollowsTheRunMode) {
  CopyUnsignedPlugin("late_runmode.dll");
  ApplyArgs({L"echovr.exe"});
  TestHook_InstallPluginLoadFilter(pluginsDir_);
  EXPECT_FALSE(Blocked("late_runmode.dll"));
  EXPECT_TRUE(TestLogContains("a plugin loads late_runmode.dll after its init"));
  EXPECT_FALSE(TestLogContains("refused a later load"));

  ClearTestLogs();
  TestHook_ResetPluginLoadFilter();
  ApplyArgs({L"echovr.exe", L"-require-signed-plugins"});
  TestHook_InstallPluginLoadFilter(pluginsDir_);
  EXPECT_TRUE(Blocked("late_runmode.dll"));
  EXPECT_TRUE(TestLogContains("refused a later load of late_runmode.dll: unsigned"));
}

TEST_F(PluginRunModeTest, WithNoArgumentTheRunIsUnsignedAllowed) {
  using nevr_plugin_trust::ModeSource;
  ApplyArgs({L"echovr.exe", L"-server", L"-windowed"});
  EXPECT_EQ(nevr_plugin_trust::ActiveMode(), nevr_plugin_trust::Mode::UnsignedAllowed);
  EXPECT_EQ(nevr_plugin_trust::ActiveModeSource(), ModeSource::Default);

  ApplyArgs({L"echovr.exe", L"-require-signed-plugins"});
  EXPECT_EQ(nevr_plugin_trust::ActiveMode(), nevr_plugin_trust::Mode::SignedOnly);
  EXPECT_EQ(nevr_plugin_trust::ActiveModeSource(), ModeSource::CommandLine);

  ApplyArgs({L"echovr.exe"});  // a later reading replaces the earlier one
  EXPECT_EQ(nevr_plugin_trust::ActiveMode(), nevr_plugin_trust::Mode::UnsignedAllowed);
  EXPECT_EQ(nevr_plugin_trust::ActiveModeSource(), ModeSource::Default);
}

// The game looks an argument up with every leading '-', '+' and 0x96 skipped and A-Z lower-cased (CSymbol64::Lookup,
// cmdline_strip.h), so each of these spellings is the flag to the game and must be signed-only here.
TEST_F(PluginRunModeTest, EverySpellingTheGameReadsAsTheFlagMakesTheRunSignedOnly) {
  for (const wchar_t* spelling :
       {L"-require-signed-plugins", L"--require-signed-plugins", L"---require-signed-plugins", L"----require-signed-plugins",
        L"+require-signed-plugins", L"++require-signed-plugins", L"-+-require-signed-plugins", L"+-require-signed-plugins",
        L"-REQUIRE-SIGNED-PLUGINS", L"-Require-Signed-Plugins", L"--Require-signed-PLUGINS", L"+REQUIRE-SIGNED-PLUGINS",
        L"---REQUIRE-SIGNED-PLUGINS"}) {
    nevr_plugin_trust::TestHook_ResetTrust();
    ApplyArgs({L"echovr.exe", spelling});
    EXPECT_EQ(nevr_plugin_trust::ActiveMode(), nevr_plugin_trust::Mode::SignedOnly) << "spelling " << spelling;
    EXPECT_EQ(nevr_plugin_trust::ActiveModeSource(), nevr_plugin_trust::ModeSource::CommandLine) << spelling;
  }
}

TEST_F(PluginRunModeTest, ThePrefixByte0x96IsADashToTheGame) {
  // 0x96 is the ANSI en dash. The game's tokenizer treats it as a dash, so the byte spelling is the flag to the game
  // whatever the wide line holds; the wide line here is the plain flag spelled without it, as a different reading.
  ApplyLine(L"echovr.exe require-signed-plugins", "echovr.exe \x96require-signed-plugins");
  EXPECT_EQ(nevr_plugin_trust::ActiveMode(), nevr_plugin_trust::Mode::SignedOnly);
  ApplyLine(L"echovr.exe require-signed-plugins", "echovr.exe \x96\x96REQUIRE-signed-plugins");
  EXPECT_EQ(nevr_plugin_trust::ActiveMode(), nevr_plugin_trust::Mode::SignedOnly);
  // The same bytes without the prefix are no option to the game.
  ApplyLine(L"echovr.exe require-signed-plugins", "echovr.exe require-signed-plugins");
  EXPECT_EQ(nevr_plugin_trust::ActiveMode(), nevr_plugin_trust::Mode::UnsignedAllowed);
}

// The game splits its ANSI line on ' ' and a '"' opens a token that runs to the next '"' with no escape (Tokenize);
// CommandLineToArgvW reads \" as a literal quote. After `-config "C:\dir\"` the game has closed the quote and
// reads the flag as an option; CommandLineToArgvW is still inside the quote and sees one argument.
TEST_F(PluginRunModeTest, ABackslashBeforeAQuoteDoesNotHideTheFlagFromTheGameReading) {
  const std::wstring line = L"echovr.exe -config \"C:\\dir\\\" -require-signed-plugins";
  int argc = 0;
  wchar_t** const argv = CommandLineToArgvW(line.c_str(), &argc);
  ASSERT_NE(argv, nullptr);
  bool argvHasTheFlag = false;
  for (int i = 0; i < argc; ++i) argvHasTheFlag = argvHasTheFlag || std::wstring(argv[i]) == L"-require-signed-plugins";
  LocalFree(argv);
  EXPECT_FALSE(argvHasTheFlag) << "the premise: CommandLineToArgvW does not see the flag on this line";

  ApplyLine(line);
  EXPECT_EQ(nevr_plugin_trust::ActiveMode(), nevr_plugin_trust::Mode::SignedOnly);
  EXPECT_EQ(nevr_plugin_trust::ActiveModeSource(), nevr_plugin_trust::ModeSource::CommandLine);
}

// The other direction: CommandLineToArgvW removes the quotes, so a quoted flag is an argument for it while the game
// reads the quoted token as a parameter, not an option. Either reading finding the flag is enough.
TEST_F(PluginRunModeTest, AFlagEitherReadingFindsMakesTheRunSignedOnly) {
  ApplyArgs({L"echovr.exe", L"\"-require-signed-plugins\""});  // CommandLineToArgvW: the flag; the game: a parameter
  EXPECT_EQ(nevr_plugin_trust::ActiveMode(), nevr_plugin_trust::Mode::SignedOnly);
  ApplyLine(L"echovr.exe -server \"-REQUIRE-SIGNED-PLUGINS\"");
  EXPECT_EQ(nevr_plugin_trust::ActiveMode(), nevr_plugin_trust::Mode::SignedOnly);
}

// Text the game does not read as the flag leaves the run unsigned-allowed. A token inside a quoted value is one token
// of the game's tokenizer (Tokenize: a '"' opens a token that runs to the closing '"'), and one argument of
// CommandLineToArgvW, in neither of which is it an option named -require-signed-plugins; a '[' token is the game's
// other quoting form (cmdline_strip.h) and a parameter. A name that differs by one letter is another option.
TEST_F(PluginRunModeTest, TextTheGameDoesNotReadAsTheFlagLeavesTheRunUnsignedAllowed) {
  for (const wchar_t* line :
       {L"echovr.exe", L"echovr.exe -windowed", L"echovr.exe -require-signed-plugin", L"echovr.exe -require-signed-pluginsx",
        L"echovr.exe -require-signed", L"echovr.exe require-signed-plugins", L"echovr.exe -requiresignedplugins",
        L"echovr.exe -require_signed_plugins", L"echovr.exe -config \"x -require-signed-plugins\"",
        L"echovr.exe -config \"-x --require-signed-plugins y\" -windowed", L"echovr.exe [-require-signed-plugins]",
        L"echovr.exe -config C:\\dir\\require-signed-plugins", L"echovr.exe -server=-require-signed-plugins"}) {
    nevr_plugin_trust::TestHook_ResetTrust();
    ApplyLine(line);
    EXPECT_EQ(nevr_plugin_trust::ActiveMode(), nevr_plugin_trust::Mode::UnsignedAllowed) << "line " << line;
    EXPECT_EQ(nevr_plugin_trust::ActiveModeSource(), nevr_plugin_trust::ModeSource::Default) << line;
  }
}

// A std::bad_alloc (or any std::exception) while the line is read must not leave the run unsigned-allowed: the flag is
// then unknown. The line has no flag, so the run is unsigned-allowed unless the fault is what decides.
TEST_F(PluginRunModeTest, AnExceptionWhileReadingTheLineMakesTheRunSignedOnly) {
  using nevr_plugin_trust::ReadStage;
  ApplyArgs({L"echovr.exe", L"-windowed"});
  ASSERT_EQ(nevr_plugin_trust::ActiveMode(), nevr_plugin_trust::Mode::UnsignedAllowed) << "the premise: no fault, no flag";

  for (ReadStage stage : {ReadStage::GameTokens, ReadStage::ArgvParsed, ReadStage::ArgvText}) {
    for (InjectedFault fault : {InjectedFault::ThrowBadAlloc, InjectedFault::ThrowLengthError}) {
      nevr_plugin_trust::TestHook_ResetTrust();
      ArmReadFault(stage, fault);
      ApplyArgs({L"echovr.exe", L"-windowed"});
      EXPECT_EQ(nevr_plugin_trust::ActiveMode(), nevr_plugin_trust::Mode::SignedOnly)
          << "stage " << static_cast<int>(stage) << " fault " << static_cast<int>(fault);
      EXPECT_EQ(nevr_plugin_trust::ActiveModeSource(), nevr_plugin_trust::ModeSource::CommandLineReadFailed);
      EXPECT_EQ(nevr_plugin_trust::TestHook_ArgvBuffersLive(), 0)
          << "the CommandLineToArgvW buffer is freed on the path that threw";
    }
  }
}

// A call that fails (not an exception) is an unreadable line too, with the buffer freed on every path.
TEST_F(PluginRunModeTest, AReadThatFailsMakesTheRunSignedOnlyAndFreesTheArgvBuffer) {
  using nevr_plugin_trust::ReadStage;
  for (ReadStage stage : {ReadStage::ArgvParse, ReadStage::ArgvSizeQuery, ReadStage::ArgvConversion}) {
    nevr_plugin_trust::TestHook_ResetTrust();
    ArmReadFault(stage, InjectedFault::CallFails);
    ApplyArgs({L"echovr.exe", L"-windowed"});
    EXPECT_EQ(nevr_plugin_trust::ActiveMode(), nevr_plugin_trust::Mode::SignedOnly) << "stage " << static_cast<int>(stage);
    EXPECT_EQ(nevr_plugin_trust::ActiveModeSource(), nevr_plugin_trust::ModeSource::CommandLineUnreadable)
        << "stage " << static_cast<int>(stage);
    EXPECT_EQ(nevr_plugin_trust::TestHook_ArgvBuffersLive(), 0) << "stage " << static_cast<int>(stage);
  }
}

// Both readings always run: a flag the game reading finds is the flag even when the other reading cannot read the line.
TEST_F(PluginRunModeTest, AReadingThatCannotReadDoesNotHideAFlagTheOtherFound) {
  ArmReadFault(nevr_plugin_trust::ReadStage::ArgvParse, InjectedFault::CallFails);
  ApplyArgs({L"echovr.exe", L"-REQUIRE-SIGNED-PLUGINS"});
  EXPECT_EQ(nevr_plugin_trust::ActiveMode(), nevr_plugin_trust::Mode::SignedOnly);
  EXPECT_EQ(nevr_plugin_trust::ActiveModeSource(), nevr_plugin_trust::ModeSource::CommandLine);
}

TEST_F(PluginRunModeTest, ARunWhoseLineCouldNotBeReadSaysSoAtWarningAndRefusesTheUnsignedPlugin) {
  ArmReadFault(nevr_plugin_trust::ReadStage::GameTokens, InjectedFault::ThrowBadAlloc);
  ApplyArgs({L"echovr.exe"});
  g_testPluginLoadPlan.push_back({"onframe", "test_plugin_onframe.dll", false, "", "{}"});

  LoadPlugins();

  EXPECT_EQ(GetLoadedPluginCount(), 0);
  EXPECT_TRUE(TestLogContainsAtLevel(EchoVR::LogLevel::Warning,
                                     "[NEVR.PLUGIN] plugin trust: mode=signed-only source=command line read failed"));
  EXPECT_TRUE(TestLogContains("the command line could not be read"));
}

TEST_F(PluginRunModeTest, ALineThatIsNotThereIsAnUnreadableOne) {
  nevr_plugin_trust::ApplyCommandLine(nullptr, "echovr.exe");
  EXPECT_EQ(nevr_plugin_trust::ActiveMode(), nevr_plugin_trust::Mode::SignedOnly);
  EXPECT_EQ(nevr_plugin_trust::ActiveModeSource(), nevr_plugin_trust::ModeSource::CommandLineUnreadable);
  nevr_plugin_trust::TestHook_ResetTrust();
  nevr_plugin_trust::ApplyCommandLine(L"echovr.exe", nullptr);
  EXPECT_EQ(nevr_plugin_trust::ActiveMode(), nevr_plugin_trust::Mode::SignedOnly);
  EXPECT_EQ(nevr_plugin_trust::ActiveModeSource(), nevr_plugin_trust::ModeSource::CommandLineUnreadable);
}

TEST_F(PluginRunModeTest, AnUnreadableCommandLineFailsClosed) {
  nevr_plugin_trust::ApplyUnreadableCommandLine();
  EXPECT_EQ(nevr_plugin_trust::ActiveMode(), nevr_plugin_trust::Mode::SignedOnly);
  EXPECT_EQ(nevr_plugin_trust::ActiveModeSource(), nevr_plugin_trust::ModeSource::CommandLineUnreadable);
  g_testPluginLoadPlan.push_back({"onframe", "test_plugin_onframe.dll", false, "", "{}"});

  LoadPlugins();

  EXPECT_EQ(GetLoadedPluginCount(), 0);
  EXPECT_TRUE(TestLogContains("plugin trust: mode=signed-only source=command line unreadable"));
}

TEST_F(PluginRunModeTest, WithNoCommandLineReadTheRunIsTheDefault) {
  EXPECT_EQ(nevr_plugin_trust::ActiveMode(), nevr_plugin_trust::Mode::UnsignedAllowed);
  EXPECT_EQ(nevr_plugin_trust::ActiveModeSource(), nevr_plugin_trust::ModeSource::Default);
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

TEST(PluginSignatureGate, NoFallbackToTheDefaultSearchInASignedOnlyRun) {
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
  const std::string source = nevrRoot_->PluginsDir() + "test_plugin_onframe.dll";
  const std::string escaped = nevrRoot_->PluginsDir() + "..\\test_plugin_escape.dll";  // in the root, the parent of the plugin folder
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
  const int fatalCallsBefore = ServerFatalCalls();
  g_testPluginLoadPlan.push_back({"partial", "test_plugin_init_fail.dll", false, "", "{}"});

  LoadPlugins();

  EXPECT_EQ(GetLoadedPluginCount(), 0);
  EXPECT_EQ(ServerFatalCalls(), fatalCallsBefore + 1);
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
  ResetServerFatal();
  g_testPluginLoadPlan.push_back({"gone", "plugin_that_does_not_exist.dll", false, "", "{}"});
  g_testPluginLoadPlan.push_back({"gone-again", "plugin_that_does_not_exist.dll", true, "", "{}"});

  LoadPlugins();

  EXPECT_EQ(ServerFatalCalls(), 1);
  EXPECT_NE(LastServerFatal().find("gone"), std::string::npos) << LastServerFatal();
  const nlohmann::json manifest = nlohmann::json::parse(BuildPluginManifestJson());
  ASSERT_EQ(manifest.size(), 2u) << manifest.dump();
  EXPECT_EQ(manifest[0].at("required"), true);
}

// The same, for a plugin that loads but whose init fails: the failure surfaces
// later (pass 2), and must still be fatal because a repeat required it.
TEST_F(PluginLoaderDiagnosticTest, RequiredRepeatOfInitFailingOptionalEntryIsFatal) {
  ResetServerFatal();
  g_testPluginLoadPlan.push_back({"flaky", "test_plugin_init_fail.dll", false, "", "{}"});
  g_testPluginLoadPlan.push_back({"flaky-again", "TEST_PLUGIN_INIT_FAIL.DLL", true, "", "{}"});

  LoadPlugins();

  EXPECT_EQ(ServerFatalCalls(), 1);
  EXPECT_NE(LastServerFatal().find("flaky"), std::string::npos) << LastServerFatal();
  EXPECT_EQ(GetLoadedPluginCount(), 0);
}

// Same, when the repeat is only caught by the same-module check.
TEST_F(PluginLoaderDiagnosticTest, RequiredRepeatUnderAnotherSpellingOfInitFailingEntryIsFatal) {
  ResetServerFatal();
  g_testPluginLoadPlan.push_back({"flaky", "test_plugin_init_fail.dll", false, "", "{}"});
  g_testPluginLoadPlan.push_back({"flaky-dotted", ".\\test_plugin_init_fail.dll", true, "", "{}"});

  LoadPlugins();

  EXPECT_EQ(ServerFatalCalls(), 1);
  EXPECT_EQ(GetLoadedPluginCount(), 0);
}

// A required repeat of a spelling that was itself skipped as the same module: the
// requirement reaches the entry that holds the module, not the skipped one. The
// holder loaded fine, so nothing is fatal.
TEST_F(PluginLoaderDiagnosticTest, RequiredRepeatOfSkippedSpellingOfLoadedEntryIsNotFatal) {
  ResetServerFatal();
  g_testPluginLoadPlan.push_back({"first", "test_plugin_onframe.dll", false, "", "{}"});
  g_testPluginLoadPlan.push_back({"second", ".\\test_plugin_onframe.dll", false, "", "{}"});
  g_testPluginLoadPlan.push_back({"third", ".\\test_plugin_onframe.dll", true, "", "{}"});

  LoadPlugins();

  EXPECT_EQ(ServerFatalCalls(), 0) << LastServerFatal();
  EXPECT_EQ(GetLoadedPluginCount(), 1);
  const nlohmann::json manifest = nlohmann::json::parse(BuildPluginManifestJson());
  ASSERT_EQ(manifest.size(), 3u) << manifest.dump();
  EXPECT_EQ(manifest[0].at("required"), true);
}

// The same chain when the holder's init fails: the fatal names the holder (the
// first entry), the entry that really holds the module.
TEST_F(PluginLoaderDiagnosticTest, RequiredRepeatOfSkippedSpellingOfInitFailingEntryNamesHolder) {
  ResetServerFatal();
  g_testPluginLoadPlan.push_back({"first", "test_plugin_init_fail.dll", false, "", "{}"});
  g_testPluginLoadPlan.push_back({"second", ".\\test_plugin_init_fail.dll", false, "", "{}"});
  g_testPluginLoadPlan.push_back({"third", ".\\test_plugin_init_fail.dll", true, "", "{}"});

  LoadPlugins();

  EXPECT_EQ(ServerFatalCalls(), 1) << LastServerFatal();
  EXPECT_NE(LastServerFatal().find("first"), std::string::npos) << LastServerFatal();
  EXPECT_EQ(LastServerFatal().find("second"), std::string::npos) << LastServerFatal();
  EXPECT_EQ(GetLoadedPluginCount(), 0);
}

// An optional first entry that loads, then a required repeat of the same spelling.
TEST_F(PluginLoaderDiagnosticTest, RequiredRepeatOfSameSpellingOfLoadedEntryIsNotFatal) {
  ResetServerFatal();
  g_testPluginLoadPlan.push_back({"first", "test_plugin_onframe.dll", false, "", "{}"});
  g_testPluginLoadPlan.push_back({"again", "test_plugin_onframe.dll", true, "", "{}"});

  LoadPlugins();

  EXPECT_EQ(ServerFatalCalls(), 0) << LastServerFatal();
  EXPECT_EQ(GetLoadedPluginCount(), 1);
}

// Control: an optional repeat of an optional entry stays non-fatal.
TEST_F(PluginLoaderDiagnosticTest, OptionalRepeatOfInitFailingOptionalEntryIsNotFatal) {
  ResetServerFatal();
  g_testPluginLoadPlan.push_back({"flaky", "test_plugin_init_fail.dll", false, "", "{}"});
  g_testPluginLoadPlan.push_back({"flaky-again", "test_plugin_init_fail.dll", false, "", "{}"});

  LoadPlugins();

  EXPECT_EQ(ServerFatalCalls(), 0);
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

// #432: an asset a plugin registered points into its image. The host drops it before FreeLibrary, so a
// plugin that never deregisters cannot leave the engine holding a pointer into freed memory.
TEST_F(PluginLoaderDiagnosticTest, UnloadReleasesAssetsRegisteredInsideThePluginImage) {
  g_testPluginLoadPlan.push_back({"onframe", "test_plugin_onframe.dll", false, "", "{}"});
  LoadPlugins();
  ASSERT_EQ(GetLoadedPluginCount(), 1);
  const HMODULE module = GetModuleHandleA("test_plugin_onframe.dll");
  ASSERT_NE(module, nullptr);

  nevr::assets::Registry& registry = nevr::assets::Global();
  const auto* inside = reinterpret_cast<const uint8_t*>(module) + 0x10;  // within the mapped image
  static const unsigned char outside[16] = {1};                          // a different image: this test
  ASSERT_EQ(registry.Register(0x11, 0x22, inside, 16, "inside"), nevr::assets::RegisterStatus::Ok);
  ASSERT_EQ(registry.Register(0x11, 0x23, outside, sizeof(outside), "outside"),
            nevr::assets::RegisterStatus::Ok);

  UnloadPlugins();

  nevr::assets::Asset found;
  EXPECT_FALSE(registry.Find(0x11, 0x22, &found)) << "an asset inside the freed image was left registered";
  EXPECT_TRUE(registry.Find(0x11, 0x23, &found)) << "an asset outside the plugin's image must stay";
  EXPECT_TRUE(TestLogContains("released 1 asset(s) still registered by a plugin at unload"));
  registry.Deregister(outside, outside + sizeof(outside));
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
  EXPECT_EQ(static_cast<uint32_t>(NEVR_PLUGIN_API_VERSION), 8u);
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

// ----------------------------------------------------------------------------
// #1094: a plugin sees the level the game holds as its main level, in every context it is handed.
// The fixture (plugin_level_test_dll.cpp) keeps the last level each kind of context carried. The game object
// is a zeroed buffer with the level written at the PC offset, so a level change is a store the test makes.
// ----------------------------------------------------------------------------

namespace evr {
extern const SymCacheEntry kSymCacheData[];  // the generated symbol cache (symcache_data.cpp)
extern const int kSymCacheCount;
}  // namespace evr

namespace {

constexpr uint64_t kArenaA = 0x576ed3f8428ebc4bULL;           // CSymbol64("mpl_arena_a"), in the symbol cache
constexpr uint64_t kCombatCombustion = 0x42670f2bed45703cULL;  // CSymbol64("mpl_combat_combustion"), in the cache

constexpr uint64_t kLobbyB2 = 0xd09afd15b1c75c04ULL;  // CSymbol64("mpl_lobby_b2"), in the symbol cache

std::vector<unsigned char> g_fakeGame;

void SetFakeGameLevel(uint64_t symbol) {
  std::memcpy(g_fakeGame.data() + EchoVR::kPcMainLevelSymbolOffset, &symbol, sizeof(symbol));
}

uint64_t LevelFixtureU64(const char* name, uint32_t which) {
  const HMODULE plugin = GetModuleHandleA("test_plugin_level.dll");
  if (plugin == nullptr) return 0xDEADDEADDEADDEADull;
  const auto fn = reinterpret_cast<uint64_t (*)(uint32_t)>(GetProcAddress(plugin, name));
  return fn != nullptr ? fn(which) : 0xDEADDEADDEADDEADull;
}

const char* LevelFixtureName(uint32_t which) {
  const HMODULE plugin = GetModuleHandleA("test_plugin_level.dll");
  if (plugin == nullptr) return "<fixture not loaded>";
  const auto fn = reinterpret_cast<const char* (*)(uint32_t)>(GetProcAddress(plugin, "NevrTestPluginLevelName"));
  return fn != nullptr ? fn(which) : "<export missing>";
}

constexpr uint32_t kInitCtx = 0;
constexpr uint32_t kFrameCtx = 1;
constexpr uint32_t kStateCtx = 2;

class PluginLevelTest : public PluginLoaderDiagnosticTest {
 protected:
  void SetUp() override {
    PluginLoaderDiagnosticTest::SetUp();
    g_fakeGame.assign(0x10000, 0);
    SetFakeGameLevel(EchoVR::kNoLevelInGame);
    g_pGame = g_fakeGame.data();
    g_testPluginLoadPlan.push_back({"lvl", "test_plugin_level.dll", false, "", "{}"});
  }
  void TearDown() override {
    g_pGame = nullptr;
    PluginLoaderDiagnosticTest::TearDown();
  }
  static void StateChange(uint32_t from, uint32_t to) {
    NvrGameContext ctx = {};
    ctx.ctx_size = sizeof(NvrGameContext);
    NotifyPluginsStateChange(&ctx, from, to);
  }
};

}  // namespace

TEST(GameLevel, TheOffsetsAreTheOnesMeasuredInTheGame) {
  EXPECT_EQ(EchoVR::kPcMainLevelSymbolOffset, 0x8a48u) << "echovr.exe: CR15Game +0x8a48, accessor 0x140100d90";
  EXPECT_EQ(EchoVR::kQuestMainLevelSymbolOffset, 0x8a40u) << "libr15.so: CurrentSpawnGameSpace 0x130065c";
}

TEST(GameLevel, ReadsTheSymbolAtThePcAndTheQuestOffsetAndMapsNoLevelToZero) {
  std::vector<unsigned char> game(0x10000, 0);
  EXPECT_EQ(EchoVR::ReadMainLevelSymbol(nullptr, EchoVR::kPcMainLevelSymbolOffset), 0u);
  const uint64_t none = EchoVR::kNoLevelInGame;
  std::memcpy(game.data() + EchoVR::kPcMainLevelSymbolOffset, &none, sizeof(none));
  EXPECT_EQ(EchoVR::ReadMainLevelSymbol(game.data(), EchoVR::kPcMainLevelSymbolOffset), 0u);
  const uint64_t pc = kArenaA;
  std::memcpy(game.data() + EchoVR::kPcMainLevelSymbolOffset, &pc, sizeof(pc));
  EXPECT_EQ(EchoVR::ReadMainLevelSymbol(game.data(), EchoVR::kPcMainLevelSymbolOffset), kArenaA);
  const uint64_t quest = kCombatCombustion;
  std::memcpy(game.data() + EchoVR::kQuestMainLevelSymbolOffset, &quest, sizeof(quest));
  EXPECT_EQ(EchoVR::ReadMainLevelSymbol(game.data(), EchoVR::kQuestMainLevelSymbolOffset), kCombatCombustion);
}

TEST(GameLevel, TheHashesAreTheGamesAndTheCacheNamesTheLevelsTheFixturesUseAndNotACustomOne) {
  EXPECT_EQ(EchoVR::CSymbol64Hash("mpl_arena_a"), kArenaA);
  EXPECT_EQ(EchoVR::CSymbol64Hash("mpl_combat_combustion"), kCombatCombustion);
  EXPECT_EQ(EchoVR::CSymbol64Hash("mpl_lobby_b2"), kLobbyB2);
  const auto nameOf = [](uint64_t hash) {
    return nevr_ui_texture_mips::NameForHash(evr::kSymCacheData, static_cast<size_t>(evr::kSymCacheCount), hash);
  };
  ASSERT_NE(nameOf(kArenaA), nullptr);
  EXPECT_STREQ(nameOf(kArenaA), "mpl_arena_a");
  ASSERT_NE(nameOf(kLobbyB2), nullptr);
  EXPECT_STREQ(nameOf(kLobbyB2), "mpl_lobby_b2");
  EXPECT_EQ(nameOf(EchoVR::CSymbol64Hash("mpl_arena_dodgeball")), nullptr);
}

TEST_F(PluginLevelTest, InitContextCarriesTheLevelTheGameAlreadyHolds) {
  SetFakeGameLevel(kArenaA);

  LoadPlugins();

  ASSERT_EQ(GetLoadedPluginCount(), 1);
  EXPECT_EQ(LevelFixtureU64("NevrTestPluginLevelCovered", kInitCtx), 1u) << "ctx_size does not reach level_name";
  EXPECT_EQ(LevelFixtureU64("NevrTestPluginLevelSymbol", kInitCtx), kArenaA);
  ASSERT_NE(LevelFixtureName(kInitCtx), nullptr);
  EXPECT_STREQ(LevelFixtureName(kInitCtx), "mpl_arena_a");
}

TEST_F(PluginLevelTest, NoLevelAndNoGameObjectBothReadAsNoLevel) {
  LoadPlugins();
  ASSERT_EQ(GetLoadedPluginCount(), 1);
  EXPECT_EQ(LevelFixtureU64("NevrTestPluginLevelSymbol", kInitCtx), 0u) << "the game holds -1";
  EXPECT_EQ(LevelFixtureName(kInitCtx), nullptr);

  g_pGame = nullptr;
  TickLoadedPluginsOnce();
  EXPECT_EQ(LevelFixtureU64("NevrTestPluginLevelCalls", kFrameCtx), 1u);
  EXPECT_EQ(LevelFixtureU64("NevrTestPluginLevelSymbol", kFrameCtx), 0u);
  EXPECT_EQ(LevelFixtureName(kFrameCtx), nullptr);
}

TEST_F(PluginLevelTest, FrameContextSeesALevelChangeWithNoStateTransition) {
  LoadPlugins();
  ASSERT_EQ(GetLoadedPluginCount(), 1);
  TickLoadedPluginsOnce();
  EXPECT_EQ(LevelFixtureU64("NevrTestPluginLevelSymbol", kFrameCtx), 0u);

  SetFakeGameLevel(kArenaA);
  TickLoadedPluginsOnce();
  EXPECT_EQ(LevelFixtureU64("NevrTestPluginLevelSymbol", kFrameCtx), kArenaA);
  EXPECT_STREQ(LevelFixtureName(kFrameCtx), "mpl_arena_a");
  EXPECT_EQ(LevelFixtureU64("NevrTestPluginLevelCalls", kStateCtx), 0u) << "no state change was announced";

  SetFakeGameLevel(kCombatCombustion);
  TickLoadedPluginsOnce();
  EXPECT_EQ(LevelFixtureU64("NevrTestPluginLevelSymbol", kFrameCtx), kCombatCombustion);
  EXPECT_STREQ(LevelFixtureName(kFrameCtx), "mpl_combat_combustion");
}

TEST_F(PluginLevelTest, StateChangeContextFollowsTheLevelAcrossLevelChangesAndIsLogged) {
  LoadPlugins();
  ASSERT_EQ(GetLoadedPluginCount(), 1);

  SetFakeGameLevel(kArenaA);
  StateChange(6, 7);
  EXPECT_EQ(LevelFixtureU64("NevrTestPluginLevelSymbol", kStateCtx), kArenaA);
  EXPECT_STREQ(LevelFixtureName(kStateCtx), "mpl_arena_a");

  ClearTestLogs();
  SetFakeGameLevel(kCombatCombustion);
  StateChange(7, 8);
  EXPECT_EQ(LevelFixtureU64("NevrTestPluginLevelSymbol", kStateCtx), kCombatCombustion);
  EXPECT_STREQ(LevelFixtureName(kStateCtx), "mpl_combat_combustion");
  EXPECT_TRUE(TestLogContainsAtLevel(
      EchoVR::LogLevel::Info,
      "[NEVR.PLUGIN] plugin=lvl state change level from=7 to=8 level_symbol=0x42670f2bed45703c "
      "level=mpl_combat_combustion"));
}

TEST_F(PluginLevelTest, TheLobbyLevelGetsItsNameInTheContextAndTheLog) {
  LoadPlugins();
  ASSERT_EQ(GetLoadedPluginCount(), 1);

  SetFakeGameLevel(kLobbyB2);
  TickLoadedPluginsOnce();
  EXPECT_EQ(LevelFixtureU64("NevrTestPluginLevelSymbol", kFrameCtx), kLobbyB2);
  ASSERT_NE(LevelFixtureName(kFrameCtx), nullptr);
  EXPECT_STREQ(LevelFixtureName(kFrameCtx), "mpl_lobby_b2");
  EXPECT_TRUE(TestLogContainsAtLevel(EchoVR::LogLevel::Info,
                                     "[NEVR.PLUGIN] main level changed: from=none to=mpl_lobby_b2 symbol=0xd09afd15b1c75c04"));
}

TEST_F(PluginLevelTest, ALevelTheSymbolTableDoesNotKnowHasASymbolAndNoName) {
  const uint64_t custom = EchoVR::CSymbol64Hash("mpl_arena_dodgeball");
  LoadPlugins();
  ASSERT_EQ(GetLoadedPluginCount(), 1);

  ClearTestLogs();
  SetFakeGameLevel(custom);
  StateChange(6, 7);

  EXPECT_EQ(LevelFixtureU64("NevrTestPluginLevelSymbol", kStateCtx), custom);
  EXPECT_EQ(LevelFixtureName(kStateCtx), nullptr);
  char hex[24];
  snprintf(hex, sizeof(hex), "0x%016llx", static_cast<unsigned long long>(custom));
  EXPECT_TRUE(TestLogContainsAtLevel(EchoVR::LogLevel::Info,
                                     std::string("[NEVR.PLUGIN] main level changed: from=none to=") + hex + " symbol=" + hex))
      << "a level with no name is logged by its hex symbol";
}

TEST_F(PluginLevelTest, TheHostLogsEachLevelChangeOnceHoweverManyPluginsAndFramesSeeIt) {
  g_testPluginLoadPlan.push_back({"lvl2", "test_plugin_onframe.dll", false, "", "{}"});
  LoadPlugins();
  ASSERT_EQ(GetLoadedPluginCount(), 2);
  TickLoadedPluginsOnce();  // settles the observer on "no level"

  ClearTestLogs();
  SetFakeGameLevel(kArenaA);
  TickLoadedPluginsOnce();
  TickLoadedPluginsOnce();
  StateChange(6, 7);
  EXPECT_EQ(TestLogCount("[NEVR.PLUGIN] main level changed:"), 1) << "one line per change, not one per read";
  EXPECT_EQ(TestLogCount("[NEVR.PLUGIN] main level changed: from=none to=mpl_arena_a"), 1);
  EXPECT_TRUE(TestLogContainsAtLevel(EchoVR::LogLevel::Info, "[NEVR.PLUGIN] main level changed: from=none to=mpl_arena_a"));

  ClearTestLogs();
  SetFakeGameLevel(EchoVR::kNoLevelInGame);
  TickLoadedPluginsOnce();
  EXPECT_EQ(TestLogCount("[NEVR.PLUGIN] main level changed:"), 1);
  EXPECT_EQ(TestLogCount("[NEVR.PLUGIN] main level changed: from=mpl_arena_a to=none"), 1);
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

// #738: a send that fails runs ix's Close callback on the sending thread, and the bridge's Close handler takes
// g_pairsMutex. Each production send on the login remote must therefore happen outside it. One test per send path;
// the remote's reader is parked so the failure is found by the send (TestHook_SendOnDroppedRemote).
static void ExpectSendOnDroppedRemoteReturns(int path, const char* what) {
  const int result = TestHook_SendOnDroppedRemote(path, 10000);
  if (result == 1) {
    ADD_FAILURE() << what << ": the sending thread deadlocked on g_pairsMutex inside ix's Close callback";
    std::fflush(stdout);
    std::_Exit(1);  // the stuck thread owns the mutex the fixture's teardown needs
  }
  EXPECT_EQ(result, 0) << what << ": no send on the dropped remote ever failed";
}

TEST_F(N61_WsBridgeTest, SendFrameToServerOnADroppedRemoteDoesNotHoldThePairsLock) {
  ExpectSendOnDroppedRemoteReturns(0, "SendFrameToServer");
}

TEST_F(N61_WsBridgeTest, AGameFrameForwardedToADroppedRemoteDoesNotHoldThePairsLock) {
  ExpectSendOnDroppedRemoteReturns(1, "the game->server forward");
}

TEST_F(N61_WsBridgeTest, TheFlushWhenTheRemoteOpensOnADroppedRemoteDoesNotHoldThePairsLock) {
  ExpectSendOnDroppedRemoteReturns(2, "the flush on the remote's open");
}

// ---- the flush's interleavings, with HandleRemoteOpen parked between its first batch
// of sends and its re-lock (the "login injected" log line). Hooks are in ws_bridge.cpp (TestHook_FlushRig_*).
void* TestHook_FlushRig_Start(unsigned int timeoutMs);
void TestHook_FlushRig_RunOpen(void* handle);
void TestHook_FlushRig_Forward(void* handle, int game, const std::string& frame);
void TestHook_FlushRig_AddMatchmaker(void* handle);
bool TestHook_FlushRig_ReplacePairAtSameAddress(void* handle, bool* allocatorReused);
void TestHook_FlushRig_ReplacementState(void* handle, bool* remoteOpen, size_t* pending);
bool TestHook_FlushRig_LoginPairOpen(void* handle);
std::vector<std::string> TestHook_FlushRig_Received(void* handle, size_t n, unsigned int timeoutMs);
void TestHook_FlushRig_Stop(void* handle);

namespace {
struct FlushPark {
  std::promise<void> parked;
  std::promise<void> release;
  std::future<void> parkedF;
  std::thread worker;
  explicit FlushPark(void* rig) {
    parkedF = parked.get_future();
    g_flushParked = &parked;
    g_flushRelease = release.get_future().share();
    g_flushPark = true;
    worker = std::thread([rig] { TestHook_FlushRig_RunOpen(rig); });
  }
  bool WaitParked() { return parkedF.wait_for(std::chrono::seconds(10)) == std::future_status::ready; }
  void Release() { release.set_value(); worker.join(); g_flushPark = false; }
};
std::string FlushShow(const std::vector<std::string>& got) {
  std::string out;
  for (const std::string& f : got) out += (f.size() <= 16 ? f : "<login " + std::to_string(f.size()) + "B>") + " ";
  return out;
}
}  // namespace

// (a)(b)(c): a game frame and a runtime frame that arrive while the flush is mid-way queue behind it, are each sent
// once, and the frame after the flush goes after them.
TEST_F(N61_WsBridgeTest, FramesDuringTheFlushQueueBehindItInOrderOnce) {
  void* rig = TestHook_FlushRig_Start(10000);
  ASSERT_NE(rig, nullptr);
  FlushPark park(rig);
  ASSERT_TRUE(park.WaitParked());
  TestHook_FlushRig_Forward(rig, 1, "g1");
  EXPECT_TRUE(TestHook_SendFrameToServer("s1"));
  EXPECT_FALSE(TestHook_FlushRig_LoginPairOpen(rig)) << "remoteOpen flipped before the drain finished";
  park.Release();
  EXPECT_TRUE(TestHook_FlushRig_LoginPairOpen(rig));
  TestHook_FlushRig_Forward(rig, 1, "g2");
  const std::vector<std::string> got = TestHook_FlushRig_Received(rig, 7, 3000);  // 6 expected; waits for a 7th
  std::fprintf(stderr, "flush order: %s\n", FlushShow(got).c_str());
  ASSERT_EQ(got.size(), 6u) << FlushShow(got);
  EXPECT_GT(got[0].size(), 16u) << "first frame is the LoginRequest";
  EXPECT_EQ(std::vector<std::string>(got.begin() + 1, got.end()),
            (std::vector<std::string>{"q1", "q2", "g1", "s1", "g2"}));
  TestHook_FlushRig_Stop(rig);
}

// TOCTOU/ABA: the login game socket closes while the flush is parked and a new game socket's pair takes the same
// address. PairRegisteredLocked compares addresses, so the parked flush adopts the new pair: it must not send the new
// pair's queued frame on the old remote, and must not mark the new pair's (still connecting) remote open.
TEST_F(N61_WsBridgeTest, AFlushDoesNotAdoptAPairRegisteredAtTheSameAddress) {
  void* rig = TestHook_FlushRig_Start(10000);
  ASSERT_NE(rig, nullptr);
  FlushPark park(rig);
  ASSERT_TRUE(park.WaitParked());
  bool reused = false;
  const bool replaced = TestHook_FlushRig_ReplacePairAtSameAddress(rig, &reused);
  park.Release();
  if (!replaced) {
    TestHook_FlushRig_Stop(rig);
    GTEST_SKIP() << "the allocator did not hand the erased pair's address back on this run";
  }
  bool open = false;
  size_t pending = 0;
  TestHook_FlushRig_ReplacementState(rig, &open, &pending);
  const std::vector<std::string> got = TestHook_FlushRig_Received(rig, 4, 1500);
  std::fprintf(stderr, "flush aba: replacement remoteOpen=%d pending=%zu; old remote received: %s\n", open ? 1 : 0,
               pending, FlushShow(got).c_str());
  EXPECT_FALSE(open) << "the new pair's remote is still connecting but was marked open";
  EXPECT_EQ(pending, 1u) << "the new pair's queued frame was taken by the old pair's flush";
  EXPECT_EQ(std::count(got.begin(), got.end(), std::string("x-for-r2")), 0) << "sent on the wrong remote";
  TestHook_FlushRig_Stop(rig);
}

// A matchmaker pair shares the login remote with remoteOpen=true from its admission (ws_bridge.cpp, the Matchmaker
// branch of the Open handler). A frame it sends while the login pair is mid-flush queues behind the flush, so the
// remote gets the frames of both sockets in the order they arrived (the flush held g_pairsMutex for its whole length
// before #738, which gave the same order).
TEST_F(N61_WsBridgeTest, AMatchmakerFrameDuringTheLoginFlushGoesAfterIt) {
  void* rig = TestHook_FlushRig_Start(10000);
  ASSERT_NE(rig, nullptr);
  TestHook_FlushRig_AddMatchmaker(rig);
  FlushPark park(rig);
  ASSERT_TRUE(park.WaitParked());
  TestHook_FlushRig_Forward(rig, 1, "g1");  // queued behind the flush
  TestHook_FlushRig_Forward(rig, 3, "m1");  // the matchmaker, while the flush is mid-way
  park.Release();
  const std::vector<std::string> got = TestHook_FlushRig_Received(rig, 5, 3000);
  std::fprintf(stderr, "flush matchmaker: %s\n", FlushShow(got).c_str());
  ASSERT_EQ(got.size(), 5u) << FlushShow(got);
  EXPECT_EQ(std::vector<std::string>(got.begin() + 1, got.end()), (std::vector<std::string>{"q1", "q2", "g1", "m1"}));
  TestHook_FlushRig_Stop(rig);
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
            if (isLogin) {
              std::lock_guard<std::mutex> lock(urisMutex_);
              if (loginFrame_.empty()) loginFrame_ = message->str;
              ++loginRequests_;
            }
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
  // The first LoginRequest frame the service received, as the game's bridge sent it.
  std::string LoginFrame() {
    std::lock_guard<std::mutex> lock(urisMutex_);
    return loginFrame_;
  }
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
  std::string loginFrame_;  // guarded by urisMutex_
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
  // Closes the socket from the game's side, as a game that quits would.
  void Disconnect() { socket_.stop(); }
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
  // Whether the bridge has closed the socket now; it does not wait.
  bool Closed() {
    std::lock_guard<std::mutex> lock(mutex_);
    return closed_;
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
    // A client run unless a test says otherwise: a server with no credential stops (#1254), and these tests are
    // about the roles, not the server's login. Without this they took the mode a test before them left behind.
    wasServerBase_ = g_isServer;
    g_isServer = ServerMode() ? TRUE : FALSE;
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
    g_isServer = wasServerBase_;
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
  // The run mode the bridge starts in (#1280): g_isServer is set before it starts, as boot.cpp does.
  virtual bool ServerMode() const { return false; }
  int fatalBefore_ = 0;
  BOOL wasServerBase_ = TRUE;
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
// Shared-route lifetime: the target a shared login frame is routed to must still be alive when the frame is sent.
// The seam runs on the remote's thread between SharedRouteLocked and sendBinary; the test holds it there and lets
// the game socket close first.
// ============================================================================

namespace {

constexpr auto kSeamPoll = std::chrono::milliseconds(10);
constexpr int kSeamPolls = 200;  // 2 seconds of bounded polling for the target to expire
constexpr auto kSeamHeld = std::chrono::seconds(5);  // the longest the seam waits to be released
constexpr auto kSeamReady = std::chrono::seconds(10);  // the longest the test waits for the seam or its result

std::weak_ptr<void> g_weak;
std::promise<void> g_inSeam;
std::future<void> g_inSeamFuture;
std::promise<void> g_gameClosing;
std::future<void> g_gameClosingFuture;
std::promise<bool> g_expiredAtSend;
std::future<bool> g_expiredAtSendFuture;

void ResetSeamState() {
  g_weak.reset();
  g_inSeam = std::promise<void>();
  g_inSeamFuture = g_inSeam.get_future();
  g_gameClosing = std::promise<void>();
  g_gameClosingFuture = g_gameClosing.get_future();
  g_expiredAtSend = std::promise<bool>();
  g_expiredAtSendFuture = g_expiredAtSend.get_future();
}

void Seam(void*) {
  g_inSeam.set_value();
  g_gameClosingFuture.wait_for(kSeamHeld);  // bounded: a test that fails before releasing does not hang the remote
  for (int poll = 0; poll < kSeamPolls && !g_weak.expired(); ++poll) std::this_thread::sleep_for(kSeamPoll);
  g_expiredAtSend.set_value(g_weak.expired());
}

// Clears the seam on every exit path, including a failed ASSERT.
struct SeamGuard {
  ~SeamGuard() { TestHook_SharedRoute_SetBeforeSend(nullptr); }
};

class SharedRouteLifetimeTest : public RolesByPathTest {
 protected:
  void SetUp() override {
    ResetSeamState();
    RolesByPathTest::SetUp();
  }
};

}  // namespace

TEST_F(SharedRouteLifetimeTest, TargetIsStillAliveAtTheSend) {
  GameSocket login;
  Admit(login, "/login");
  ASSERT_TRUE(login.WaitForFrames(1)) << "the /login socket never received the reply to the LoginRequest";
  g_weak = TestHook_FirstServiceClientWeak();
  ASSERT_FALSE(g_weak.expired()) << "the login game socket is not on the service listener";

  SeamGuard guard;
  TestHook_SharedRoute_SetBeforeSend(&Seam);
  remote_.PushToClients(nevr_evr_codec::kSymLoginSettings);
  ASSERT_EQ(g_inSeamFuture.wait_for(kSeamReady), std::future_status::ready)
      << "the shared-route send never reached the seam";

  login.Disconnect();
  g_gameClosing.set_value();
  ASSERT_EQ(g_expiredAtSendFuture.wait_for(kSeamReady), std::future_status::ready) << "the seam never reported";
  const bool expired = g_expiredAtSendFuture.get();
  TestHook_SharedRoute_SetBeforeSend(nullptr);
  EXPECT_FALSE(expired) << "target was freed between SharedRouteLocked and sendBinary";
}

TEST_F(SharedRouteLifetimeTest, HeldTargetIsNotExpiredAtTheSend) {
  GameSocket login;
  Admit(login, "/login");
  ASSERT_TRUE(login.WaitForFrames(1)) << "the /login socket never received the reply to the LoginRequest";
  g_weak = TestHook_FirstServiceClientWeak();
  ASSERT_FALSE(g_weak.expired()) << "the login game socket is not on the service listener";

  SeamGuard guard;
  TestHook_SharedRoute_SetBeforeSend(&Seam);
  remote_.PushToClients(nevr_evr_codec::kSymLoginSettings);
  ASSERT_EQ(g_inSeamFuture.wait_for(kSeamReady), std::future_status::ready)
      << "the shared-route send never reached the seam";

  g_gameClosing.set_value();  // release the seam without closing the login socket
  ASSERT_EQ(g_expiredAtSendFuture.wait_for(kSeamReady), std::future_status::ready) << "the seam never reported";
  const bool expired = g_expiredAtSendFuture.get();
  TestHook_SharedRoute_SetBeforeSend(nullptr);
  EXPECT_FALSE(expired) << "a held target expired before the send";
}

TEST_F(RolesByPathTest, LoginSlotIsEmptyOnceTheLoginSocketIsRetired) {
  GameSocket login;
  Admit(login, "/login");
  ASSERT_TRUE(login.WaitForFrames(1)) << "the /login socket never received the reply to the LoginRequest";
  ASSERT_NE(TestHook_LoginGameWsPtr(), nullptr) << "precondition: the login slot was not set";
  login.Disconnect();
  ASSERT_TRUE(YieldUntil([] { return TestHook_ConnectedGameSockets() == 0; })) << "the login socket never closed on the bridge";
  EXPECT_EQ(TestHook_LoginGameWsPtr(), nullptr) << "the login slot still holds the retired socket";
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

// #1082, through the real router: the login the bridge injects follows the game's own No VR flag. The flag word
// stands in for game+0x7ae0 (the hook is -1 outside these tests).
// A little-endian u64 at `offset` of a frame (the platform at 40 and the account at 48 of a LoginRequest).
static uint64_t FrameU64(const std::string& frame, size_t offset) {
  uint64_t value = 0;
  for (size_t i = 0; i < 8; ++i) value |= static_cast<uint64_t>(static_cast<unsigned char>(frame[offset + i])) << (8 * i);
  return value;
}

// The local user's UUID in the party/friend requests the bridge now builds: V5(<prefix>-<account>) of the login.
static bool SelfUuidIs(const std::string& xpid) {
  const auto out = nevr_social_party::Global().RefreshFriends();
  if (out.size() != 1U || out[0].payload.size() < 24U) return false;
  const nevr_social_party::Uuid want = nevr_social_party::UuidV5Nil(xpid);
  return std::memcmp(out[0].payload.data() + 8, want.data(), want.size()) == 0;
}

class GameFlagScope {
 public:
  GameFlagScope(int64_t flags, BOOL isServer) : wasServer_(g_isServer) {
    g_isServer = isServer;
    TestHook_SetGameFlagWord(flags);
    g_testDiscordId = kTestAccountIdText.c_str();
    g_testPassword = "test-password";  // a server needs the pair (#1254)
  }
  ~GameFlagScope() {
    g_testPassword = nullptr;
    g_testDiscordId = nullptr;
    TestHook_SetGameFlagWord(-1);
    g_isServer = wasServer_;
  }

 private:
  BOOL wasServer_;
};

TEST_F(RouterAllTest, ANoVrClientsInjectedLoginIsDmoNotTheOvrOrgOfItsVrSelf) {
  GameFlagScope flags(0x2100000, FALSE);  // -spectatorstream ORs 0x2100000
  GameSocket login;
  AdmitLogin(login);
  const std::string frame = remote_.LoginFrame();
  ASSERT_GE(frame.size(), 56U);
  const uint64_t account = FrameU64(frame, 48);
  EXPECT_EQ(account, kTestAccountId);
  EXPECT_EQ(FrameU64(frame, 40), 7ULL) << "a No VR client logs in as DMO";
  EXPECT_FALSE(LogLinesContaining("login injected xpid=DMO-" + std::to_string(account) + " platform=7").empty());
  EXPECT_TRUE(LogLinesContaining("login injected xpid=OVR-ORG-").empty());
  EXPECT_FALSE(LogLinesContaining("login platform=7 (DMO) no_vr=1 game_flags=0x2100000 is_server=0").empty());
  EXPECT_TRUE(SelfUuidIs("DMO-" + std::to_string(account))) << "the party/friend requests name the local user by its login";
}

TEST_F(RouterAllTest, AVrClientsInjectedLoginStaysOvrOrg) {
  GameFlagScope flags(0x8000000, FALSE);  // -noovr alone sets bit 27, which is not the flag
  GameSocket login;
  AdmitLogin(login);
  const std::string frame = remote_.LoginFrame();
  ASSERT_GE(frame.size(), 56U);
  const uint64_t account = FrameU64(frame, 48);
  EXPECT_EQ(account, kTestAccountId);
  EXPECT_EQ(FrameU64(frame, 40), 4ULL);
  EXPECT_FALSE(LogLinesContaining("login injected xpid=OVR-ORG-" + std::to_string(account) + " platform=4").empty());
  EXPECT_TRUE(LogLinesContaining("login injected xpid=DMO-").empty());
  EXPECT_TRUE(SelfUuidIs("OVR-ORG-" + std::to_string(FrameU64(frame, 48))));
}

TEST_F(RouterAllTest, AServerWithTheNoVrFlagStillLogsInAsOvrOrg) {
  GameFlagScope flags(0x100000, TRUE);
  GameSocket login;
  AdmitLogin(login);
  const std::string frame = remote_.LoginFrame();
  ASSERT_GE(frame.size(), 56U);
  EXPECT_EQ(FrameU64(frame, 40), 4ULL);
}

// The same decision through the bridge's own rules (network.session_router absent: the legacy Open handler, the
// default), which inject the login by their own path.
TEST_F(RolesByPathTest, LegacyNoVrClientsInjectedLoginIsDmoNotTheOvrOrgOfItsVrSelf) {
  GameFlagScope flags(0x2100000, FALSE);
  GameSocket login;
  Admit(login, "/login");
  ASSERT_TRUE(login.WaitForSymbol(nevr_evr_codec::kSymLoginSettings));
  const std::string frame = remote_.LoginFrame();
  ASSERT_GE(frame.size(), 56U);
  const uint64_t account = FrameU64(frame, 48);
  EXPECT_EQ(account, kTestAccountId);
  EXPECT_EQ(FrameU64(frame, 40), 7ULL);
  EXPECT_FALSE(LogLinesContaining("login injected xpid=DMO-" + std::to_string(account) + " platform=7").empty());
  EXPECT_TRUE(LogLinesContaining("login injected xpid=OVR-ORG-").empty());
  EXPECT_FALSE(LogLinesContaining("login platform=7 (DMO) no_vr=1 game_flags=0x2100000 is_server=0").empty());
  EXPECT_TRUE(SelfUuidIs("DMO-" + std::to_string(account))) << "the party/friend requests name the local user by its login";
}

TEST_F(RolesByPathTest, LegacyVrClientsInjectedLoginStaysOvrOrg) {
  GameFlagScope flags(0x8000000, FALSE);
  GameSocket login;
  Admit(login, "/login");
  ASSERT_TRUE(login.WaitForSymbol(nevr_evr_codec::kSymLoginSettings));
  const std::string frame = remote_.LoginFrame();
  ASSERT_GE(frame.size(), 56U);
  EXPECT_EQ(FrameU64(frame, 40), 4ULL);
  EXPECT_FALSE(LogLinesContaining("login injected xpid=OVR-ORG-" + std::to_string(FrameU64(frame, 48)) + " platform=4").empty());
  EXPECT_TRUE(LogLinesContaining("login injected xpid=DMO-").empty());
  EXPECT_TRUE(SelfUuidIs("OVR-ORG-" + std::to_string(FrameU64(frame, 48))));
}

// L5: a dedicated server has no login to complete; the service's LoginFailure reaches the game as a LoginSuccess.
// KNOWN-WRONG #1306: every refusal is replaced, whatever its status, and the default path says so only at Debug.
TEST_F(RouterAllTest, AServersLoginFailureReachesTheGameAsALoginSuccess) {
  GameFlagScope server(-1, TRUE);  // a server with a credential: without one it stops (#1254)
  remote_.SetLoginReply(nevr_evr_codec::kSymLoginFailure);
  GameSocket login;
  Admit(login, "/login");
  ASSERT_TRUE(login.WaitForSymbol(nevr_evr_codec::kSymLoginSuccess)) << "the game was left at the login gate";
  EXPECT_FALSE(login.HasReceived(nevr_evr_codec::kSymLoginFailure));
  // The substitution is said at Info, with its reason: Debug is off in production.
  bool saidAtInfo = false;
  {
    std::lock_guard<std::mutex> lock(g_testLogMutex);
    for (const auto& entry : g_testLogLeveled) {
      if (entry.first == EchoVR::LogLevel::Info &&
          entry.second.find("the service answered the server's /login with a LoginFailure") != std::string::npos) {
        saidAtInfo = true;
      }
    }
  }
  EXPECT_TRUE(saidAtInfo);
}

// L6: an Unrequire nobody asked for is dropped and counted, not handed to a socket.
TEST_F(RouterAllTest, AnUnrequireNobodyAskedForIsDroppedAndCounted) {
  GameSocket login;
  AdmitLogin(login);
  remote_.PushToClients(nevr_evr_codec::kSymConnectionUnrequire);
  ASSERT_TRUE(YieldUntil([this] { return RouterStats().droppedUnrequires == 1; })) << "the Unrequire was not counted";
  EXPECT_EQ(login.Count(nevr_evr_codec::kSymConnectionUnrequire), 0);
}

// #1280: the bridge started in a named run mode. g_isServer is set before the bridge starts, as boot.cpp does; this
// file's default is a server (g_isServer above), so a test of client behaviour names the client mode.
template <typename Base, BOOL kIsServer>
class RunModeFixture : public Base {
 protected:
  bool ServerMode() const override { return kIsServer != FALSE; }
};
using ServerRolesByPathTest = RunModeFixture<RolesByPathTest, TRUE>;
using ServerRouterAllTest = RunModeFixture<RouterAllTest, TRUE>;
using ClientRouterAllTest = RunModeFixture<RouterAllTest, FALSE>;

// A login the service accepted is followed by the friend-list subscription, sent once, on the session.
TEST_F(ClientRouterAllTest, ALoginSuccessIsFollowedByTheFriendListSubscription) {
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
TEST_F(ClientRouterAllTest, ARuntimeFrameIsRefusedBeforeTheSessionAndSentAfter) {
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

// A round trip that proves the service has read everything sent on the session before it: the service answers a
// DocumentRequest in order, on the same connection.
static void RoundTripOnTheSession(GameSocket& login) {
  login.Send(nevr_evr_codec::kSymDocumentRequest);
  ASSERT_TRUE(login.WaitForSymbol(nevr_evr_codec::kSymDocumentSuccess)) << "the session never answered";
}

// #1280: a server whose login the service accepts subscribes to no friend list (legacy path, the default).
TEST_F(ServerRolesByPathTest, AServerSendsNoFriendListSubscriptionAfterTheServiceAcceptsItsLogin) {
  GameFlagScope server(-1, TRUE);  // a server with a credential: without one it stops (#1254)
  remote_.SetLoginReply(nevr_evr_codec::kSymLoginSuccess);
  GameSocket login;
  Admit(login, "/login");
  ASSERT_TRUE(login.WaitForSymbol(nevr_evr_codec::kSymLoginSuccess)) << "the server was left at the login gate";
  RoundTripOnTheSession(login);
  EXPECT_EQ(remote_.ReceivedCount(nevr_evr_codec::FirstSymbol(nevr_evr_codec::BuildFriendListSubscribe())), 0);
}

// #1280: the same through the router.
TEST_F(ServerRouterAllTest, AServerSendsNoFriendListSubscriptionAfterTheServiceAcceptsItsLogin) {
  GameFlagScope server(-1, TRUE);  // a server with a credential: without one it stops (#1254)
  remote_.SetLoginReply(nevr_evr_codec::kSymLoginSuccess);
  GameSocket login;
  Admit(login, "/login");
  ASSERT_TRUE(login.WaitForSymbol(nevr_evr_codec::kSymLoginSuccess)) << "the server was left at the login gate";
  RoundTripOnTheSession(login);
  EXPECT_EQ(remote_.ReceivedCount(nevr_evr_codec::FirstSymbol(nevr_evr_codec::BuildFriendListSubscribe())), 0);
}

// #1280: on a server the runtime's social sender sends nothing, while the self-check sender (its RemoteLogSet is not
// social) still reaches the service.
TEST_F(ServerRolesByPathTest, TheRuntimesSocialSenderSendsNothingOnAServer) {
  GameFlagScope server(-1, TRUE);  // a server with a credential: without one it stops (#1254)
  GameSocket login;
  Admit(login, "/login");
  ASSERT_TRUE(login.WaitForSymbol(nevr_evr_codec::kSymLoginSettings)) << "the login never reached the service";
  const nevr_social_party::Sender sender = nevr_social_party::SenderSlot().load();
  ASSERT_NE(sender, nullptr);
  const nevr_social_party::Message refresh = nevr_social_party::Global().RefreshFriends().at(0);
  EXPECT_FALSE(sender(nevr_social_party::Frame(refresh)));
  EXPECT_FALSE(nevr_social_party::Send(nevr_social_party::Global().RefreshFriends()));
  EXPECT_TRUE(TestHook_SendFrameToServer(nevr_evr_codec::BuildMessage(nevr_evr_codec::kSymDocumentRequest, "")));
  ASSERT_TRUE(login.WaitForSymbol(nevr_evr_codec::kSymDocumentSuccess)) << "the self-check sender's frame was lost";
  EXPECT_EQ(remote_.ReceivedCount(refresh.symbol), 0);
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

TEST_F(SharedRouteLifetimeTest, LoginFailureSendHoldsTheGameSocketAcrossTheSend) {
  remote_.SetLoginReplyFrame(BuildLoginFailureFrame(24 + 64, 400, std::string(64, 'x')));
  GameSocket login;
  Admit(login, "/login");
  g_weak = TestHook_FirstServiceClientWeak();
  ASSERT_FALSE(g_weak.expired()) << "the login game socket is not on the service listener";

  SeamGuard guard;
  TestHook_SharedRoute_SetBeforeSend(&Seam);
  ASSERT_EQ(g_inSeamFuture.wait_for(kSeamReady), std::future_status::ready)
      << "the login-failure send never reached the seam";

  login.Disconnect();
  g_gameClosing.set_value();
  ASSERT_EQ(g_expiredAtSendFuture.wait_for(kSeamReady), std::future_status::ready) << "the seam never reported";
  const bool expired = g_expiredAtSendFuture.get();
  TestHook_SharedRoute_SetBeforeSend(nullptr);
  EXPECT_FALSE(expired) << "the game socket was freed before the login-failure send";
}

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

TEST(SecurityDiagnostics, AServersLoginFailureSummaryDoesNotClaimItIsExpectedEveryBoot) {
  ClearTestLogs();
  const std::string frame = BuildLoginFailureFrame(24 + 4, 401, "nope");

  EXPECT_TRUE(TestHook_LogLoginFailureDiagnostic(frame, true));
  std::lock_guard<std::mutex> lock(g_testLogMutex);
  ASSERT_EQ(g_testLogMessages.size(), 1U);
  EXPECT_NE(g_testLogMessages[0].find("login failed status=401"), std::string::npos);
  EXPECT_NE(g_testLogMessages[0].find("synthesized LoginSuccess"), std::string::npos);
  EXPECT_NE(g_testLogMessages[0].find("credential is checked at ServerDB registration"), std::string::npos);
  EXPECT_EQ(g_testLogMessages[0].find("expected every boot"), std::string::npos);
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
  // Per-message detail is Debug on a server too (#1292): an Info record here is a per-frame line at the default level.
  EXPECT_TRUE(TestLogContainsAtLevel(EchoVR::LogLevel::Debug, "msg=0 sym=0x1111111111111111"));
  EXPECT_FALSE(TestLogContainsAtLevel(EchoVR::LogLevel::Info, "msg=0 sym=0x1111111111111111"));
}

TEST(WsBridgeFrameLog, StopsAtATruncatedMessageAndSaysSo) {
  ClearTestLogs();
  std::string truncated = BuildMarkedMessage(0x3333333333333333ULL, std::string(10, 'c'));
  truncated.resize(truncated.size() - 4);
  EXPECT_EQ(TestHook_LogFrameMessages("game->server", 0, truncated), 1);
  EXPECT_TRUE(TestLogContains("declares 10 bytes but 6 remain"));
}

// ---- #929: a client's Info records for the lobby find, join, session success and session failure frames ----
//
// Every test drives ObserveSocialFrames, the function the bridge calls for each frame in both directions
// (TestHook_ObserveSocialFrames), so the records are asserted where the bridge writes them. Payloads are
// built here byte by byte from the layouts in compat/legacy_codec.cpp (FindToLegacy, JoinToLegacy,
// SuccessV5ToLegacy, FailureToLegacy), not by calling the code under test.
namespace lobby_frame_test {

constexpr uint64_t kFindSym = 0x312c2a01819aa3f5ULL;     // SNSLobbyFindSessionRequestv11
constexpr uint64_t kJoinSym = 0x2f03468f77ffb211ULL;     // SNSLobbyJoinSessionRequestv7
constexpr uint64_t kSuccessSym = 0x6d4de3650ee3110fULL;  // SNSLobbySessionSuccessv5
constexpr uint64_t kFailureSym = 0x4ae8365ebc45f96cULL;  // SNSLobbySessionFailurev4
constexpr uint64_t kOvrOrg = 4;

// The GUID text and the payload builders of the success and failure frames are the golden's
// (lobby_record_golden.h): one set of bytes for both test suites, so the PC and the Quest records see the same input.
using lobby_record_golden::FailurePayload;
using lobby_record_golden::FailureWithMessage;
using lobby_record_golden::Guid;
using lobby_record_golden::PutLe;
using lobby_record_golden::SuccessPayload;

std::string Entrants(const std::vector<uint64_t>& accounts) {
  std::string out;
  for (const uint64_t account : accounts) {
    PutLe(out, kOvrOrg, 8);
    PutLe(out, account, 8);
  }
  return out;
}

constexpr uint64_t kMode = 0x1234567890abcdefULL;  // in no symbol table: logged as hex
const char kSettingsSentinel[] = "{\"sentinel_settings_text\":true}";

std::string FindPayload(const std::vector<uint64_t>& accounts, const std::string& currentLobby) {
  std::string out;
  PutLe(out, 1, 8);                                 // version lock
  PutLe(out, kMode, 8);                             // mode
  PutLe(out, 0x2222222222222222ULL, 8);             // level
  PutLe(out, kOvrOrg, 8);                           // platform
  out += Guid(0xa0);                                // login session
  PutLe(out, accounts.size(), 1);                   // entrant count
  PutLe(out, 0, 4);                                 // flags
  PutLe(out, 0, 3);                                 // pad
  out += currentLobby;                              // lobby being left
  out += Guid(0xb0);                                // group
  out += kSettingsSentinel;
  out.push_back('\0');
  out += Entrants(accounts);
  return out;
}

std::string JoinPayload(const std::vector<uint64_t>& accounts) {
  std::string out;
  out += Guid(0xc0);                                // lobby to join
  PutLe(out, 1, 8);                                 // version lock
  PutLe(out, kOvrOrg, 8);                           // platform
  out += Guid(0xa0);                                // login session
  PutLe(out, accounts.size(), 4);                   // entrant count in the low byte
  PutLe(out, 0, 4);
  PutLe(out, 0, 8);
  out += kSettingsSentinel;
  out.push_back('\0');
  out += Entrants(accounts);
  PutLe(out, 0xffff, 2);                            // team tail
  return out;
}

uint64_t g_fakeNowMs = 0;
uint64_t FakeNowMs() { return g_fakeNowMs; }

// The party a session record names (TestHook_SetLobbyPartySource). ClientRun installs a view with no party; a test
// names the party it reads by setting g_fakePartyView, so no test reads the process-wide party state it did not set.
nevr_social_party::View g_fakePartyView;
nevr_social_party::View FakePartyView() { return g_fakePartyView; }

// A view of a party `partyId` of `size` members, led by `ownerId`, with the local account `selfId` (0: not known).
nevr_social_party::View PartyView(uint64_t partyId, size_t size, uint64_t ownerId, uint64_t selfId) {
  nevr_social_party::View view;
  view.partyId = partyId;
  view.ownerId = ownerId;
  view.selfId = selfId;
  view.members.assign(size, nevr_social_party::Member{});
  return view;
}

size_t CountLogLines(const std::string& needle) {
  std::lock_guard<std::mutex> lock(g_testLogMutex);
  return static_cast<size_t>(std::count_if(g_testLogMessages.begin(), g_testLogMessages.end(),
                                           [&needle](const std::string& line) { return line.find(needle) != std::string::npos; }));
}

// A client run: g_isServer false, a fresh lobby-record limiter and request tracker, a clock the test moves by hand,
// and a party with no members (g_fakePartyView) until a test names one.
class ClientRun {
 public:
  ClientRun() : wasServer_(g_isServer) {
    g_isServer = FALSE;
    g_fakeNowMs = 1000;
    g_fakePartyView = nevr_social_party::View{};
    TestHook_ResetLobbyFrameLimiter();
    TestHook_SetLobbyFrameClock(&FakeNowMs);
    TestHook_SetLobbyPartySource(&FakePartyView);
    ClearTestLogs();
  }
  ~ClientRun() {
    g_isServer = wasServer_;
    TestHook_SetLobbyPartySource(nullptr);
    TestHook_SetLobbyFrameClock(nullptr);
    TestHook_ResetLobbyFrameLimiter();
  }

 private:
  BOOL wasServer_;
};

}  // namespace lobby_frame_test

// The records of one scenario, byte for byte (#1429, Rule 16): a find, then a success and a failure that answer it. The
// same bytes and the same expected text are in lobby_record_golden.h, which the Quest router's test also compares with.
TEST(WsBridgeLobbyFrameLog, SessionRecordsMatchTheGoldenLines) {
  using namespace lobby_frame_test;
  namespace golden = lobby_record_golden;
  ClientRun run;
  g_fakePartyView = PartyView(golden::kPartyId, golden::kPartyMembers, golden::kPartyOwner, golden::kPartySelf);
  g_fakeNowMs = golden::kFindAt;
  TestHook_ObserveSocialFrames("game->server", golden::kConn,
                               BuildMarkedMessage(kFindSym, FindPayload({1001}, Guid(0xd0))));
  g_fakeNowMs = golden::kSuccessAt;
  TestHook_ObserveSocialFrames("server->game", golden::kConn, BuildMarkedMessage(kSuccessSym, SuccessPayload()));
  g_fakeNowMs = golden::kFailureAt;
  TestHook_ObserveSocialFrames("server->game", golden::kConn, BuildMarkedMessage(kFailureSym, FailurePayload(8)));
  EXPECT_EQ(LogLinesContaining("lobby session success"), std::vector<std::string>{golden::kSuccessAfterFind});
  EXPECT_EQ(LogLinesContaining("lobby session failure"), std::vector<std::string>{golden::kFailureAfterSuccess});
}

// A success with no find sent before it and no party: the request fields and the party fields both read none.
TEST(WsBridgeLobbyFrameLog, ASessionRecordWithNoRequestAndNoPartyNamesNone) {
  using namespace lobby_frame_test;
  namespace golden = lobby_record_golden;
  ClientRun run;
  TestHook_ObserveSocialFrames("server->game", golden::kConn, BuildMarkedMessage(kSuccessSym, SuccessPayload()));
  EXPECT_EQ(LogLinesContaining("lobby session success"),
            std::vector<std::string>{golden::kSuccessNoRequestNoParty});
}

// The bridge cannot bound how often the game sends a join (its party-follow loop runs from the game's own
// update), so a kind writes a burst of records per window and counts the rest.
TEST(WsBridgeLobbyFrameLog, AJoinStormWritesItsBurstAndCountsTheRest) {
  using namespace lobby_frame_test;
  ClientRun run;
  for (int i = 0; i < 50; ++i) {
    TestHook_ObserveSocialFrames("game->server", 3, BuildMarkedMessage(kJoinSym, JoinPayload({1001})));
  }
  EXPECT_EQ(CountLogLines("lobby join requested"), 10U);
  EXPECT_EQ(CountLogLines("suppressed"), 0U) << "the count is written when the window ends, not before";
  // A different kind has its own burst: the answer to the storm is still recorded.
  TestHook_ObserveSocialFrames("server->game", 3, BuildMarkedMessage(kFailureSym, FailurePayload(8)));
  EXPECT_TRUE(TestLogContains("lobby session failure conn=3 code=8 reason=lobby_full"));
}

TEST(WsBridgeLobbyFrameLog, TheSuppressedCountIsWrittenBeforeTheFirstRecordOfTheNextWindow) {
  using namespace lobby_frame_test;
  ClientRun run;
  for (int i = 0; i < 50; ++i) {
    TestHook_ObserveSocialFrames("game->server", 3, BuildMarkedMessage(kJoinSym, JoinPayload({1001})));
  }
  g_fakeNowMs += 10000;
  ClearTestLogs();
  TestHook_ObserveSocialFrames("game->server", 3, BuildMarkedMessage(kJoinSym, JoinPayload({1001})));
  std::lock_guard<std::mutex> lock(g_testLogMutex);
  ASSERT_EQ(g_testLogLeveled.size(), 2U);
  EXPECT_EQ(g_testLogLeveled[0].first, EchoVR::LogLevel::Info);
  EXPECT_NE(g_testLogLeveled[0].second.find("[NEVR.WS] lobby frame records suppressed kind=join count=40 window_ms=10000"),
            std::string::npos)
      << g_testLogLeveled[0].second;
  EXPECT_NE(g_testLogLeveled[1].second.find("lobby join requested"), std::string::npos) << g_testLogLeveled[1].second;
}

TEST(WsBridgeLobbyFrameLog, AnotherKindsFrameReportsTheEndedWindowToo) {
  using namespace lobby_frame_test;
  ClientRun run;
  for (int i = 0; i < 13; ++i) {
    TestHook_ObserveSocialFrames("game->server", 3, BuildMarkedMessage(kJoinSym, JoinPayload({1001})));
  }
  g_fakeNowMs += 10000;
  ClearTestLogs();
  TestHook_ObserveSocialFrames("server->game", 3, BuildMarkedMessage(kFailureSym, FailurePayload(8)));
  EXPECT_TRUE(TestLogContains("lobby frame records suppressed kind=join count=3 window_ms=10000"));
  EXPECT_TRUE(TestLogContains("lobby session failure conn=3 code=8"));
}

TEST(WsBridgeLobbyFrameLog, AWindowThatSuppressedNothingWritesNoSummary) {
  using namespace lobby_frame_test;
  ClientRun run;
  for (int i = 0; i < 5; ++i) {
    TestHook_ObserveSocialFrames("game->server", 3, BuildMarkedMessage(kJoinSym, JoinPayload({1001})));
  }
  g_fakeNowMs += 10000;
  TestHook_ObserveSocialFrames("game->server", 3, BuildMarkedMessage(kJoinSym, JoinPayload({1001})));
  EXPECT_EQ(CountLogLines("lobby join requested"), 6U);
  EXPECT_EQ(CountLogLines("suppressed"), 0U);
}

TEST(WsBridgeLobbyFrameLog, FramesTheDecodeCannotReadAreLimitedLikeTheRest) {
  using namespace lobby_frame_test;
  ClientRun run;
  for (int i = 0; i < 50; ++i) {
    TestHook_ObserveSocialFrames("game->server", 2, BuildMarkedMessage(kJoinSym, std::string(10, '\x01')));
  }
  EXPECT_EQ(CountLogLines("lobby join request could not be read"), 10U);
}

TEST(WsBridgeLobbyFrameLog, ClientLogsAFindRequestAtInfoWithItsEntrantsAndLobbies) {
  using namespace lobby_frame_test;
  ClientRun run;
  TestHook_ObserveSocialFrames("game->server", 2,
                               BuildMarkedMessage(kFindSym, FindPayload({1001, 1002, 1003}, Guid(0xd0))));
  EXPECT_TRUE(TestLogContainsAtLevel(
      EchoVR::LogLevel::Info,
      "[NEVR.WS] lobby find requested conn=2 mode=0x1234567890abcdef entrants=3 "
      "accounts=OVR-ORG-1001,OVR-ORG-1002,OVR-ORG-1003 current_lobby=44332211-6655-8877-99AA-BBCCDDEEFFD0 "
      "group=44332211-6655-8877-99AA-BBCCDDEEFFB0"));
}

TEST(WsBridgeLobbyFrameLog, ClientLogsAFindRequestFromNoLobbyAsNone) {
  using namespace lobby_frame_test;
  ClientRun run;
  TestHook_ObserveSocialFrames("game->server", 2, BuildMarkedMessage(kFindSym, FindPayload({7}, std::string(16, '\0'))));
  EXPECT_TRUE(TestLogContainsAtLevel(EchoVR::LogLevel::Info, "entrants=1 accounts=OVR-ORG-7 current_lobby=none "));
}

TEST(WsBridgeLobbyFrameLog, ClientLogsAJoinRequestAtInfoWithTheLobbyItNames) {
  using namespace lobby_frame_test;
  ClientRun run;
  TestHook_ObserveSocialFrames("game->server", 3, BuildMarkedMessage(kJoinSym, JoinPayload({1001})));
  EXPECT_TRUE(TestLogContainsAtLevel(
      EchoVR::LogLevel::Info,
      "[NEVR.WS] lobby join requested conn=3 lobby=44332211-6655-8877-99AA-BBCCDDEEFFC0 entrants=1 "
      "accounts=OVR-ORG-1001"));
}

TEST(WsBridgeLobbyFrameLog, ClientLogsASessionSuccessAtInfoWithTheLobbyItGot) {
  using namespace lobby_frame_test;
  ClientRun run;
  TestHook_ObserveSocialFrames("server->game", 3, BuildMarkedMessage(kSuccessSym, SuccessPayload()));
  EXPECT_TRUE(TestLogContainsAtLevel(
      EchoVR::LogLevel::Info,
      "[NEVR.WS] lobby session success conn=3 lobby=44332211-6655-8877-99AA-BBCCDDEEFFC0 "
      "group=44332211-6655-8877-99AA-BBCCDDEEFFB0"));
}

TEST(WsBridgeLobbyFrameLog, ClientLogsASessionFailureAtInfoWithItsCodeAndReason) {
  using namespace lobby_frame_test;
  ClientRun run;
  TestHook_ObserveSocialFrames("server->game", 3, BuildMarkedMessage(kFailureSym, FailurePayload(8)));
  EXPECT_TRUE(TestLogContainsAtLevel(
      EchoVR::LogLevel::Info,
      "[NEVR.WS] lobby session failure conn=3 code=8 reason=lobby_full group=44332211-6655-8877-99AA-BBCCDDEEFFB0"));
  ClearTestLogs();
  TestHook_ObserveSocialFrames("server->game", 3, BuildMarkedMessage(kFailureSym, FailurePayload(99)));
  EXPECT_TRUE(TestLogContainsAtLevel(EchoVR::LogLevel::Info, "lobby session failure conn=3 code=99 reason=unknown "));
}

TEST(WsBridgeLobbyFrameLog, EveryMessageOfABatchedFrameGetsItsOwnRecord) {
  using namespace lobby_frame_test;
  ClientRun run;
  TestHook_ObserveSocialFrames("server->game", 3,
                               BuildMarkedMessage(kFailureSym, FailurePayload(8)) +
                                   BuildMarkedMessage(kSuccessSym, SuccessPayload()));
  EXPECT_TRUE(TestLogContains("lobby session failure conn=3 code=8"));
  EXPECT_TRUE(TestLogContains("lobby session success conn=3 lobby="));
}

// The failure record gives the count of its message's bytes and never the text (logging.md Rule 4 and Rule 16).
TEST(WsBridgeLobbyFrameLog, AFailureRecordCountsItsMessageAndNeverWritesItsText) {
  using namespace lobby_frame_test;
  ClientRun run;
  TestHook_ObserveSocialFrames("server->game", 3,
                               BuildMarkedMessage(kFailureSym, FailureWithMessage(8, "sentinel-remote-message")));
  EXPECT_TRUE(TestLogContainsAtLevel(EchoVR::LogLevel::Info,
                                     "lobby session failure conn=3 code=8 reason=lobby_full "
                                     "group=44332211-6655-8877-99AA-BBCCDDEEFFB0 message_bytes=23"));
  EXPECT_FALSE(TestLogContains("sentinel-remote-message"));
  ClearTestLogs();
  TestHook_ObserveSocialFrames("server->game", 3, BuildMarkedMessage(kFailureSym, FailureWithMessage(8, "")));
  EXPECT_TRUE(TestLogContains("lobby session failure conn=3 code=8 reason=lobby_full "
                              "group=44332211-6655-8877-99AA-BBCCDDEEFFB0 message_bytes=0"));
  ClearTestLogs();
  TestHook_ObserveSocialFrames("server->game", 3,
                               BuildMarkedMessage(kFailureSym, FailureWithMessage(8, std::string(80, 'y'))));
  EXPECT_TRUE(TestLogContains("group=44332211-6655-8877-99AA-BBCCDDEEFFB0 message_bytes=64"));
  EXPECT_FALSE(TestLogContains("yyyy"));
}

TEST(WsBridgeLobbyFrameLog, NoRecordCarriesSettingsTextOrTheServersMessage) {
  using namespace lobby_frame_test;
  ClientRun run;
  TestHook_ObserveSocialFrames("game->server", 2, BuildMarkedMessage(kFindSym, FindPayload({1}, Guid(0xd0))));
  TestHook_ObserveSocialFrames("game->server", 2, BuildMarkedMessage(kJoinSym, JoinPayload({1})));
  TestHook_ObserveSocialFrames("server->game", 2, BuildMarkedMessage(kFailureSym, FailurePayload(8)));
  EXPECT_TRUE(TestLogContains("lobby find requested"));
  EXPECT_FALSE(TestLogContains("sentinel_settings_text"));
  EXPECT_FALSE(TestLogContains("sentinel-remote-message"));
}

TEST(WsBridgeLobbyFrameLog, AFrameTooShortForItsLayoutIsReportedNotReadPastItsEnd) {
  using namespace lobby_frame_test;
  ClientRun run;
  TestHook_ObserveSocialFrames("game->server", 2, BuildMarkedMessage(kJoinSym, std::string(10, '\x01')));
  TestHook_ObserveSocialFrames("server->game", 2, BuildMarkedMessage(kFailureSym, std::string(12, '\x01')));
  EXPECT_TRUE(TestLogContainsAtLevel(EchoVR::LogLevel::Warning,
                                     "[NEVR.WS] lobby join request could not be read conn=2 payload_bytes=10"));
  EXPECT_TRUE(TestLogContainsAtLevel(EchoVR::LogLevel::Warning,
                                     "[NEVR.WS] lobby session failure could not be read conn=2 payload_bytes=12"));
  EXPECT_FALSE(TestLogContains("lobby join requested"));
  EXPECT_FALSE(TestLogContains("lobby session failure conn="));
}

TEST(WsBridgeLobbyFrameLog, ARequestHeardFromTheServerAndAReplyHeardFromTheGameAreNotLobbyRecords) {
  using namespace lobby_frame_test;
  ClientRun run;
  TestHook_ObserveSocialFrames("server->game", 2, BuildMarkedMessage(kJoinSym, JoinPayload({1})));
  TestHook_ObserveSocialFrames("game->server", 2, BuildMarkedMessage(kFailureSym, FailurePayload(8)));
  TestHook_ObserveSocialFrames("game->server", 2, BuildMarkedMessage(kSuccessSym, SuccessPayload()));
  EXPECT_FALSE(TestLogContains("lobby "));
}

// The lobby records are the same on a server as on a client (#1292): LogFrameMessages is Debug on both, so
// the Info records do not double anything, and a server's record of a find is the one a client writes.
TEST(WsBridgeLobbyFrameLog, AServerWritesTheLobbyRecordsToo) {
  using namespace lobby_frame_test;
  ClientRun run;
  g_isServer = TRUE;  // ClientRun's limiter and clock; only the run mode differs. Its destructor restores it.
  TestHook_ObserveSocialFrames("game->server", 2,
                               BuildMarkedMessage(kFindSym, FindPayload({1001, 1002, 1003}, Guid(0xd0))));
  EXPECT_TRUE(TestLogContainsAtLevel(EchoVR::LogLevel::Info, "[NEVR.WS] lobby find requested conn=2 mode="));
}

// ---- #1413: the party and the request a session record names ----
//
// A session success or failure says which party the client is in (party_id, party_size, party_leader) and which
// find or join the game sent last on this client (last_request, its age at the answer, request_outstanding: yes only
// for the first answer to that request). Every answer is taken by the request tracker, also one whose record the
// limiter suppresses, so the next record's "outstanding" is the same as it would be with no suppression.
TEST(WsBridgeLobbyFrameLog, ASessionSuccessNamesThePartyAndTheFindItAnswers) {
  using namespace lobby_frame_test;
  ClientRun run;
  g_fakePartyView = PartyView(7, 3, 100, 100);
  g_fakeNowMs = 1000;
  TestHook_ObserveSocialFrames("game->server", 2, BuildMarkedMessage(kFindSym, FindPayload({1001}, Guid(0xd0))));
  g_fakeNowMs = 1250;
  TestHook_ObserveSocialFrames("server->game", 3, BuildMarkedMessage(kSuccessSym, SuccessPayload()));
  EXPECT_TRUE(TestLogContainsAtLevel(
      EchoVR::LogLevel::Info,
      "[NEVR.WS] lobby session success conn=3 lobby=44332211-6655-8877-99AA-BBCCDDEEFFC0 "
      "group=44332211-6655-8877-99AA-BBCCDDEEFFB0 party_id=7 party_size=3 party_leader=yes "
      "last_request=find last_request_age_ms=250 request_outstanding=yes"));
}

TEST(WsBridgeLobbyFrameLog, ASessionFailureOutsideAPartyWithNoRequestSaysSo) {
  using namespace lobby_frame_test;
  ClientRun run;
  TestHook_ObserveSocialFrames("server->game", 3, BuildMarkedMessage(kFailureSym, FailureWithMessage(8, "")));
  EXPECT_TRUE(TestLogContainsAtLevel(
      EchoVR::LogLevel::Info,
      "[NEVR.WS] lobby session failure conn=3 code=8 reason=lobby_full group=44332211-6655-8877-99AA-BBCCDDEEFFB0 "
      "message_bytes=0 party_id=none party_size=0 party_leader=no last_request=none last_request_age_ms=none "
      "request_outstanding=no"));
}

TEST(WsBridgeLobbyFrameLog, ThePartyLeaderIsUnknownWhileTheLocalAccountIsNotKnown) {
  using namespace lobby_frame_test;
  ClientRun run;
  g_fakePartyView = PartyView(7, 1, 100, 0);
  TestHook_ObserveSocialFrames("server->game", 3, BuildMarkedMessage(kSuccessSym, SuccessPayload()));
  EXPECT_TRUE(TestLogContainsAtLevel(EchoVR::LogLevel::Info, "party_id=7 party_size=1 party_leader=unknown "));
}

TEST(WsBridgeLobbyFrameLog, AMemberOfAPartyLedByAnotherAccountIsNotItsLeader) {
  using namespace lobby_frame_test;
  ClientRun run;
  g_fakePartyView = PartyView(7, 2, 77, 100);
  TestHook_ObserveSocialFrames("server->game", 3, BuildMarkedMessage(kFailureSym, FailureWithMessage(8, "")));
  EXPECT_TRUE(TestLogContainsAtLevel(EchoVR::LogLevel::Info,
                                     "party_id=7 party_size=2 party_leader=no last_request=none "));
}

TEST(WsBridgeLobbyFrameLog, OnlyTheFirstAnswerToARequestIsOutstanding) {
  using namespace lobby_frame_test;
  ClientRun run;
  g_fakePartyView = PartyView(7, 2, 77, 100);
  g_fakeNowMs = 1000;
  TestHook_ObserveSocialFrames("game->server", 3, BuildMarkedMessage(kJoinSym, JoinPayload({1001})));
  g_fakeNowMs = 1100;
  TestHook_ObserveSocialFrames("server->game", 3, BuildMarkedMessage(kSuccessSym, SuccessPayload()));
  EXPECT_TRUE(TestLogContainsAtLevel(EchoVR::LogLevel::Info,
                                     "last_request=join last_request_age_ms=100 request_outstanding=yes"));
  ClearTestLogs();
  g_fakeNowMs = 1200;
  TestHook_ObserveSocialFrames("server->game", 3, BuildMarkedMessage(kFailureSym, FailureWithMessage(8, "")));
  EXPECT_TRUE(TestLogContainsAtLevel(
      EchoVR::LogLevel::Info,
      "lobby session failure conn=3 code=8 reason=lobby_full group=44332211-6655-8877-99AA-BBCCDDEEFFB0 "
      "message_bytes=0 party_id=7 party_size=2 party_leader=no last_request=join last_request_age_ms=200 "
      "request_outstanding=no"));
}

// The success window holds ten records; the eleventh answer is suppressed, and it still takes the outstanding request.
// The next written record shows the request as already answered, and the window's count is written before it.
TEST(WsBridgeLobbyFrameLog, ASuppressedAnswerStillTakesTheRequestItAnswers) {
  using namespace lobby_frame_test;
  ClientRun run;
  g_fakePartyView = PartyView(7, 2, 77, 100);
  g_fakeNowMs = 1000;
  for (int i = 0; i < 10; ++i) {
    TestHook_ObserveSocialFrames("server->game", 3, BuildMarkedMessage(kSuccessSym, SuccessPayload()));
  }
  EXPECT_EQ(CountLogLines("lobby session success"), 10U);
  g_fakeNowMs = 1100;
  TestHook_ObserveSocialFrames("game->server", 3, BuildMarkedMessage(kJoinSym, JoinPayload({1001})));
  g_fakeNowMs = 1200;
  TestHook_ObserveSocialFrames("server->game", 3, BuildMarkedMessage(kSuccessSym, SuccessPayload()));
  EXPECT_EQ(CountLogLines("lobby session success"), 10U) << "the eleventh answer is suppressed";
  g_fakeNowMs = 11200;
  ClearTestLogs();
  TestHook_ObserveSocialFrames("server->game", 3, BuildMarkedMessage(kSuccessSym, SuccessPayload()));
  EXPECT_TRUE(TestLogContainsAtLevel(EchoVR::LogLevel::Info,
                                     "[NEVR.WS] lobby frame records suppressed kind=success count=1 window_ms=10000"));
  EXPECT_TRUE(TestLogContainsAtLevel(
      EchoVR::LogLevel::Info,
      "lobby session success conn=3 lobby=44332211-6655-8877-99AA-BBCCDDEEFFC0 "
      "group=44332211-6655-8877-99AA-BBCCDDEEFFB0 party_id=7 party_size=2 party_leader=no "
      "last_request=join last_request_age_ms=10100 request_outstanding=no"));
}

TEST(WsBridgeLobbyFrameLog, ARequestFromAnEarlierRunIsNotNamedInANewRun) {
  using namespace lobby_frame_test;
  {
    ClientRun earlier;
    TestHook_ObserveSocialFrames("game->server", 2, BuildMarkedMessage(kFindSym, FindPayload({1001}, Guid(0xd0))));
  }
  ClientRun run;
  TestHook_ObserveSocialFrames("server->game", 3, BuildMarkedMessage(kSuccessSym, SuccessPayload()));
  EXPECT_TRUE(TestLogContainsAtLevel(EchoVR::LogLevel::Info,
                                     "last_request=none last_request_age_ms=none request_outstanding=no"));
}

// The party read when no test has set a view is the process-wide party state (TestHook_SetLobbyPartySource(nullptr)).
TEST(WsBridgeLobbyFrameLog, TheLocalPartyStateIsReadWhenTheAnswerIsWritten) {
  using namespace lobby_frame_test;
  ClientRun run;
  TestHook_SetLobbyPartySource(nullptr);
  nevr_social_party::State& state = nevr_social_party::Global();
  state.SetSelf(4001);
  std::string created;
  PutLe(created, 42, 8);    // party id
  PutLe(created, 4001, 8);  // owner: the local account
  EXPECT_TRUE(state.Feed(nevr_social_party::ReplySymbol("PartyCreateSuccess"),
                         reinterpret_cast<const uint8_t*>(created.data()), created.size(), 1000, nullptr));
  TestHook_ObserveSocialFrames("server->game", 3, BuildMarkedMessage(kSuccessSym, SuccessPayload()));
  EXPECT_TRUE(TestLogContainsAtLevel(EchoVR::LogLevel::Info,
                                     "party_id=42 party_size=1 party_leader=yes last_request=none"));
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

// #1280: a dedicated server is not a player and declares no social level, so the service sends it none of the
// newer social messages; a client declares the level it parses.
TEST(WsBridgeLoginRequest, AServerDeclaresNoSocialLevelAndAClientDeclaresItsOwn) {
  const auto socialLevel = [](BOOL isServer) {
    const BOOL wasServer = g_isServer;
    g_isServer = isServer;
    const std::string request = TestHook_BuildLoginRequest(77, 4, "name", "");
    g_isServer = wasServer;
    const size_t jsonOffset = 56;
    if (request.size() <= jsonOffset) return -1;
    return nlohmann::json::parse(request.substr(jsonOffset, request.size() - jsonOffset - 1)).at("nevr_social").get<int>();
  };
  EXPECT_EQ(socialLevel(TRUE), 0);
  EXPECT_EQ(socialLevel(FALSE), nevr_social_party::kSocialLevel);
  EXPECT_EQ(nevr_social_party::kSocialLevel, 1);
}

TEST(WsBridgeLoginRequest, JsonCarriesIdentityCredentialsAndMeasuredSystemInfo) {
  const BOOL wasServer = g_isServer;
  g_isServer = FALSE;  // a client's login (this file's default is a server)
  const std::string request = TestHook_BuildLoginRequest(77, 3, "A \"quoted\" name", "access-token");
  g_isServer = wasServer;
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

// #432: the assets a plugin carries are reported beside the plugin list so the game service knows which levels
// this game client can load. Absent when there are none, so a plugin-less login is byte-identical to before.
TEST(LoginProfile, NoAssetsMeansNoAssetsKey) {
  nevr_login_profile::LoginProfileInputs inputs;
  inputs.account_id = 90210;
  const nlohmann::json profile = nlohmann::json::parse(nevr_login_profile::BuildLoginProfileJson(inputs));
  EXPECT_FALSE(profile.contains("nevr_assets"));
}

TEST(LoginProfile, AssetsAreCarriedAsAnArrayNextToThePluginList) {
  nevr_login_profile::LoginProfileInputs inputs;
  inputs.account_id = 90210;
  inputs.plugins = nlohmann::json::parse(R"([{"name":"maps","loaded":true}])");
  inputs.assets = nlohmann::json::parse(
      R"([{"type":"0xa71a14664bd4e047","name":"0x1111222233334444","label":"level_a","bytes":64}])");
  const nlohmann::json profile = nlohmann::json::parse(nevr_login_profile::BuildLoginProfileJson(inputs));
  ASSERT_TRUE(profile.contains("nevr_assets"));
  ASSERT_TRUE(profile.at("nevr_assets").is_array());
  EXPECT_EQ(profile.at("nevr_assets"), inputs.assets);
  EXPECT_EQ(profile.at("nevr_plugins"), inputs.plugins);
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

// SelectPlatformCode: the bridge logs in as platform 4 (OVR_ORG), the provider it forces into the game's own
// CNSUser. A login as platform 6 (-noovr) made Nakama answer the game's later LobbyPlayerSessionsRequest (sent
// as OVR-ORG) with "requesting player not found in match", so the game never reached a lobby host: the platform
// the login carries and the CNSUser nibble must be one value. The one exception is a client the game marks No VR
// (#1082): it logs in as 7 (DMO), and PatchCnsUserLoginState and the provider-prefix detour use the same 7.
TEST(WsBridgeSelectPlatform, OvrOrgForAVrClientToMatchTheGamesOwnIdentity) {
  EXPECT_EQ(TestHook_SelectPlatformCode(true, false), 4ULL);
  EXPECT_EQ(TestHook_SelectPlatformCode(false, false), 4ULL);
  // The code the bridge logs in as is the one labelled OVR-ORG, the provider the game names itself with.
  EXPECT_STREQ(TestHook_PlatformPrefix(TestHook_SelectPlatformCode(false, false)), "OVR-ORG");
}

TEST(WsBridgeSelectPlatform, DmoForAClientTheGameMarksNoVr) {
  EXPECT_EQ(TestHook_SelectPlatformCode(true, true), 7ULL);
  EXPECT_EQ(TestHook_SelectPlatformCode(false, true), 7ULL);
  EXPECT_STREQ(TestHook_PlatformPrefix(TestHook_SelectPlatformCode(false, true)), "DMO");
  // Through the bridge's own login builder: the NoVR frame carries platform 7 and the same account as the VR one.
  const std::string vrRequest = TestHook_BuildLoginRequest(999888777ULL, TestHook_SelectPlatformCode(false, false), "Test", "");
  const std::string noVrRequest = TestHook_BuildLoginRequest(999888777ULL, TestHook_SelectPlatformCode(false, true), "Test", "");
  ASSERT_GE(noVrRequest.size(), 56U);
  EXPECT_EQ(ReadLe64(vrRequest, 40), 4ULL);
  EXPECT_EQ(ReadLe64(noVrRequest, 40), 7ULL);
  EXPECT_EQ(ReadLe64(noVrRequest, 48), ReadLe64(vrRequest, 48));
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

// #800: the wrapper's hook belongs to the host. The runtime records the bytes the host read after it
// enabled its hook (the host's jump), not the bytes it finds when it gets round to recording, so a
// re-hook between the host's enable and the runtime's init is a mismatch from the first VerifyAll.
TEST(N84_HookGuard, RecordBytes_HostBytesMatchingTheTargetIsNoMismatch) {
    nevr_hook_guard::ResetForTest();
    GuardScratch s;
    ASSERT_NE(s.page, nullptr);
    unsigned char hostBytes[16];
    memset(hostBytes, 0x90, sizeof(hostBytes));
    hostBytes[0] = 0xE9;
    memcpy(s.page, hostBytes, sizeof(hostBytes));

    nevr_hook_guard::RecordBytes(s.page, "host_hook", hostBytes);
    EXPECT_EQ(nevr_hook_guard::RecordedCount(), 1);
    EXPECT_EQ(nevr_hook_guard::VerifyAll("test:host-bytes"), 0);
    EXPECT_TRUE(nevr_hook_guard::IsOurDetour(s.page));
}

TEST(N84_HookGuard, RecordBytes_HostBytesDifferingFromTheTargetIsAMismatch) {
    nevr_hook_guard::ResetForTest();
    GuardScratch s;
    ASSERT_NE(s.page, nullptr);
    unsigned char hostBytes[16];
    memset(hostBytes, 0x90, sizeof(hostBytes));
    hostBytes[0] = 0xE9;
    memcpy(s.page, hostBytes, sizeof(hostBytes));
    static_cast<unsigned char*>(s.page)[1] = 0x42;  // another hooker wrote over the host's jump

    nevr_hook_guard::RecordBytes(s.page, "host_hook_rehooked", hostBytes);
    EXPECT_EQ(nevr_hook_guard::VerifyAll("test:host-bytes-rehooked"), 1);
    EXPECT_FALSE(nevr_hook_guard::IsOurDetour(s.page));
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

// The table holds every install site the runtime has. 63 is the count of sites in src/ that reach
// nevr_hook_guard::Record (counted in hook_guard.cpp, next to kMaxGuarded); a capacity below it drops
// guards on a healthy boot. A new site raises that count and this number with it.
TEST(N84_HookGuard, CapacityCoversEveryInstallSiteTheRuntimeHas) {
    constexpr int kInstallSitesInSource = 65;
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

// The text the menu draws (#747): "nEVR " and the project version exactly as CMake set it, so a screenshot names the build.
TEST(BuildIdentity, MenuVersionLabelIsNevrAndTheProjectVersion) {
    const nevr_build_identity::Info& id = nevr_build_identity::Get();
    EXPECT_EQ(nevr_build_identity::MenuVersionLabel(), "nEVR " + id.project_version);
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
        ResetServerFatal();
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
        EXPECT_EQ(ServerFatalCalls(), 0);
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
        EXPECT_EQ(ServerFatalCalls(), 0)
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
        EXPECT_EQ(ServerFatalCalls(), 0);
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
        EXPECT_EQ(ServerFatalCalls(), 0)
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
    memcpy(base + nevr_patch_addresses::PROVIDER_STRING_DMO, "DMO", 4);
    {
        XpidBaseScope scope(base);
        PatchProviderPrefixOvrOrg();
        EXPECT_FALSE(g_bootHookFailed) << "a working detour must not mark the boot failed";
        EXPECT_STREQ(g_bootHookFailedCause, "");
        using PrefixFn = const char* (*)(uint32_t*);
        uint32_t provider = 2;  // PSN: the detour answers OVR-ORG whatever the game asked for
        const char* prefix = reinterpret_cast<PrefixFn>(base + nevr_patch_addresses::GET_PROVIDER_PREFIX)(&provider);
        EXPECT_STREQ(prefix, "OVR-ORG");
        // #1082: the nibble 7 a No VR login sets answers the game's own DMO entry; the other bits of the
        // login-state word are not part of the provider.
        provider = 7;
        EXPECT_STREQ(reinterpret_cast<PrefixFn>(base + nevr_patch_addresses::GET_PROVIDER_PREFIX)(&provider), "DMO");
        provider = 0x17;
        EXPECT_STREQ(reinterpret_cast<PrefixFn>(base + nevr_patch_addresses::GET_PROVIDER_PREFIX)(&provider), "DMO");
        provider = 4;
        EXPECT_STREQ(reinterpret_cast<PrefixFn>(base + nevr_patch_addresses::GET_PROVIDER_PREFIX)(&provider), "OVR-ORG");
        provider = 0x14;
        EXPECT_STREQ(reinterpret_cast<PrefixFn>(base + nevr_patch_addresses::GET_PROVIDER_PREFIX)(&provider), "OVR-ORG");
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

// ============================================================================
// #1254: what a refused login does. A server with no credential, or one the service refuses, stops through
// ServerFatal with the cause and never sends account 0 or connects unauthenticated; a client says a refusal
// once and does not go back to the service for the same sign-in. compat/login_refusal.h holds the decisions
// (test_login_refusal); these drive them through the real bridge, the legacy rules and the router.
// ============================================================================

namespace {

// A community service that refuses every upgrade with `status`, and counts the requests it refused. ix::HttpServer
// cannot stand in: it hands a websocket upgrade to its websocket path, which accepts it.
class RefusingServer final : public ix::SocketServer {
 public:
  RefusingServer(int port, int status, std::atomic<int>& requests)
      : ix::SocketServer(port, "127.0.0.1"), status_(status), requests_(requests) {}
  ~RefusingServer() override { stop(); }

 private:
  void handleConnection(std::unique_ptr<ix::Socket> socket, std::shared_ptr<ix::ConnectionState> state) override {
    const auto never = [] { return false; };
    bool first = true;
    for (;;) {  // the request head, up to its blank line
      const auto [ok, line] = socket->readLine(never);
      if (!ok) break;
      if (first) {
        ++requests_;
        first = false;
      }
      if (line == "\r\n" || line == "\n") break;
    }
    socket->writeBytes("HTTP/1.1 " + std::to_string(status_) +
                           " Refused\r\nContent-Length: 0\r\nConnection: close\r\n\r\n",
                       never);
    socket->close();
    state->setTerminated();
  }
  size_t getConnectedClientsCount() override { return 0; }

  int status_;
  std::atomic<int>& requests_;
};

class RefusingService {
 public:
  bool Start(int status) {
    std::random_device device;
    std::mt19937 generator(device());
    std::uniform_int_distribution<int> ports(49152, 65535);
    for (int attempt = 0; attempt < 20; ++attempt) {
      const int port = ports(generator);
      server_ = std::make_unique<RefusingServer>(port, status, requests_);
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
  std::string Uri() const { return "ws://127.0.0.1:" + std::to_string(port_) + "/"; }
  int Requests() const { return requests_.load(); }

 private:
  std::atomic<int> requests_{0};
  std::unique_ptr<RefusingServer> server_;
  int port_ = 0;
};

class RefusedLoginTest : public ::testing::Test {
 protected:
  virtual const char* RouterMode() const { return nullptr; }
  virtual bool ServerRun() const { return true; }
  void SetUp() override {
    ix::initNetSystem();
    TestHook_N61_ResetState();
    ClearTestLogs();
    g_testRouterMode = RouterMode();
    wasServer_ = g_isServer;
    g_isServer = ServerRun() ? TRUE : FALSE;
    ResetServerFatal();
    LastServerFatal().clear();
    g_testDiscordId = nullptr;
    g_testPassword = nullptr;
  }
  void TearDown() override {
    if (HasFailure()) {
      // The bridge's own lines are the evidence of a failed run (they are otherwise swallowed by the Log stub).
      std::lock_guard<std::mutex> lock(g_testLogMutex);
      for (const std::string& line : g_testLogMessages) fprintf(stderr, "[bridge log] %s\n", line.c_str());
    }
    EXPECT_TRUE(YieldUntil([] { return TestHook_ConnectedGameSockets() == 0; }));
    StopWebSocketBridgeListener();
    TestHook_SetRouterGate(-1);
    TestHook_SetRefuseRemoteOpens(false);
    g_testRouterMode = nullptr;
    g_testDiscordId = nullptr;
    g_testPassword = nullptr;
    g_isServer = wasServer_;
    TestHook_N61_ResetState();
    SetWebSocketBridgeTarget("");
    refusing_.Stop();
    if (accepting_started_) {
      EXPECT_TRUE(accepting_.WaitForNoClients());
      accepting_.Stop();
    }
  }
  void StartBridgeTo(const std::string& remoteUri) {
    SetWebSocketBridgeTarget(remoteUri.c_str());
    InstallWebSocketBridge();
    port_ = GetWebSocketBridgePort();
    ASSERT_NE(port_, 0);
  }
  void StartRefusing(int status) {
    ASSERT_TRUE(refusing_.Start(status));
    StartBridgeTo(refusing_.Uri());
  }
  void StartAccepting() {
    ASSERT_TRUE(accepting_.Start());
    accepting_started_ = true;
    StartBridgeTo(accepting_.Uri());
  }
  void GiveTheServerACredential() {
    g_testDiscordId = kTestAccountIdText.c_str();
    g_testPassword = "test-password";
  }
  static bool WaitForServerFatal() { return YieldUntil([] { return ServerFatalCalls() >= 1; }); }

  RemoteService accepting_;
  RefusingService refusing_;
  bool accepting_started_ = false;
  BOOL wasServer_ = TRUE;
  uint16_t port_ = 0;
};

class RefusedLoginRouterTest : public RefusedLoginTest {
 protected:
  const char* RouterMode() const override { return "all"; }
};

class RefusedLoginClientTest : public RefusedLoginTest {
 protected:
  bool ServerRun() const override { return false; }
};

// The token the bridge reads as the client's sign-in (TokenAuth_GetToken, which token-auth registers in production).
const char* g_testToken = nullptr;
const char* TestTokenFn() { return g_testToken; }

class RefusedLoginRouterClientTest : public RefusedLoginTest {
 protected:
  const char* RouterMode() const override { return "all"; }
  bool ServerRun() const override { return false; }
};

// A server with no credential: it stops with the cause before any connection to the service, and never sends
// the LoginRequest (account 0, unauthenticated) that it used to.
void ExpectNoCredentialServerStops(RemoteService& service, uint16_t port) {
  GameSocket login;
  ASSERT_TRUE(login.Connect(port, "/login"));
  ASSERT_TRUE(YieldUntil([] { return ServerFatalCalls() >= 1; })) << "the server did not stop";
  EXPECT_EQ(ServerFatalCalls(), 1) << LastServerFatal();
  EXPECT_NE(LastServerFatal().find("server login refused"), std::string::npos) << LastServerFatal();
  EXPECT_NE(LastServerFatal().find("identity.discord_id"), std::string::npos) << LastServerFatal();
  EXPECT_NE(LastServerFatal().find("auth.password"), std::string::npos) << LastServerFatal();
  EXPECT_NE(LastServerFatal().find("test config"), std::string::npos) << LastServerFatal();
  EXPECT_TRUE(login.WaitForClose()) << "the game's login socket was left open";
  EXPECT_EQ(service.Opens(), 0) << "the server connected to the service without a credential";
  EXPECT_EQ(service.LoginRequests(), 0) << "the server sent a LoginRequest without a credential";
}

// A server whose credential the service refuses: it stops with the status and the config.
void ExpectRefusedServerStops(RefusingService& service, uint16_t port, int status) {
  GameSocket login;
  ASSERT_TRUE(login.Connect(port, "/login"));
  ASSERT_TRUE(YieldUntil([] { return ServerFatalCalls() >= 1; })) << "the server did not stop";
  EXPECT_EQ(ServerFatalCalls(), 1) << LastServerFatal();
  EXPECT_NE(LastServerFatal().find("server login refused"), std::string::npos) << LastServerFatal();
  EXPECT_NE(LastServerFatal().find("HTTP " + std::to_string(status)), std::string::npos) << LastServerFatal();
  EXPECT_NE(LastServerFatal().find("test config"), std::string::npos) << LastServerFatal();
  EXPECT_GE(service.Requests(), 1);
}

// A client says the refusal once, and the next login socket of the same sign-in does not go back to the service.
void ExpectClientSaysRefusalOnce(RefusingService& service, uint16_t port) {
  GameSocket first;
  ASSERT_TRUE(first.Connect(port, "/login"));
  ASSERT_TRUE(first.WaitForClose()) << "the refused login socket was left open";
  ASSERT_TRUE(WaitForLogLines("login refused conn=", 1));
  EXPECT_EQ(service.Requests(), 1);
  GameSocket second;
  ASSERT_TRUE(second.Connect(port, "/login"));
  ASSERT_TRUE(second.WaitForClose()) << "the next login socket was left open";
  EXPECT_EQ(service.Requests(), 1) << "the client went back to the service with a sign-in it was refused for";
  EXPECT_EQ(LogLinesContaining("login refused conn=").size(), 1u) << "the refusal was said more than once";
  EXPECT_EQ(ServerFatalCalls(), 0) << "a client must not stop";
}

}  // namespace

TEST_F(RefusedLoginTest, AServerWithNoCredentialStopsWithTheCauseAndNeverConnects) {
  StartAccepting();
  ExpectNoCredentialServerStops(accepting_, port_);
}

TEST_F(RefusedLoginRouterTest, AServerWithNoCredentialStopsWithTheCauseAndNeverConnects) {
  StartAccepting();
  ExpectNoCredentialServerStops(accepting_, port_);
}

// #1532: a discord id without a password is no longer a missing key: the server signs in by device code, and with no
// token from it the stop names the sign-in, not a key.
TEST_F(RefusedLoginTest, AServerWithOnlyADiscordIdAndNoSignInStopsAsAServerWithNoSignIn) {
  StartAccepting();
  g_testDiscordId = kTestAccountIdText.c_str();
  GameSocket login;
  ASSERT_TRUE(login.Connect(port_, "/login"));
  ASSERT_TRUE(YieldUntil([] { return ServerFatalCalls() >= 1; }));
  EXPECT_NE(LastServerFatal().find("device sign-in ended without a token"), std::string::npos) << LastServerFatal();
  EXPECT_EQ(LastServerFatal().find("missing auth.password"), std::string::npos) << LastServerFatal();
  EXPECT_EQ(accepting_.Opens(), 0);
}

// The password path keeps its rule: a password with no discord id names the discord id.
TEST_F(RefusedLoginTest, AServerWithAPasswordAndNoDiscordIdNamesJustTheDiscordId) {
  StartAccepting();
  g_testPassword = "test-password";
  GameSocket login;
  ASSERT_TRUE(login.Connect(port_, "/login"));
  ASSERT_TRUE(YieldUntil([] { return ServerFatalCalls() >= 1; }));
  EXPECT_NE(LastServerFatal().find("missing identity.discord_id)"), std::string::npos) << LastServerFatal();
  EXPECT_EQ(LastServerFatal().find("missing identity.discord_id, auth.password"), std::string::npos)
      << LastServerFatal();
  EXPECT_EQ(accepting_.Opens(), 0);
}

// #1532: no password, but the device sign-in produced a token: the server presents it like a client and is not stopped.
void ExpectDeviceSignInServerConnects(RemoteService& service, uint16_t port) {
  RegisterModuleProc("TokenAuth_GetToken", reinterpret_cast<void*>(&TestTokenFn));
  g_testToken = "device-sign-in-token";
  GameSocket login;
  ASSERT_TRUE(login.Connect(port, "/login"));
  ASSERT_TRUE(login.WaitForSymbol(nevr_evr_codec::kSymLoginSettings));
  EXPECT_EQ(ServerFatalCalls(), 0) << LastServerFatal();
  EXPECT_EQ(service.LoginRequests(), 1);
  g_testToken = nullptr;
}

TEST_F(RefusedLoginTest, AServerWithNoPasswordAndADeviceTokenConnectsAndIsNotStopped) {
  StartAccepting();
  ExpectDeviceSignInServerConnects(accepting_, port_);
}

TEST_F(RefusedLoginRouterTest, AServerWithNoPasswordAndADeviceTokenConnectsAndIsNotStopped) {
  StartAccepting();
  ExpectDeviceSignInServerConnects(accepting_, port_);
}

// A device token the service refuses stops the server with a cause that does not send the operator to the password keys.
TEST_F(RefusedLoginTest, AServerWhoseDeviceTokenTheServiceRefusesStopsWithoutBlamingThePasswordKeys) {
  RegisterModuleProc("TokenAuth_GetToken", reinterpret_cast<void*>(&TestTokenFn));
  g_testToken = "device-sign-in-token";
  StartRefusing(401);
  GameSocket login;
  ASSERT_TRUE(login.Connect(port_, "/login"));
  ASSERT_TRUE(YieldUntil([] { return ServerFatalCalls() >= 1; })) << "the server did not stop";
  EXPECT_NE(LastServerFatal().find("device sign-in token"), std::string::npos) << LastServerFatal();
  EXPECT_NE(LastServerFatal().find("HTTP 401"), std::string::npos) << LastServerFatal();
  EXPECT_EQ(LastServerFatal().find("check identity.discord_id"), std::string::npos) << LastServerFatal();
  g_testToken = nullptr;
}

TEST_F(RefusedLoginTest, AServerWithACredentialConnectsAndIsNotStopped) {
  StartAccepting();
  GiveTheServerACredential();
  GameSocket login;
  ASSERT_TRUE(login.Connect(port_, "/login"));
  ASSERT_TRUE(login.WaitForSymbol(nevr_evr_codec::kSymLoginSettings));
  EXPECT_EQ(ServerFatalCalls(), 0) << LastServerFatal();
  EXPECT_EQ(accepting_.LoginRequests(), 1);
}

TEST_F(RefusedLoginClientTest, AClientWithNoCredentialIsNotStoppedByTheServerRule) {
  StartAccepting();
  GameSocket login;
  ASSERT_TRUE(login.Connect(port_, "/login"));
  ASSERT_TRUE(YieldUntil([&] { return accepting_.Opens() >= 1; })) << "a client was kept from connecting";
  EXPECT_EQ(ServerFatalCalls(), 0) << LastServerFatal();
}

TEST_F(RefusedLoginTest, AServerWhoseCredentialTheServiceRefusesStopsWithTheStatus) {
  GiveTheServerACredential();
  StartRefusing(401);
  ExpectRefusedServerStops(refusing_, port_, 401);
}

TEST_F(RefusedLoginRouterTest, AServerWhoseCredentialTheServiceRefusesStopsWithTheStatus) {
  GiveTheServerACredential();
  StartRefusing(403);
  ExpectRefusedServerStops(refusing_, port_, 403);
}

TEST_F(RefusedLoginTest, AnOutageIsNotARefusalAndDoesNotStopTheServer) {
  GiveTheServerACredential();
  StartRefusing(503);
  GameSocket login;
  ASSERT_TRUE(login.Connect(port_, "/login"));
  ASSERT_TRUE(login.WaitForClose());
  EXPECT_GE(refusing_.Requests(), 1);
  EXPECT_EQ(ServerFatalCalls(), 0) << LastServerFatal();
}

TEST_F(RefusedLoginClientTest, AClientSaysARefusalOnceAndDoesNotGoBackForTheSameSignIn) {
  StartRefusing(401);
  ExpectClientSaysRefusalOnce(refusing_, port_);
}

// A refusal is held against the sign-in that was refused, not against the client: another sign-in is tried.
void ExpectADifferentSignInIsTriedAgain(RefusingService& service, uint16_t port) {
  RegisterModuleProc("TokenAuth_GetToken", reinterpret_cast<void*>(&TestTokenFn));
  g_testToken = "sign-in-A";
  GameSocket first;
  ASSERT_TRUE(first.Connect(port, "/login"));
  ASSERT_TRUE(first.WaitForClose());
  ASSERT_TRUE(WaitForLogLines("login refused conn=", 1));
  g_testToken = "sign-in-B";
  GameSocket second;
  ASSERT_TRUE(second.Connect(port, "/login"));
  ASSERT_TRUE(second.WaitForClose());
  ASSERT_TRUE(WaitForLogLines("login refused conn=", 2)) << "a new sign-in's refusal was not said";
  EXPECT_EQ(service.Requests(), 2) << "a sign-in that was not refused before was not tried";
  g_testToken = nullptr;
}

TEST_F(RefusedLoginClientTest, ADifferentSignInAfterARefusalIsTriedAgainAndSaidAgain) {
  StartRefusing(401);
  ExpectADifferentSignInIsTriedAgain(refusing_, port_);
}

TEST_F(RefusedLoginRouterClientTest, ADifferentSignInAfterARefusalIsTriedAgainAndSaidAgain) {
  StartRefusing(401);
  ExpectADifferentSignInIsTriedAgain(refusing_, port_);
}

TEST_F(RefusedLoginClientTest, ALoginAfterARefusedOneIsStillALoginNotAShareOfNothing) {
  StartRefusing(401);
  GameSocket first;
  ASSERT_TRUE(first.Connect(port_, "/login"));
  ASSERT_TRUE(first.WaitForClose());
  ASSERT_TRUE(WaitForLogLines("login refused conn=", 1));
  GameSocket second;
  ASSERT_TRUE(second.Connect(port_, "/login"));
  ASSERT_TRUE(second.WaitForClose());
  // The second socket is refused without a trip to the service; the slot it took must go back too, or the third
  // /login is taken for a share of a login session that does not exist.
  GameSocket third;
  ASSERT_TRUE(third.Connect(port_, "/login"));
  ASSERT_TRUE(third.WaitForClose());
  const std::vector<std::string> admitted = LogLinesContaining("game socket admitted");
  ASSERT_EQ(admitted.size(), 3u);
  for (size_t i = 1; i < admitted.size(); ++i) {
    EXPECT_NE(admitted[i].find("role=login"), std::string::npos) << admitted[i];
    EXPECT_EQ(admitted[i].find("login-claimed"), std::string::npos) << admitted[i];
  }
}

TEST_F(RefusedLoginRouterClientTest, AClientSaysARefusalOnceAndDoesNotGoBackForTheSameSignIn) {
  StartRefusing(401);
  ExpectClientSaysRefusalOnce(refusing_, port_);
}

// #1446: once a client's sign-in has ended without a token, nothing the service says can change it, so the bridge
// answers the game's login itself: the game gets a LoginFailure, the service is not asked and the socket stays open.
namespace {
bool g_testSignInEndedWithoutToken = false;
bool TestSignInEndedWithoutToken() { return g_testSignInEndedWithoutToken; }

// The sign-in state for one test. Cleared when the test ends, so no later test sees an ended sign-in.
class SignInEndedForTest {
 public:
  SignInEndedForTest() {
    RegisterModuleProc("TokenAuth_SignInEndedWithoutToken", reinterpret_cast<void*>(&TestSignInEndedWithoutToken));
    g_testSignInEndedWithoutToken = true;
  }
  ~SignInEndedForTest() { g_testSignInEndedWithoutToken = false; }
  SignInEndedForTest(const SignInEndedForTest&) = delete;
  SignInEndedForTest& operator=(const SignInEndedForTest&) = delete;
};
}  // namespace

TEST_F(RefusedLoginClientTest, AClientWhoseSignInEndedWithoutATokenGetsALoginFailureAndTheServiceIsNotAsked) {
  StartRefusing(401);
  SignInEndedForTest ended;
  GameSocket login;
  ASSERT_TRUE(login.Connect(port_, "/login"));
  ASSERT_TRUE(login.WaitForFrames(1)) << "the game got no answer to its login";
  const std::optional<nevr_evr_codec::LoginFailure> failure =
      nevr_evr_codec::ParseLoginFailure(login.FrameWith(nevr_evr_codec::kSymLoginFailure));
  ASSERT_TRUE(failure.has_value()) << "the answer is not a LoginFailure";
  EXPECT_EQ(failure->statusCode, 7u);
  EXPECT_EQ(refusing_.Requests(), 0) << "the service was asked about a sign-in that had ended";
  std::this_thread::sleep_for(std::chrono::milliseconds(1500));
  EXPECT_FALSE(login.Closed()) << "the bridge closed the login socket, and the game's reconnects are the loop";
}

TEST_F(RefusedLoginClientTest, ALaterLoginAfterAnEndedSignInIsAnsweredTheSameWayAndSaidOnce) {
  StartRefusing(401);
  SignInEndedForTest ended;
  for (int attempt = 0; attempt < 3; ++attempt) {
    GameSocket login;
    ASSERT_TRUE(login.Connect(port_, "/login")) << "attempt " << attempt;
    ASSERT_TRUE(login.WaitForFrames(1)) << "attempt " << attempt;
  }
  EXPECT_EQ(refusing_.Requests(), 0);
  EXPECT_EQ(LogLinesContaining("login answered locally").size(), 1u) << "the decision was said more than once";
  EXPECT_EQ(LogLinesContaining("login attempts answered locally").size(), 0u)
      << "a later attempt inside the window is counted, not written";
  EXPECT_LE(LogLinesContaining("No discord ID").size(), 1u) << "the discord id line repeats on every attempt";
  const std::vector<std::string> admitted = LogLinesContaining("game socket admitted");
  ASSERT_EQ(admitted.size(), 3u);
  for (const std::string& line : admitted) {
    EXPECT_NE(line.find("role=login"), std::string::npos) << line;
    EXPECT_EQ(line.find("login-claimed"), std::string::npos) << line;
  }
}

TEST_F(RefusedLoginClientTest, AConfigSocketIsNotAnsweredHereAndStillGoesToTheService) {
  StartRefusing(401);
  SignInEndedForTest ended;
  GameSocket config;
  ASSERT_TRUE(config.Connect(port_, "/config"));
  ASSERT_TRUE(config.WaitForClose()) << "the refused config socket was left open";
  EXPECT_EQ(refusing_.Requests(), 1) << "the config socket was not sent to the service";
}
