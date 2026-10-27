#pragma once

// The levels this install can load, read from the package manifests the engine adds at boot (#990).
//
// A manifest (docs/reference/package-manifest-format.md) lists every resource of a package set as a
// (type, name) record. A level's root resource has the on-disk type CGameLevelResourceWin10 and its name
// is the level's symbol; that symbol is what GameServerRegistration reports as installed_levels.
//
// Pure: std and zstd only, no windows.h and no Log(), so the unit test links it on its own. The
// registration caller (registration_metadata.cpp) owns the install path, the logging and the fallback.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace nevr_game_server {

// CSymbol64("CGameLevelResource" + "Win10"): the on-disk type of a level's root resource
// (docs/engine/asset-system.md, "Names"; prediction2.md in the registration scratch).
inline constexpr uint64_t kLevelRootType = 0xe8e38d7781a338a6ULL;

// The package sets the boot adds (InitGlobals AddPackage calls): CSymbol64("r14netclient") and
// CSymbol64("r14netclient_en_US"). Computed by the caller from the names, not hardcoded here.
std::vector<uint64_t> BootPackageSets();

// The install root for an echovr.exe at exePath (its directory is bin/win10): the engine's project root.
std::filesystem::path InstallRootFromExe(const std::filesystem::path& exePath);

// Every <root>/_data/<version>/rad15/win10/manifests folder that exists, sorted. Empty when there is no data dir.
std::vector<std::filesystem::path> FindManifestDirs(const std::filesystem::path& exePath);

// Level-root symbols in one manifest file image, in record order. Returns false with a reason in *error
// when the bytes are not a readable manifest (wrong magic or version, bad lengths, a zstd frame that
// disagrees with the container). Nothing is allocated before the frame's declared size has been checked.
bool ParseLevelRoots(const std::vector<uint8_t>& file, std::vector<uint64_t>* levels, std::string* error);

struct InstalledLevels {
  std::vector<uint64_t> levels;      // sorted, no duplicates, across every set read
  std::size_t manifestsRead = 0;     // sets whose file parsed
  std::size_t pluginLevels = 0;      // level-root registrations a plugin made (AddRegisteredLevels)
  std::vector<std::string> problems;  // one line per set that was missing, too large, or did not parse
};

// Reads <manifestDir>/<%016llx set id> for each set and collects the level roots. A missing, oversized or
// unreadable file is a problem line, never an exception: the other sets still count.
InstalledLevels CollectInstalledLevels(const std::filesystem::path& manifestDir, const std::vector<uint64_t>& setIds);

// CollectInstalledLevels over several manifest folders, merged (sorted, no duplicates).
InstalledLevels CollectInstalledLevelsFromDirs(const std::vector<std::filesystem::path>& manifestDirs,
                                               const std::vector<uint64_t>& setIds);

// Adds the names of the level-root registrations (type, name) a plugin made through NEVR_RegisterAsset to the
// manifest levels. Other types are ignored. The result stays sorted, without duplicates.
void AddRegisteredLevels(const std::vector<std::pair<uint64_t, uint64_t>>& registered, InstalledLevels* levels);

// The ServerDB list parameters (guilds, regions) exactly as nakama's parseUserQueryCommaDelimited reads them
// (server/evr_session.go): split on ",", empty items skipped, each item cut to maxLength bytes, items the
// pattern rejects dropped, then sorted. Nothing is trimmed: a space is not a valid item, and nakama drops it.
enum class ListItemKind { kRegion, kGuild };
inline constexpr std::size_t kServerDbListMaxItemBytes = 32;
std::vector<std::string> ParseServerDbList(std::string_view csv, ListItemKind kind, std::size_t* rejected);

// "a,b,,c" -> {"a", "b", "c"}: the split alone, empty items dropped.
std::vector<std::string> SplitCsvList(std::string_view csv);

}  // namespace nevr_game_server
