#include "core/cargo_session_pin.h"

#include <algorithm>
#include <utility>

#include "core/combat_level_class.h"
#include "core/map_session_pin.h"

namespace nevr_cargo_pin {

namespace {

constexpr const char* kHexDigits = "0123456789abcdef";
constexpr std::size_t kHex16Digits = 16;        // the record's type and name, and the digest's prefix the pin reads
constexpr std::size_t kHexBitsPerDigit = 4;
constexpr std::uint64_t kHexDigitMask = 0xF;
constexpr unsigned kHexLetterBase = 10;         // 'a' and 'A' read as 10
constexpr std::uint64_t kPinFlip = 1;           // the pin of a value that is 0 or kCargoPinNone is the value XOR this
constexpr const char* kEmptyDigest = "empty";   // the digest the line of an empty record carries

// The reasons (PLAN-v7 5.5 table) a decision names; the refused tokens are the gate's own (cdn_script_gate.h) or the registrar's.
constexpr const char* kReasonNotCombat = "not_combat";
constexpr const char* kReasonUnpinned = "unpinned";
constexpr const char* kReasonLabelOff = "label_off";
constexpr const char* kReasonRequiredOnly = "required_only";
constexpr const char* kReasonSignedOnly = "signed_only";
constexpr const char* kReasonPackHash = "pack_hash";
constexpr const char* kReasonRefused = "refused";
constexpr const char* kReasonServerNone = "server_none";
constexpr const char* kReasonNotDownloaded = "not_downloaded";
constexpr const char* kReasonUndeliverable = "undeliverable";
constexpr const char* kReasonNoSpace = "no_space";
constexpr const char* kReasonUpdated = "updated";
constexpr const char* kReasonServerBehind = "server_behind";
constexpr const char* kReasonUnchecked = "unchecked";
constexpr const char* kTokenSignedOnly = "signed_only";
constexpr const char* kTokenPackHash = "pack_hash";

// The notice lines (PLAN-v7 5.5, rows 4-14). Each is at most nevr_map_session_pin::kNoticeLineMax bytes; the test pins the counts.
constexpr const char* kLineLabelOff = "EchoCombat is off in config.yaml (assets.labels): turn it on";
constexpr const char* kLineRequiredOnly = "EchoCombat is off: remove assets.required_only in config.yaml";
constexpr const char* kLineSignedOnly = "EchoCombat is off under -require-signed-plugins: drop the flag";
constexpr const char* kLineScriptShadowed = "Old EchoCombat files in bin\\win10\\scripts block it: remove them";
constexpr const char* kLinePackHash = "A content pack replaces EchoCombat: remove the pack to play";
constexpr const char* kLineRefusedHead = "EchoCombat refused (";
constexpr const char* kLineRefusedTail = "); see the nevr log";
constexpr const char* kLineServerNone = "This server has no EchoCombat content: join another match";
constexpr const char* kLineNotDownloaded = "EchoCombat not downloaded yet: restart to play this match";
constexpr const char* kLineUndeliverable = "EchoCombat could not be downloaded: play another mode for now";
constexpr const char* kLineNoSpace = "No disk space for EchoCombat: free some space, then restart";
constexpr const char* kLineUpdated = "EchoCombat was updated: restart the game to play this match";
constexpr const char* kLineServerBehind = "This server's EchoCombat is out of date: join another match";
constexpr const char* kLineUnchecked = "EchoCombat differs from this server: restart the game to update";

// `value` as 16 lowercase hex digits, zero-padded, no 0x.
std::string Hex16(std::uint64_t value) {
  std::string text(kHex16Digits, '0');
  for (std::size_t i = 0; i < kHex16Digits; ++i) {
    text[kHex16Digits - 1 - i] = kHexDigits[value & kHexDigitMask];
    value >>= kHexBitsPerDigit;
  }
  return text;
}

unsigned HexValue(char c) {
  if (c >= '0' && c <= '9') return static_cast<unsigned>(c - '0');
  if (c >= 'a' && c <= 'f') return kHexLetterBase + static_cast<unsigned>(c - 'a');
  if (c >= 'A' && c <= 'F') return kHexLetterBase + static_cast<unsigned>(c - 'A');
  return 0;
}

// The first kHex16Digits characters of a hex digest, read big-endian.
std::uint64_t ParseHex16(const std::string& text) {
  std::uint64_t value = 0;
  for (std::size_t i = 0; i < kHex16Digits && i < text.size(); ++i) {
    value = (value << kHexBitsPerDigit) | HexValue(text[i]);
  }
  return value;
}

std::string RecordLine(const std::string& label, const std::string& platform, std::uint64_t type, std::uint64_t name,
                       const std::string& digest) {
  return label + "\t" + platform + "\t" + Hex16(type) + "\t" + Hex16(name) + "\t" + digest + "\n";
}

CargoDecision Stay(std::string reason) {
  CargoDecision decision;
  decision.reason = std::move(reason);
  return decision;
}

CargoDecision Leave(std::string reason, std::string line) {
  CargoDecision decision;
  decision.leave = true;
  decision.reason = std::move(reason);
  decision.line = std::move(line);
  return decision;
}

}  // namespace

std::string CargoPinText(const nevr_cdn::Manifest& manifest, const nevr_cdn::Platform& platform, const nevr_cdn::Policy& policy) {
  const std::string platform_name = platform.name;
  std::vector<std::string> lines;
  for (std::size_t i = 0; i < kCargoPinLabelCount; ++i) {
    const std::string label = kCargoPinLabels[i];
    if (nevr_cdn::StateOf(label, policy) == nevr_cdn::LabelState::kOff) continue;
    for (const nevr_cdn::Entry& e : manifest.entries) {
      if (e.label != label || e.platform != platform_name) continue;
      lines.push_back(RecordLine(label, platform_name, e.type, e.name, e.sha256));
    }
    for (const nevr_cdn::EmptyRecord& r : manifest.empties) {
      if (r.label != label || (!r.platform.empty() && r.platform != platform_name)) continue;
      lines.push_back(RecordLine(label, platform_name, r.type, r.name, kEmptyDigest));
    }
  }
  std::sort(lines.begin(), lines.end());
  std::string text;
  for (const std::string& line : lines) text += line;
  return text;
}

std::uint64_t CargoPinFromHex(const std::string& sha256_hex) {
  std::uint64_t value = ParseHex16(sha256_hex);
  if (value == kCargoPinAbsent || value == kCargoPinNone) value ^= kPinFlip;
  return value;
}

std::uint64_t CargoPinOf(const nevr_cdn::Manifest& manifest, const nevr_cdn::Platform& platform, const nevr_cdn::Policy& policy,
                         const Sha256HexFn& sha256_hex) {
  const std::string text = CargoPinText(manifest, platform, policy);
  if (text.empty()) return kCargoPinNone;
  return CargoPinFromHex(sha256_hex(text));
}

bool IsCargoPinLevel(std::uint64_t level) {
  return nevr_combat_level::IsCombatMap(level) || level == kCombatPebblesLevel || level == kCombatPebblesPayloadLevel;
}

CargoBootStatus BootStatus(const nevr_cdn::Mounted& mounted, const std::vector<nevr_cdn::GatedLabel>& gated,
                           const nevr_cdn::Policy& policy) {
  for (std::size_t i = 0; i < kCargoPinLabelCount; ++i) {
    const std::string label = kCargoPinLabels[i];
    for (const nevr_cdn::GatedLabel& g : gated) {
      if (g.label == label) return CargoBootStatus{CargoBoot::kRefused, g.reason};
    }
    const nevr_cdn::LabelState state = nevr_cdn::StateOf(label, policy);
    if (state == nevr_cdn::LabelState::kOff) return CargoBootStatus{CargoBoot::kOff, std::string()};
    if (policy.required_only && state != nevr_cdn::LabelState::kRequired) {
      return CargoBootStatus{CargoBoot::kRequiredOnlyOff, std::string()};
    }
    const nevr_cdn::LabelMount* share = nullptr;
    for (const nevr_cdn::LabelMount& lm : mounted.per_label) {
      if (lm.label == label) share = &lm;
    }
    const bool whole = share != nullptr && share->missing == 0 && share->refused == 0 && share->names > 0;
    if (!whole) return CargoBootStatus{CargoBoot::kNotWhole, std::string()};
  }
  return CargoBootStatus{CargoBoot::kWhole, std::string()};
}

std::uint64_t CargoBootPin(const nevr_cdn::Manifest& boot, const nevr_cdn::Platform& platform, const nevr_cdn::Mounted& mounted,
                           const std::vector<nevr_cdn::GatedLabel>& gated, const nevr_cdn::Policy& policy,
                           const Sha256HexFn& sha256_hex) {
  if (BootStatus(mounted, gated, policy).state != CargoBoot::kWhole) return kCargoPinNone;
  return CargoPinOf(boot, platform, policy, sha256_hex);
}

nevr_map_session_pin::CargoOutcome OutcomeOf(const CargoView& view, std::uint64_t level, std::uint64_t server_pin) {
  const CargoDecision decision = DecideCargo(view, level, server_pin);
  nevr_map_session_pin::CargoOutcome outcome;
  outcome.leave = decision.leave;
  outcome.id = kCargoPinLabels[0];
  outcome.reason = decision.reason;
  outcome.title = kCargoNoticeTitle;
  outcome.line = decision.line;
  outcome.local = view.local;
  outcome.manifest_known = view.manifest_known;
  outcome.manifest = view.manifest;
  return outcome;
}

CargoDecision DecideCargo(const CargoView& view, std::uint64_t level, std::uint64_t server_pin) {
  if (!IsCargoPinLevel(level)) return Stay(kReasonNotCombat);
  if (server_pin == kCargoPinAbsent) return Stay(kReasonUnpinned);
  // Row 3 (PLAN-v7 5.5, "The rows, first match wins"): `server_pin == B` stays, with no further condition. Two "no set" pins
  // (B == S == kCargoPinNone) therefore stay here; row 10 is reached only by a client that has a set (PLAN-v7 5.5 row 10, and
  // PLAN-v7 5.5, the R11 test list, test 11: "a client with a set leaves with row 10").
  if (server_pin == view.local) return Stay(std::string());
  if (view.status.state == CargoBoot::kOff) return Leave(kReasonLabelOff, kLineLabelOff);
  if (view.status.state == CargoBoot::kRequiredOnlyOff) return Leave(kReasonRequiredOnly, kLineRequiredOnly);
  if (view.status.state == CargoBoot::kRefused) {
    const std::string& token = view.status.token;
    if (token == kTokenSignedOnly) return Leave(kReasonSignedOnly, kLineSignedOnly);
    if (token == nevr_cdn::kGateShadowed) return Leave(nevr_cdn::kGateShadowed, kLineScriptShadowed);
    if (token == kTokenPackHash) return Leave(kReasonPackHash, kLinePackHash);
    return Leave(kReasonRefused,
                 nevr_map_session_pin::FitLine({{kLineRefusedHead}, {token, true}, {kLineRefusedTail}}));
  }
  if (server_pin == kCargoPinNone) return Leave(kReasonServerNone, kLineServerNone);
  if (view.manifest_known && server_pin == view.manifest) {
    if (view.status.state == CargoBoot::kWhole) return Leave(kReasonUpdated, kLineUpdated);
    if (view.fetch == CargoFetch::kUndeliverable) return Leave(kReasonUndeliverable, kLineUndeliverable);
    if (view.fetch == CargoFetch::kNoSpace) return Leave(kReasonNoSpace, kLineNoSpace);
    return Leave(kReasonNotDownloaded, kLineNotDownloaded);
  }
  if (view.manifest_known) return Leave(kReasonServerBehind, kLineServerBehind);
  CargoDecision decision = Leave(kReasonUnchecked, kLineUnchecked);
  decision.write_marker = true;
  return decision;
}

}  // namespace nevr_cargo_pin
