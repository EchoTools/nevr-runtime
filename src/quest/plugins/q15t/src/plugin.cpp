// The plugin's entry points (the ABI of extension/plugin_interface.h, Quest additions of ADR 0019 section 4).
//
// No namespace-scope object has a constructor: the repack's static-init check refuses a plugin that has one.
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "aibots.h"
#include "bvh.h"
#include "clock_levels.h"
#include "custctx.h"
#include "decals.h"
#include "meshstep.h"
#include "droneguard.h"
#include "dronekill.h"
#include "dronemuzzle.h"
#include "ec.h"
#include "ecfx.h"
#include "nevr/plugin_interface.h"
#include "fxtrail.h"
#include "game_tick.h"
#include "gunconnect.h"
#include "headset_gates.h"
#include "hudmeter.h"
#include "impactsnd.h"
#include "instmodel.h"
#include "levelname.h"
#include "boosterpool.h"
#include "music.h"
#include "peerclamp.h"
#include "pfx.h"
#include "plugin_args.h"
#include "plugin_log.h"
#include "powerboost.h"
#include "refresh_rate.h"
#include "script_redirect.h"
#include "sensorwd.h"
#include "shaderstep.h"
#include "shieldgamma.h"
#include "sendguard.h"
#include "tablestep.h"
#include "combat_gates.h"
#include "voipdrop.h"
#include "wallfire.h"
#include "weapon_arm.h"

