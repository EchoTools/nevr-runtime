/* nevr-quest crash-reporter — load-time entry glue.
 *
 * This library ships as libovrplatformloader.so. It is pulled into the process
 * as a DT_NEEDED dependency of libr15.so (and libpnsovr/libpnsrad), so its ELF
 * constructor runs before any game code — the arm64/Bionic analogue of the
 * Windows BugSplat64.dll static-import trick. The real Oculus Platform loader
 * is renamed libovrplatformloader_orig.so and pulled in via an added DT_NEEDED
 * (see the patchelf --add-needed post-build step); its 1420 exports resolve to
 * consumers through Bionic's local-group symbol lookup.
 */

#include "activation.h"
#include "hand_flip.h"
#include "sentinel.h"
#include "sentinel_log.h"
#include "generated/nevr_build_info.h"
#include "got_hook.h"
#include "clock_levels_hook.h"
#include "hook_install.h"
#include "hook_log.h"
#include "hook_report.h"
#include "login_prompt_hook.h"
#include "login_title_hook.h"
#include "core/crash_report.h"
#include "quest/cargo/cargo_service.h"
#include "quest/combat/combat_active.h"
#include "quest/combat/combat_install.h"
#include "quest/combat/equip_refire.h"
#include "quest/combat/match_tags.h"
#include "build_variant.h"
#include "quest/invite/frame_dispatch.h"
#include "main_level.h"
#include "menu_cards_hook.h"
#include "quest/invite/wizard_metric_hook.h"
#include "quest/invite/join_game_quest.h"
#include "data_wiring.h"
#include "obb_skip_hook.h"
#include "os_build_gate_hook.h"
#include "permission_request.h"
#include "permission_wait_hook.h"
#include "crash_first_hook.h"
#include "quest/integration/dlopen_hook.h"
#include "quest/integration/post_load.h"
#include "pns_provider_hook.h"
#include "process_end_hook.h"
#include "hand_pose_log_hook.h"
#include "quit_hook.h"
#include "render_config.h"
#include "texture_menu_hook.h"
#include "texture_quality_hook.h"
#include "vk_alloc_check.h"
#include "vk_free_guard.h"
#include "resource_request_hook.h"
#include "toast_presenter.h"
#include "vk_ext_window.h"
#include "pinned_targets.h"
#include "quest/integration/ctor_sequence.h"
#include "quest/integration/entry_hooks.h"
#include "quest/redirect/hook_adapter.h"

#include <jni.h>

#include <atomic>
#include <cstdint>

namespace {

// "The basics" GOT hook: prove we can intercept a real call libr15.so makes on
// live hardware, without needing libr15.so's own internal functions
// reconstructed first. clock_gettime is chosen deliberately: its signature is
// unambiguous POSIX (no risk of a wrong-arity/wrong-return-type call corrupting
// the engine's real args), and it's called continuously by any real-time engine
// loop. The install line in the log says the slot was patched; the reporter thread
// (hook_report.h) logs the call counter when it first moves and, if it keeps moving, at
// most once a minute.
//
// This translation unit is built with -fno-exceptions (callback_thunk.h requires
// it): the handler is noexcept and nothing in it can unwind.
using ClockThunk = sentinel::pinned::ClockGettimeThunk;
sentinel::GotHook     g_clockHook;
std::atomic<uint64_t> g_clockGettimeCalls{0};

int HookedClockGettime(ClockThunk::Fn original, clockid_t clk_id, struct timespec* tp) noexcept {
    // Runs on every clock_gettime libr15 makes, on any thread, possibly from a signal
    // handler: one atomic increment and the original call, nothing else.
    g_clockGettimeCalls.fetch_add(1, std::memory_order_relaxed);
    return original(clk_id, tp);
}

// The hook: the thunk's entry and its handler, recorded for the build-time frame sensor.
NEVR_HOOK_RECORD(kClockHook, ClockThunk, &HookedClockGettime);

}  // namespace

