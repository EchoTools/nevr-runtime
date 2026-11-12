#include "quest/data/boot_stub.h"

namespace nevr_quest::data::boot_stub {

std::string SetFileName(std::uint64_t) { return std::string(); }
std::string ManifestsDir(const std::string& root) { return root; }
std::string ManifestPath(const std::string& root, std::uint64_t) { return root; }
std::string StubFileBytes() { return std::string(); }

}  // namespace nevr_quest::data::boot_stub