#ifndef Q15T_BUILD_ID
#define Q15T_BUILD_ID "b243509c08ce677aeb95fa348016949b3fc45230"
#endif
// ELF vaddr of the game library's JUMP_SLOT for vrapi_SetClockLevels (readelf -rW of the pinned build).
#ifndef Q15T_SLOT_SETCLOCKLEVELS
#define Q15T_SLOT_SETCLOCKLEVELS 0x36c75f0ULL
#endif
#ifndef Q15T_SLOT_SETDISPLAYREFRESHRATE
#define Q15T_SLOT_SETDISPLAYREFRESHRATE 0x36ec5f0ULL
#endif
// Gameplay gates (off in the perf arms): the party-permissions expression and the bot-body chooser stop treating the
// Quest headset as a reason to refuse combat / pick the arena bot. ELF vaddrs of the pinned build (readelf -rW, disassembly).
#ifndef Q15T_GATE_PERM
#define Q15T_GATE_PERM 0
#endif
#ifndef Q15T_GATE_BOTS
#define Q15T_GATE_BOTS 0
#endif
#ifndef Q15T_SLOT_PARTYMEMBERHEADSETTYPE
#define Q15T_SLOT_PARTYMEMBERHEADSETTYPE 0x36fd880ULL
#define Q15T_REAL_PARTYMEMBERHEADSETTYPE 0x129b168ULL
#endif
#ifndef Q15T_SLOT_HEADSETTYPE
#define Q15T_SLOT_HEADSETTYPE 0x36f9fd8ULL
#define Q15T_REAL_HEADSETTYPE 0x1a6f164ULL
#endif
#ifndef Q15T_PERM_FN_LO
#define Q15T_PERM_FN_LO 0x232c794ULL
#define Q15T_PERM_FN_HI 0x232c854ULL
#endif
// The menu gate (default ON in the player build): CVR::HeadsetType's answer to the headset-type expression's Bind (ELF 0x202ba74..0x202bac8,
// disassembly 0x212ba74, 84 bytes) is the PC build's Link kind (4), so the Bind sets the link flag and not the Quest flag and no menu script is told it runs on a Quest headset (R1_functions.md:90).
#ifndef Q15T_GATE_LINK
#define Q15T_GATE_LINK 0
#endif
#ifndef Q15T_LEVELNAME
#define Q15T_LEVELNAME 0
#endif
#ifndef Q15T_LINK_GATE_LIBS
#define Q15T_LINK_GATE_LIBS "none"  // the CMake default names the combat menu scripts
#endif
#ifndef Q15T_LINK_FN_LO
#define Q15T_LINK_FN_LO 0x202ba74ULL
#define Q15T_LINK_FN_HI 0x202bac8ULL
#endif
#ifndef Q15T_BOTS_FN_LO
#define Q15T_BOTS_FN_LO 0x124dd38ULL
#define Q15T_BOTS_FN_HI 0x124e004ULL
#endif
#ifndef Q15T_HZ_MATCH90
#define Q15T_HZ_MATCH90 0
#endif
// T-26: a new profile starts with the gun on the right hand, once per user id (weapon_arm.h). Plugin behaviour.
#ifndef Q15T_T26
#define Q15T_T26 0
#endif
// Gameplay and fix modules (src/<m>.{h,cpp}, docs/quest/q15t/modules/<m>.md). CMakeLists.txt owns the options and their defaults (each with the
// reason, from the module's doc); like Q15T_T26, the default HERE is 0, so a build that names no option installs none of them and the
// perf arms and the host-test arms stay unchanged. heaps and memsys are not wired: they need the loader's early phase, which a plugin
// that also needs the loaded game library cannot share (docs/quest/q15t/modules/heaps.md section 1; CMakeLists.txt).
#ifndef Q15T_FXTRAIL
#define Q15T_FXTRAIL 0
#endif
#ifndef Q15T_VOIPDROP
#define Q15T_VOIPDROP 0
#endif
#ifndef Q15T_CUSTCTX
#define Q15T_CUSTCTX 0
#endif
#ifndef Q15T_DRONEGUARD
#define Q15T_DRONEGUARD 0  // Q15T_DRONEGUARD_IDX (the second stage) is read by droneguard.cpp itself
#endif
#ifndef Q15T_PEERCLAMP
#define Q15T_PEERCLAMP 0
#endif
#ifndef Q15T_GUNCONNECT
#define Q15T_GUNCONNECT 0
#endif
#ifndef Q15T_AIBOTS
#define Q15T_AIBOTS 0
#endif
#ifndef Q15T_BVH
#define Q15T_BVH 0
#endif
#ifndef Q15T_WALLFIRE  // needs Q15T_BVH
#define Q15T_WALLFIRE 0
#endif
#ifndef Q15T_IMPACTSND
#define Q15T_IMPACTSND 0
#endif
#ifndef Q15T_EC
#define Q15T_EC 0
#endif
#ifndef Q15T_ECFX  // needs Q15T_EC
#define Q15T_ECFX 0
#endif
#ifndef Q15T_DRONEKILL
#define Q15T_DRONEKILL 0
#endif
#ifndef Q15T_DRONEMUZZLE  // needs Q15T_DRONEKILL
#define Q15T_DRONEMUZZLE 0
#endif
#ifndef Q15T_POWERBOOST  // 0 off, 1 read-only, 2 swallow, 3 cone (cone sends from ec's net tick: needs Q15T_EC)
#define Q15T_POWERBOOST 0
#endif
#ifndef Q15T_HUDMETER
#define Q15T_HUDMETER 0
#endif
#ifndef Q15T_TABLESTEP  // issue #1552: shader-set table growth step 32 -> 1 (tablestep.cpp)
#define Q15T_TABLESTEP 0
#endif
#ifndef Q15T_SENDGUARD  // script send to a game space that is not loaded: dropped, not faulted (sendguard.cpp)
#define Q15T_SENDGUARD 0
#endif
#ifndef Q15T_SHIELDGAMMA  // bitmask: 1 gamma, 2 tube mirror
#define Q15T_SHIELDGAMMA 0
#endif
#ifndef Q15T_SHADERSTEP
#define Q15T_SHADERSTEP 0
#endif
#ifndef Q15T_MUSIC
#define Q15T_MUSIC 0
#endif
#ifndef Q15T_SENSORWD
#define Q15T_SENSORWD 0
#endif
#ifndef Q15T_INSTMODEL  // CGInstancedModelResource's constructor with growth step 1 (instmodel.h)
#define Q15T_INSTMODEL 0
#endif
#ifndef Q15T_DECALS  // the level decals of decals.bin as dynamic-mesh quads (decals.h); after impactsnd: it chains on the Shutdown wrapper
#define Q15T_DECALS 0
#endif
#ifndef Q15T_PFX  // the particle-effect renderer's gate, read by the pfx* modules (pfx.h, issue #1631)
#define Q15T_PFX 0
#endif
// The game-thread tick: CncaGame::ExclusiveUpdateGame's JUMP_SLOT, its function, and CR15Game's vptr value (R17; q15t
// the source build's gunconnect kGotExclusiveUpdateGame / kFnExclusiveUpdateGame / kGameVptr).
#ifndef Q15T_SLOT_EXCLUSIVEUPDATEGAME
#define Q15T_SLOT_EXCLUSIVEUPDATEGAME 0x36e7f28ULL
#define Q15T_REAL_EXCLUSIVEUPDATEGAME 0x17ef62cULL
#define Q15T_GAME_VPTR 0x362ed30ULL
#endif

