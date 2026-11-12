// Skeleton for the red run: the declarations the tests use, with empty answers. Replaced by the implementation in the next commit.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace nevr_quest::data::boot_stub {

inline constexpr std::uint64_t kSetIds[] = {0, 0};
inline constexpr std::size_t kSetCount = sizeof(kSetIds) / sizeof(kSetIds[0]);
inline constexpr const char* kVersionFolder = "";
inline constexpr const char* kPlatformPath = "";

std::string SetFileName(std::uint64_t set_id);
std::string ManifestsDir(const std::string& root);
std::string ManifestPath(const std::string& root, std::uint64_t set_id);
std::string StubFileBytes();

}  // namespace nevr_quest::data::boot_stub
