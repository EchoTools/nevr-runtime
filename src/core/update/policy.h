#pragma once

// The runtime's half of self-update, the decisions (docs/adr/0024-pc-self-update.md, sections 3, 6 and 8). Pure: no
// I/O, no windows.h, so a test drives every branch. The worker that acts on them is core/update/worker.h.
//
//   Decide        update.enabled / -no-update / a dev build / a host that cannot swap  ->  why no check runs
//   ResolveChannel  update.channel in config.yaml, else the installer's channel in update\state.json, else the core's default
//   BuildCurrentJson / ParseStageResult  the two JSON documents of the core's NevrPcStage (installer/include/nevr_install.h)
//   BuildStagedJson  update\staged.json, the file whose existence tells the host there is something to swap
//   PendingTarget / StateChannel / StateQuarantine  what the runtime reads from the host's files in the update folder
//   ConfirmDue     the 10 s of per-frame work that mark a swapped release as started (section 6)

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/update/names.h"

namespace nevr_update {

/// Why no check runs (ADR 0024, section 8: the `update disabled` line's `reason`). kNone means a check may run.
enum class Disabled { kNone, kDevBuild, kNoRoot, kFlag, kConfig, kHostCannotSwap };

/// The word the log line carries: dev_build, no_root, flag, config, host_cannot_swap; "none" for kNone.
const char* DisabledWord(Disabled reason);

struct Switches {
  bool dev_build = false;                  ///< `NEVR-PLUGIN-TRUST dev` (nevr_plugin_trust::IsDevBuild): a developer's runtime is never replaced ...
  /// ... unless the build also has a usable update source of its own (ResolveSource returned kUsed). Only a dev build ever sets it.
  bool dev_source_override = false;
  bool flag_no_update = false;             ///< `-no-update` on the command line
  std::optional<bool> config_enabled;      ///< `update.enabled` in config.yaml; unset means on
  bool host_can_swap = false;              ///< NevrHostContext::capabilities bit 0 (nevr_runtime_abi::HostCanSwap)
  bool no_root = false;                    ///< %LOCALAPPDATA% is not set or does not fit: there is no nevr root to stage into or confirm in
};

/// The first reason that applies, in this order: dev build without a source of its own, no root, flag, config, host. The order is the log's: a dev build says
/// dev_build even when the flag is also given. No root comes before the switches because it stops the confirmation too.
Disabled Decide(const Switches& switches);

/// What a dev build may use in place of the release host and the pinned keys (`update.base`, `update.pins` of config.yaml). The core
/// takes them as `base` and `pins` of its open-config JSON (installer/src/ffi.rs config_from_json); `pins` REPLACES the built-in keys
/// there (Pins::parse is assigned, not merged), so a dev build with pins trusts only those keys for that base.
struct SourceOverride {
  std::string base;  ///< http(s) base, no trailing '/', no credentials, query or fragment; the core joins "<base>/<name>"
  std::string pins;  ///< the core's pin text, "LABEL BASE64KEY" lines; empty means the core's built-in pins
};

enum class SourceVerdict { kNone, kUsed, kIgnoredRelease, kRejected };
/// none, used, ignored, rejected: the `outcome` field of the `update source` line.
const char* SourceVerdictWord(SourceVerdict verdict);

struct SourceResolution {
  SourceVerdict verdict = SourceVerdict::kNone;
  SourceOverride use;  ///< set when verdict is kUsed
  /// For kIgnoredRelease and kRejected: the config key the verdict is about (update.base or update.pins) and a plain word.
  std::string key;
  std::string reason;
  bool pins_given = false;
  std::size_t pins_count = 0;  ///< kUsed: how many keys `use.pins` holds
};

/// The longest base and pins text a config may hold: a bound on a value read from a file, not a device limit.
inline constexpr std::size_t kMaxSourceBaseBytes = 200;
inline constexpr std::size_t kMaxSourcePinsBytes = 4096;
/// A minisign public key as the core pins it: 42 bytes, 56 base64 characters (installer/src/manifest.rs Pins::parse).
inline constexpr std::size_t kPinKeyBase64Chars = 56;

/// The one place a source override is decided. `base` and `pins` are the raw `update.base` and `update.pins` values (nullopt: the key
/// is absent). A RELEASE build (dev_build false) uses neither, whatever they hold: kIgnoredRelease when either is given, kNone when
/// neither is. A dev build uses the pair only when both parts are valid: base is http:// or https://, a host, no control or space
/// character, no '@', '?' or '#'; pins (only with a base) are pin lines of a label and a 56-character base64 key. Anything else is
/// kRejected with the key and a reason word, and nothing is used: a dev build then stays disabled (dev_build).
SourceResolution ResolveSource(bool dev_build, const std::optional<std::string>& base, const std::optional<std::string>& pins);

enum class ChannelSource { kConfig, kState, kDefault };
/// config, state, default: the `channel_source` field of the `update check` line.
const char* ChannelSourceWord(ChannelSource source);

struct Channel {
  /// Empty for kDefault: the core picks its own default channel.
  std::string name;
  ChannelSource source = ChannelSource::kDefault;
  /// A value was given in config.yaml and is not a channel name ([A-Za-z0-9._-]{1,32}); it was ignored.
  bool config_rejected = false;
};

/// update.channel from config.yaml, else `channel` from update\state.json, else the core's default. A value that is not a
/// channel name is never passed on; an empty one counts as absent.
Channel ResolveChannel(const std::optional<std::string>& config_channel, const std::optional<std::string>& state_channel);

struct QuarantineEntry {
  std::string version;
  std::string runtime_sha256;
};

/// `{"version", "runtime_sha256", "quarantine": [{"version", "runtime_sha256"}]}`, the NevrPcStage `current_json`.
std::string BuildCurrentJson(const std::string& version, const std::string& runtime_sha256,
                             const std::vector<QuarantineEntry>& quarantine);

struct StagedFile {
  std::string path;  ///< the platform's runtime ("nevr-runtime.dll", "libnevr-runtime.so") or "plugins/<name>.dll" (PC only)
  std::uint64_t size = 0;
  std::string sha256;
};

/// The NevrPcStage result: {"action": "none"|"staged"|"blocked", "version", "direction", "reason", "files": [...]}.
struct StageResult {
  std::string action;
  std::string version;
  std::string direction;
  std::string reason;
  std::vector<StagedFile> files;
};

/// False, with `error`, when the text is not a result of that shape or names a version, path or hash outside the
/// vocabulary the host accepts (core/update/names.h): such a result is never published. The members are those of the
/// platform whose runtime is `runtime_file` (names.h ParseMemberOf: nevr-runtime.dll and its plugins on PC, libnevr-runtime.so
/// alone on Quest), and a staged result lists that runtime exactly once.
bool ParseStageResult(const std::string& text, StageResult* out, std::string* error,
                      std::string_view runtime_file = nevr_update_names::kRuntimeMember);

/// update\staged.json: {"version", "files": [{"path", "size", "sha256"}]}, the spelling the host reads.
std::string BuildStagedJson(const StageResult& staged);

/// `to.sha256` and `to.version` of update\pending.json. False when the text is not a pending record.
struct PendingTarget {
  std::string version;
  std::string sha256;
};
bool ParsePendingTarget(const std::string& text, PendingTarget* out);

/// `channel` of update\state.json, when it has a string one.
std::optional<std::string> StateChannel(const std::string& state_text);
/// `quarantine` of update\state.json: each entry with a version and a runtime sha256. Entries without both are skipped.
std::vector<QuarantineEntry> StateQuarantine(const std::string& state_text);

/// update\state.json after a confirmation: `current` is the confirmed release; every other key (owned, discards, quarantine,
/// channel) is kept as it was. False when `state_text` is not a JSON object.
bool ConfirmState(const std::string& state_text, const std::string& version, const std::string& runtime_sha256,
                  std::string* out);

/// The per-frame work, in milliseconds, after the first call that marks a swapped release as started (section 6).
inline constexpr std::uint64_t kConfirmAfterMs = 10000;
/// True once `now_ms` is at least kConfirmAfterMs after `first_ms`. A clock that went backwards is not due.
bool ConfirmDue(std::uint64_t first_ms, std::uint64_t now_ms);

}  // namespace nevr_update
