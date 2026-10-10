#pragma once
// procfs/sysfs/statvfs readers for the hardware dump (#335). Every path is read under `root` ("" on the
// device; a temporary directory in the host tests), and every result is a field (hwdump_field.h).

#include <nlohmann/json.hpp>

#include <cstddef>
#include <string>
#include <vector>

namespace nevr_quest::hwdump {

// The file's text, at most `max_bytes`; a longer file is cut and says so ("truncated":true beside the
// value). Trailing newlines are kept: raw files (/proc/cpuinfo) are recorded as they are.
nlohmann::json ReadTextFile(const std::string& root, const std::string& path, std::size_t max_bytes = 256 * 1024);

// A one-line sysfs attribute, with the trailing newline removed.
nlohmann::json ReadAttribute(const std::string& root, const std::string& path);

// The names in a directory (not "." / ".."), sorted, that start with `prefix`; a field, so an unreadable
// directory is an error rather than an empty list.
nlohmann::json ListDirectory(const std::string& root, const std::string& path, const std::string& prefix = "");

// Names from a ListDirectory field; empty when the field failed.
std::vector<std::string> ListedNames(const nlohmann::json& listing);

// statvfs: block size, total/free/available bytes, total/free inodes.
nlohmann::json StatVfs(const std::string& root, const std::string& path);

}  // namespace nevr_quest::hwdump