NEVR_DECLARE_SUPPORTED_BUILD_IDS(Q15T_BUILD_ID);

// What this binary was compiled with, as one string in .rodata: `just build-quest-q15t` reads it back from the built library and
// refuses a build whose defaults are not the shipped ones (a stale CMake cache once built a library with the ec words empty).
#ifdef Q15T_EC_DEFAULT_WORDS
#define Q15T_MARK_EC_WORDS Q15T_EC_DEFAULT_WORDS
#else
#define Q15T_MARK_EC_WORDS ""
#endif
#ifndef Q15T_ECFX_DEFAULT_ON
#define Q15T_ECFX_DEFAULT_ON 0
#endif
#define Q15T_MARK_STR2(x) #x
#define Q15T_MARK_STR(x) Q15T_MARK_STR2(x)
extern "C" __attribute__((used, retain, visibility("hidden"))) const char q15t_config_marker[] =
    "q15t-config ec_words=[" Q15T_MARK_EC_WORDS "] ecfx_default_on=" Q15T_MARK_STR(Q15T_ECFX_DEFAULT_ON)
    " gate_link=" Q15T_MARK_STR(Q15T_GATE_LINK) " gate_perm=" Q15T_MARK_STR(Q15T_GATE_PERM) " gate_bots=" Q15T_MARK_STR(Q15T_GATE_BOTS)
    " levelname=" Q15T_MARK_STR(Q15T_LEVELNAME) " hz_level=" Q15T_MARK_STR(Q15T_HZ_MATCH90) " build_id=" Q15T_BUILD_ID;