namespace nevr_quest::integration {

// The counters are registered by the constructor sequence together with every other hook's, before
// the single StartReporter (ctor_sequence.h).
bool RegisterClockCounters() noexcept {
    bool ok = sentinel::RegisterReportCounter("clock_gettime_calls", &g_clockGettimeCalls);
    ok = sentinel::RegisterReportCounter("clock_gettime_thunk_faults", &ClockThunk::FaultCounter(),
                                         sentinel::ReportKind::kFaults) && ok;
    return ok;
}

// A failed install is logged by GotHook with its status and leaves the original
// call intact; it is never fatal to the host process.
bool InstallClockHook() noexcept {
    ClockThunk::Arm(kClockHook);
    return sentinel::InstallThunk<ClockThunk>(g_clockHook, sentinel::pinned::LibR15ClockGettime()) ==
           sentinel::GotStatus::kOk;
}

// The frame dispatcher (#966 S7): the Update slot is taken on every launch, so the dispatcher runs whether or not the
// login prompt is on. LogInstall writes the one line that says which (installed, or off with the GOT status).
bool InstallFrameDispatchHook() noexcept {
    const sentinel::GotStatus status = nevr_quest::login_prompt::InstallNetGameUpdate();
    nevr_quest::invite::frame::LogInstall(status == sentinel::GotStatus::kOk, sentinel::GotStatusName(status));
    return status == sentinel::GotStatus::kOk;
}

// The login-failed page's title hook (#1137) is the prompt's second half: its counters register in the same step, so the
// reporter holds both or the step reports the refusal and the prompt installs nothing.
bool RegisterLoginPromptCounters() noexcept {
    const bool prompt = nevr_quest::login_prompt::RegisterCounters();
    const bool title = nevr_quest::login_title::RegisterCounters();
    return prompt && title;
}

bool RegisterObbSkipCounters() noexcept { return nevr_quest::obb_skip::RegisterCounters(); }

// The OBB-mount skip (#319). A refused install is logged by GotHook and leaves the game's call intact.
bool InstallObbSkipHook(bool countersRegistered) noexcept {
    return nevr_quest::obb_skip::InstallIfCounted(countersRegistered);
}

// The process ends when the game's android_main returns (#621). A refused install is logged by GotHook and leaves
// the game's call intact.
void SetProcessEndCallback(void (*callback)() noexcept) noexcept { nevr_quest::process_end::SetBeforeExit(callback); }
bool InstallProcessEndHook() noexcept { return nevr_quest::process_end::Install(); }

// Our crash handler stays first (#1605): libr15's sigaction and signal slots keep the game's crash handler from displacing it,
// and the dlopen hook (live on every launch from here) hooks the same two slots of libpnsrad, libpnsradmatchmaking and
// libpnsovr when the game loads them. A refused install is logged by GotHook and leaves the game's calls intact.
bool InstallCrashFirstHook() noexcept {
    const bool slots = nevr_quest::crash_first::Install();
    nevr_quest::integration::SetModuleLoadedObserver(&nevr_quest::crash_first::OnModuleLoaded);
    sentinel::GotStatus status = sentinel::GotStatus::kNotInstalled;
    const bool dlopenLive = nevr_quest::integration::InstallDlopenHook(&status) == nevr_quest::integration::DlopenInstall::kOk;
    sentinel::LogFields(dlopenLive ? sentinel::LogLevel::kInfo : sentinel::LogLevel::kError, "crash_first_late_install",
                        {{"result", dlopenLive ? "armed" : "dlopen_hook_refused"}, {"status", sentinel::GotStatusName(status)}});
    return slots && dlopenLive;
}

namespace {
// The game's permission request on its own activity, with the hook's request code (#947): the activity is the one
// android_main was given (process_end_hook.h).
const char* RequestOnGameActivity(const char* const* names, std::uint64_t count, int requestCode) noexcept {
    return nevr_quest::permission_request::Request(nevr_quest::process_end::CurrentApp(), names, count, requestCode);
}
}  // namespace

// The game waits for the storage permission it just asked for (#640). A refused install is logged by GotHook and
// leaves the game's call intact. The first-run data step (#807, data_wiring.h) runs right after that hook's wait, so it
// is armed first, and so is the requester that makes the request with a defined request code (#947).
bool InstallPermissionWaitHook() noexcept {
    nevr_quest::data_wiring::Options options;
    options.show = &nevr_quest::toast::ShowPreferUiThread;  // #829: the UI thread's looper runs the Toast, else the caller's
    options.show_ui = &nevr_quest::toast::ShowOnMainLooper;  // read by the development probe's `ui-toast` kind only (#829)
    options.show_post = &nevr_quest::toast::ShowOnUiThread;  // and `ui-post`
    options.allow_fetch = nevr_build::kIsDev;  // a release build only looks and tells the player; it never downloads
    // `assets.whole_game` (quest_config.h): the CDN holds every asset of the game, so the data step writes the stub manifests and
    // fetches no data zip, in any build. The configuration is resolved before this step runs (ctor_sequence.cpp).
    // The download and the mount that must finish before the step returns (`all_assets_ready`, docs/adr/0045 decision 11): the first-run
    // prefetch of every manifest object into the cargo store, then the mount of the shadow the engine's first archive load reads. It is set
    // beside the mode and from the same configuration, so a launch with the mode on never reaches the step without it.
    options.all_assets = sentinel::ActiveConfig().assets.wholeGame;
    options.all_assets_ready = nevr_quest::cargo::MakeProductionFirstRunWorker(sentinel::ActiveConfig().assets);
    options.probe_enabled = nevr_build::kIsDev;  // the headset probe is for development builds only
    if (!nevr_build::kIsDev) options.hold_ms = 0;  // no presenter is known to draw on a headset: a release build holds nothing
    nevr_quest::data_wiring::Install(options);
    // The main-menu QUIT finishes the activity (#1276), installed on every launch from this step. Its quit_install line
    // says whether it went in; a refused install leaves the game's QUIT as it is.
    nevr_quest::quit::Install();
    // The controller type and raw pose log (#1986): two hooks and a poller thread; a pose is changed only while Controller Hand Flip
    // is On (hand_flip.h, off by default; KNOWN-WRONG #2043 the toggle is the interim). Its hand_pose_log_install line says whether
    // it went in; a refused install leaves the game's calls as they are. The toggle's choice is the configuration's graphics.hand_flip
    // (#2082): config.yaml's path and what the reader found are handed over before the hooks that read them exist.
    const nevr_quest::HandFlipChoice& handFlip = sentinel::ActiveConfig().handFlip;
    nevr_quest::hand_flip::Configure(nevr_quest::ConfigFilePath(sentinel::FilesDir()).c_str(),
                                     handFlip.source == nevr_quest::Source::kFile, handFlip.on);
    nevr_quest::hand_pose_log::Install();
    nevr_quest::permission_wait::SetRequester(&RequestOnGameActivity);
    nevr_quest::permission_wait::SetStorageNeeded(&nevr_quest::data_wiring::StorageNeeded);
    return nevr_quest::permission_wait::Install();
}

// Combat name tags on by default (#2090): CR15Game's two constructor slots, installed before the game builds its game object (the
// sentinel's constructor runs while libr15 is being loaded). A copy of the settings the constructor left at the stock default gets
// bit 13; a saved choice still wins, because LoadSettings runs after the constructor. Its combat_match_tags_install line says whether
// it went in; a refused install leaves the game's constructor as it is. The sequence asks only where combat runs (docs/adr/0047).
bool InstallMatchTagsHook() noexcept { return nevr_quest::combat::match_tags::Install(); }

// The equipment change event the scripts dropped is sent again (#2121): the six words that hold CR15NetPlayerEquipmentCS's three
// setters, a word that holds anything but the setter refused on its own, and a frame work on the game thread. Its
// combat_equip_refire_install line says which words went in; a refused install leaves the game's calls as they are. The sequence
// asks only where combat runs (docs/adr/0047).
bool InstallEquipRefireHook() noexcept { return nevr_quest::combat::equip_refire::Install(); }

// The Vulkan extension window (#868): the device extensions libr15 requires stay inside the 128 entries it reads. A
// refused install is logged by GotHook and leaves the game's calls intact.
bool InstallVkExtWindowHook() noexcept {
    // Libr15's array of free GPU ranges has no bound check (#1759): a free that would overflow it leaks the range instead. Its
    // vk_free_guard_install line says whether it went in; a refused install leaves the game's Free as it is.
    nevr_quest::vk_free_guard::Install();
    // Libr15's Allocate does not read vkAllocateMemory's result (#2262): an allocation reported as made with no memory is refused,
    // so CreateDeviceResource never binds it. Its vk_alloc_check_install line says whether it went in.
    nevr_quest::vk_alloc_check::Install();
    return nevr_quest::vk_window::Install();
}

// The game never refuses a headset for its OS build number (#945). A refused install is logged by GotHook and by the
// hook's own line, and leaves the game's call intact.
bool InstallOsBuildGateHook() noexcept { return nevr_quest::os_build_gate::Install(); }

// The eye-buffer size libr15 creates its swap chain with (#943). A refused install is logged by GotHook and by the hook's
// own line, and leaves the game's calls intact.
bool RegisterTextureQualityCounters() noexcept {
    const bool quality = nevr_quest::texture_quality::RegisterCounters();
    const bool menu = nevr_quest::texture_menu::RegisterCounters();  // both are asked: one refusal must not hide the other
    return quality && menu;
}
// The eye-size hook, then the Texture Resolution control and the live apply (#1012), which need the eye-size hook installed:
// the control is offered only when it has answered for the first query (texture_menu_hook.h).
bool InstallTextureQualityHook(nevr_quest::texture_quality::Level level, bool fromConfig,
                               bool countersRegistered) noexcept {
    if (!nevr_quest::texture_quality::InstallIfCounted(level, fromConfig, countersRegistered)) return false;
    // A level chosen in the menu is written to the same config.yaml the key was read from.
    nevr_quest::texture_menu::SetConfigFile(nevr_quest::ConfigFilePath(sentinel::FilesDir()).c_str());
    return nevr_quest::texture_menu::Install(level, fromConfig);
}

// The render-configuration log (#1139): the VrApi slots it reads and the poller thread. A refused install is logged by GotHook
// and by the hook's own line, and leaves the game's calls intact.
bool InstallRenderConfigHook(nevr_quest::texture_quality::Level level, bool fromConfig) noexcept {
    return nevr_quest::render_config::Install(level, fromConfig);
}

// The resource request hook (#871, #1737 C5): installed in every build; only its per-request log lines are development builds only; a release build logs failed requests, rate limited
// (ResourceRequestLogEnabled, the `logLines` argument). A refused install is logged by GotHook and leaves the game's call intact.
bool ResourceRequestLogEnabled() noexcept { return nevr_build::kIsDev; }
bool RegisterResourceRequestCounters() noexcept { return nevr_quest::resource_request::RegisterCounters(); }
bool InstallResourceRequestLogHook(bool countersRegistered, bool logLines) noexcept {
    return nevr_quest::resource_request::InstallCounted(countersRegistered, logLines);
}

// The clock-level override (#928). It owns no slot: the render-configuration hook owns vrapi_SetClockLevels and asks the
// override which levels to call with. The counters are registered with the others, before the single StartReporter.
bool RegisterClockLevelsCounters() noexcept { return nevr_quest::clock_levels::RegisterCounters(); }
bool ArmClockLevelsHook(bool countersRegistered, bool overrideOn) noexcept {
    return nevr_quest::clock_levels::ArmIfCounted(countersRegistered, overrideOn);
}
// The rad provider in pnsovr's place (#1220): development builds only. A refused install is logged by GotHook and by the
// hook's own line, and leaves the game's provider in place.
bool PnsProviderEnabled() noexcept { return nevr_build::kIsDev; }
bool InstallPnsProviderHook() noexcept { return nevr_quest::pns_provider::Install(); }

// The CREATE GAME and JOIN VIA CODE cards (#966): a data-slot hook on the streaming reader's read-finished callback. A
// refused install is logged by GotHook and by the hook's own line, and leaves the game's calls intact.
bool InstallMenuCardsHook() noexcept { return nevr_quest::menu_cards::Install(); }

// The CREATE GAME / JOIN VIA CODE observer (#966, ADR 0017 S2) on libr15's MetricLog. A refused install is logged by GotHook and by
// the observer's own line, and leaves the game's call intact.
// The join's answer applier (#966 S4) is registered with the observer. It acts only while the invite service is on (the
// network.invite_codes feature), so with the feature off it takes no answers and logs nothing.
bool InstallWizardMetricHook() noexcept {
    const bool observer = nevr_quest::wizard_metric::Install();
    // A full frame dispatcher refuses the applier; the join then takes no answer, so the refusal is logged here.
    if (!nevr_quest_invite_join::RegisterAnswerApplier()) {
        sentinel::LogFields(sentinel::LogLevel::kWarn, "invite_answer_applier",
                            {{"tag", "NEVR.INVITE"}, {"result", "off"}, {"reason", "frame_work_full"}});
    }
    return observer;
}

// The game's main level for the plugins (#1106). The source and the frame work register once; a refused frame work is logged
// by main_level::Install, and the plugins then keep the level they were given at init.
bool InstallPluginMainLevel() noexcept { return nevr_quest::main_level::Install(); }

// The gate on the folded combat modules (#2170, combat/combat_active.h): the answer is made here, once the configuration is resolved, from
// the variant the constructor recorded, this build's kind and the configuration's combat.force. entry.cpp holds nevr_build::kIsDev.
void ResolveCombatActive(bool forceAsked) noexcept { nevr_quest::combat::Resolve(nevr_build::kIsDev, forceAsked); }

// The combat area's steps (#2171, combat/combat_install.h): each asks combat::Active() itself, so a call that reaches one in a process
// where combat does not run installs nothing and says so once.
bool CombatActive() noexcept { return nevr_quest::combat::Active(); }
bool InstallCombatEarly() noexcept { return nevr_quest::combat::InstallEarly().ran; }
bool InstallCombatNormal() noexcept { return nevr_quest::combat::InstallNormal().ran; }
void NoteCombatNotStarted(const char* reason) noexcept { nevr_quest::combat::NoteNormalNotStarted(reason); }
void NoteCombatProcessEnd() noexcept { nevr_quest::combat::NoteProcessEnd(); }
void LogCombatConfig(const nevr_quest::CombatConfig& config) noexcept {
  nevr_quest::combat::SetConfig(config);  // the modules' switches, read by the install steps that follow
  nevr_quest::combat::LogConfig(config);
}

// The sign-in prompt in the game's login-error text (#239). Passes the game's message through until
// token auth publishes a prompt; a refused install is logged by GotHook and leaves the call intact.
bool InstallLoginPromptHook(bool countersRegistered) noexcept {
    return nevr_quest::login_prompt::InstallIfCounted(countersRegistered);
}

// The login prompt's counters, written once at process exit (#1391).
void FlushLoginPromptCountersAtExit() noexcept { nevr_quest::login_prompt::FlushCountersAtExit(); }

// The title of the login-failed page (#1137): installed after the game maps the page's script library, so the
// post-load action asks again after each dlopen until it answers anything but kModuleNotLoaded. A refused install is
// logged by GotHook and by the hook's own line and leaves the game's constructor call intact.
sentinel::GotStatus InstallLoginTitleHook() noexcept { return nevr_quest::login_title::Install(); }

}  // namespace nevr_quest::integration

