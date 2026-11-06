// Quest client configuration and feature activation: pure, no Android or game headers, so the
// host test and the NDK build compile this one source.
//
// The file is `config.yaml` in the app's external files directory: the same file format and parser as the PC
// (nevr_config, src/core/nevr_config.cpp), so the keys are the PC's keys (docs/reference/example-config.yaml).
// It is never `config.json`, never a `.env`, and nothing here reads the environment (the `${VAR}`
// interpolation in a value is nevr_config's, as on the PC).
//
// Precedence per key: file value, else the value embedded at build time, else absent. A value in the file that
// is invalid (including the empty string) is rejected and the embedded default stays: the file cannot clear a
// default. A file that is unusable as a whole (a YAML error, an unknown top-level key, a top level that is not a
// mapping, a malformed `plugins` list, an unset `${VAR:?}`, nesting or alias expansion past nevr_config's bounds,
// too large) contributes nothing: the embedded defaults stay and one Error line names the file and why.
//
// Features have no switch of their own except where the PC has a key. Redirect is on when a target resolves
// (`services.socket_uri` or `auth.http_uri`), the bridge when the redirect is on and `services.socket_uri`
// resolves, login when the bridge is on and `auth.server_key` resolves; the social facade is on unless
// `social.facade` is false (PC's key and default) and still needs login here; its status names ride with it.
// `social.send_presence_to_meta: false` answers the rich presence locally; `diagnostics.hardware_dump: true`
// turns the hardware dump on, `diagnostics.ui_event_probe` (on by default) sets the invite/party tab event (#318),
// `diagnostics.pnsrad_provider: true` loads libpnsrad as the platform provider instead of libpnsovr (#1220; needs login), and
// `diagnostics.clock_levels_override` (default false) decides whether the CPU and GPU clock levels libr15 asks VrApi for are
// raised from (2, 2) to (4, 4) (#928), and `network.invite_codes` (#966) turns the invite codes on (off unless the file says on;
// it needs login and `auth.http_uri`).
// A feature whose prerequisite is off or missing is forced off with a logged reason.
//
// `graphics.texture_quality` (#943) is one of stock, very_low, low, medium, high, very_high, ultra (texture_quality.h); absent, it is
// texture_quality::kDefaultLevel (`stock` for now). It is not a feature and has no prerequisite. Any mistake in it gives
// `stock`, never the default: a word that is not a level (wrong case, a number, empty), a value that is not a word (a
// mapping, a list, a key with no value), an unset ${VAR}, a `graphics` section that is not a mapping, or a file that is
// rejected as a whole. Each is one Warning that names the key (and the word, when it is short printable ASCII). A key under
// `graphics` other than `texture_quality` (a misspelling, another case) is one Warning that names it, and changes nothing else.
// `stock` leaves the headset runtime's own eye-buffer size. The key is also where a choice made in the Texture Resolution control is
// kept (texture_menu_hook.h): the sentinel writes it through NevrConfig::Set, so the next launch reads the player's last choice here.
//
// `graphics.hand_flip` (#2082) is the Controller Hand Flip choice (hand_flip.h): a boolean, `true` is On; absent, it is Off (nothing is
// written for a default). A value that is no boolean (a word, a number, a list, a mapping, a key with no value, an unset ${VAR}) is Off
// with one Warning that names the key, never the value. It is also where a choice made in the menu is kept: the sentinel writes it
// through NevrConfig::Set (hand_flip.h), so the next launch reads the player's last choice here. A file rejected as a whole says
// nothing about it: Off, from the default.
//
// `combat.force` (#2170, docs/adr/0046 rule 5) is a boolean: `true` asks for the folded combat modules to run whatever the APK's
// variant. Absent, it is not asked (from the default). A value that is no boolean (a word, a number, a list, a mapping, a key with no
// value, an unset ${VAR}) is not asked, with one Warning that names the key, never the value. Only a development build honours it
// (combat::Decide); a release build refuses it with a Warning of its own. The configuration reads the request, nothing more; the `combat_config` line reports it (combat/combat_install.h).
//
// `combat.heaps` and `combat.memsys` (#2175) are booleans, true when absent: `false` leaves the game's heap sizes (heaps) or its render-target
// pool and System heap size (memsys) as the game has them. A value that is no boolean is true, with one Warning that names the key, never the
// value. They only matter in the q15t and combat APKs, where the modules run.
//
// `combat.meshstep`, `combat.instmodel`, `combat.shaderstep` and `combat.tablestep` (#2177) are booleans in the same way, true when absent: `false`
// leaves the game's table growth step of 32 for that resource class even in a combat match. All four are on by default because the upstream build the
// community combat port ships turns the same change on (`--resource-table-step-1`, in its wave-2 recipe).
#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "core/nevr_config.h"
#include "quest/sentinel/texture_quality.h"

