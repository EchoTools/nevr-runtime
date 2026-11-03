#include "quest/integration/headset_identity_step.h"

#include <cstddef>

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

// Reads one system property into `out`, empty when the property is absent. A host build has no property service, so every
// property is empty there (the tests supply the inputs themselves).
void ReadProperty(const char* name, char (&out)[kPropertyBytes]) {
  out[0] = '\0';
#if defined(__ANDROID__)
  __system_property_get(name, out);
#else
  (void)name;
#endif
}

}  // namespace

bool IdentifyHeadset() {
  char manufacturer[kPropertyBytes];
  char model[kPropertyBytes];
  char device[kPropertyBytes];
  ReadProperty("ro.product.manufacturer", manufacturer);
  ReadProperty("ro.product.model", model);
  ReadProperty("ro.product.device", device);

  headset_profile::Inputs inputs;
  inputs.manufacturer = manufacturer;
  inputs.model = model;
  inputs.device = device;
  inputs.systemName = "";  // OpenXR system name: not read yet (ADR 0031, R1)

  const headset_profile::Match match = headset_profile::Identify(inputs);
  texture_quality::SetHeadsetProfile(match.profile);

  sentinel::LogFields(sentinel::LogLevel::kInfo, "headset_profile",
                      {{"manufacturer", manufacturer},
                       {"model", model},
                       {"device", device},
                       {"system_name", "unread"},
                       {"rule", match.rule},
                       {"profile", match.profile->name}});
  return true;
}

}  // namespace nevr_quest::integration
