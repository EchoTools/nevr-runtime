#include "runtime/lifecycle/boot.h"
#include "runtime/lifecycle/bridge_policy.h"
#include "runtime/lifecycle/cli.h"
#include "runtime/lifecycle/config.h"
#include "runtime/lifecycle/service_config.h"  // NevrCfgGetFlat (N133 S4a: config.yaml reads)
#include "runtime/lifecycle/crash_recovery.h"
#include "runtime/lifecycle/game_data_notice.h"
#include "runtime/server/installed_levels.h"
#include "runtime/lifecycle/return_to_lobby.h"
#include "runtime/lifecycle/return_to_lobby_hold.h"
#include "runtime/lifecycle/initialize.h"
#include "runtime/lifecycle/state_machine.h"
#include "runtime/patch/mode_patches.h"
#ifdef NEVR_SCENARIO_CONTROL
#include "runtime/scenario/scenario_control.h"
#endif
#include "runtime/patch/xpid_patch.h"
#include "runtime/patch/menu_friends_pack.h"
#include "runtime/patch/menu_friends_avatars.h"
#include "runtime/patch/resource_override.h"
#include "runtime/patch/asset_cdn.h"
#include "runtime/patch/credits_block.h"
#include "runtime/patch/credits_text_install.h"
#include "runtime/patch/invite_choice_hook.h"
#include "runtime/patch/render_config_hook.h"
#include "runtime/patch/invite_config.h"
#include "runtime/patch/invite_service.h"
#include "runtime/patch/menu_cards.h"
#include "runtime/patch/netgun_bullets.h"
#include "runtime/patch/netgun_bullet_ids.h"
#include "runtime/patch/ui_text_writer.h"
#include "runtime/patch/invite_wizard_hook.h"
#include "runtime/patch/arena_public_clock.h"
#include "runtime/patch/coop_ai_trace.h"
#include "runtime/patch/loading_tips.h"
#include "runtime/frame/frame_stats.h"
#include "runtime/patch/server_bots_install.h"
#include "runtime/patch/server_team_size_install.h"
#include "runtime/ext/plugin_loader.h"
#include "runtime/ext/plugin_trust.h"
#include "runtime/ext/self_provenance.h"
#include "extension/module_interface.h"

// Statically-linked module entry points (the modules are linked into
// BugSplat64.dll). Each module's symbols are prefixed to avoid
// collisions — both define NvrModuleInit/NvrModuleApiVersion/etc.
namespace nevr_token_auth {
std::string GetToken();  // token_auth.h: thread-safe; statically linked, so called directly
bool SignInEndedWithoutToken();  // token_auth.h (#1446)
}

extern "C" {
// platform_compat
int platform_compat_Init(const NvrModuleContext* ctx);
uint32_t platform_compat_ApiVersion(void);
void platform_compat_Shutdown(void);
// token_auth
int token_auth_Init(const NvrModuleContext* ctx);
uint32_t token_auth_ApiVersion(void);
void token_auth_Shutdown(void);
const char* TokenAuth_GetToken(void);
uint64_t TokenAuth_GetDiscordId(void);
const char* TokenAuth_GetUsername(void);
}
#include "runtime/ext/module_loader.h"
#include "runtime/compat/ws_bridge.h"
#include "runtime/patch/pnsrad_enabler.h"
#include "runtime/patch/binary_bug_fixes.h"
#include "runtime/hook/addresses.h"
#include "runtime/lifecycle/server_timestep.h"
#include "runtime/lifecycle/cmdline_strip.h"
#include "runtime/lifecycle/cli_check.h"
#include "runtime/lifecycle/cli_reject.h"
#include "runtime/lifecycle/timestep_log.h"
#include "core/build_identity.h"
#include "core/cli_flags.h"
#include "core/cli_option_value.h"
#include "core/globals.h"
#include "core/logging.h"
#include "abi/echovr_functions.h"
#include "extension/module_interface.h"

#include <cstdlib>
#include <exception>
#include <new>
#include <atomic>
#include <ctime>
#include <shellapi.h>

