// The social feature switch.
//
// docs/adr/0003 ("Integration contract") specifies a fourth Feature, `social`, owned by the config
// package (quest_config.*). That package does not carry it yet, so the integration reads
// `features.social` itself, from the same file text the config package already read, and applies the
// documented prerequisite (social needs login). When the config package gains the feature, this
// file is deleted and the call site reads FeatureEnabled(kSocial).
#pragma once

#include <string>

#include "quest/sentinel/quest_config.h"

namespace nevr_quest::integration {

// True only when `fileText` is a JSON object whose `features.social` is the boolean true. Absent
// text, malformed JSON, a non-object, a non-boolean value or an oversized text are all "not
// requested" (the config package rejects such a file whole; so does this). Never throws.
bool SocialRequested(const std::string* fileText) noexcept;

// Social is effective only when requested and the login feature is effective (and so bridge and
// redirect are). `reason` receives the token of the first failed condition, or "ok".
bool SocialEffective(bool requested, const nevr_quest::Features& effective, const char** reason) noexcept;

}  // namespace nevr_quest::integration
