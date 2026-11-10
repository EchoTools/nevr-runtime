// content_json.h: a PC content pack's descriptor, `<root>\content\<id>\content.json` (#1424, docs/reference/content-packs.md).
//
// Format 1, one JSON object:
//   format     1
//   id         the pack's id (pack_rules.h), equal to its folder's name
//   version    text, shown in the log
//   data_dir   the game's data folder the pack overlays, relative to the game folder ("_data/<version>/rad15/win10")
//   manifests  a list; each entry replaces one stock manifest of package set `package` (16 lowercase hex):
//                stock_sha256    the sha-256 of the stock manifest it was built from
//                stock_packages  how many packages the stock set has (`<package>_0` .. `_<n-1>`)
//                file            the pack's manifest, a path in `files`
//                packages        the packages the pack adds, `packages/<package>_<n>` with n >= stock_packages, each in `files`
//   scripts    a list of `scripts/<16 lowercase hex>.dll`, each in `files`: the script DLLs the pack replaces or adds
//   files      an object: path -> sha-256 (64 hex), every file of the pack
// A path is relative to the pack folder, '/'-separated, each part a plain name (pack_rules.h). Nothing here touches a file:
// ParseContentJson checks the text and names what is wrong; the caller hashes the files (content_pack.h).
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace nevr_content {

inline constexpr std::size_t kMaxContentJsonBytes = 1u << 20;
inline constexpr std::size_t kMaxPackFiles = 4096;

struct ManifestEntry {
  std::string package;       // the package set, 16 lowercase hex
  std::string stock_sha256;  // of the stock manifest the pack was built from
  std::uint32_t stock_packages = 0;
  std::string file;                   // the pack's manifest, relative to the pack folder
  std::vector<std::string> packages;  // relative to the pack folder; file names `<package>_<n>`, n >= stock_packages
};

struct Descriptor {
  std::string id;
  std::string version;
  std::string data_dir;  // relative to the game folder, '/'-separated
  std::vector<ManifestEntry> manifests;
  std::vector<std::string> scripts;            // `scripts/<16 hex>.dll`
  std::map<std::string, std::string> files;    // path -> sha-256, lowercase
};

// A path inside the pack (or the data folder inside the game folder): '/'-separated plain names, at most 200 bytes, no empty,
// "." or ".." part, nothing absolute.
bool IsRelativePlainPath(const std::string& path);

// The file name of a '/'-separated path.
std::string LeafOf(const std::string& path);

// Parses and checks content.json. `folder_id` is the name of the pack's folder: the descriptor's id must equal it. False with a
// token in `reason`: too_large, not_json, bad_format, bad_id, id_mismatch, bad_version, no_files, too_many_files,
// bad_file_entry, bad_data_dir, no_manifests, bad_manifest, duplicate_manifest, bad_package, duplicate_package, bad_scripts,
// bad_script, duplicate_script, unlisted_file (a key names a path `files` does not list). `detail` names the entry.
bool ParseContentJson(const std::string& text, const std::string& folder_id, Descriptor* out, std::string* reason,
                      std::string* detail);

}  // namespace nevr_content