namespace {

// #515: what ApplyFlag did, for LogTimestepArgs.
nevr_server_timestep::FlagRecord g_flagRecord;


// A command-line value as UTF-8, for the paths the config.yaml choice works in.
std::string WideToUtf8(const wchar_t* wide) {
  const int needed = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
  if (needed <= 1) return std::string();
  std::string out(static_cast<std::size_t>(needed), '\0');
  WideCharToMultiByte(CP_UTF8, 0, wide, -1, out.data(), needed, nullptr, nullptr);
  out.resize(static_cast<std::size_t>(needed) - 1);
  return out;
}

// N146: only this minimal state is needed before the original command-line
// preprocessing.  In particular, do not start modules, plugins, file I/O, or
// the bridge here: client D3D initialization is still in the original call.
// The one exception is the plugins config.yaml marks `early: true` (#496), which
// PreprocessCommandLineHook loads right after this: they serve game data the
// original call reads, and must stay off the graphics path.
// -server is needed for the config loader's fail-loud policy. -config is
// needed by LoadLocalConfigHook while the original is running. -config-file and
// -config-path decide which config.yaml the early plugin pass reads (#1255). -require-signed-plugins
// is needed by the early plugin pass (#924): it sets the plugin trust mode every pass and
// the late-load filter read.
void PreflightRuntimeBootstrap() {
  static bool s_done = false;
  if (s_done) return;
  s_done = true;

  // The game reads its arguments from GetCommandLineA() with its own tokenizer, which differs from
  // CommandLineToArgvW; ApplyCommandLine reads the line both ways and a line it cannot read is signed-only.
  nevr_plugin_trust::ApplyCommandLine(GetCommandLineW(), GetCommandLineA());

  int argc = 0;
  LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  if (argv == nullptr) return;

  // #1253: a command line the run cannot use ends it here, before the game, the plugins or the bridge start
  // (cli_check.h). The refusal names the token and why, with the build version, on stderr and in the log.
  nevr_cli_check::Rejection rejection;
  if (nevr_cli_check::Check(argc, argv, &rejection)) {
    RejectCommandLine(WideToUtf8(argv[rejection.index]), WideToUtf8(rejection.why.c_str()));  // does not return
  }

  for (int i = 0; i < argc; ++i) {
    const LPWSTR arg = argv[i];
    if (nevr_cli::IsFlag(arg, L"-server")) {
      g_isServer = TRUE;
      g_isHeadless = TRUE;
      g_noOvr = TRUE;  // -server implies -noovr unconditionally
    } else if (nevr_cli::IsFlag(arg, L"-headless")) {
      // Read here with -server, so g_isHeadless is set before the deferred pass applies the headless gates.
      // The deferred parse logs the token.
      g_isHeadless = TRUE;
    } else if (nevr_cli::IsFlag(arg, L"-windowed")) {
      g_isWindowed = TRUE;
      g_noOvr = TRUE;  // -windowed implies -noovr (no VR headset)
    } else if (nevr_cli::IsFlag(arg, L"-config")) {
      // #587: `-config -server` gives -config no value; -server is still read. The main parse below
      // logs the missing value; this early pass runs before the log exists.
      if (const wchar_t* value = nevr_cli::OptionValue(argc, argv, i)) {
        WideCharToMultiByte(CP_UTF8, 0, value, -1, g_customConfigPath, MAX_PATH, NULL, NULL);
        ++i;
      }
    } else if (nevr_cli::IsFlag(arg, L"-config-file")) {
      // #1255: the one flag that names config.yaml. A flag with no value is kept as given, with an empty value, and
      // the choice below refuses it.
      g_configYamlFlags.config_file_given = true;
      if (const wchar_t* value = nevr_cli::OptionValue(argc, argv, i)) {
        g_configYamlFlags.config_file = WideToUtf8(value);
        ++i;
      }
    } else if (nevr_cli::IsFlag(arg, L"-config-path")) {
      // #1255: registered for the game's parser, refused by the choice below. Its value is skipped, not used.
      g_configYamlFlags.config_path_given = true;
      if (nevr_cli::OptionValue(argc, argv, i) != nullptr) ++i;
    }
  }
  LocalFree(argv);

  // #1255: which config.yaml this run reads, decided now (the first read is the early plugin pass below). A
  // command line that names no usable file ends the run here with exit code 64; nothing has started yet.
  NevrCfgResolveConfigYaml();

  // -windowed implies no VR (never spectator-stream).  The windowed-mode
  // flag is set directly on the game instance at offset 31456 (0x7AE0) with bit
  // 0x0100000.  Spectator stream would set 0x2100000 (bit 0x0100000 +
  // 0x2000000), which is what PatchSpectatorStreamCheck does by NOPping
  // the CLI check — but that also forces the spectator flow.  Setting
  // only 0x0100000 skips VR without forcing spectator mode; the game
  // reaches the main menu normally.
  //
  // The bit is keyed on -windowed, not on -server: it is a client flag.  A
  // -server run begins multiplayer after its login through the state machine
  // (state_machine.cpp, issue #45: no menu runs on a server to begin it), and
  // only there: a server must not get 0x2100000 either, because the spectator
  // flow begins multiplayer a second time (see PreprocessCommandLineHook).
  if (g_isWindowed && g_pGame != nullptr) {
    auto* windowedFlags = reinterpret_cast<UINT64*>(
        reinterpret_cast<CHAR*>(g_pGame) + nevr_patch_addresses::GAME_WINDOWED_FLAGS_OFFSET);
    *windowedFlags |= 0x0100000;
  }

  // #511: a server runs on a fixed time step, so its headless loop is paced. The step itself is set
  // when the local config loads (config.cpp, LoadLocalConfigHook). ApplyFlag leaves a client alone.
  UINT64* timestepFlags =
      nevr_server_timestep::FieldAt(g_pGame, nevr_patch_addresses::GAME_TIMESTEP_FLAGS_OFFSET);
  const nevr_server_timestep::FlagOutcome flagOutcome =
      nevr_server_timestep::ApplyFlag(g_isServer != FALSE, timestepFlags);
  g_flagRecord.Store(flagOutcome);
}

}  // namespace

/// <summary>
/// A detour hook for the game's command line pre-processing method, used to parse command line arguments.
/// </summary>
/// <param name="pGame">A pointer to the game instance.</param>
UINT64 PreprocessCommandLineHook(PVOID pGame) {
  // N146: the first original call initializes the client D3D path.  It can also
  // be the ONLY call, so waiting for a second invocation leaves the bridge and
  // all deferred runtime setup permanently disabled.  Store the game instance
  // before D3D starts so its device hook can complete setup immediately after
  // creation. A dedicated server has no graphics-device rendezvous, so it
  // bootstraps immediately after its first original call returns.
  g_pGame = pGame;
  PreflightRuntimeBootstrap();
  // Wine skips the early VEH while server mode is unknown. Decide now, before
  // the original command-line processing can enter server-specific game code.
  static bool s_vehDecisionLogged = false;
  if (!s_vehDecisionLogged) {
    s_vehDecisionLogged = true;
    const auto vehResult = InstallVEH();
    const char* status = vehResult == nevr_veh_policy::InstallResult::Installed ? "installed"
                         : vehResult == nevr_veh_policy::InstallResult::DeferredWine ? "skipped" : "failed";
    Log(vehResult == nevr_veh_policy::InstallResult::Failed ? EchoVR::LogLevel::Error : EchoVR::LogLevel::Info,
        "[NEVR.CRASH] veh post_cli status=%s mode=%s", status, g_isServer ? "server" : "client");
  }
  // The -spectatorstream check (SPECTATORSTREAM_CHECK, a JE inside PreprocessCommandLine) is not
  // NOPped here, before the first original call, on a server or a client. With the JE NOPped that
  // call falls through to `or qword [game+0x7AE0], 0x2100000` (echovr.exe 0x140116F4D), the
  // spectator flow, and in that flow the game begins multiplayer by itself (on a server against
  // the production services, about ten seconds after the login). On a server the call the state
  // machine queues when the login completes (state_machine.cpp, #514) has begun it already, so the
  // game's begin is a second one: it rebinds the HTTP API listener, the bind fails and the server
  // exits (ForceFatalExit(62)). The checked NOP is applied by PatchEnableServer in
  // RunDeferredRuntimeBootstrap, after this call returns.
  // #496: plugins marked `early: true` load here, once, before the original call: the
  // game reads its manifests and opens its packages during it, before the graphics
  // device exists (where the rest load), so a plugin that serves game data from
  // elsewhere has to be in place now. Nothing marked early: nothing loads here.
  static std::atomic_bool s_earlyPlugins{false};
  if (!s_earlyPlugins.exchange(true, std::memory_order_acq_rel)) {
    LoadPlugins(PluginPhase::Early);
  }
  UINT64 result = EchoVR::PreprocessCommandLine(pGame);
  if (g_isServer) {
    RunDeferredRuntimeBootstrap(pGame, "Preprocess first-call server bootstrap");
  }
  return result;
}