namespace {

void Report(const char* hook, const q15t::InstallResult& r) {
  using namespace q15t;
  const NevrLogField done[] = {{"hook", hook},
                               {"status", StatusName(r.status)},
                               {"reason", r.reason},
                               {"real_resolved", r.real_resolved ? "1" : "0"}};
  Log(r.status == SwapStatus::kOk ? NEVR_LOG_INFO : NEVR_LOG_ERROR, "hook install", done, 4);
}

bool Covers(const NvrGameContext* ctx, std::size_t end_of_field) { return ctx->ctx_size >= end_of_field; }

// Every compiled-in module, by name, for the init line.
constexpr char kCompiledModules[] =
#if Q15T_FXTRAIL
    "fxtrail "
#endif
#if Q15T_VOIPDROP
    "voipdrop "
#endif
#if Q15T_CUSTCTX
    "custctx "
#endif
#if Q15T_DRONEGUARD
    "droneguard "
#endif
#if Q15T_PEERCLAMP
    "peerclamp "
#endif
#if Q15T_GUNCONNECT
    "gunconnect "
#endif
#if Q15T_AIBOTS
    "aibots "
#endif
#if Q15T_BVH
    "bvh "
#endif
#if Q15T_WALLFIRE
    "wallfire "
#endif
#if Q15T_IMPACTSND
    "impactsnd "
#endif
#if Q15T_EC
    "ec "
#endif
#if Q15T_ECFX
    "ecfx "
#endif
#if Q15T_DRONEKILL
    "dronekill "
#endif
#if Q15T_DRONEMUZZLE
    "dronemuzzle "
#endif
#if Q15T_POWERBOOST
    "powerboost "
#endif
#if Q15T_HUDMETER
    "hudmeter "
#endif
#if Q15T_SHIELDGAMMA
    "shieldgamma "
#endif
#if Q15T_TABLESTEP
    "tablestep "
#endif
#if Q15T_SENDGUARD
    "sendguard "
#endif
#if Q15T_LEVELNAME
    "levelname "
    "boosterpool "
#endif
#if Q15T_MUSIC
    "music "
#endif
#if Q15T_SENSORWD
    "sensorwd "
#endif
#if Q15T_INSTMODEL
    "instmodel "
#endif
#if Q15T_SHADERSTEP
    "shaderstep "
#endif
#if Q15T_DECALS
    "decals "
#endif
#if Q15T_PFX
    "pfx "
#endif
    "";

// One structured line per module install: INFO when it installed, INFO when its own off switch kept it out (reason "off" or
// "off_file": a choice, not a failure), ERROR with the module name and the reason otherwise.
[[maybe_unused]] void ReportModule(const char* module, const q15t::InstallResult& r) {
  using namespace q15t;
  const bool ok = r.status == SwapStatus::kOk;
  const bool off = !ok && (std::strcmp(r.reason, "off") == 0 || std::strcmp(r.reason, "off_file") == 0);
  // A refusal always names a reason: the module's token, else the swap status that stopped it.
  const char* reason = r.reason[0] != 0 || ok ? r.reason : StatusName(r.status);
  const NevrLogField f[] = {{"module", module},
                            {"status", off ? "off" : StatusName(r.status)},
                            {"reason", reason},
                            {"real_resolved", r.real_resolved ? "1" : "0"}};
  Log(ok || off ? NEVR_LOG_INFO : NEVR_LOG_ERROR, ok || off ? "module install" : "module install FAILED", f, 4);
}

// A module that could not even be attempted (an engine export is missing, a module it needs is not in).
[[maybe_unused]] void ReportModuleRefused(const char* module, const char* reason) {
  using namespace q15t;
  const NevrLogField f[] = {{"module", module}, {"status", "not_installed"}, {"reason", reason}, {"real_resolved", "0"}};
  Log(NEVR_LOG_ERROR, "module install FAILED", f, 4);
}

// A module with no install of its own (its work resolves on the first game tick): its registration is its install.
[[maybe_unused]] void ReportModuleTick(const char* module, bool registered, bool* want_tick) {
  using namespace q15t;
  const NevrLogField f[] = {{"module", module},
                            {"status", registered ? "tick_registered" : "not_installed"},
                            {"reason", registered ? "resolves on the first game tick" : "game tick table full"},
                            {"real_resolved", "0"}};
  Log(registered ? NEVR_LOG_INFO : NEVR_LOG_ERROR, registered ? "module install" : "module install FAILED", f, 4);
  if (registered) *want_tick = true;
}

// A tick for a module whose install went through. A full table is an ERROR naming the module.
[[maybe_unused]] void AddModuleTick(const char* module, q15t::TickFn fn, bool* want_tick) {
  using namespace q15t;
  if (AddGameTick(fn)) {
    *want_tick = true;
    return;
  }
  const NevrLogField f[] = {{"module", module}, {"status", "no_tick"}, {"reason", "game tick table full"}, {"real_resolved", "0"}};
  Log(NEVR_LOG_ERROR, "module install FAILED", f, 4);
}

// The first install that did not go through, for modules that report several hooks.
[[maybe_unused]] q15t::InstallResult FirstFailure(bool ok, const q15t::InstallResult* hooks, int n) {
  if (ok) {
    q15t::InstallResult r;
    r.status = q15t::SwapStatus::kOk;
    return r;
  }
  for (int i = 0; i < n; ++i)
    if (hooks[i].status != q15t::SwapStatus::kOk) return hooks[i];
  q15t::InstallResult r;
  r.reason = "rolled_back";
  return r;
}

}  // namespace

