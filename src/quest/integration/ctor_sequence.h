// The sentinel constructor's sequence (docs/adr/0003, "Integration"): what runs, in which order, what
// depends on what, and how one piece failing leaves the others running.
//
// The sequence is pure policy over an abstract `Steps`, so the host test drives it with fakes and checks
// the order, the gating by feature, the counter registration before the single StartReporter, and the
// failure isolation. The production Steps (production_steps.cpp) bind each step to the real library.
//
// Order:
//   1  ArmCrashReporter              breakpad, before anything else can crash
//   2  ResolveConfig                 InitActivation: embedded defaults + config.yaml, every state logged
//      PlanPlugins                   the plugin plan (#438, docs/adr/0019): the `plugins:` list read into one record per
//                                    entry, the entries the plan alone settles written, no plugin code run; every launch
//   3  RegisterCounters              every counter of every hook that will be installed (clock, redirect,
//                                    dlopen, login, social, login prompt), BEFORE ...
//   4  StartReporter                 ... the single StartReporter (the reporter refuses a later register)
//      AttachLibR15                  the build-time libr15 patch (docs/adr/0021): checks the live libr15 against the patch
//                                    table, builds the trampolines, fills every slot and registers the plugin identity and
//                                    the GOT backend's build-ID alias (integration/libr15_attach_step.h). The first libr15
//                                    step: right after the reporter started (so its lines reach the same sinks) and BEFORE
//                                    the early plugin pass (a plugin's build ID is decided at Load time, from what Attach
//                                    recorded), the clock hook and every other libr15 hook. Every launch, no feature, no
//                                    counters
//      RunEarlyPlugins               the `early` plugin entries (#438): after Attach, before the clock hook;
//                                    skipped when the plan has none
//   5  InstallClockHook              the always-on proof hook
//   6  StartTokenAuth                needed by the bridge (the remote JWT) and the login rewrite
//   7  InstallLoginPrompt            the sign-in prompt on the game's login-error screen (#239): two libr15
//                                    slots (SetDelimitedErrorMessage, CR15NetGame::Update); wherever token
//                                    auth is wanted, after it started (its GameTextPresenter publishes it)
//   8  StartBridge                   loopback listener + router; the redirect needs its port
//   9  InstallRedirect               CJson::TString thunks on libr15
//  10  InstallSocial                 the NEVR social facade on libr15's CNSProvider::Social slot
//  11  InstallDlopenHook             the post-load installs: login hook (and with it the login
//                                    prerequisites), matchmaking redirect, and the normal plugin pass (#438)
//      InstallObbSkip                the OBB-mount skip (#319, #866): three libr15 slots (AStorageManager_mountObb,
//                                    AStorageManager_getMountedObbPath, CSysInfo::GetDataRootDir); on every launch, whatever the
//                                    configuration says, right after the clock hook so it is in place before
//                                    libr15 runs CSysFile::Init. Its counters are registered with the others
//                                    (step 3).
//      InstallProcessEnd             the process ends when the game's android_main returns (#621): one libr15
//                                    slot, on every launch, right after the OBB skip; no counters
//      InstallPermissionWait         the game waits for the storage permission it just asked for (#640): one
//                                    libr15 slot (CSysInfo::RequestPermissions), on every launch, right after
//                                    the process-end hook; no counters
//      InstallVkExtWindow            the device extensions libr15 requires stay inside the 128 entries it reads
//                                    (#868): two libr15 slots (CSysModule::FindMethod, vrapi_GetDeviceExtensionsVulkan),
//                                    on every launch, right after the permission-wait hook; no counters
//      InstallOsBuildGate            the game never refuses a headset for its OS build number (#945): one libr15 slot
//                                    (CSysInfo::OSBuildVersion), on every launch, right after the Vulkan extension
//                                    window; no counters. CR15NetGame::Initialize, its only caller, runs after the
//                                    constructor.
//      InstallResourceRequestLog     the log-only resource request hook (#871): one libr15 slot
//                                    (CResourceManager::FillResourceFileRequest), in a development build only
//                                    (Steps::ResourceRequestLogWanted; a release build skips it and registers no
//                                    counters), right after the OS build floor hook. Its counters are registered
//                                    with the others (step 3).
//      InstallTextureQuality         the eye-buffer size libr15 creates its swap chain with (#943): one libr15 slot
//                                    (vrapi_GetSystemPropertyInt), on every launch, right after the OS build floor hook (and the resource request log, when wanted) and
//                                    before the hardware dump (which then shares the slot); the level is the config's
//                                    graphics.texture_quality, `stock` when the configuration cannot be resolved; `stock`
//                                    installs nothing. Its counters are registered with the others (step 3), unless `stock`.
//      InstallRenderConfig           the render-configuration log line (#1139, docs/adr/0022-render-config-log.md): five libr15
//                                    VrApi slots (refresh rate, clock levels, property set, system property float, frame
//                                    submit) and a poller thread, on every launch, right after the texture-quality hook (it
//                                    reports that hook's level and source) and before the hardware dump (which then leaves
//                                    the slots both hook to this step); no counters.
//      InstallCreditsText            the project section of the CREDITS page (#970): the credits slot of the patched libr15
//                                    (ADR 0021 section 8) gets a buffer of the build's block and the game's own text, right
//                                    after the texture hook; no counters. A data store: no code page is written. Refused
//                                    (the step reports failure) unless Attach verified libr15; the game then shows its own text.
//      InstallMenuCards              the CREATE GAME and JOIN VIA CODE cards on the main menu (#966): one libr15 data slot
//                                    (CResourceManager::AsyncResourceIOCallback, the streaming reader's read-finished
//                                    callback), on every launch, right after the credits step; no counters. A data write:
//                                    no code page is touched. Without the stock page's digest pinned it serves the page
//                                    unchanged and logs the digest.
//
// Dependencies (a failed or skipped piece disables what needs it and nothing else):
//   Attach not verified     -> NOTHING is gated on it by the sequence: AttachLibR15 reports failure with the reason token
//                              (libr15_not_patched for the stock file, build_id_mismatch for any other, module_not_loaded,
//                              no_build_id) and every later step runs. What each libr15 consumer then does is its own rule:
//                              the GOT-only hooks carry the patched build ID in their target (pinned_targets.h), so on a
//                              stock libr15 each refuses by itself (kBuildIdMismatch) and the game runs unmodified; the five
//                              inline rows (lobby_full x3, ui_texture, splash_skip) and the credits slot refuse as
//                              not_verified (RowActivation and CreditsSlot read Attach's record). A row or trampoline
//                              failure inside Attach ends the process itself (ADR 0021 sections 6 and 7); it does not come back.
//   token auth fails        -> the login prompt (token auth is what publishes it), bridge, login, social
//                              and the redirect-through-the-bridge are not started
//   bridge fails            -> login, social and the redirect-through-the-bridge are not started
//   redirect counters fail  -> redirect (and the matchmaking install) is not installed
//   dlopen counters fail    -> the dlopen hook, and so the login and matchmaking installs, are not installed
//   dlopen hook not live    -> the plugin entries still pending (the normal pass has no moment to run in) end as
//                              `not_started` with reason post_load_hook_unavailable (Steps::FinishPlugins)
//   login counters fail     -> the login hook is not installed (the matchmaking install is unaffected)
//   social counters fail    -> social is not installed
//   prompt counters fail    -> the login-prompt hook is not installed
//   obb counters fail       -> the OBB-mount skip is not installed
//   resource request counters fail -> the resource request log is not installed
//   texture counters fail   -> the texture-quality hook is not installed (not asked of `stock`)
// "redirect-through-the-bridge" is the redirect when the bridge feature is effective: a redirect that
// points the game at a loopback port nobody listens on, or straight at a TLS endpoint the game cannot
// speak, is worse than leaving the game's own hosts.
//
// Nothing here blocks: no step waits on the network, a thread or a lock held by the game.
#pragma once