// #515: what happened to a server's time-step arguments, from what GameMainWrapperHook recorded and what
// ApplyServerTimestep and ApplyFlag recorded, not from the configured rate. The level and text are
// nevr_timestep_log::Decide (a tested pure function). It does not depend on boot.cpp's own reading of the
// command line: the hook decides from the game's parser (see runtime/lifecycle/cmdline_strip.h),
// CommandLineToArgvW differs on quotes. The game's own "CL Args: %s" line (NRadPhysics::InitGlobals,
// echovr.exe 0x141548da0) still prints the original command line. It allocates inside a game-called hook, so
// a failure is caught by type and logged, never allowed to leave.
// #587: a value option followed by another option (or by nothing) takes no value; the next token is parsed
// as the option it is. The option is named, its (absent) value is not.
static void LogOptionWithoutValue(const char* option) {
  Log(EchoVR::LogLevel::Warning, "[NEVR.PATCH] %s was given without a value; it is ignored", option);
}

// #697: what the command line's spelling of a NEVR flag earns. A deprecated alias is reported once per run
// per alias (the text is nevr_cli::kAliasWarningFormat, pinned by test_cmdline_strip); a token with three or
// more leading dashes on a NEVR flag is rejected, since the game's own lookup would still read it.
static void NoteCommandLineToken(const wchar_t* token, const nevr_cli::Resolved& named) {
  static nevr_cli::AliasReports s_reports;
  if (named.rejected) {
    Log(EchoVR::LogLevel::Warning,
        "[NEVR.PATCH] %ls has more than two leading dashes; NEVR ignores it (write -name or --name)", token);
    return;
  }
  if (s_reports.First(named)) Log(EchoVR::LogLevel::Warning, nevr_cli::kAliasWarningFormat, named.alias, named.flag);
}

static void LogTimestepArgs() {
  try {
    const nevr_timestep_log::Inputs in = nevr_timestep_log::BuildInputs(
        GetTimestepStripReport(), g_isServer != FALSE, nevr_cmdline::HasTimestepOption(GetCommandLineA()),
        ServerStepWrite(), g_flagRecord.Load(), g_headlessTickRateHz);
    for (const nevr_timestep_log::Line& line : nevr_timestep_log::Decide(in)) {
      Log(line.level == nevr_timestep_log::Level::kWarning ? EchoVR::LogLevel::Warning : EchoVR::LogLevel::Info,
          "[NEVR.PATCH] %s", line.text.c_str());
    }
  } catch (const std::bad_alloc&) {
    Log(EchoVR::LogLevel::Warning, "[NEVR.PATCH] time-step argument report failed (out of memory)");
  } catch (const std::exception&) {
    Log(EchoVR::LogLevel::Warning, "[NEVR.PATCH] time-step argument report failed (std::exception)");
  }
}

// #960: a game whose data folder holds no level manifest starts, finds nothing, and closes with no message. The
// check is the registration's own manifest finder (FindManifestDirs), so the two agree on what counts as data.
// Every run logs the result. A client with a screen also gets one dialog, opened on its own thread so the boot
// thread is never held by a modal box (the graphics-driver notice's pattern, log/builtin_filter.cpp). Nothing
// here stops the game.
static DWORD WINAPI GameDataDialogThread(LPVOID text) {
  MessageBoxA(nullptr, static_cast<const char*>(text), "Echo VR - game data missing",
              MB_OK | MB_ICONWARNING | MB_SETFOREGROUND | MB_TOPMOST);
  return 0;
}

static void CheckGameData() {
  char exePath[MAX_PATH] = {};
  const DWORD length = GetModuleFileNameA(reinterpret_cast<HMODULE>(EchoVR::g_GameBaseAddress), exePath, MAX_PATH);
  if (length == 0 || length >= MAX_PATH) {
    Log(EchoVR::LogLevel::Warning, "[NEVR.PATCH] game data check skipped: the game's exe path is unavailable error=%lu",
        GetLastError());
    return;
  }
  const std::filesystem::path exe(exePath);
  const std::string dataDir = (nevr_game_server::InstallRootFromExe(exe) / "_data").string();
  if (!nevr_game_server::FindManifestDirs(exe).empty()) {
    Log(EchoVR::LogLevel::Info, "[NEVR.PATCH] game data present: level manifests under %s", dataDir.c_str());
    return;
  }
  // The outcome of the dialog, named in the log line below. A failure to open it is logged with its error and
  // the game is left to run; the line still says the player was not told.
  std::string dialog = "not_shown reason=" + std::string(g_isServer ? "server_mode" : "headless");
  if (nevr_game_data::DialogDue(g_isServer != FALSE, g_isHeadless != FALSE)) {
    dialog = "not_shown reason=out_of_memory";
    char* text = _strdup(nevr_game_data::Text(dataDir).c_str());
    if (text != nullptr) {
      HANDLE thread = CreateThread(nullptr, 0, GameDataDialogThread, text, 0, nullptr);
      if (thread != nullptr) {
        CloseHandle(thread);
        dialog = "opened";
      } else {
        dialog = "not_shown reason=thread_failed error=" + std::to_string(GetLastError());
      }
    }
  }
  Log(EchoVR::LogLevel::Error,
      "[NEVR.PATCH] game data missing: no level manifest under %s; the game cannot load and closes at start "
      "dialog=%s",
      dataDir.c_str(), dialog.c_str());
}

