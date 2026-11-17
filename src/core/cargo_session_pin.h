// The cargo pin (PLAN-v7 5.5, R7): the one value that says which EchoCombat content set a run holds, and the decision a client makes
// from it against a server's pin. Pure: no Env, no Log(), no game. The PC wiring (R11) feeds it the boot mount, the fetch state and
// the manifest of each fetch, and the session pin's tick (core/map_session_pin.h) acts on the decision.
//
// The pin is the SHA-256 of the sorted records of the pin labels (kCargoPinLabels) for one platform, taken over the records of the
// manifest the set was mounted from. A set whose pin label did not mount whole has no pin (kCargoPinNone), so a partial set never
// matches a full one.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "cdn/cdn_manifest.h"
#include "cdn/cdn_plan.h"
#include "cdn/cdn_script_gate.h"
#include "cdn/cdn_store.h"
#include "core/map_session_pin.h"

namespace nevr_cargo_pin {

// The labels the pin covers, the same list on both roles (a later label with `scripts` joins it in code, PLAN-v7 5.5 "Labels").
inline constexpr const char* kCargoPinLabels[] = {"combat_content"};
inline constexpr std::size_t kCargoPinLabelCount = 1;

// The pin of a set that is not whole, and of a server with no set: CSymbol64Hash("nevr_cargo_pin_none") (symbol_hash.h). Never 0:
// 0 is the absent key.
inline constexpr std::uint64_t kCargoPinNone = 0x3a6a199923324d75ULL;
// The owner JSON's value for "no pin" from an older server: the key is absent.
inline constexpr std::uint64_t kCargoPinAbsent = 0;

// The title of a cargo notice (the map pin's title is its own).
inline constexpr const char* kCargoNoticeTitle = "EchoCombat content mismatch";

// The two payload levels the game's session starts with that carry EchoCombat, next to the four combat maps (PLAN-v7 5.5 "Scope").
inline constexpr std::uint64_t kCombatPebblesLevel = 0xdf4aaba6cea689c9ULL;     // mpl_combat_pebbles
inline constexpr std::uint64_t kCombatPebblesPayloadLevel = 0xea49cde332aa536fULL;  // pty_mpl_combat_pebbles

// The digest function the pin takes: the SHA-256 of `bytes` as 64 lowercase hex characters (the run's hasher on the PC, the portable
// one in tests).
using Sha256HexFn = std::function<std::string(const std::string& bytes)>;

// The input text of the pin for `manifest` on `platform`: for each pin label that `policy` serves, every entry and every empty record
// of the label whose platform is `platform.name` (an empty record whose platform is "" belongs to the plan's platform), one line each,
// `<label>\t<platform>\t<type hex16>\t<name hex16>\t<sha256 or "empty">\n`, sorted bytewise and concatenated. Empty when no record.
std::string CargoPinText(const nevr_cdn::Manifest& manifest, const nevr_cdn::Platform& platform, const nevr_cdn::Policy& policy);

// The pin of the records of `manifest` for `platform` under `policy`: the first 8 bytes of the text's SHA-256, big-endian, with 0 and
// kCargoPinNone replaced by the value XOR 1. kCargoPinNone when the text is empty (no record of a pin label for this platform).
std::uint64_t CargoPinOf(const nevr_cdn::Manifest& manifest, const nevr_cdn::Platform& platform, const nevr_cdn::Policy& policy,
                         const Sha256HexFn& sha256_hex);

// The value of a digest: the first 16 hex characters, read big-endian, with the 0 and kCargoPinNone rule above.
std::uint64_t CargoPinFromHex(const std::string& sha256_hex);

// Whether a session at `level` is one the pin compares: the four combat maps (combat_level_class.h) and the two payload levels.
bool IsCargoPinLevel(std::uint64_t level);

// The boot status of the pin labels, first match wins over the labels in order (each label in this order):
//   kRefused         the boot gate turned the label off for a reason; `token` is the gate's reason (script_undelivered, ...)
//   kOff             the policy has the label off
//   kRequiredOnlyOff the policy's required_only is set and the label is not required
//   kWhole           the label has mounted every name and none was left out (missing 0, refused 0, names above 0)
//   kNotWhole        anything else
// The gate comes first: ApplyGate turns a gated label off, so the policy alone cannot tell the two apart (PLAN-v7 G3).
enum class CargoBoot { kWhole, kNotWhole, kOff, kRequiredOnlyOff, kRefused };
struct CargoBootStatus {
  CargoBoot state = CargoBoot::kNotWhole;
  std::string token;  // kRefused: the gate's reason; empty otherwise
};
CargoBootStatus BootStatus(const nevr_cdn::Mounted& mounted, const std::vector<nevr_cdn::GatedLabel>& gated,
                           const nevr_cdn::Policy& policy);

// The pin of the set this run mounted (B): kCargoPinNone unless BootStatus is kWhole, else CargoPinOf over the boot manifest.
std::uint64_t CargoBootPin(const nevr_cdn::Manifest& boot, const nevr_cdn::Platform& platform, const nevr_cdn::Mounted& mounted,
                           const std::vector<nevr_cdn::GatedLabel>& gated, const nevr_cdn::Policy& policy,
                           const Sha256HexFn& sha256_hex);

// The fetch state the decision reads (PLAN-v7 5.5 G6): `none` for a settled or an unrecorded fetch, `undeliverable` when the CDN's copy
// of the set cannot be had, `no_space` when the device has no room for it.
enum class CargoFetch { kNone, kUndeliverable, kNoSpace };

// What a client knows when it decides (PLAN-v7 5.5 "The decision and the notice").
struct CargoView {
  std::uint64_t local = kCargoPinNone;        // B, the pin of the set this run mounted
  CargoBootStatus status;                     // BootStatus of the pin labels
  CargoFetch fetch = CargoFetch::kNone;
  bool manifest_known = false;                // M is known: a fetch of this run has parsed a manifest
  std::uint64_t manifest = kCargoPinNone;     // M, the pin of the newest manifest this run parsed (only when manifest_known)
};

// The outcome of one decision. `leave` takes the player out of the session with `line` as the notice; `reason` names the row
// (`stay` rows name a reason only where the row has one). `write_marker` is row 14: the leave owner writes the refetch marker.
struct CargoDecision {
  bool leave = false;
  bool write_marker = false;
  std::string reason;
  std::string line;  // the notice line, at most nevr_map_session_pin::kNoticeLineMax bytes; empty for a stay
};

// The decision (PLAN-v7 5.5 table, first match wins). `level` is the session's level; `server_pin` the server's pin from its owner JSON
// (kCargoPinAbsent: the key is absent, an older server).
CargoDecision DecideCargo(const CargoView& view, std::uint64_t level, std::uint64_t server_pin);

// The decision as the session pin's tick acts on it (map_session_pin.h CargoOutcome, PLAN-v7 5.5 "One leave owner"): the leave, the
// row's reason and notice, the title, the label, and the inputs the decision named (B, and M when known). The marker of row 14
// (`CargoDecision::write_marker`) is not carried: the PC binding writes it, and it is wired with the feed (PLAN-v7 R6b-2).
nevr_map_session_pin::CargoOutcome OutcomeOf(const CargoView& view, std::uint64_t level, std::uint64_t server_pin);

}  // namespace nevr_cargo_pin
