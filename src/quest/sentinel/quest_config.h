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
// turns the hardware dump on, and `diagnostics.ui_event_probe` (on by default) sets the invite/party tab event (#318).
// A feature whose prerequisite is off or missing is forced off with a logged reason.
//
// `graphics.texture_quality` (#943) is one of stock, very_low, low, medium, high, very_high, ultra (texture_quality.h); absent, it is
// texture_quality::kDefaultLevel (`stock` for now). It is not a feature and has no prerequisite. Any mistake in it gives
// `stock`, never the default: a word that is not a level (wrong case, a number, empty), a value that is not a word (a
// mapping, a list, a key with no value), an unset ${VAR}, a `graphics` section that is not a mapping, or a file that is
// rejected as a whole. Each is one Warning that names the key (and the word, when it is short printable ASCII). A key under
// `graphics` other than `texture_quality` (a misspelling, another case) is one Warning that names it, and changes nothing else.
// `stock` leaves the headset runtime's own eye-buffer size.
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

enum class Feature { kRedirect, kBridge, kLogin, kSocial, kHwDump, kPresenceNames, kPresenceLocal, kUiEventProbe };

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
};

// The texture-quality setting (#943), after the file and the default have spoken.
struct TextureChoice {
  texture_quality::Level level = texture_quality::kDefaultLevel;
  Source source = Source::kAbsent;  // kFile: the file gave the level; anything else: the default did, or a mistake fell back
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
  // The file's `plugins:` list as the PC parser read it (#438); empty when the file is absent or rejected. The plugin loader
  // plans from it (quest/plugin/plugin_runtime.h).
  std::vector<nevr::PluginSpec> plugins;
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

// The shared redirect policy (runtime/lifecycle/service_redirect) gated by the redirect and bridge
// features. nullopt leaves the game's original value untouched.
std::optional<std::string> ResolveQuestRedirect(const ResolvedConfig& config, const std::string& url,
                                           bool bridgeReady, unsigned bridgePort);

}  // namespace nevr_quest