extern "C" {

// Ground-truth marker export: proves our code is in the artifact (BAC-2).
__attribute__((visibility("default")))
const char* nevr_sentinel_marker() {
    return "nevr-quest-sentinel v1 (libovrplatformloader hijack, breakpad)";
}

// Earliest reachable hook: runs while the dynamic linker resolves libr15's
// DT_NEEDED closure, before libr15's JNI_OnLoad / ANativeActivity_onCreate.
__attribute__((constructor))
static void nevr_sentinel_ctor() {
    // The crash handlers first (#1689): a crash anywhere below, the begin record and the UI looper capture included, leaves a
    // report the next start sends. Arm is idempotent: the sequence's first step calls it again after setting the log sink.
    sentinel::Arm();
    // The variant the repack stamped beside this library (#2170, build_variant.h), read before the first line so that line names it. A
    // stamp that is missing, unreadable or wrong is no variant; the begin record says which. The combat gate takes the word now and
    // makes its answer when the configuration is resolved (ResolveCombatActive).
    const nevr_quest::build_variant::Stamp variant = nevr_quest::build_variant::ReadOwnStamp();
    nevr_quest::combat::SetVariant(variant.variant);
    // The crash report names the build too (core/crash_report.h, ADR 0040 rule 2): its `build:` line is set now, so a crash anywhere below carries it.
    char buildText[nevr_crash::kBuildBytes];
    std::snprintf(buildText, sizeof(buildText), "version=%s kind=%s variant=%s", nevr_build::kVersion,
                  nevr_build::kIsDev ? "dev" : "release", nevr_quest::build_variant::VariantName(variant.variant));
    nevr_crash::SetBuild(buildText);
    nevr_quest::integration::LogConstructorBegin(nevr_build::kVersion, nevr_build::kCommit,
                                                      nevr_build::kIsDev ? "dev" : "release", variant);
    // The login_host fallback carries this build's tuple (login_fallback.h): set before the sequence installs the redirect.
    nevr_quest::redirect::SetLoginIdentity(
        {"nevr_quest", nevr_build::kIsDev ? "dev" : "release", nevr_build::kVersion, nevr_build::kCommit});
    // #829: the thread that loads the library takes the UI looper for the floor message's presenter (it logs what it found).
    nevr_quest::toast::CaptureUiLooper();
    // Everything the constructor does is one sequence (integration/ctor_sequence.h): crash reporter,
    // configuration, counters, the single reporter start, then each hook gated by its feature switch.
    nevr_quest::integration::RunSentinelConstructor();
}

// Belt-and-suspenders: if anything System.loadLibrary's this by name, arm here
// too. (Under DT_NEEDED loading the runtime does not auto-call this.)
JNIEXPORT jint JNI_OnLoad(JavaVM* /*vm*/, void* /*reserved*/) {
    sentinel::Arm();
    return JNI_VERSION_1_6;
}

}  // extern "C"