NEVR_PLUGIN_API NvrPluginInfo NvrPluginGetInfo(void) {
  return {"q15t", "display clock and refresh-rate witnesses and candidates", 0, 2, 0};
}
NEVR_PLUGIN_API uint32_t NvrPluginGetApiVersion(void) { return NEVR_PLUGIN_API_VERSION; }
NEVR_PLUGIN_API uint32_t NvrPluginGetCapabilities(void) {
  uint32_t caps = NEVR_PLUGIN_CAP_HOOKS_ENGINE;
  // The clock raise (args {clock: raise}) is a runtime switch the loader cannot see here, and it is not gameplay: it asks the headset
  // for higher CPU/GPU levels (performance and heat, no physics, weapons, movement or rules). It is not part of the declaration.
  if (Q15T_HZ_MATCH90 || Q15T_T26) caps |= NEVR_PLUGIN_CAP_ALTERS_GAMEPLAY;
  // Modules that change what a player experiences or what the game computes (per their docs: droneguard and peerclamp say so; the
  // others skip, add or answer game logic). Not here: fxtrail, voipdrop, custctx (crash guards), impactsnd (sound ids), music (audio events), sensorwd (a stuck scanner sphere), hudmeter and
  // shieldgamma (what is drawn), ecfx (rides ec).
  if (Q15T_DRONEGUARD) caps |= NEVR_PLUGIN_CAP_ALTERS_GAMEPLAY;
  if (Q15T_PEERCLAMP) caps |= NEVR_PLUGIN_CAP_ALTERS_GAMEPLAY;
  if (Q15T_DRONEKILL) caps |= NEVR_PLUGIN_CAP_ALTERS_GAMEPLAY;
  if (Q15T_DRONEMUZZLE) caps |= NEVR_PLUGIN_CAP_ALTERS_GAMEPLAY;
  if (Q15T_POWERBOOST > 0) caps |= NEVR_PLUGIN_CAP_ALTERS_GAMEPLAY;
  if (Q15T_AIBOTS) caps |= NEVR_PLUGIN_CAP_ALTERS_GAMEPLAY;
  if (Q15T_GUNCONNECT) caps |= NEVR_PLUGIN_CAP_ALTERS_GAMEPLAY;
  if (Q15T_BVH) caps |= NEVR_PLUGIN_CAP_ALTERS_GAMEPLAY;       // walls block line of sight, bullets and AI
  if (Q15T_WALLFIRE) caps |= NEVR_PLUGIN_CAP_ALTERS_GAMEPLAY;  // no shot through a wall
  if (Q15T_LEVELNAME) caps |= NEVR_PLUGIN_CAP_ALTERS_GAMEPLAY;  // the full-size global_mp loads
  if (Q15T_EC) caps |= NEVR_PLUGIN_CAP_ALTERS_GAMEPLAY | NEVR_PLUGIN_CAP_NETWORK;  // EC network datagrams; tac-mod list; drone kill feed
  if (Q15T_GATE_LINK) caps |= NEVR_PLUGIN_CAP_ALTERS_GAMEPLAY;  // the menus stop treating the headset as a Quest
  if (Q15T_GATE_PERM || Q15T_GATE_BOTS) caps |= NEVR_PLUGIN_CAP_ALTERS_GAMEPLAY | NEVR_PLUGIN_CAP_ALTERS_RULES;
  return caps;
}

