#include "quest/combat/combat_install.h"

#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>

#include "quest/combat/aibots.h"
#include "quest/combat/boosterpool.h"
#include "quest/combat/bullet_velocity_guard.h"
#include "quest/combat/bvh.h"
#include "quest/combat/combat_active.h"
#include "quest/combat/combat_scope.h"
#include "quest/combat/combat_tick.h"
#include "quest/combat/custctx.h"
#include "quest/combat/decals.h"
#include "quest/combat/droneguard.h"
#include "quest/combat/dronekill.h"
#include "quest/combat/dronemuzzle.h"
#include "quest/combat/ec.h"
#include "quest/combat/ecfx.h"
#include "quest/combat/frozen_frame_exit.h"
#include "quest/combat/fxtrail.h"
#include "quest/combat/gunconnect.h"
#include "quest/combat/headset_gates.h"
#include "quest/combat/heaps.h"
#include "quest/combat/hudmeter.h"
#include "quest/combat/impactsnd.h"
#include "quest/combat/instmodel.h"
#include "quest/combat/levelname.h"
#include "quest/combat/memsys.h"
#include "quest/combat/meshstep.h"
#include "quest/combat/music.h"
#include "quest/combat/peerclamp.h"
#include "quest/combat/pfx.h"
#include "quest/combat/pfxtev.h"
#include "quest/combat/powerboost.h"
#include "quest/combat/refresh_rate.h"
#include "quest/combat/script_redirect.h"
#include "quest/combat/shieldgamma.h"
#include "quest/combat/sound_symbol_guard.h"
#include "quest/combat/texel_buffer_guard.h"
#include "quest/combat/texture_quest2.h"
#include "quest/combat/sendguard.h"
#include "quest/combat/sensorwd.h"
#include "quest/combat/shaderstep.h"
#include "quest/combat/tablestep.h"
#include "quest/combat/voipdrop.h"
#include "quest/combat/wallfire.h"
#include "quest/combat/weapon_arm.h"
#include "quest/sentinel/hook_log.h"

