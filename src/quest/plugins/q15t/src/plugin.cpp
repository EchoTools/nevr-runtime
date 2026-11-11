// The plugin's entry points (the ABI of extension/plugin_interface.h, Quest additions of ADR 0019 section 4).
//
// No namespace-scope object has a constructor: the repack's static-init check refuses a plugin that has one.
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "nevr/plugin_interface.h"
#include "game_tick.h"
#include "levelname.h"
#include "boosterpool.h"
#include "plugin_log.h"
#include "script_redirect.h"
#include "module_switch.h"

#ifndef Q15T_BUILD_ID
#define Q15T_BUILD_ID "b243509c08ce677aeb95fa348016949b3fc45230"
#endif
#ifndef Q15T_LEVELNAME
#define Q15T_LEVELNAME 0
#endif
// Gameplay and fix modules (src/<m>.{h,cpp}, docs/quest/q15t/modules/<m>.md). CMakeLists.txt owns the options and their defaults (each with the
// reason, from the module's doc); the default HERE is 0, so a build that names no option installs none of them and the
// perf arms and the host-test arms stay unchanged. heaps and memsys are not modules of this plugin: they are the sentinel's early combat
// step (src/quest/combat/heaps.h, memsys.h).
// The game-thread tick: CncaGame::ExclusiveUpdateGame's JUMP_SLOT, its function, and CR15Game's vptr value (R17; q15t
// the source build's gunconnect kGotExclusiveUpdateGame / kFnExclusiveUpdateGame / kGameVptr).
#ifndef Q15T_SLOT_EXCLUSIVEUPDATEGAME
#define Q15T_SLOT_EXCLUSIVEUPDATEGAME 0x36e7f28ULL
#define Q15T_REAL_EXCLUSIVEUPDATEGAME 0x17ef62cULL
#define Q15T_GAME_VPTR 0x362ed30ULL
#endif

NEVR_DECLARE_SUPPORTED_BUILD_IDS(Q15T_BUILD_ID);

// What this binary was compiled with, as one string in .rodata: `just build-quest-q15t` reads it back from the built library and
// refuses a build whose defaults are not the shipped ones.
#define Q15T_MARK_STR2(x) #x
#define Q15T_MARK_STR(x) Q15T_MARK_STR2(x)
extern "C" __attribute__((used, retain, visibility("hidden"))) const char q15t_config_marker[] =
    "q15t-config levelname=" Q15T_MARK_STR(Q15T_LEVELNAME) " build_id=" Q15T_BUILD_ID;

