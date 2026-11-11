#pragma once

namespace nevr_quest::integration {

// Reads one system property into `value` (PROP_VALUE_MAX bytes, NUL-terminated) and returns its length, 0 when the property is
// absent: the shape of __system_property_get. The step takes it as a parameter so a host test can supply recorded property sets.
using PropertyReader = int (*)(const char* name, char* value);

// The headset identity step (ADR 0031): reads the headset's system properties, names the headset with headset_profile::Identify,
// hands the profile to the texture-quality hook, and writes one Info `headset_profile` line. The line carries every input, the
// rule matched, the profile chosen and the consequence: the high eye pair, the System heap and the level rate the profile gives.
// Every launch, once, before any consumer reads a setting. Returns true; the sequence (ctor_sequence.cpp) contains an exception
// as a step failure.
bool IdentifyHeadset();

// The same step over the properties `read` gives. IdentifyHeadset() is this with the device's property service.
bool IdentifyHeadsetWith(PropertyReader read);

}  // namespace nevr_quest::integration