namespace nevr_quest::combat {

namespace {

// The configuration the rows read their switch from (SetConfig). A plain object: it is written once on the constructor's thread before
// either step runs, and the normal step's thread is ordered after it by the post-load action's atomics.
nevr_quest::CombatConfig g_config;

// A row's install function reads its module's switch here, so a module's own code takes the switch as an argument and a host test
// drives it without a configuration.
ModuleResult ScriptRedirectNormal() noexcept { return script_redirect::Install(); }
ModuleResult HeapsEarly() noexcept { return heaps::Install(g_config.heaps.on, g_config.memsys.on); }
ModuleResult MemsysEarly() noexcept { return memsys::Install(g_config.memsys.on); }
// The texel-buffer guard (#1650): its two requests go to vk_ext_window before the game's first Vulkan lookup, so its row is early.
ModuleResult TexelBufferGuardEarly() noexcept { return texel_buffer_guard::Early(g_config.texelBufferGuard.on); }
ModuleResult RefreshRateNormal() noexcept {
  return refresh_rate::Install(g_config.refreshRate.on, g_config.refreshRateLobby.on, g_config.refreshRateQuest2At72.on);
}
// The scope's refresh is a look at the game's tables four times a second in every level, arena included, so it is registered only when a module that
// reads it is on: with every reader's key off, the row is off and arena pays nothing for it. A module that moves in and reads the scope adds its switch here.
ModuleResult ScopeNormal() noexcept {
  const bool anyReader = g_config.hudmeter.on || g_config.shieldgammaGamma.on || g_config.shieldgammaTubeMirror.on || g_config.fxtrail.on ||
                         g_config.peerclamp.on || g_config.custctx.on || g_config.frozenFrameExit.on || g_config.soundSymbolGuard.on ||
                         g_config.gunconnect.on;
  if (!anyReader) return {false, "off_config", /*off=*/true};
  return scope::InstallNormal();
}
// The frozen-frame exit (#2377, D8 (a): the combat APK only, this table's row): its key, and the limit with whether the file gave it.
ModuleResult FrozenFrameExitNormal() noexcept {
  return frozen_frame_exit::Install(g_config.frozenFrameExit.on, g_config.frozenFrameExitS.value,
                                    g_config.frozenFrameExitS.source == nevr_quest::Source::kFile);
}
// The Quest 2 step-down (#1737): combat's look handed to the texture-quality owner; its lobby switch rides on it.
ModuleResult TextureQuest2Normal() noexcept {
  return texture_quest2::Install(g_config.textureQuest2StepDown.on, g_config.textureQuest2Lobby.on);
}
ModuleResult HudmeterNormal() noexcept { return hudmeter::Install(g_config.hudmeter.on); }
ModuleResult ShieldgammaNormal() noexcept {
  return shieldgamma::Install(g_config.shieldgammaGamma.on, g_config.shieldgammaTubeMirror.on);
}
ModuleResult MeshstepNormal() noexcept { return meshstep::Install(g_config.meshstep.on); }
ModuleResult TablestepNormal() noexcept { return tablestep::Install(g_config.tablestep.on); }
ModuleResult InstmodelNormal() noexcept { return instmodel::Install(g_config.instmodel.on); }
ModuleResult ShaderstepNormal() noexcept { return shaderstep::Install(g_config.shaderstep.on); }

// levelname and boosterpool are one feature behind one key (combat.levelname); the booster's substitution is right only while levelname's level
// is the loaded one, so it asks whether levelname installed.
ModuleResult LevelNameNormal() noexcept { return levelname::Install(g_config.levelname.on); }
ModuleResult BoosterPoolNormal() noexcept { return boosterpool::Install(g_config.levelname.on, levelname::Installed()); }

// The three modules of #2185. Each is on unless its key says false (the reference build's shipped build has all three on); aibots also takes its scenario text and whether this is a development build.
ModuleResult GunconnectNormal() noexcept { return gunconnect::Install(g_config.gunconnect.on); }
ModuleResult AibotsNormal() noexcept {
  aibots::Switches switches;
  switches.on = g_config.aibots.on;
  switches.scenario = g_config.aibotsScenario.source == nevr_quest::Source::kFile ? g_config.aibotsScenario.text : nullptr;
  switches.devBuild = DevBuild();
  return aibots::Install(switches);
}
ModuleResult WeaponArmNormal() noexcept { return weapon_arm::Install(g_config.weaponArm.on); }

ModuleResult ImpactsndNormal() noexcept { return impactsnd::Install(g_config.impactsnd.on, g_config.impactsndWrite.on); }
ModuleResult DecalsNormal() noexcept {
  decals::Switches switches;
  switches.on = g_config.decals.on;
  switches.flipU = g_config.decalsFlipU.on;
  switches.flipV = g_config.decalsFlipV.on;
  return decals::Install(switches);
}

ModuleResult EcNormal() noexcept {
  ec::Switches switches;
  switches.on = g_config.ec.on;
  switches.words = g_config.ecWords.source == nevr_quest::Source::kFile ? g_config.ecWords.text : nullptr;
  return ec::Install(switches);
}
ModuleResult EcfxNormal() noexcept { return ecfx::Install(g_config.ecfx.on); }
ModuleResult PowerboostNormal() noexcept { return powerboost::Install(g_config.powerboostStage.value); }
ModuleResult DronekillNormal() noexcept {
  dronekill::Words words;
  words.explosion = g_config.dronekillExplosion.on;  // #1737: the drone explosion rides on dronekill (docs/quest/combat/drone_explosion.md)
  words.hold = g_config.dronekillHold.on;            // #1737: its first-system off held back (off by default: dronekill.h "The hold")
  return dronekill::Install(g_config.dronekill.on, words);
}
ModuleResult BulletVelocityGuardNormal() noexcept { return bullet_velocity_guard::Install(g_config.bulletVelocityGuard.on); }
ModuleResult SoundSymbolGuardNormal() noexcept { return sound_symbol_guard::Install(g_config.soundSymbolGuard.on); }
ModuleResult DronemuzzleNormal() noexcept { return dronemuzzle::Install(g_config.dronemuzzle.on); }

ModuleResult GatesNormal() noexcept {
  headset_gates::Switches switches;
  switches.perm = g_config.gatePerm.on;
  switches.bots = g_config.gateBots.on;
  switches.link = g_config.gateLink.on;
  switches.linkLibs = g_config.gateLinkLibs.source == nevr_quest::Source::kFile ? g_config.gateLinkLibs.text : nullptr;
  return headset_gates::Install(switches);
}
ModuleResult BvhNormal() noexcept {
  bvh::Switches switches;
  switches.bvh = g_config.bvh.on;
  switches.gate = g_config.bvhGate.on;
  switches.touch = g_config.bvhTouch.on;
  return bvh::Install(switches);
}
ModuleResult WallfireNormal() noexcept {
  return wallfire::Install(g_config.wallfire.on);
}
ModuleResult PfxNormal() noexcept { return pfx::Install(g_config.pfx.on); }
ModuleResult PfxtevNormal() noexcept { return pfxtev::Install(g_config.pfxtev.on); }
ModuleResult SendguardNormal() noexcept { return sendguard::Install(g_config.sendguard.on); }
ModuleResult SensorwdNormal() noexcept { return sensorwd::Install(g_config.sensorwd.on); }
ModuleResult MusicNormal() noexcept { return music::Install(g_config.music.on); }
// The crash and timing guards (#2178). fxtrail, peerclamp and custctx read the combat level answer, so their switches are readers of the scope row above.
ModuleResult FxtrailNormal() noexcept { return fxtrail::Install(g_config.fxtrail.on); }
ModuleResult VoipdropNormal() noexcept { return voipdrop::Install(g_config.voipdrop.on); }
ModuleResult CustctxNormal() noexcept { return custctx::Install(g_config.custctx.on); }
ModuleResult DroneguardNormal() noexcept { return droneguard::Install(g_config.droneguard.on, g_config.droneguardStage1.on); }
ModuleResult PeerclampNormal() noexcept { return peerclamp::Install(g_config.peerclamp.on); }

// The registration table, in install order (combat_install.h, "The table"). Each module's unit adds its row at the position the q15t
// plugin installed it, and nowhere else; the count is deduced.
//   sendguard is installed where the plugin installed it: in the normal phase after the modules that hook the tick, before the two decisions.
//   heaps, then memsys: the heap sizes are ours before the pool and the witness hook go in (the plugin's order); both are early,
//   before android_main reads the sizes.
constexpr std::array kModules{
    Module{"heaps", &HeapsEarly, nullptr},
    Module{"memsys", &MemsysEarly, nullptr},
    // texel_buffer_guard (#1650): registers two device-function requests with vk_ext_window (no hook of its own); early, because the requests must
    // be in before install_vk_ext_window and the game's first lookup. It touches no slot a plugin could hook (README rule 8). Acts in every level
    // (README rule 9, a default the game replaces before any read).
    Module{"texel_buffer_guard", &TexelBufferGuardEarly, nullptr, "combat.texel_buffer_guard", &nevr_quest::CombatConfig::texelBufferGuard,
           &texel_buffer_guard::End},
    // script_redirect (#2189): first of the normal rows (script_redirect.h), so a script load the later rows' installs cause is already redirected.
    Module{"script_redirect", nullptr, &ScriptRedirectNormal},
    // tick, then refresh_rate (#2172): the game-thread tick installs first (combat_tick.h), and every module with a per-frame callback registers
    // it from its own step, which may come before or after the tick's install.
    Module{"tick", nullptr, &tick::InstallNormal},
    // scope (#2186): the combat level answer (combat_scope.h) is refreshed from the tick, so it comes after the tick's install and before every
    // module that reads it.
    Module{"scope", nullptr, &ScopeNormal},
    // frozen_frame_exit (#2377): reads the combat level answer, so right after the scope's row; the render-configuration hook it observes is
    // installed by the constructor sequence, long before this step.
    Module{"frozen_frame_exit", nullptr, &FrozenFrameExitNormal, "combat.frozen_frame_exit", &nevr_quest::CombatConfig::frozenFrameExit,
           &frozen_frame_exit::NoteEnd},
    Module{"refresh_rate", nullptr, &RefreshRateNormal},
    // texture_quest2 (#1737): a tick callback of its own (it reads the init-fiber count, which the scope does not), so after the tick's row; not
    // a reader of the scope's answer. texture_quest2_lobby is its switch with no step of its own.
    Module{"texture_quest2_step_down", nullptr, &TextureQuest2Normal, "combat.texture_quest2_step_down", &nevr_quest::CombatConfig::textureQuest2StepDown},
    Module{"texture_quest2_lobby", nullptr, nullptr, "combat.texture_quest2_lobby", &nevr_quest::CombatConfig::textureQuest2Lobby},
    // The resource table steps (#2177), in the plugin's order: meshstep, tablestep, instmodel, shaderstep. Each acts only while a combat map is
    // loaded (table_step.h), so they read the game the tick verified: they come after the tick's row.
    Module{"meshstep", nullptr, &MeshstepNormal, "combat.meshstep", &nevr_quest::CombatConfig::meshstep},
    // impactsnd (#2183) sits where the plugin installed it, after meshstep and before the other table steps; it acts only in combat levels (impactsnd.h).
    // impactsnd_write, decals_flip_u and decals_flip_v are switches of their module's with no step of their own (as bvh_gate and bvh_touch below): each
    // row exists so the switch is one generated `combat_config_switch` record.
    Module{"impactsnd", nullptr, &ImpactsndNormal, "combat.impactsnd", &nevr_quest::CombatConfig::impactsnd},
    Module{"impactsnd_write", nullptr, nullptr, "combat.impactsnd_write", &nevr_quest::CombatConfig::impactsndWrite},
    Module{"tablestep", nullptr, &TablestepNormal, "combat.tablestep", &nevr_quest::CombatConfig::tablestep},
    Module{"instmodel", nullptr, &InstmodelNormal, "combat.instmodel", &nevr_quest::CombatConfig::instmodel},
    Module{"shaderstep", nullptr, &ShaderstepNormal, "combat.shaderstep", &nevr_quest::CombatConfig::shaderstep},
    // decals (#2183): last of the plugin's table steps; its Shutdown handler chains above impactsnd's, so impactsnd's row comes first. It acts only in combat
    // levels (decals.h).
    Module{"decals", nullptr, &DecalsNormal, "combat.decals", &nevr_quest::CombatConfig::decals},
    Module{"decals_flip_u", nullptr, nullptr, "combat.decals_flip_u", &nevr_quest::CombatConfig::decalsFlipU},
    Module{"decals_flip_v", nullptr, nullptr, "combat.decals_flip_v", &nevr_quest::CombatConfig::decalsFlipV},
    // ec, then ecfx (#2187): the EC client first, then the modules that ride its net tick; plugin.cpp's order.
    Module{"ec", nullptr, &EcNormal, "combat.ec", &nevr_quest::CombatConfig::ec},
    Module{"ecfx", nullptr, &EcfxNormal, "combat.ecfx", &nevr_quest::CombatConfig::ecfx},
    // dronekill, then dronemuzzle (#2187): the plugin's order (after ec, whose kill feed dronekill registers with; dronemuzzle needs dronekill's shot).
    Module{"dronekill", nullptr, &DronekillNormal, "combat.dronekill", &nevr_quest::CombatConfig::dronekill, &dronekill::NoteEnd},
    // dronekill_explosion (#1737): a switch of dronekill's with no step of its own (as impactsnd_write), read by DronekillNormal.
    Module{"dronekill_explosion", nullptr, nullptr, "combat.dronekill_explosion", &nevr_quest::CombatConfig::dronekillExplosion},
    // dronekill_hold (#1737): the same, the explosion's hold (default off), read by DronekillNormal.
    Module{"dronekill_hold", nullptr, nullptr, "combat.dronekill_hold", &nevr_quest::CombatConfig::dronekillHold},
    // bullet_velocity_guard (#1737 row 031): a thunk on FireBullet's slot, immediately above dronemuzzle's row: dronemuzzle's assembly entry on
    // that slot must stay the newest hook there (a later install is refused slot_held_by_entry), so the guard installs first and the entry calls it.
    Module{"bullet_velocity_guard", nullptr, &BulletVelocityGuardNormal, "combat.bullet_velocity_guard", &nevr_quest::CombatConfig::bulletVelocityGuard, &bullet_velocity_guard::NoteEnd},
    // sound_symbol_guard (#1737): a caller-aware thunk on Play3DSound's slot (no other hook there); it reads the combat level answer, so after `scope`,
    // and its switch is one of ScopeNormal()'s readers.
    Module{"sound_symbol_guard", nullptr, &SoundSymbolGuardNormal, "combat.sound_symbol_guard", &nevr_quest::CombatConfig::soundSymbolGuard, &sound_symbol_guard::NoteEnd},
    Module{"dronemuzzle", nullptr, &DronemuzzleNormal, "combat.dronemuzzle", &nevr_quest::CombatConfig::dronemuzzle},
    // powerboost last of the five: its link / unlink sends hang on ec's net tick.
    Module{"powerboost", nullptr, &PowerboostNormal},
    // gunconnect, aibots, weapon_arm (#2185), in the plugin's order: after the table steps, which the plugin installed earlier in its list, they register tick
    // callbacks, so they come after the tick's row; before the gates, which the plugin installed after all its modules.
    Module{"gunconnect", nullptr, &GunconnectNormal, "combat.gunconnect", &nevr_quest::CombatConfig::gunconnect},
    Module{"aibots", nullptr, &AibotsNormal, "combat.aibots", &nevr_quest::CombatConfig::aibots},
    Module{"weapon_arm", nullptr, &WeaponArmNormal, "combat.weapon_arm", &nevr_quest::CombatConfig::weaponArm},
    // gates (#2180): the plugin installed them after its tick and its modules (after the table steps and the EC modules above); they register the tick callback the bot gate reads the game from.
    Module{"gates", nullptr, &GatesNormal},
    // bvh, then wallfire (#2182): the plugin installed them after its tick modules; wallfire needs the tree's four hooks, and both register or read
    // the scope the tick refreshes.
    // bvh_gate and bvh_touch are switches of bvh's with no step of their own: their rows exist so each switch is one generated `combat_config_switch` record.
    Module{"bvh", nullptr, &BvhNormal, "combat.bvh", &nevr_quest::CombatConfig::bvh},
    Module{"bvh_gate", nullptr, nullptr, "combat.bvh_gate", &nevr_quest::CombatConfig::bvhGate},
    Module{"bvh_touch", nullptr, nullptr, "combat.bvh_touch", &nevr_quest::CombatConfig::bvhTouch},
    Module{"wallfire", nullptr, &WallfireNormal, "combat.wallfire", &nevr_quest::CombatConfig::wallfire},
    // sendguard: the script-event guard, installed after the modules that hook the tick and before the decisions (#2179).
    Module{"sendguard", nullptr, &SendguardNormal, "combat.sendguard", &nevr_quest::CombatConfig::sendguard, &sendguard::NoteEnd},
    // The crash and timing guards (#2178), in the plugin's order: fxtrail, voipdrop, custctx, droneguard, peerclamp. They hook the game's own slots and
    // read the scope's answer where they change a value (fxtrail, peerclamp, custctx's pool refusal), so they come after the scope's row; each writes its
    // counts at the end of the process. droneguard_stage1 is a switch of droneguard's with no step of its own: its row exists so the switch is one
    // generated `combat_config_switch` record.
    Module{"fxtrail", nullptr, &FxtrailNormal, "combat.fxtrail", &nevr_quest::CombatConfig::fxtrail, &fxtrail::NoteEnd},
    Module{"voipdrop", nullptr, &VoipdropNormal, "combat.voipdrop", &nevr_quest::CombatConfig::voipdrop, &voipdrop::NoteEnd},
    Module{"custctx", nullptr, &CustctxNormal, "combat.custctx", &nevr_quest::CombatConfig::custctx, &custctx::NoteEnd},
    Module{"droneguard", nullptr, &DroneguardNormal, "combat.droneguard", &nevr_quest::CombatConfig::droneguard, &droneguard::NoteEnd},
    Module{"droneguard_stage1", nullptr, nullptr, "combat.droneguard_stage1", &nevr_quest::CombatConfig::droneguardStage1},
    Module{"peerclamp", nullptr, &PeerclampNormal, "combat.peerclamp", &nevr_quest::CombatConfig::peerclamp, &peerclamp::NoteEnd},
    // pfx, then pfxtev (#2188): a decision and no hook, so after the modules that install one; the event ids sit under the gate.
    Module{"pfx", nullptr, &PfxNormal},
    Module{"pfxtev", nullptr, &PfxtevNormal},
    // hudmeter, then shieldgamma (#2186): the plugin installed both after its crash guards and before levelname and sensorwd. Both read the combat level
    // answer (combat_scope.h), so they come after the scope's row. shieldgamma_gamma and shieldgamma_tube_mirror are switches of shieldgamma's with no step of
    // their own: each row exists so the switch is one generated `combat_config_switch` record.
    Module{"hudmeter", nullptr, &HudmeterNormal, "combat.hudmeter", &nevr_quest::CombatConfig::hudmeter},
    Module{"shieldgamma", nullptr, &ShieldgammaNormal},
    Module{"shieldgamma_gamma", nullptr, nullptr, "combat.shieldgamma_gamma", &nevr_quest::CombatConfig::shieldgammaGamma},
    Module{"shieldgamma_tube_mirror", nullptr, nullptr, "combat.shieldgamma_tube_mirror", &nevr_quest::CombatConfig::shieldgammaTubeMirror},
    // levelname, then boosterpool (#2181): the plugin installed the pair together, the level name first, after shieldgamma and before sensorwd. The bot
    // gate (above) reads the LoadLevelName word levelname takes at each bot call, not at install, so its row may come first.
    Module{"levelname", nullptr, &LevelNameNormal, "combat.levelname", &nevr_quest::CombatConfig::levelname, &levelname::NoteEnd},
    Module{"boosterpool", nullptr, &BoosterPoolNormal, nullptr, nullptr, &boosterpool::NoteEnd},
    // sensorwd, then music (#2184): the plugin installed sensorwd after its levelname pair and music after decals, so both come after the modules that
    // move before them; each registers a tick callback, which the tick row (above) lets them do before or after its install.
    Module{"sensorwd", nullptr, &SensorwdNormal, "combat.sensorwd", &nevr_quest::CombatConfig::sensorwd},
    Module{"music", nullptr, &MusicNormal, "combat.music", &nevr_quest::CombatConfig::music},
};

// Per phase: 0 not yet, 1 ran or was found inactive, 2 ended as not started. One atomic each: the early step runs on the constructor's
// thread, the normal step on the thread that returned from the game's first dlopen, and the process end on a helper thread.
std::atomic<int> g_phaseState[2] = {{0}, {0}};
constexpr int kNotYet = 0;
constexpr int kDone = 1;
constexpr int kEnded = 2;
// A process where combat does not run says so once, whichever phase asks first.
std::atomic<bool> g_inactiveSaid{false};

std::atomic<int>& StateOf(Phase phase) { return g_phaseState[phase == Phase::kEarly ? 0 : 1]; }

const char* const kInactiveMessage = "folded combat modules do not install in this run; the game keeps its own behaviour";

void WriteSkipped(Phase phase) {
  sentinel::LogFields(sentinel::LogLevel::kInfo, "combat_install",
                      {{"phase", PhaseName(phase)}, {"state", "skipped"}, {"variant", build_variant::VariantName(RecordedVariant())},
                       {"active", 0}, {"active_reason", ReasonName(ActiveReason())}, {"modules", 0}, {"installed", 0},
                       {"failed", 0}, {"msg", kInactiveMessage}});
}

}  // namespace

const char* PhaseName(Phase phase) noexcept { return phase == Phase::kEarly ? "early" : "normal"; }

Outcome RunPhase(Phase phase, const Module* table, std::size_t count) noexcept {
  Outcome outcome;
  int expected = kNotYet;
  if (!StateOf(phase).compare_exchange_strong(expected, kDone, std::memory_order_acq_rel)) return outcome;  // ran, or ended
  if (!Active()) {
    if (!g_inactiveSaid.exchange(true, std::memory_order_acq_rel)) WriteSkipped(phase);
    return outcome;
  }
  for (std::size_t i = 0; i < count; ++i) {
    const Module& module = table[i];
    const InstallFn fn = phase == Phase::kEarly ? module.early : module.normal;
    if (fn == nullptr) continue;
    const ModuleResult result = fn();
    ++outcome.modules;
    const char* outcomeName = "failed";
    if (result.off) {
      ++outcome.off;
      outcomeName = "off";
    } else if (result.installed) {
      ++outcome.installed;
      outcomeName = "installed";
    } else {
      ++outcome.failed;
    }
    // One record per module, as its step returns (N17: each outcome by name). A list of every module on the phase line grows with the table
    // and stops fitting one 768-byte sentinel line, so the phase line keeps the counts and the consequence, and each outcome is its own record.
    sentinel::LogFields(result.installed || result.off ? sentinel::LogLevel::kInfo : sentinel::LogLevel::kWarn, "combat_install_module",
                        {{"phase", PhaseName(phase)}, {"module", module.name}, {"outcome", outcomeName},
                         {"status", result.status != nullptr ? result.status : "unset"}});
  }
  outcome.ran = true;
  char msg[160];
  if (outcome.modules == 0) {
    std::snprintf(msg, sizeof(msg), "no combat module has a step in this phase yet; nothing of combat's is installed");
  } else {
    char off[80] = "";
    if (outcome.off != 0) std::snprintf(off, sizeof(off), "; %zu off by %s config key or with nothing to act on", outcome.off, outcome.off == 1 ? "its" : "their");
    std::snprintf(msg, sizeof(msg), "%zu of %zu combat modules installed in this phase%s%s", outcome.installed, outcome.modules, off,
                  outcome.failed == 0 ? "" : "; the others keep the game's own behaviour");
  }
  sentinel::LogFields(outcome.failed == 0 ? sentinel::LogLevel::kInfo : sentinel::LogLevel::kWarn, "combat_install",
                      {{"phase", PhaseName(phase)}, {"state", "ran"}, {"variant", build_variant::VariantName(RecordedVariant())},
                       {"active", 1}, {"active_reason", ReasonName(ActiveReason())}, {"modules", outcome.modules},
                       {"installed", outcome.installed}, {"failed", outcome.failed}, {"off", outcome.off}, {"msg", msg}});
  return outcome;
}

const Module* ShippedModules(std::size_t* count) noexcept {
  *count = kModules.size();
  return kModules.data();
}

Outcome InstallEarly() noexcept { return RunPhase(Phase::kEarly, kModules.data(), kModules.size()); }

Outcome InstallNormal() noexcept { return RunPhase(Phase::kNormal, kModules.data(), kModules.size()); }

void RunEnd(const Module* table, std::size_t count) noexcept {
  if (!Active() || StateOf(Phase::kNormal).load(std::memory_order_acquire) != kDone) return;  // nothing was installed
  for (std::size_t i = 0; i < count; ++i) {
    if (table[i].end != nullptr) table[i].end();
  }
}

void NoteProcessEnd() noexcept { RunEnd(kModules.data(), kModules.size()); }

void NoteNormalNotStarted(const char* reasonToken) noexcept {
  if (!Active()) return;  // combat does not run here: nothing was owed
  int expected = kNotYet;
  if (!StateOf(Phase::kNormal).compare_exchange_strong(expected, kEnded, std::memory_order_acq_rel)) return;  // ran, or already ended
  sentinel::LogFields(sentinel::LogLevel::kWarn, "combat_install",
                      {{"phase", "normal"}, {"state", "not_started"}, {"reason", reasonToken != nullptr ? reasonToken : "unset"},
                       {"variant", build_variant::VariantName(RecordedVariant())}, {"active", 1},
                       {"active_reason", ReasonName(ActiveReason())}, {"modules", 0}, {"installed", 0}, {"failed", 0},
                       {"msg", "the combat modules' post-load step had no moment to run in, so none of them is installed; "
                               "the game keeps its own behaviour"}});
}

void LogConfig(const nevr_quest::CombatConfig& config) noexcept {
  const auto source = [](const nevr_quest::Source s) { return s == nevr_quest::Source::kFile ? "config" : "default"; };
  sentinel::LogFields(sentinel::LogLevel::kInfo, "combat_config",
                      {{"force", config.force.on ? 1 : 0}, {"force_source", config.force.source == nevr_quest::Source::kFile ? "config" : "default"},
                       {"heaps", config.heaps.on ? 1 : 0}, {"heaps_source", config.heaps.source == nevr_quest::Source::kFile ? "config" : "default"},
                       {"memsys", config.memsys.on ? 1 : 0}, {"memsys_source", config.memsys.source == nevr_quest::Source::kFile ? "config" : "default"},
                       {"refresh_rate", config.refreshRate.on ? 1 : 0}, {"refresh_rate_source", source(config.refreshRate.source)},
                       {"refresh_rate_lobby", config.refreshRateLobby.on ? 1 : 0}, {"refresh_rate_lobby_source", source(config.refreshRateLobby.source)},
                       {"refresh_rate_quest2_72", config.refreshRateQuest2At72.on ? 1 : 0},
                       {"refresh_rate_quest2_72_source", source(config.refreshRateQuest2At72.source)},
                       {"pfx", config.pfx.on ? 1 : 0}, {"pfx_source", source(config.pfx.source)},
                       {"pfxtev", config.pfxtev.on ? 1 : 0}, {"pfxtev_source", source(config.pfxtev.source)},
                       {"msg", "combat.force asks for the folded combat modules whatever the APK's variant; only a development build "
                               "honours it (see combat_active); a module's own key set to false turns that module off"}});
  // One record per module switch, from the registration table (combat_install.h).
  for (const Module& module : kModules) {
    if (module.switchMember == nullptr) continue;
    const nevr_quest::CombatSwitch& sw = config.*module.switchMember;
    sentinel::LogFields(sentinel::LogLevel::kInfo, "combat_config_switch",
                        {{"module", module.name}, {"key", module.switchKey}, {"on", sw.on ? 1 : 0}, {"source", source(sw.source)}});
  }
  // The two keys of the EC modules and POWER BOOST that are not a boolean (#2187): the EC client's words (a text, its own `combat_ec_words` line
  // says what was read) and POWER BOOST's stage (an integer).
  sentinel::LogFields(sentinel::LogLevel::kInfo, "combat_config_switch",
                      {{"module", "ec"}, {"key", "combat.ec_words"}, {"on", config.ec.on ? 1 : 0}, {"source", source(config.ecWords.source)}});
  sentinel::LogFields(sentinel::LogLevel::kInfo, "combat_config_switch",
                      {{"module", "powerboost"}, {"key", "combat.powerboost_stage"}, {"stage", config.powerboostStage.value},
                       {"on", config.powerboostStage.value > 0 ? 1 : 0}, {"source", source(config.powerboostStage.source)}});
  // A log line is one fixed 768-byte buffer (sentinel/hook_log.cpp): the headset gates' keys are a line of their own, so no line is cut.
  sentinel::LogFields(sentinel::LogLevel::kInfo, "combat_config_gates",
                      {{"gate_perm", config.gatePerm.on ? 1 : 0}, {"gate_perm_source", source(config.gatePerm.source)},
                       {"gate_bots", config.gateBots.on ? 1 : 0}, {"gate_bots_source", source(config.gateBots.source)},
                       {"gate_link", config.gateLink.on ? 1 : 0}, {"gate_link_source", source(config.gateLink.source)},
                       {"gate_link_libs_source", source(config.gateLinkLibs.source)},
                       {"msg", "the headset gates' keys (src/quest/combat/headset_gates.h); gate_link_libs is a text and is never logged, only where it came from"}});
}

void SetConfig(const nevr_quest::CombatConfig& config) noexcept { g_config = config; }

void ResetInstallForTest() noexcept {
  g_config = nevr_quest::CombatConfig{};
  StateOf(Phase::kEarly).store(kNotYet, std::memory_order_release);
  StateOf(Phase::kNormal).store(kNotYet, std::memory_order_release);
  g_inactiveSaid.store(false, std::memory_order_release);
}

}  // namespace nevr_quest::combat