namespace {

void Report(const char* hook, const q15t::InstallResult& r) {
  using namespace q15t;
  const NevrLogField done[] = {{"hook", hook},
                               {"status", StatusName(r.status)},
                               {"reason", r.reason},
                               {"real_resolved", r.real_resolved ? "1" : "0"}};
  Log(r.status == SwapStatus::kOk ? NEVR_LOG_INFO : NEVR_LOG_ERROR, "hook install", done, 4);
}

bool Covers(const NevrGameContext* ctx, std::size_t end_of_field) { return ctx->ctx_size >= end_of_field; }

// Every compiled-in module, by name, for the init line.
constexpr char kCompiledModules[] =
#if Q15T_LEVELNAME
    "levelname "
    "boosterpool "
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

NEVR_PLUGIN_API NevrPluginInfo NevrPluginGetInfo(void) {
  return {"q15t", "combat gameplay modules", 0, 2, 0};
}
NEVR_PLUGIN_API uint32_t NevrPluginGetApiVersion(void) { return NEVR_PLUGIN_API_VERSION; }
NEVR_PLUGIN_API uint32_t NevrPluginGetCapabilities(void) {
  uint32_t caps = NEVR_PLUGIN_CAP_HOOKS_ENGINE;
  // Modules that change what a player experiences or what the game computes (per their docs, the others skip, add or answer game
  // logic). Not here: the crash and timing guards (fxtrail, voipdrop, custctx, droneguard, peerclamp) are the runtime's now.
  if (Q15T_LEVELNAME) caps |= NEVR_PLUGIN_CAP_ALTERS_GAMEPLAY;  // the full-size global_mp loads
  return caps;
}

NEVR_PLUGIN_API int NevrPluginInitEx(const NevrGameContext* ctx, const char* /*args_json*/) {
  if (ctx == nullptr) return 1;
  using namespace q15t;
  if (Covers(ctx, offsetof(NevrGameContext, log) + sizeof(ctx->log))) SetLogContext(ctx);
  const bool covers_id = Covers(ctx, offsetof(NevrGameContext, game_build_id) + sizeof(ctx->game_build_id));
  const bool quest = (ctx->flags & NEVR_HOST_PLATFORM_QUEST) != 0;
  const char* id = covers_id && ctx->game_build_id != nullptr ? ctx->game_build_id : "";
  const NevrLogField start[] = {{"game_build_id", id}, {"pinned", Q15T_BUILD_ID}, {"modules", kCompiledModules}};
  Log(NEVR_LOG_INFO, "q15t init", start, 3);
  if (!quest || !covers_id || std::strcmp(id, Q15T_BUILD_ID) != 0) {
    Log(NEVR_LOG_WARNING, "refused: not the pinned game build", start, 3);
    return 0;  // loaded, nothing installed
  }
  RegisterScriptRedirect(ctx);  // the combat libraries (#1737): all or nothing; plugin init runs after the first hooked dlopen
                               // returns a handle (script_redirect.h, Register), so loads before that are not redirected
  // levelname and its companion boosterpool: built in, less the kill file `levelname.off` (the runtime's headset gates read `combat.levelname`
  // for the same fact: the bot-body gate needs the full-size global_mp that levelname loads).
  [[maybe_unused]] const bool levelname_on = Q15T_LEVELNAME != 0 && !ModuleSwitchPresent("levelname.off");
  // KNOWN-WRONG #2168: unused: no module of the plugin registers a game tick, so want_tick stays false and the tick below is never installed. It is removed when the fold ends.
  bool want_tick = false;
  [[maybe_unused]] const std::uintptr_t base = ctx->base_addr;
  // The modules, in an order the docs fix (bvh, wallfire, impactsnd, decals, ec, ecfx, powerboost, dronekill and dronemuzzle are the runtime's, src/quest/combat/).
  // Every AddGameTick comes before InstallGameTick below. A module's own lines carry its detail;
  // ReportModule puts its name and result in the init record. Nothing here changes what InitEx returns (see the end).
#if Q15T_LEVELNAME
  if (levelname_on) {
    ReportModule("levelname", InstallLevelName(base));
    ReportModule("boosterpool", InstallBoosterPool(base));  // the companion word row, as a GOT hook (boosterpool.h)
  } else {
    InstallResult off;
    off.reason = "off_file";  // levelname.off turns the pair off together
    ReportModule("levelname", off);
    ReportModule("boosterpool", off);
  }
#endif
  if (want_tick) {
    Report("CncaGame::ExclusiveUpdateGame (game-thread tick)",
           InstallGameTick(ctx->base_addr, Q15T_SLOT_EXCLUSIVEUPDATEGAME, Q15T_REAL_EXCLUSIVEUPDATEGAME, Q15T_GAME_VPTR));
    const KV ticks[] = {{"registered", GameTickCount()}, {"max", kMaxGameTicks}};
    LogNums(NEVR_LOG_INFO, "game tick callbacks", ticks, 2);
  }
  // Always 0 from here: a non-zero return makes the loader dlclose the plugin, which is fatal once any word is swapped. A module's
  // refusal is its ERROR line above, never this return value.
  return 0;
}

NEVR_PLUGIN_API void NevrPluginShutdown(void) {}