#include <cstddef>
#include <cstdint>

#include "quest/sentinel/quest_config.h"

namespace nevr_quest::integration {

enum class StepState : std::uint8_t {
  kSkipped,   // not wanted (feature off) or a dependency failed; `reason` says which
  kOk,
  kFailed,    // the step reported failure
  kThrew,     // the step threw a std::exception (contained)
};

const char* StepStateName(StepState state);

// One entry per step, in the order they ran, for the log and for the tests.
enum class StepId : std::uint8_t {
  kArmCrashReporter,
  kResolveConfig,
  kPlanPlugins,
  kRegisterClockCounters,
  kRegisterRedirectCounters,
  kRegisterDlopenCounters,
  kRegisterLoginCounters,
  kRegisterSocialCounters,
  kRegisterLoginPromptCounters,
  kRegisterObbSkipCounters,
  kRegisterResourceRequestCounters,
  kRegisterTextureQualityCounters,
  kStartReporter,
  kAttachLibR15,
  kRunEarlyPlugins,
  kInstallClockHook,
  kInstallObbSkip,
  kInstallProcessEnd,
  kInstallPermissionWait,
  kInstallVkExtWindow,
  kInstallOsBuildGate,
  kInstallResourceRequestLog,
  kInstallTextureQuality,
  kInstallRenderConfig,
  kInstallCreditsText,
  kInstallMenuCards,
  kStartTokenAuth,
  kInstallLoginPrompt,
  kStartBridge,
  kInstallRedirect,
  kInstallSocial,
  kInstallDlopenHook,
  kInstallHwDump,
  kCount,
};

const char* StepName(StepId id);

struct StepOutcome {
  StepState state = StepState::kSkipped;
  const char* reason = "not_run";  // fixed token
};

struct ConstructorReport {
  StepOutcome steps[static_cast<int>(StepId::kCount)];
  StepId order[static_cast<int>(StepId::kCount)] = {};  // the ids in the order they were attempted
  int attempted = 0;
  const StepOutcome& at(StepId id) const { return steps[static_cast<int>(id)]; }
};

// How many plugin entries wait for each install moment after the plan (docs/adr/0019, section 3).
struct PluginCounts {
  std::size_t early = 0;   // wait for the early pass (after the reporter starts)
  std::size_t normal = 0;  // wait for the first post-load call
};

// Every method may throw std::exception (contained by the sequence) or return false.
class Steps {
 public:
  virtual ~Steps() = default;

