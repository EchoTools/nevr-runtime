// Host-test stand-in for the configure-time build info: no release version.
#pragma once

namespace nevr_build {

inline constexpr const char* kVersion = "host-test";
inline constexpr const char* kCommit = "host-test";

}  // namespace nevr_build