NEVR_PLUGIN_API int NvrPluginInitEx(const NvrGameContext* ctx, const char* args_json) {
  if (ctx == nullptr) return 1;
  using namespace q15t;
  if (Covers(ctx, offsetof(NvrGameContext, log) + sizeof(ctx->log))) SetLogContext(ctx);
  const bool covers_id = Covers(ctx, offsetof(NvrGameContext, game_build_id) + sizeof(ctx->game_build_id));
  const bool quest = (ctx->flags & NEVR_HOST_PLATFORM_QUEST) != 0;
  const char* id = covers_id && ctx->game_build_id != nullptr ? ctx->game_build_id : "";
  // args {clock: raise} turns the clock raise on; absent or "log" keeps the log-only default; anything else stays log-only, loudly.
  char clock_arg[24];
  const bool has_clock_arg = ArgString(args_json, "clock", clock_arg, sizeof(clock_arg));
  const bool raise = has_clock_arg && std::strcmp(clock_arg, "raise") == 0;
  SetClockRaise(raise);
  const NevrLogField start[] = {{"game_build_id", id}, {"pinned", Q15T_BUILD_ID},
                                {"clock_candidate", raise ? "1" : "0"}, {"clock_arg", clock_arg},
                                {"hz_mode", Q15T_HZ_MATCH90 ? "level" : "witness"}, {"t26", Q15T_T26 ? "1" : "0"},
                                {"modules", kCompiledModules}};
  Log(NEVR_LOG_INFO, "q15t init", start, 7);
  if (has_clock_arg && !raise && std::strcmp(clock_arg, "log") != 0) {
    const NevrLogField bad[] = {{"key", "clock"}, {"value", clock_arg}, {"effective", "log"}};
    Log(NEVR_LOG_WARNING, "unknown clock arg: staying log-only (use raise or log)", bad, 3);
  }
  if (!quest || !covers_id || std::strcmp(id, Q15T_BUILD_ID) != 0) {
    Log(NEVR_LOG_WARNING, "refused: not the pinned game build", start, 7);
    return 0;  // loaded, nothing installed
  }
  RegisterScriptRedirect(ctx);  // the combat libraries (#1737): all or nothing, from init, before any game load
  Report("vrapi_SetClockLevels", InstallClockLevels(ctx->base_addr, Q15T_SLOT_SETCLOCKLEVELS));
  Report("vrapi_SetDisplayRefreshRate", InstallRefreshRate(ctx->base_addr, Q15T_SLOT_SETDISPLAYREFRESHRATE));
  // The three combat gates (combat_gates.h): what is built, less what a kill file turns off; the bot-body gate only with levelname.
  [[maybe_unused]] const CombatGatesChoice gates =
      DecideCombatGates({Q15T_GATE_PERM != 0, Q15T_GATE_BOTS != 0, Q15T_LEVELNAME != 0}, ReadCombatGatesOffFiles());
  {
    const NevrLogField f[] = {{"gate_perm", gates.perm ? "on" : "off"},        {"gate_perm_reason", gates.perm ? "installing" : gates.perm_reason},
                              {"gate_bots", gates.bots ? "on" : "off"},        {"gate_bots_reason", gates.bots ? "installing" : gates.bots_reason},
                              {"levelname", gates.levelname ? "on" : "off"},   {"levelname_reason", gates.levelname ? "installing" : gates.levelname_reason}};
    Log(NEVR_LOG_INFO, "q15t_combat_gates_install: which combat gates this run installs (a Quest gets combat permission, the combat bot body, the full global_mp)", f, 6);
  }
#if Q15T_PFX
  InitPfxGate();  // the gate every pfx* module reads; logs its own "pfx gate" line (pfx.h)
#endif
  bool want_tick = false;
  [[maybe_unused]] const std::uintptr_t base = ctx->base_addr;
  // The modules, in an order the docs fix: wallfire after bvh; ecfx, powerboost's net tick and dronekill's kill feed after ec;
  // dronemuzzle after dronekill. Every AddGameTick comes before InstallGameTick below. A module's own lines carry its detail;
  // ReportModule puts its name and result in the init record. Nothing here changes what InitEx returns (see the end).
#if Q15T_FXTRAIL
  ReportModule("fxtrail", InstallFxTrail(base));
#endif
#if Q15T_VOIPDROP
  ReportModule("voipdrop", InstallVoipDrop(base));
#endif
#if Q15T_CUSTCTX
  ReportModule("custctx", InstallCustCtx(base));
#endif
#if Q15T_MESHSTEP
  ReportModule("meshstep", InstallMeshStep(base));
#endif
#if Q15T_DRONEGUARD
  ReportModule("droneguard", InstallDroneGuard(base));
#endif
#if Q15T_PEERCLAMP
  ReportModule("peerclamp", InstallPeerClamp(base));
#endif
#if Q15T_BVH
  {
    const InstallResult r = InstallBvh(base);
    ReportModule("bvh", r);
    if (r.status == SwapStatus::kOk) AddModuleTick("bvh", &BvhTick, &want_tick);
  }
#endif
#if Q15T_WALLFIRE
  ReportModule("wallfire", InstallWallfire(base));  // refuses itself (bvh_not_installed) when bvh did not install
#endif
#if Q15T_IMPACTSND
  ReportModule("impactsnd", InstallImpactSnd(base));
  AddModuleTick("impactsnd", &ImpactSndTick, &want_tick);  // the tick does nothing when the install was refused
#endif
  [[maybe_unused]] bool ec_on = false;
#if Q15T_EC
  {
    const InstallResult r = InstallEc(base);
    ReportModule("ec", r);
    ec_on = r.status == SwapStatus::kOk;
  }
#endif
#if Q15T_ECFX
  ReportModule("ecfx", InstallEcfx(base));
#endif
#if Q15T_DRONEKILL
  {
    DroneKillFns fns{};
    if (!ResolveDroneKillFns(base, &fns)) {
      ReportModuleRefused("dronekill", "engine_export_missing");
    } else {
      const DroneKillInstall dk = InstallDroneKill(base, DroneKillWords{}, fns);
      const InstallResult hooks[] = {dk.actor, dk.enter, dk.fire, dk.attack};
      ReportModule("dronekill", FirstFailure(dk.ok, hooks, 4));
      if (dk.ok && dk.needs_tick) AddModuleTick("dronekill", &DroneKillTick, &want_tick);
#if Q15T_EC
      if (dk.ok) {  // the server's "drone killed" datagrams reach `kill` through ec's recvfrom wrapper
        EcOnDroneKilled(&DroneKillOnKilled);
        const NevrLogField f[] = {{"module", "dronekill"}, {"feed", "ec recvfrom"}, {"ec_on", ec_on ? "1" : "0"}};
        Log(NEVR_LOG_INFO, "module link", f, 3);
      }
#endif
    }
  }
#endif
#if Q15T_DRONEMUZZLE
  if (!DroneKillShotActive()) {
    ReportModuleRefused("dronemuzzle", "needs_dronekill_shot");
  } else {
    const DroneMuzzleInstall dm = InstallDroneMuzzle(base);
    const InstallResult hooks[] = {dm.teleport, dm.fire};
    ReportModule("dronemuzzle", FirstFailure(dm.ok, hooks, 2));
    if (dm.ok) AddModuleTick("dronemuzzle", &DroneMuzzleTick, &want_tick);
  }
#endif
#if Q15T_POWERBOOST > 0
  {
    bool net_tick = false;
#if Q15T_POWERBOOST >= 3 && Q15T_EC
    net_tick = ec_on && EcOnNetTick(&PowerboostNetTick);  // stage 3 sends from ec's recvfrom wrapper; without it the stage is refused
#endif
    const InstallResult r = InstallPowerboost(base, Q15T_POWERBOOST, net_tick);
    ReportModule("powerboost", r);
#if Q15T_POWERBOOST >= 3
    if (r.status == SwapStatus::kOk) AddModuleTick("powerboost", &PowerboostGameTick, &want_tick);
#endif
  }
#endif
#if Q15T_HUDMETER
  ReportModule("hudmeter", InstallHudmeter(base));
#endif
#if Q15T_SHIELDGAMMA
  ReportModule("shieldgamma", InstallShieldGamma(base, (Q15T_SHIELDGAMMA & 1) != 0, (Q15T_SHIELDGAMMA & 2) != 0));
#endif
#if Q15T_TABLESTEP
  ReportModule("tablestep", InstallTableStep(base));
#endif
#if Q15T_SENDGUARD
  ReportModule("sendguard", InstallSendGuard(base));
#endif
#if Q15T_LEVELNAME
  if (gates.levelname) {
    ReportModule("levelname", InstallLevelName(base));
    ReportModule("boosterpool", InstallBoosterPool(base));  // the companion word row, as a GOT hook (boosterpool.h)
  } else {
    InstallResult off;
    off.reason = gates.levelname_reason;  // "off_file": levelname.off turns the pair off together
    ReportModule("levelname", off);
    ReportModule("boosterpool", off);
  }
#endif
#if Q15T_SENSORWD
  ReportModule("sensorwd", InstallSensorWd(base));
  AddModuleTick("sensorwd", &SensorWdTick, &want_tick);  // one bool test per frame when the install was refused
#endif
#if Q15T_INSTMODEL
  ReportModule("instmodel", InstallInstModel(base));  // no tick: the swapped constructor does its work when the object is built
#endif
#if Q15T_SHADERSTEP
  ReportModule("shaderstep", InstallShaderStep(base));
#endif
#if Q15T_DECALS
  ReportModule("decals", InstallDecals(base));  // after impactsnd: its Shutdown wrapper chains on impactsnd's
#endif
#if Q15T_MUSIC
  {
    const InstallResult r = InstallMusic(base);
    ReportModule("music", r);
    if (r.status == SwapStatus::kOk) AddModuleTick("music", &MusicTick, &want_tick);
  }
#endif
#if Q15T_GUNCONNECT
  InitGunConnect(base, GunConnectFns{});  // an all-null table: resolved from the loaded game library on the first tick
  ReportModuleTick("gunconnect", AddGameTick(&GunConnectTick), &want_tick);
#endif
#if Q15T_AIBOTS
  InitAiBots(base, AiBotsFns{});
  ReportModuleTick("aibots", AddGameTick(&AiBotsTick), &want_tick);
#endif
#if Q15T_HZ_MATCH90
  AddModuleTick("refresh_rate", &RefreshMatchTick, &want_tick);
#endif
#if Q15T_T26
  {
    WeaponArmFns arm_fns{};
    if (ResolveWeaponArmFns(&arm_fns)) {
      InitWeaponArm(ctx->base_addr, arm_fns);
      AddModuleTick("weapon_arm", &WeaponArmTick, &want_tick);
    } else {
      Log(NEVR_LOG_ERROR, "t26 not installed: an engine export is missing (not the pinned build?)");
    }
  }
#endif
  if (want_tick) {
    Report("CncaGame::ExclusiveUpdateGame (game-thread tick)",
           InstallGameTick(ctx->base_addr, Q15T_SLOT_EXCLUSIVEUPDATEGAME, Q15T_REAL_EXCLUSIVEUPDATEGAME, Q15T_GAME_VPTR));
    const KV ticks[] = {{"registered", GameTickCount()}, {"max", kMaxGameTicks}};
    LogNums(NEVR_LOG_INFO, "game tick callbacks", ticks, 2);
  }
#if Q15T_GATE_PERM
  if (gates.perm) {
    ReportModule("gate_perm", InstallPartyHeadsetGate(ctx->base_addr, Q15T_SLOT_PARTYMEMBERHEADSETTYPE, {Q15T_PERM_FN_LO, Q15T_PERM_FN_HI},
                                                      Q15T_REAL_PARTYMEMBERHEADSETTYPE));
  } else {
    InstallResult off;
    off.reason = gates.perm_reason;
    ReportModule("gate_perm", off);
  }
#endif
#if Q15T_GATE_BOTS || Q15T_GATE_LINK
  // The link gate answers only the script libraries the policy names (headset_gates.h): the build's list, or the first line of
  // <data directory>/gate_libs.txt when that file exists ("all", "none", or name fragments such as "0e4384a5 1703a89b").
  {
    char spec[640] = Q15T_LINK_GATE_LIBS;
    const char* source = "build default";
#if Q15T_EC
    char line[640];
    if (EcReadDataLine("gate_libs.txt", line, sizeof line)) {
      std::memcpy(spec, line, sizeof spec);
      source = "gate_libs.txt";
    }
#endif
    SetLinkGateLibs(spec);
    const NevrLogField f[] = {{"libs", spec[0] != 0 ? spec : "none"}, {"source", source}};
    Log(NEVR_LOG_INFO, "headset link gate policy", f, 2);
  }
  // One wrapper on the one CVR::HeadsetType slot serves both gates; a gate that is off gets an empty caller range.
  Report("CVR::HeadsetType (caller-range gates)",
         InstallHeadsetGate(ctx->base_addr, Q15T_SLOT_HEADSETTYPE,
                            gates.bots ? CallerRange{Q15T_BOTS_FN_LO, Q15T_BOTS_FN_HI} : CallerRange{0, 0}, Q15T_REAL_HEADSETTYPE,
                            Q15T_GATE_LINK ? CallerRange{Q15T_LINK_FN_LO, Q15T_LINK_FN_HI} : CallerRange{0, 0}, kLinkHeadset));
#endif
  // Always 0 from here: a non-zero return makes the loader dlclose the plugin, which is fatal once any word is swapped. A module's
  // refusal is its ERROR line above, never this return value.
  return 0;
}

NEVR_PLUGIN_API void NvrPluginShutdown(void) {}