namespace nevr_quest {

inline constexpr const char* kConfigFileName = "config.yaml";
// A file of this name in the files directory is not read; the sentinel reports once that it is present and ignored.
inline constexpr const char* kLegacyConfigFileName = "nevr-quest.json";
inline constexpr std::size_t kMaxConfigBytes = 64 * 1024;

enum class Feature {
  kRedirect,
  kBridge,
  kLogin,
  kSocial,
  kHwDump,
  kPresenceNames,
  kPresenceLocal,
  kUiEventProbe,
  kPnsradProvider,
  kClockLevels,
  kInviteCodes,
};

enum class Source { kAbsent, kEmbedded, kFile };

enum class LogLevel { kInfo, kWarn, kError };

// What the build embedded (empty string = nothing embedded for that key).
struct EmbeddedDefaults {
  const char* socketUri = "";
  const char* httpUri = "";
  const char* httpKey = "";
  const char* serverKey = "";
};

struct Value {
  std::string text;
  Source source = Source::kAbsent;
};

struct Features {
  bool redirect = false;
  bool bridge = false;
  bool login = false;
  bool social = false;
  // The hardware/environment dump (#335): `diagnostics.hardware_dump`, never on unless the file asks for it.
  bool hwdump = false;
  // Answer the rich presence's destination lookup from a built-in table ("Social Lobby", "Arena", ...) when
  // Meta's GetDestinations failed (#393): the status under the player's name stops reading the game's second
  // field. On whenever the social facade is (it has no key); needs the facade (the status travels in its member
  // data).
  bool presenceNames = false;
  // Rich presence is not sent to Meta's platform service (#396, option b of #393): the presence object's
  // ShareData, RefreshDestinations and Clear are answered locally, so no group_presence request leaves (they
  // fail for this APK's identity anyway). Each changed presence document the game sets is published to the NEVR
  // session as SNSPresenceUpdateRequest instead (the server half is in nakama; a server without it drops the frame).
  // `social.send_presence_to_meta: false` turns it on; needs the social facade.
  bool presenceLocal = false;
  // The invite/party tab event (#391, #318): the Invite Members and own-id Party slots also post the friends script
  // event, which switches the tablet to its Friends list. On by default; `diagnostics.ui_event_probe: false` turns it
  // off, and it needs the social facade.
  bool uiEventProbe = true;
  // Raise the clock levels libr15 asks VrApi for from (2, 2) to (4, 4) (#928): `diagnostics.clock_levels_override`. The
  // render-configuration hook's handler reports the levels whatever this says; this decides whether it rewrites them. No
  // prerequisite. Off by default, in the struct and in the configuration: it stays off until a headset A/B shows a benefit.
  bool clockLevels = false;
  // The game's platform provider is its own libpnsrad, never libpnsovr (#1220, docs/adr/0026): `diagnostics.pnsrad_provider: true`.
  // Off by default: nothing sends the login on the rad provider yet (#1220). Needs login.
  bool pnsradProvider = false;
  // The invite codes for the CREATE GAME / JOIN VIA CODE wizard (#966, docs/adr/0017): `network.invite_codes`, read with the
  // PC's switch words and off unless the file says on. Needs login (the token) and auth.http_uri (where the token goes).
  bool inviteCodes = false;
};

// The texture-quality setting (#943), after the file and the default have spoken.
struct TextureChoice {
  texture_quality::Level level = texture_quality::kDefaultLevel;
  Source source = Source::kAbsent;  // kFile: the file gave the level; anything else: the default did, or a mistake fell back
};

// The Controller Hand Flip setting (#2082), after the file and the default have spoken.
struct HandFlipChoice {
  bool on = false;
  Source source = Source::kAbsent;  // kFile: the file gave the value (true or false); anything else: the default, Off
};

// The combat.force request (#2170, docs/adr/0046 rule 5), after the file and the default have spoken: the file asks for the folded
// combat modules to run whatever the APK's variant. Whether it is honoured is combat::Decide's rule (combat/combat_active.h).
struct CombatForceChoice {
  bool on = false;
  Source source = Source::kAbsent;  // kFile: the file gave the value (true or false); anything else: the default, not asked
};

// A folded combat module's switch (docs/adr/0046 rule 4): `combat.<module>`, a boolean that is on unless the file says false.
struct CombatSwitch {
  bool on = true;
  Source source = Source::kAbsent;  // kFile: the file gave the value (true or false); anything else: the default, on
};

// A folded combat module's text value (`combat.gate_link_libs`). `source` is kFile when the file gave a string (an empty one included: it means
// "none"); otherwise the module's default stands and `text` is empty. A fixed buffer, so the configuration stays trivially copyable.
inline constexpr std::size_t kCombatTextMax = 640;
struct CombatText {
  char text[kCombatTextMax] = {};
  Source source = Source::kAbsent;
};

// A folded combat module's integer (`combat.powerboost_stage`). `source` is kFile when the file gave an integer in the module's range; otherwise
// `value` is the module's default.
struct CombatInt {
  int value = 0;
  Source source = Source::kAbsent;
};

// The `combat:` section (docs/adr/0046 rule 4): one member per key, each defaulting to the shipped behaviour. The keys are `force` and, for
// each folded module, its switch; a module's unit adds its member here, with the row in KnownSections (src/core/nevr_config.cpp), the
// example config and the `combat_config` line (combat/combat_install.h).
struct CombatConfig {
  CombatForceChoice force;
  CombatSwitch heaps;   // `combat.heaps`: the four heap sizes (combat/heaps.h)
  CombatSwitch memsys;  // `combat.memsys`: the render-target pool and, with it, the System heap size (combat/memsys.h)
  CombatSwitch refreshRate{true, Source::kAbsent};            // `combat.refresh_rate`: the level rate rule (off: the game's rate always)
  CombatSwitch refreshRateLobby{false, Source::kAbsent};      // `combat.refresh_rate_lobby`: the combat lobby asks for the level rate too (default OFF: arena shares the lobby levels)
  CombatSwitch refreshRateQuest2At72{true, Source::kAbsent};  // `combat.refresh_rate_quest2_72`: a Quest 2's level rate is 72, not 90 (combat/refresh_rate.h)
  CombatSwitch meshstep;    // `combat.meshstep`: mesh-list tables grow by 1 in a combat match (combat/meshstep.h)
  CombatSwitch instmodel;   // `combat.instmodel`: instanced-model tables grow by 1 in a combat match (combat/instmodel.h)
  CombatSwitch shaderstep;  // `combat.shaderstep`: shader-set tables built by ConstructInstance grow by 1 in a combat match (combat/shaderstep.h)
  CombatSwitch tablestep;   // `combat.tablestep`: shader-set tables built by CreateInstance grow by 1 in a combat match (combat/tablestep.h)
  CombatSwitch gatePerm{true, Source::kAbsent};   // `combat.gate_perm`: a Quest in a party may use combat (combat/headset_gates.h)
  CombatSwitch gateBots{true, Source::kAbsent};   // `combat.gate_bots`: a combat match picks the combat bot body (only where levelname is installed)
  CombatSwitch gateLink{true, Source::kAbsent};   // `combat.gate_link`: the combat menus are not told this is a Quest
  CombatText gateLinkLibs;                        // `combat.gate_link_libs`: which script libraries the menu gate answers; unset: the module's 21
  CombatSwitch pfx;     // `combat.pfx`: the particle-renderer gate, open in combat maps only (combat/pfx.h)
  CombatSwitch pfxtev;  // `combat.pfxtev`: the event-effect id decision, combat maps only (combat/pfxtev.h)
  CombatSwitch ec;      // `combat.ec`: the EC network client and the writer of re33_ec_page (combat/ec.h)
  CombatText ecWords;   // `combat.ec_words`: the EC client's words; unset: `types=VPTO equip hide` (combat/ec.h)
  CombatSwitch ecfx;    // `combat.ecfx`: the NEUTRON effects drain on the EC client's net tick (combat/ecfx.h)
  CombatInt powerboostStage{3, Source::kAbsent};  // `combat.powerboost_stage`: POWER BOOST's stage, 0 off .. 3 swallow + cone (combat/powerboost.h)
  CombatSwitch dronekill{true, Source::kAbsent};    // `combat.dronekill`: a combat drone's kill, lock-on, gun, chase and shot rules (combat/dronekill.h)
  CombatSwitch dronemuzzle{true, Source::kAbsent};  // `combat.dronemuzzle`: a drone's bullets leave its gun; needs dronekill's shot (combat/dronemuzzle.h)
};

struct LogEvent {
  LogLevel level;
  std::string message;  // never contains a configured value, only key and feature names; the one exception is a rejected
                        // graphics.texture_quality word, named when it is at most 32 printable ASCII characters
};

struct ResolvedConfig {
  Value socketUri;
  Value httpUri;
  Value httpKey;
  Value serverKey;
  Features requested;  // what the file and the defaults ask for
  Features effective;  // requested, after prerequisite checks
  TextureChoice textureQuality;
  HandFlipChoice handFlip;
  CombatConfig combat;
  // The file's `plugins:` list as the PC parser read it (#438); empty when the file is absent or rejected. The plugin loader
  // plans from it (quest/plugin/plugin_runtime.h).
  std::vector<nevr::PluginSpec> plugins;
  // The file existed and was rejected as a whole, so `plugins` is empty and the embedded defaults stand. The plugin loader
  // reads it to say on each packaged plugin's record that the operator's file did not load (docs/adr/0019 section 2).
  bool fileRejected = false;
};

struct LoadResult {
  ResolvedConfig config;
  bool fileRejected = false;  // the file existed but was not usable as a whole
  std::vector<LogEvent> events;
};

// `fileText` is null when the file does not exist. `filePath` is only what the log lines name. Never throws:
// every exception the YAML parser raises is caught here, by name, and becomes a rejected file.
LoadResult ResolveConfig(const EmbeddedDefaults& defaults, const std::string* fileText,
                         const std::string& filePath = kConfigFileName);

// `<filesDir>/config.yaml` and `<filesDir>/nevr-quest.json`, tolerant of a trailing slash.
std::string ConfigFilePath(const std::string& filesDir);
std::string LegacyConfigFilePath(const std::string& filesDir);

bool FeatureEnabled(const ResolvedConfig& config, Feature feature);
const char* FeatureName(Feature feature);
const char* SourceName(Source source);
// "config" when the file gave the texture level, else "default": the words the install log line and the config report use.
const char* TextureSourceName(const TextureChoice& choice);
// "config" when the file gave the hand flip value, else "default".
const char* HandFlipSourceName(const HandFlipChoice& choice);

// The shared redirect policy (runtime/lifecycle/service_redirect) gated by the redirect and bridge
// features. nullopt leaves the game's original value untouched.
std::optional<std::string> ResolveQuestRedirect(const ResolvedConfig& config, const std::string& url,
                                           bool bridgeReady, unsigned bridgePort);

}  // namespace nevr_quest
