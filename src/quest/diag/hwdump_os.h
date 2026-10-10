#pragma once
// The "os" section of the hardware dump (#335): what the kernel and libc tell an app, read with native calls
// only (no JNI). Paths are read under `root` ("" on the device). The Android-only parts (system properties)
// are fields with an error elsewhere.

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace nevr_quest::hwdump {

struct OsInputs {
  std::string root;                       // "" on the device
  std::vector<std::string> statvfs_paths; // device paths to report free space for
};

nlohmann::json ComposeOs(const OsInputs& in);

// The properties libr15 and the dump read by name (`os.properties_wanted`).
const std::vector<std::string>& WantedProperties();

// The paths ComposeOs always writes, whatever the device has: the "never omitted" contract, pinned by the
// host test.
const std::vector<std::string>& FixedOsFieldPaths();

}  // namespace nevr_quest::hwdump
