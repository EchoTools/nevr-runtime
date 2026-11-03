#include "quest/integration/headset_identity_step.h"

#include <cstddef>
#include <cstdio>

#if defined(__ANDROID__)
#include <sys/system_properties.h>
#endif

#include "quest/headset_profile.h"
#include "quest/sentinel/hook_log.h"
#include "quest/sentinel/texture_quality_hook.h"

namespace nevr_quest::integration {

namespace {

// PROP_VALUE_MAX on Android: the longest value a system property holds.
constexpr std::size_t kPropertyBytes = 92;
// "eye_high=WxH system_heap_mib=N level_hz=N" is well under this.
constexpr std::size_t kConsequenceBytes = 96;

// The property service on Android. A host build has none, so every property is absent there.
int ReadSystemProperty(const char* name, char* value) {
#if defined(__ANDROID__)
  return __system_property_get(name, value);
#else
  (void)name;
  value[0] = '\0';
  return 0;
#endif
}

// What the profile gives, for the headset_profile line: its high eye pair, its System heap (0 keeps the row's target) and its
// level rate. The full eye table is in headset_profile.h.
void FormatConsequence(const headset_profile::Profile& profile, char (&out)[kConsequenceBytes]) {
  const headset_profile::EyeSize& high = profile.eye[static_cast<std::size_t>(texture_quality::Level::kHigh)];
  if (profile.systemHeapMiB > 0) {
    std::snprintf(out, sizeof out, "eye_high=%dx%d system_heap_mib=%d level_hz=%.0f", high.width, high.height,
                  profile.systemHeapMiB, static_cast<double>(profile.levelHz));
  } else {
    std::snprintf(out, sizeof out, "eye_high=%dx%d system_heap_mib=row_target level_hz=%.0f", high.width, high.height,
                  static_cast<double>(profile.levelHz));
  }
}

}  // namespace

bool IdentifyHeadsetWith(PropertyReader read) {
  char manufacturer[kPropertyBytes] = {};
  char model[kPropertyBytes] = {};
  char device[kPropertyBytes] = {};
  read("ro.product.manufacturer", manufacturer);
  read("ro.product.model", model);
  read("ro.product.device", device);

  headset_profile::Inputs inputs;
  inputs.manufacturer = manufacturer;
  inputs.model = model;
  inputs.device = device;
  inputs.systemName = "";  // OpenXR system name: not read yet (ADR 0031, R1)

  const headset_profile::Match match = headset_profile::Identify(inputs);
  texture_quality::SetHeadsetProfile(match.profile);

  char consequence[kConsequenceBytes] = {};
  FormatConsequence(*match.profile, consequence);

  sentinel::LogFields(sentinel::LogLevel::kInfo, "headset_profile",
                      {{"manufacturer", manufacturer},
                       {"model", model},
                       {"device", device},
                       {"system_name", "unread"},
                       {"rule", match.rule},
                       {"profile", match.profile->name},
                       {"consequence", consequence}});
  return true;
}

bool IdentifyHeadset() { return IdentifyHeadsetWith(&ReadSystemProperty); }

}  // namespace nevr_quest::integration
