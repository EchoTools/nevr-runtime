#pragma once

namespace nevr_quest::integration {

// The headset identity step (ADR 0031): reads the headset's system properties, names the headset with
// headset_profile::Identify, hands the profile to the texture-quality hook, and writes one Info `headset_profile` line that
// carries every input, the rule matched and the profile chosen. Every launch, once, before any consumer reads a setting.
// Returns true; the sequence (ctor_sequence.cpp) contains an exception as a step failure.
bool IdentifyHeadset();

}  // namespace nevr_quest::integration