  virtual void ArmCrashReporter() = 0;
  // InitActivation, then the resolved configuration.
  virtual const nevr_quest::ResolvedConfig& ResolveConfig() = 0;
  // The plugin plan from the resolved configuration (#438): one record per entry the plan settles, no plugin code run.
  virtual PluginCounts PlanPlugins() = 0;

  virtual bool RegisterClockCounters() = 0;
  virtual bool RegisterRedirectCounters() = 0;
  virtual bool RegisterDlopenCounters() = 0;
  virtual bool RegisterLoginCounters() = 0;
  virtual bool RegisterSocialCounters() = 0;
  virtual bool RegisterLoginPromptCounters() = 0;
  // The OBB-mount skip's counters (#319), registered on every launch.
  virtual bool RegisterObbSkipCounters() = 0;
  // Whether the log-only resource request hook (#871) is wanted: a development build (nevr_build::kIsDev). A release build
  // registers no counters for it and installs nothing.
  virtual bool ResourceRequestLogWanted() = 0;
  // The resource request log's counters (#871), registered when ResourceRequestLogWanted.
  virtual bool RegisterResourceRequestCounters() = 0;
  // The texture-quality hook's counters (#943), registered when the level is not `stock`.
  virtual bool RegisterTextureQualityCounters() = 0;
  virtual bool StartReporter() = 0;
  // The build-time libr15 patch (ADR 0021): Attach, then the plugin identity and the build-ID alias. True when Attach verified
  // libr15; false, with the reason token as the step's detail, for any other result. It is the first libr15 step and runs
  // every launch.
  virtual bool AttachLibR15() = 0;
  // The early plugin pass (#438); the reporter is running. True when the pass ran.
  virtual bool RunEarlyPlugins() = 0;
  // Ends every plugin entry still pending as `not_started` with `reason` (a plugin_plan.h reason token).
  virtual void FinishPlugins(const char* reason) = 0;

