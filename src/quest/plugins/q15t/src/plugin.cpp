// The plugin's entry points (the ABI of extension/plugin_interface.h, Quest additions of ADR 0019 section 4).
//
// No namespace-scope object has a constructor: the repack's static-init check refuses a plugin that has one.
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "nevr/plugin_interface.h"
#include "game_tick.h"
#include "plugin_log.h"
#include "script_redirect.h"

#ifndef Q15T_BUILD_ID
#define Q15T_BUILD_ID "b243509c08ce677aeb95fa348016949b3fc45230"
#endif
// Gameplay and fix modules: none are left in this plugin. Every one is the runtime's (src/quest/combat/<m>.{h,cpp}, docs/quest/combat/<m>.md).
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
    "q15t-config build_id=" Q15T_BUILD_ID;

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
constexpr char kCompiledModules[] = "";

}  // namespace

NEVR_PLUGIN_API NevrPluginInfo NevrPluginGetInfo(void) {
  return {"q15t", "combat gameplay modules", 0, 2, 0};
}
NEVR_PLUGIN_API uint32_t NevrPluginGetApiVersion(void) { return NEVR_PLUGIN_API_VERSION; }
NEVR_PLUGIN_API uint32_t NevrPluginGetCapabilities(void) {
  uint32_t caps = NEVR_PLUGIN_CAP_HOOKS_ENGINE;
  // No module of the plugin changes what a player experiences or what the game computes: every gameplay module, levelname and boosterpool
  // included, is the runtime's (src/quest/combat/), so the plugin claims no ALTERS_GAMEPLAY.
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
  // KNOWN-WRONG #2168: unused: no module of the plugin registers a game tick, so want_tick stays false and the tick below is never installed. It is removed when the fold ends.
  bool want_tick = false;
  [[maybe_unused]] const std::uintptr_t base = ctx->base_addr;
  // The plugin installs no module: every one is the runtime's (src/quest/combat/), levelname and boosterpool included. Every AddGameTick
  // comes before InstallGameTick below. Nothing here changes what InitEx returns (see the end).
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
