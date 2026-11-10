/* The gate on the folded combat modules (#2170, docs/adr/0046 rule 5): which APKs run them.
 *
 * What a module asks. A combat module folded from the q15t plugin installs only when `combat::Active()` is true, and a decision a
 * sentinel hook asks of the combat area (the refresh-rate rule, ADR 0046 rule 3) answers with the game's own value when it is false.
 * The release APK's base is the reference build, whose own combat modules live in its `libvrapi.so` and hook libr15 words
 * (docs/adr/0034-one-hook-manager-per-process.md); a folded module installed there would be a second owner of the same behaviour. The
 * combat code is compiled into every sentinel, so one loader asset per tag serves every variant (ADR 0040 rule 1), and this gate is
 * what keeps it from running in the APKs that do not carry combat.
 *
 * The rule. Active is true for the `q15t` and `combat` variants (build_variant.h RunsFoldedCombat), the APKs that carry the q15t
 * libraries, and for no other. A development build (`nevr_build::kIsDev`) may also force it with `combat.force: true` in config.yaml
 * (quest_config.h CombatForceChoice); a release build may not, and says so in a Warning rather than ignoring it silently. The variant
 * is read from the APK's stamp before the first log line (SetVariant); the answer is made once the configuration is resolved
 * (Resolve, from the constructor sequence's config step), because `combat.force` lives there. Until then nothing is active.
 *
 * Not gated by this. Combat-area code that ships to every APK on purpose is outside the rule and says so in its header:
 * match_tags.h (name tags on by default, #2090) and equip_refire.h (the equipment change event sent again, #2121) install on every launch.
 *
 * Lines (sentinel log), one per process:
 *   {"event":"combat_active","variant":"none|q15t|combat|perf-probe|arena","kind":"dev|release","active":1|0,
 *    "reason":"variant|forced|not_a_combat_variant","force":"not_asked|honoured|redundant|refused","msg":"<what follows>"}
 * Warning when `force` is `refused` (a release build was asked to force combat on), Info otherwise.
 *
 * Built with -fno-exceptions, like every combat translation unit.
 */
#pragma once

#include "quest/sentinel/build_variant.h"

namespace nevr_quest::combat {

// Why a decision is what it is.
enum class Reason {
  kUnresolved,            // the configuration has not been resolved yet: nothing is active
  kVariantCarriesCombat,  // the APK's variant is `q15t` or `combat`
  kForced,                // a development build asked for it with combat.force, and the variant does not carry it
  kNotACombatVariant,     // a plain release, the measurement build, or an unreadable stamp: the game keeps its own behaviour
};

// What became of a combat.force request.
enum class ForceOutcome {
  kNotAsked,   // the configuration did not ask
  kHonoured,   // a development build forced combat on for a variant that does not carry it
  kRedundant,  // asked, and the variant carries combat anyway
  kRefused,    // asked in a release build: not honoured
};

struct Decision {
  bool active = false;
  Reason reason = Reason::kUnresolved;
  ForceOutcome force = ForceOutcome::kNotAsked;
};

const char* ReasonName(Reason reason) noexcept;  // "variant", "forced", "not_a_combat_variant", "unresolved"
const char* ForceName(ForceOutcome outcome) noexcept;  // "not_asked", "honoured", "redundant", "refused"

// The rule, pure: the APK's variant, whether this is a development build, whether the configuration asked to force combat on.
Decision Decide(build_variant::Variant variant, bool devBuild, bool forceAsked) noexcept;

// Records the variant read from the stamp. Called once, before the first log line, from the sentinel's constructor. A call after the
// answer was made changes nothing.
void SetVariant(build_variant::Variant variant) noexcept;

// Makes the answer from the variant SetVariant recorded (a plain release when none was) and logs the combat_active line. Called once the
// configuration is resolved, with `nevr_build::kIsDev` and the configuration's combat.force. A second call changes nothing and logs nothing.
void Resolve(bool devBuild, bool forceAsked) noexcept;

// The variant SetVariant recorded (kNone until it was called): the word the combat_install lines name beside the answer.
build_variant::Variant RecordedVariant() noexcept;

// True when the folded combat modules run in this process. False until Resolve has been called.
bool Active() noexcept;
// True when Resolve was told this is a development build (`nevr_build::kIsDev`). False until Resolve has been called. A module that offers a
// development-only aid (aibots' request file) asks here, so no module reads the build kind for itself.
bool DevBuild() noexcept;
// Why Active() is what it is; kUnresolved until Resolve has been called.
Reason ActiveReason() noexcept;

// ---- test support ------------------------------------------------------------------------------------------------------

// Forgets the variant and the answer, so the next Resolve makes and logs a new one.
void ResetForTest() noexcept;

}  // namespace nevr_quest::combat