void RunDeferredRuntimeBootstrap(PVOID pGame, const char* trigger) {
  // The game can reach this through a post-device graphics hook and, for a
  // dedicated server, through the second-preprocess fallback above. Mark
  // initialization started before any work so a re-entrant engine call cannot
  // initialise modules/listeners twice.
  static std::atomic_bool s_oneTimeSetupStarted{false};
  bool expected = false;
  if (!s_oneTimeSetupStarted.compare_exchange_strong(
          expected, true, std::memory_order_acq_rel, std::memory_order_acquire)) {
    return;
  }
  Log(EchoVR::LogLevel::Info, "[NEVR.BOOT] runtime bootstrap trigger=%s",
      trigger ? trigger : "(unknown)");

  // Deferred from Initialize() — file I/O deadlocks during DllMain loader lock.
  // config.json is optional (issue #21); redirects are armed here whether or not
  // one was found — g_earlyConfigPtr does not gate them.
  LoadEarlyConfig();
  ArmServiceRedirects();
  SetResourceOverrideFatalLineSink(&WriteCrashRecordLine);
  InstallResourceOverride();
  // #1535: the FRIENDS panel (its pictures set, the menu transforms, AddPackage) and its avatars.
  nevr_menu_friends_pack::Install(reinterpret_cast<std::uintptr_t>(EchoVR::g_GameBaseAddress));
  nevr_menu_friends_avatars::Install(reinterpret_cast<std::uintptr_t>(EchoVR::g_GameBaseAddress));

  // Early-detect -server before auth — full CLI parse happens below.
  // The token_auth MODULE needs to know server mode: a server with auth.password skips the device-code flow, one
  // without it signs in by device code (#1532).
  {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 0; i < argc; ++i) {
      if (nevr_cli::IsFlag(argv[i], L"-server")) {
        g_isServer = TRUE;
        g_noOvr = TRUE;  // -server implies -noovr unconditionally
        break;
      }
    }
    LocalFree(argv);
  }

  // Install server-mode fatal error handler BEFORE loading modules.
  // If a required module fails to load, module_loader.cpp calls FatalError,
  // which must route through ForceFatalExit rather than blocking on a modal
  // MessageBoxA. In client mode the handler is NOT installed — the user at the
  // screen should see the modal dialog so they know the game fatally failed.
  if (g_isServer) {
    // #879: the mic provider answers "no device" on a server (mic_provider.cpp), so no capture device is opened.
    Log(EchoVR::LogLevel::Info, "[NEVR.MIC] server: no capture device; Mic* calls answer no-device");
    InstallFatalErrorHandler();

    // Deferred fatal-condition checks — these conditions are detected in
    // Initialize() before g_isServer is known, so we check them now that the
    // fatal-error handler is installed.  If any fail, the server dies
    // immediately with the cause as the last log line rather than limping along
    // in a degraded state for hours.
    //
    // Issue #21: a missing/unparseable _local/config.json is deliberately NOT
    // one of them any more. That check (N48) predates N133, which moved every
    // NEVR setting to config.yaml; config.yaml has its own fail-loud policy
    // (service_config.cpp NevrCfg()). config.json now only feeds keys the stock
    // engine reads natively (publisher_lock is supplied by JsonValueAsStringHook
    // when absent); a server with no config.json boots and logs in.
    if (g_bootHookFailed) {
      // The cause was recorded where the flag was set (NoteBootHookFailureCause, initialize.h), so the last line names the site.
      ServerFatal("One or more boot hooks failed to install — server would be degraded: %s",
                  g_bootHookFailedCause[0] != '\0' ? g_bootHookFailedCause : "cause not recorded");
    }

    // #592: the two game functions that begin multiplayer after the login are checked once, here;
    // a mismatch exits the server now rather than leaving it logged in and idle after its login.
    ValidateServerBringup();
  }

  // Load modules. Order matters — dependencies must load first.
  {
    static NvrModuleContext moduleCtx = {};
    moduleCtx.base_addr = (uintptr_t)EchoVR::g_GameBaseAddress;
    moduleCtx.early_config = g_earlyConfigPtr;
    moduleCtx.flags = 0;
    if (g_isServer)   moduleCtx.flags |= NEVR_MODULE_HOST_IS_SERVER;
    if (g_isHeadless) moduleCtx.flags |= NEVR_MODULE_HOST_IS_HEADLESS;
    if (g_noOvr)      moduleCtx.flags |= NEVR_MODULE_HOST_IS_NOOVR;
    if (!g_isServer)  moduleCtx.flags |= NEVR_MODULE_HOST_IS_CLIENT;
    moduleCtx.log = (void (*)(int, const char*, ...))Log;
    moduleCtx.get_proc = ResolveModuleProc;
    // N133 S5: modules read config.yaml through the SAME accessor the runtime uses,
    // so a module never parses the game JSON and never sees a value the runtime
    // wouldn't. NevrCfgGetFlat's fail-loud is mode-aware (server=fatal/client=warn),
    // which a module inherits by calling through this pointer.
    moduleCtx.config_get = &NevrCfgGetFlat;
    SetModuleContext(&moduleCtx);

    // Platform compat — Schannel TLS hooks, CreateDirectory fixes, MSXML6 pass-through hook.
    // Must load before any network-using code. Statically linked (2026-08-02).
    {
      uint32_t apiVer = platform_compat_ApiVersion();
      if (!NvrModuleApiVersionSupported(apiVer)) {
        Log(EchoVR::LogLevel::Error,
            "[NEVR.MODULE] platform_compat: API v%u exceeds host v%u — refusing (rebuild the "
            "module against this host)",
            apiVer, static_cast<uint32_t>(NEVR_MODULE_API_VERSION));
        FatalError("Module API version unsupported", "platform_compat");
      }
      int rc = platform_compat_Init(&moduleCtx);
      if (rc != 0) {
        Log(EchoVR::LogLevel::Error, "[NEVR.MODULE] platform_compat: init failed with code %d", rc);
        FatalError("Module init failed", "platform_compat");
      }
      RegisterStaticModule("platform_compat", apiVer, nullptr, nullptr, platform_compat_Shutdown);
    }

    // Token auth — device code authentication, JWT refresh.
    // Must load before ws_bridge (ws_bridge reads the JWT via get_proc).
    // Statically linked (2026-08-02).
    {
      uint32_t apiVer = token_auth_ApiVersion();
      if (!NvrModuleApiVersionSupported(apiVer)) {
        Log(EchoVR::LogLevel::Error,
            "[NEVR.MODULE] token_auth: API v%u exceeds host v%u — refusing (rebuild the module "
            "against this host)",
            apiVer, static_cast<uint32_t>(NEVR_MODULE_API_VERSION));
        FatalError("Module API version unsupported", "token_auth");
      }
      int rc = token_auth_Init(&moduleCtx);
      if (rc != 0) {
        Log(EchoVR::LogLevel::Error, "[NEVR.MODULE] token_auth: init failed with code %d", rc);
        FatalError("Module init failed", "token_auth");
      }
      RegisterStaticModule("token_auth", apiVer, nullptr, nullptr, token_auth_Shutdown);

      // Register token_auth exports for cross-module access (ws_bridge reads these
      // via ResolveModuleProc). Static link means no GetProcAddress — call directly.
      RegisterModuleProc("TokenAuth_GetToken", (void*)TokenAuth_GetToken);
      RegisterModuleProc("TokenAuth_GetDiscordId", (void*)TokenAuth_GetDiscordId);
      RegisterModuleProc("TokenAuth_GetUsername", (void*)TokenAuth_GetUsername);
      // #1446: the bridge answers a login itself once the sign-in has ended without a token.
      RegisterModuleProc("TokenAuth_SignInEndedWithoutToken",
                         reinterpret_cast<void*>(&nevr_token_auth::SignInEndedWithoutToken));
    }

    // N92: ws_bridge is not a module. It is compiled into this DLL and started
    // below, after the CLI is parsed; config.cpp calls
    // IsWebSocketBridgeActive()/GetWebSocketBridgePort() directly.
  }

  // Parse command line arguments.
  int argc = 0;
  LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);

  for (int i = 0; i < argc; ++i) {
    const LPWSTR arg = argv[i];
    const nevr_cli::Resolved named = nevr_cli::Resolve(arg);
    NoteCommandLineToken(arg, named);
    const wchar_t* const flag = named.flag != nullptr ? named.flag : L"";

    if (lstrcmpW(flag, L"-server") == 0) {
      g_isServer = TRUE;
      g_isHeadless = TRUE;  // -server forces -headless unconditionally (never separable)
    } else if (lstrcmpW(flag, L"-offline") == 0) {
      g_isOffline = TRUE;
    } else if (lstrcmpW(flag, L"-no-console") == 0) {
      g_noConsole = TRUE;
    } else if (lstrcmpW(flag, L"-windowed") == 0) {
      g_isWindowed = TRUE;
    } else if (lstrcmpW(flag, L"-no-exit-on-error") == 0) {
      g_exitOnError = FALSE;
    } else if (lstrcmpW(flag, L"-exit-on-error") == 0) {
      // Deprecated — exit-on-error is now the default
      Log(EchoVR::LogLevel::Warning, "[NEVR.PATCH] -exit-on-error is a no-op (exiting on error is the default)");
    } else if (lstrcmpW(flag, L"-no-telemetry") == 0) {
      g_telemetryEnabled = FALSE;
    } else if (lstrcmpW(flag, L"-telemetry-rate") == 0) {
      if (const wchar_t* value = nevr_cli::OptionValue(argc, argv, i)) {
        g_telemetryRateHz = std::wcstoul(value, nullptr, 10);
        if (g_telemetryRateHz == 0) g_telemetryRateHz = 10;
        ++i;
      } else {
        LogOptionWithoutValue("-telemetry-rate");
      }
    } else if (lstrcmpW(flag, L"-telemetry-diag") == 0) {
      g_telemetryDiag = TRUE;
    } else if (lstrcmpW(flag, L"-timestamps") == 0) {
      g_timestampLogs = TRUE;
    } else if (lstrcmpW(flag, L"-upnp") == 0) {
      g_upnpEnabled = TRUE;
    } else if (lstrcmpW(flag, L"-config") == 0) {
      if (const wchar_t* value = nevr_cli::OptionValue(argc, argv, i)) {
        WideCharToMultiByte(CP_UTF8, 0, value, -1, g_customConfigPath, MAX_PATH, NULL, NULL);
        ++i;
      } else {
        LogOptionWithoutValue("-config");
      }
    } else if (lstrcmpW(flag, L"-config-file") == 0 || lstrcmpW(flag, L"-config-path") == 0) {
      // Read and acted on by PreflightRuntimeBootstrap (#1255); here the value is only consumed.
      if (nevr_cli::OptionValue(argc, argv, i) != nullptr) ++i;
    } else if (lstrcmpW(flag, L"-region") == 0 || lstrcmpW(flag, L"-serverregion") == 0) {
      if (const wchar_t* value = nevr_cli::OptionValue(argc, argv, i)) {
        WideCharToMultiByte(CP_UTF8, 0, value, -1, g_regionOverride, sizeof(g_regionOverride), NULL, NULL);
        ++i;
      } else {
        LogOptionWithoutValue("-region");
      }
    } else if (lstrcmpW(flag, L"-trace-exports") == 0) {
      // Consumed here so it is not mistaken for anything else; the tracer reads its own value from the
      // command line (runtime/hook/export_tracer.cpp), so no ordering with this parse can matter.
      if (nevr_cli::OptionValue(argc, argv, i) != nullptr) {
        ++i;
      } else {
        LogOptionWithoutValue("-trace-exports");
      }
    } else if (lstrcmpW(flag, L"-timestep") == 0 || lstrcmpW(flag, L"-fixedtimestep") == 0) {
      // #515: a server's time step is NEVR's. GameMainWrapperHook (crash_recovery.cpp) removed these
      // arguments from the string the game parses, before the game read it; LogTimestepArgs reports it
      // once, from what the hook recorded. Here the value is only consumed so it is not read as an option.
      if (lstrcmpW(flag, L"-timestep") == 0) {
        if (nevr_cli::OptionValue(argc, argv, i) != nullptr) {
          ++i;
        } else {
          LogOptionWithoutValue("-timestep");
        }
      }
    } else if (lstrcmpW(flag, L"-require-signed-plugins") == 0) {
      // Already applied by PreflightRuntimeBootstrap, before the early plugin pass: nothing to do here. The plugin
      // loader states the mode in effect, and where it came from, when it starts ("plugin trust: mode=...").
    } else if (lstrcmpW(flag, L"-headless") == 0) {
      // N99: -headless is never "redundant", and the log must not tell the
      // operator to remove it: removing the flag opens a window.
      // -headless is a NATIVE echovr.exe token; the game's own arg handler applies
      // pGame+0x1D4 &= 0xFFFEFEFE when it is present. The preflight pass set
      // g_isHeadless from it, so PatchEnableHeadless runs the same gate set as -server.
      // Without those gates a client run reaches the game's D3D12 device init with no
      // GPU path and aborts ("Unknown error while loading the game"; addresses.h).
      Log(EchoVR::LogLevel::Info,
          "[NEVR.PATCH] -headless: native echovr.exe flag; headless gates applied, the same set -server runs.");
    } else if (lstrcmpW(flag, L"-noovr") == 0) {
      // Native token, and the game handles it itself: a single flag write,
      // orq $0x8000000, 0x7ae0(%rdi) at 0x1401168BA. NOT pGame+0x178 — that is
      // the CMainArgs POINTER, and the claim that it was a flag field is what
      // kept this open in N99 for a week. Closed not-a-defect in N113.
      //
      // g_noOvr is not "only a global": it drives NEVR_MODULE_HOST_IS_NOOVR
      // (:75) the NEVR_MODULE_HOST_IS_NOOVR flag the modules read, which is the
      // behaviour -noovr exists to produce (the login platform code does not depend on it:
      // SelectPlatformCode follows the game's own No VR flag, bit 20, which -noovr does not set). The game's own
      // -noovr-requires--spectatorstream assert is bypassed by the argument-lookup hook
      // (patch/noovr_arglookup.cpp), inert unless the token is present.
      g_noOvr = TRUE;
    }
  }

  LocalFree(argv);

  LogTimestepArgs();

  // -server forces -headless unconditionally (set in the preflight pass above, with -headless).
  // The OR here is a safety net: if g_isHeadless was set to TRUE for any other
  // reason, -noovr is also applied. g_isHeadless is set by -server or -headless.
  g_isHeadless = g_isHeadless || g_isServer;
  // -server implies -noovr unconditionally.
  g_noOvr = g_noOvr || g_isServer;
  if (g_isServer) {
    Log(EchoVR::LogLevel::Info, "[NEVR.PATCH] Server mode — headless + noovr applied");
  }

  // Auto-enable -no-console on Wine/Linux
  if (!g_noConsole) {
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    if (ntdll && GetProcAddress(ntdll, "wine_get_version") != NULL) {
      g_noConsole = TRUE;
      Log(EchoVR::LogLevel::Info, "[NEVR.PATCH] Wine detected — defaulting to -no-console");
    }
  }

  // The argument combinations (-server with -offline or -windowed) are refused in PreflightRuntimeBootstrap (#1253).

  // Detect dbgcore.dll in the game directory — prevents accidental hijack.
  // In the launcher path, BugSplat64.dll is loaded by echovr_game.dll. If a
  // leftover dbgcore.dll is also present in the game directory, the OS loader
  // resolves it during import resolution BEFORE the launcher can intervene,
  // causing a double-load condition. No command-line flag skips this check: a
  // legacy dbgcore.dll is an older runtime, and the player is told to rename it.
  {
    char exePath[MAX_PATH];
    DWORD len = GetModuleFileNameA(NULL, exePath, MAX_PATH);
    if (len > 0 && len < MAX_PATH) {
      char* lastSlash = strrchr(exePath, '\\');
      if (!lastSlash) lastSlash = strrchr(exePath, '/');
      if (lastSlash) {
        *(lastSlash + 1) = '\0';
        char dbgcorePath[MAX_PATH];
        int written = snprintf(dbgcorePath, MAX_PATH, "%sdbgcore.dll", exePath);
        if (written > 0 && written < MAX_PATH) {
          if (GetFileAttributesA(dbgcorePath) != INVALID_FILE_ATTRIBUTES) {
            if (g_isServer) {
              // The refusal is logged before FatalError: on a client FatalError only shows a dialog (#726).
              Log(EchoVR::LogLevel::Error,
                  "[NEVR.PATCH] dbgcore.dll present in game directory — refusing to start "
                  "(rename it to dbgcore.dll.legacy-<date> or remove it)");
              FatalError(
                  "dbgcore.dll detected in game directory — legacy DLL hijack artifact; rename it "
                  "(dbgcore.dll.legacy-<date>) or remove it",
                  "dbgcore.dll hijack detected");
            } else {
              Log(EchoVR::LogLevel::Error,
                  "[NEVR.PATCH] dbgcore.dll present in game directory — refusing to start "
                  "(rename it to dbgcore.dll.legacy-<date> or remove it)");
              FatalError(
                  "dbgcore.dll detected in the game directory.\n\n"
                  "This file is a legacy DLL hijack artifact, and the runtime will not start beside "
                  "it. Rename it (for example to dbgcore.dll.legacy-<date>) or remove it.\n\n"
                  "If you are using the NEVR launcher, remove dbgcore.dll from the game "
                  "directory and use BugSplat64.dll instead.",
                  "dbgcore.dll hijack detected");
            }
          }
        }
      }
    }
  }

  // Store the game pointer globally for social feature access
  g_pGame = pGame;

  // Apply patches based on arguments.
  if (g_isOffline) {
    PatchEnableOffline();
  }

  if (g_isHeadless) {
    PatchEnableHeadless(pGame);
  }

  // If the windowed, server, or headless flags were provided, apply the windowed mode patch to not use a VR headset.
  if (g_isWindowed || g_isServer || g_isHeadless) {
    using namespace nevr_patch_addresses;
    // Set windowed mode flag in game structure
    UINT64* windowedFlags = reinterpret_cast<UINT64*>(static_cast<CHAR*>(pGame) + GAME_WINDOWED_FLAGS_OFFSET);
    *windowedFlags |= 0x0100000;  // Enable windowed mode (spectator uses 0x2100000 for additional settings)
  }

  // Force the game to load pnsrad.dll instead of pnsovr.dll.
  // Must run before the game's module loader starts.
  nevr_pnsrad_enabler::Init((uintptr_t)EchoVR::g_GameBaseAddress);

  // Block Oculus Platform SDK on server/headless/windowed — client needs Oculus
  // Platform services only when running with actual VR hardware.
  if (g_isServer || g_isHeadless || g_isWindowed) {
    PatchBypassOvrPlatform();
    PatchBlockOculusSDK();
  }

  // Apply patches to force the game to load as a server.
  if (g_isServer) {
    PatchEnableServer();
    // #1774: the loading-tip script nodes return at once on a server (patch/loading_tips.h).
    nevr_loading_tips::Install(reinterpret_cast<std::uintptr_t>(EchoVR::g_GameBaseAddress));
    PatchDisableWwise();
    PatchLogServerProfile();
    // Issue #63: co-op AI bots stand still on community servers; log what gates them.
    nevr_coop_ai_trace::Install(reinterpret_cast<std::uintptr_t>(EchoVR::g_GameBaseAddress));
    // #982: a public Arena match's clock keeps counting through goals, like a private match (arena_public_clock_rules.h).
    nevr_arena_public_clock::Install();

    // Server frame pacing (CPrecisionSleep::BusyWait) is patched only by
    // patch/binary_bug_fixes.cpp, which validates the address and saves the
    // original byte. A second writer here would race the shutdown restore that
    // depends on that saved byte.
  }

  // N131: cosmetics are client-only — a headless server has nothing to render and
  // must not open the CDN connection (it opens ServerDB + login only). AssetCDN
  // was called UNCONDITIONALLY from initialize.cpp:364, which runs before the CLI
  // is parsed, so g_isServer would still be FALSE there and a server would fetch tints it
  // never draws. It is called here, post-CLI-parse where g_isServer is known, gated on
  // client — the same deferral InstallResourceOverride uses (boot.cpp:29). The
  // loadout SAVE/CURRENT protocol in gameserver_callbacks.cpp is independent of this hook,
  // so gating it off on a server does not affect loadout handling.
  if (!g_isServer) {
    nevr_asset_cdn::Initialize();
  }

  // #960: the game data check, every run. Before the client-only patches so a missing folder is logged even when they
  // are skipped.
  CheckGameData();

  // N148 (#938): the project section of the in-game CREDITS page. Client only: a server has no page, and the
  // decision is logged either way (N122/N124). Post-CLI-parse for the same reason as the CDN above. The
  // runtime owns two byte-patch sites here (a data-blob load, not a function, so there is nothing to hook);
  // Install validates both and the blob before writing and leaves the game's credits alone on any mismatch.
  if (!g_isServer) {
    nevr_credits_text::Install(nevr_credits_block::kText, nevr_credits_block::kSize);
  } else {
    Log(EchoVR::LogLevel::Info, "[NEVR.PATCH] credits_text skipped reason=server_mode (no credits page on a server)");
  }

  // #906, ADR 0017 D4: the CREATE GAME and JOIN VIA CODE cards on the main menu. Client only: a server draws no menu.
  // The page is the player's own, pinned by sha256 and extended in memory when the loader delivers it; any other
  // page is served unchanged. Registered after InstallResourceOverride, which created the registry.
  if (!g_isServer) {
    nevr_menu_cards::Install();
  } else {
    Log(EchoVR::LogLevel::Info, "[NEVR.MENU] main-menu cards skipped reason=server_mode (a server draws no menu)");
  }

  // #1420: the magnum, smg, Chain and rifle bullets of the combat levels get the scout bullet's particle effects, in
  // copies of the stock CParticleEffectCR and CR15NetBulletCR pinned by sha256; any other bytes are served unchanged.
  // Client only: a server draws no particles. Registered after InstallResourceOverride, which created the registry.
  if (!g_isServer) {
    nevr_netgun_bullets::Install();
  } else {
    Log(EchoVR::LogLevel::Info,
        "[NEVR.RESOURCE] netgun bullet effects skipped reason=server_mode (a server draws no particle effects)");
  }

  // #1420: the four unfinished weapons (magnum, smg, Chain, rifle) fire the assault's bullet once their netgun entries name
  // it. The transform rewrites the player's own stock CR15NetGunCR in memory; both client and server load it. Registered
  // after InstallResourceOverride, which created the registry.
  nevr_netgun_bullet_ids::Install();

  // #906, #1156, ADR 0017: observe the CREATE GAME / JOIN VIA CODE wizard's click metric. Client only. A typed code is
  // posted to the game service when network.invite_codes is on (the service below; on unless the config says off), and so is
  // a create; the answer is written into the menu by nevr_invite_apply::OnFrame (frame/tick.cpp). The hook forwards every
  // submit unchanged.
  if (!g_isServer) {
    // The observers install before the service starts, so the service's start line says whether they did: a refused observer
    // keeps the service off (its own Warning carries the address and the bytes; patch/invite_service.h).
    const bool choiceInstalled = nevr_invite_choice_hook::Install(reinterpret_cast<std::uintptr_t>(GetModuleHandleA(nullptr)));
    const bool wizardInstalled = nevr_invite_hook::Install(reinterpret_cast<std::uintptr_t>(GetModuleHandleA(nullptr)));
    nevr_invite_service::Settings invite;
    const char* inviteText = NevrCfgGetFlat("nevr_invite_codes");  // null when absent; the service says which in its start line
    const bool inviteStructured = NevrCfgGetFlatIsStructured("nevr_invite_codes");  // a list or a map is not a value (#1156)
    invite.invite_codes = nevr_invite::ParseSwitch(inviteText, inviteStructured);
    invite.invite_source = nevr_invite::SourceOf(inviteText, inviteStructured);
    const char* httpUri = NevrCfgGetFlat("nevr_http_uri");
    invite.http_uri = httpUri != nullptr ? httpUri : "";
    invite.token = [] { return nevr_token_auth::GetToken(); };  // thread-safe; the export TokenAuth_GetToken is not
    invite.log = [](nevr_invite::LogLevel level, const std::string& line) {
      Log(level == nevr_invite::LogLevel::Warning ? EchoVR::LogLevel::Warning : EchoVR::LogLevel::Info, "%s", line.c_str());
    };
    invite.salt = std::to_string(GetTickCount64());
    invite.hooks_refused = !(choiceInstalled && wizardInstalled);
    nevr_invite_service::Start(std::move(invite));
  } else {
    Log(EchoVR::LogLevel::Info, "[NEVR.INVITE] service skipped reason=server_mode");
    Log(EchoVR::LogLevel::Info, "[NEVR.INVITE] wizard observer skipped reason=server_mode");
    Log(EchoVR::LogLevel::Info, "[NEVR.INVITE] choice observer skipped reason=server_mode");
  }

  // #1139, ADR 0022: the render_config line. The two observers on the game's graphics-settings code go in on a client
  // only; Install logs the server-mode skip itself, so the decision is in one tested place.
  nevr_render_config_hook::Install(reinterpret_cast<std::uintptr_t>(GetModuleHandleA(nullptr)), g_isServer != FALSE);

  // #896, #906: the UI text writer (the first accessor of the UI seam). Client only: a server draws no menu. It pins the
  // game's functions and logs the result; nothing is written until a request is posted. The SetText hook (menu version label) is already in, which the pin accepts.
  if (!g_isServer) {
    nevr_ui_text::Install(reinterpret_cast<std::uintptr_t>(GetModuleHandleA(nullptr)));
  } else {
    Log(EchoVR::LogLevel::Info, "[NEVR.UI] text writer skipped reason=server_mode (a server draws no menu)");
  }

  // N92: start the WebSocket bridge in-process, as part of this DLL rather than
  // a separate module DLL, so there is one copy of the bridge and no second,
  // divergent one (session sharing and the fake-LoginSuccess path live in the
  // same code).
  //
  // Started here, before plugins, because config.cpp's service redirect needs the
  // bridge port and the game asks for redirects during PreprocessCommandLine.
  // N133 S4a: the bridge target (nevr_socket_uri) now comes from config.yaml
  // services.socket_uri via nevr_config, not the game JSON. No g_earlyConfigPtr
  // guard here — the value does not live in the early game config; the bridge
  // starts iff socket_uri is configured (absent -> no bridge, unchanged). First
  // NevrCfg() access happens here, after the CLI loop and after the preflight's
  // choice of config.yaml (#1255), after g_isServer/InstallFatalErrorHandler — a bad config.yaml or
  // an unset required secret fails loud at this point in server mode.
  {
    // #843: a server takes the same built-in defaults as a client; this line names the ones it uses.
    NevrCfgLogBuiltinDefaultsTaken();
    const char* socketUri = NevrCfgGetFlat("nevr_socket_uri");
    const bool hasSocketUri = socketUri && socketUri[0] != '\0';
    const char* allowOffline = NevrCfgGetFlat("nevr_allow_offline_server");
    if (g_isServer && nevr_bridge_policy::IsUnrecognized(allowOffline)) {  // a server-only key
      Log(EchoVR::LogLevel::Warning,
          "[NEVR.WS] services.allow_offline_server is not a boolean (use true/false); treating it as false");
    }
    switch (nevr_bridge_policy::Decide(hasSocketUri, g_isServer != FALSE, nevr_bridge_policy::IsTruthy(allowOffline))) {
      case nevr_bridge_policy::Outcome::Start:
        SetWebSocketBridgeTarget(socketUri);
        InstallWebSocketBridge();
        break;
      case nevr_bridge_policy::Outcome::SkipClient:
        Log(EchoVR::LogLevel::Warning,
            "[NEVR.WS] no services.socket_uri (neither config.yaml nor an embedded build default) "
            "— bridge NOT started; the game will talk to services directly and login injection "
            "cannot fire");
        break;
      case nevr_bridge_policy::Outcome::SkipOfflineServer:
        Log(EchoVR::LogLevel::Warning,
            "[NEVR.WS] no services.socket_uri — bridge NOT started; services.allow_offline_server is "
            "set, so this server boots offline and will never log in or register");
        break;
      case nevr_bridge_policy::Outcome::RefuseServer:
        // A server without the bridge never sends a LoginRequest: it idles silently (#16).
        ServerFatal(
            "no services.socket_uri in config.yaml — a dedicated server cannot log in without the "
            "login bridge. Set services.socket_uri, or services.allow_offline_server: true for an "
            "offline boot");
        break;
    }
  }

  // #58: hold the game's return to lobby for an empty server (network.empty_server_ttl_seconds,
  // default 0 = off). Server only: a client's returns to lobby are the player's.
  if (g_isServer) {
    std::string ttlProblem;
    const uint64_t ttlSeconds =
        nevr_return_to_lobby_hold::ParseTtlSeconds(NevrCfgGetFlat("nevr_empty_server_ttl_s"), &ttlProblem);
    if (!ttlProblem.empty()) {
      Log(EchoVR::LogLevel::Warning,
          "[NEVR.PATCH] network.empty_server_ttl_seconds is %s; using %llu s", ttlProblem.c_str(),
          static_cast<unsigned long long>(ttlSeconds));
    }
    if (!nevr_return_to_lobby::Configure(ttlSeconds)) {
      Log(EchoVR::LogLevel::Warning, "[NEVR.PATCH] empty-server TTL requested but not armed; the server keeps today's behaviour");
    }
  }

  // #954: the PC frame-time probe. diagnostics.frame_stats (default off) switches the loop-interval ring and
  // the 10 s [NEVR.PERF] line; Configure logs the resolved value at Info whether it is on or off (N122/N124).
  {
    int recognised = 1;
    const nevr_frame_stats::Mode frameStats =
        static_cast<nevr_frame_stats::Mode>(NevrCfgFrameStatsMode(&recognised));
    nevr_frame_stats::Configure(reinterpret_cast<std::uintptr_t>(EchoVR::g_GameBaseAddress), g_isServer != FALSE,
                                frameStats, recognised != 0);
  }
  // #1423: AI bots on a server (network.bots_per_session, absent = 0): on a server with a count above 0 the sites are
  // checked once here, and the driver (GameServerLib::Update) runs only when they hold; a client logs that it does not
  // apply (N122/N124).
  nevr_server_bots::Install(g_isServer != FALSE);
  // #1422: a server's match team size (network.max_team_size, absent = the game's 5), at ServerAssignTeam's
  // two checked-patch sites; a client logs that it does not apply (N122/N124).
  nevr_server_team_size::Install(g_isServer != FALSE);

  // The runtime's own signature (#842, ADR 0010): one line says whether this is the signed release or a self-built or dev
  // build. The word is what the server registration carries (nevr_build_identity::Provenance). Reads the file, so it runs
  // here, after the original PreprocessCommandLine call (N146), not in the preflight.
  {
    const nevr_build_identity::Info& buildId = nevr_build_identity::Get();
    const nevr_self_provenance::Result provenance = nevr_self_provenance::CheckOwnModule(
        buildId.project_version, nevr_plugin_trust::IsDevBuild(), nevr_plugin_trust::ProductionPolicy(),
        static_cast<int64_t>(std::time(nullptr)));
    nevr_build_identity::SetProvenance(provenance.word);
    Log(EchoVR::LogLevel::Info, "[NEVR.BOOT] runtime provenance=%s version=%s reason=\"%s\"", provenance.word,
        provenance.version.c_str(), provenance.reason.c_str());
  }

  // Load external plugins from plugins/ subdirectory
  LoadPlugins();

  // Crash frames in modules/plugins are unattributable without this (N85).
  RefreshModuleCache();
  ResolveShutdownDependencies();  // N62

  Log(EchoVR::LogLevel::Info,
      "[NEVR.BOOT] runtime bootstrap complete early_config=%s bridge=%s port=%u",
      g_earlyConfigPtr != nullptr ? "true" : "false", IsWebSocketBridgeActive() ? "true" : "false",
      static_cast<unsigned>(GetWebSocketBridgePort()));

#ifdef NEVR_SCENARIO_CONTROL
  nevr_scenario_control::Start();  // test builds only (mingw-scenario preset)
#endif
}