  virtual bool InstallClockHook() = 0;
  // `countersRegistered`: the result of RegisterObbSkipCounters (the hook's own rule: without its counters it
  // logs the skip and installs nothing). True when all three slots hold their thunks.
  virtual bool InstallObbSkip(bool countersRegistered) = 0;
  // The process ends when the game's android_main returns (#621): one libr15 slot, on every launch.
  virtual bool InstallProcessEnd() = 0;
  // The game waits for the storage permission it just asked for (#640): one libr15 slot, on every launch.
  virtual bool InstallPermissionWait() = 0;
  // The device extensions libr15 requires stay inside the 128 entries it reads (#868): two libr15 slots
  // (CSysModule::FindMethod, vrapi_GetDeviceExtensionsVulkan), on every launch.
  virtual bool InstallVkExtWindow() = 0;
  // The game never refuses a headset for its OS build number (#945): one libr15 slot (CSysInfo::OSBuildVersion), on
  // every launch.
  virtual bool InstallOsBuildGate() = 0;
  // `countersRegistered`: the result of RegisterResourceRequestCounters (the hook's own rule: without its counters it logs the
  // skip and installs nothing). True when the FillResourceFileRequest slot holds the thunk.
  virtual bool InstallResourceRequestLog(bool countersRegistered) = 0;
  // The eye-buffer size (#943): one libr15 slot, on every launch. `choice` is the level and where it came from;
  // `countersRegistered` is the result of RegisterTextureQualityCounters (a `stock` level needs none). The step applies the
  // hook's own rule (texture_quality::InstallIfCounted): without its counters it logs the skip and installs nothing.
  virtual bool InstallTextureQuality(const nevr_quest::TextureChoice& choice, bool countersRegistered) = 0;
  // The render-configuration log (#1139): five libr15 slots and the poller thread, on every launch, no counters. `choice` is
  // the texture-quality level and where it came from (the line says what set the eye size).
  virtual bool InstallRenderConfig(const nevr_quest::TextureChoice& choice) = 0;
  // The project section of the CREDITS page (#970): the patched libr15's credits slot, on every launch, no counters. True when
  // the slot holds the buffer.
  virtual bool InstallCreditsText() = 0;
  // The CREATE GAME and JOIN VIA CODE cards on the main menu (#966): one libr15 data slot (the streaming reader's read-finished
  // callback), on every launch, no counters.
  virtual bool InstallMenuCards() = 0;
  virtual bool StartTokenAuth() = 0;
  // `countersRegistered`: the result of RegisterLoginPromptCounters. The step applies the hook's own rule
  // (login_prompt::InstallIfCounted): without its counters it logs the skip and installs nothing.
  virtual bool InstallLoginPrompt(bool countersRegistered) = 0;
  virtual bool StartBridge() = 0;
  virtual bool InstallRedirect() = 0;
  virtual bool InstallSocial() = 0;
  // `login` / `matchmaking` / `plugins`: which post-load installs the dlopen hook is for (`plugins`: the normal plugin pass).
  virtual bool InstallDlopenHook(bool login, bool matchmaking, bool plugins) = 0;
  // The hardware/environment dump (#335): its libr15 hooks and its thread. No reporter counters.
  virtual bool StartHwDump() = 0;

  // One line per step outcome and per decision. `step` and `reason` are fixed tokens.
  virtual void Note(const char* step, const char* state, const char* reason) = 0;
};

// Runs the sequence. Never throws, never blocks.
ConstructorReport RunConstructorSequence(Steps& steps) noexcept;

}  // namespace nevr_quest::integration
