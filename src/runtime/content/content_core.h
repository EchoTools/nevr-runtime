// content_core.h: the part of serving a content pack that both builds compile, PC (#1424) and Quest (#2312)
// (docs/reference/content-packs.md).
//
// The decision over an installed pack, the per-file checks, the table of the game paths the pack serves and the text of every
// [NEVR.CONTENT] line live here once. What differs between the builds is the file access (Win32 on PC, POSIX on Quest), behind
// PackFiles, the facts about the run each platform reads for itself (CoreInputs), and the hook that hands the game a served file
// (content_pack_install.cpp on PC, quest/sentinel/content_pack_hook.cpp on Quest). No platform API, no logging call: a line is
// built here and written by the platform's own log.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace nevr_content {

enum class Outcome {
  kNone,     // no pack is installed
  kServed,   // the pack is checked and its files are served
  kRefused,  // a pack is installed and is not served; the reason says why
};

const char* OutcomeName(Outcome outcome);

struct Decision {
  Outcome outcome = Outcome::kNone;
  std::string id;
  std::string version;
  std::string reason;  // a token when refused
  std::string detail;  // what the token is about
  std::size_t manifests = 0;
  std::size_t packages = 0;
  std::size_t scripts = 0;
  std::size_t files_checked = 0;
};

// What one pack file is served in place of.
enum class Kind { kManifest, kPackage, kScript };

const char* KindName(Kind kind);

// The level a line is written at; each platform maps it onto its own log.
enum class LineLevel { kDebug, kInfo, kWarning };

// One pack file, opened for the check and held open from then on (until PackFiles::ReleaseAll).
struct HeldFile {
  bool opened = false;
  unsigned long error = 0;  // the platform's error code when the open failed
  long long size = -1;      // the file's size in bytes, -1 when it does not read
  std::string sha256;       // 64 lowercase hex, streamed through the held handle from its start; "" when it does not read
  std::string serve_path;   // what the game's open function is handed in place of its own path for this file
};

// The file access the decision needs, the one part that is written per platform.
class PackFiles {
 public:
  virtual ~PackFiles() = default;
  // The folders directly under `dir`, by name, sorted ("." and ".." left out); none when `dir` does not list.
  virtual std::vector<std::string> Folders(const std::string& dir) = 0;
  // The whole of `path` when it reads and holds at most `max_bytes` bytes.
  virtual bool ReadText(const std::string& path, std::size_t max_bytes, std::string* text) = 0;
  // Opens a pack file for reading and, when it opened, holds it until ReleaseAll, so it cannot be changed under the game
  // (PC: read sharing only) or so the game is handed the very file that was checked (Quest: serve_path names the held
  // descriptor).
  virtual HeldFile Hold(const std::string& path) = 0;
  virtual void ReleaseAll() = 0;
  // The sha-256 of a stock file (opened, hashed and closed), "" when it does not read.
  virtual std::string Sha256Of(const std::string& path) = 0;
  virtual bool Exists(const std::string& path) = 0;
  // `relative` ('/'-separated, content_json.h) below `dir`, in the platform's spelling.
  virtual std::string Join(const std::string& dir, const std::string& relative) = 0;
  // The spelling a path is looked up by in the table: the stock paths are stored in it, and the game's path is turned into it
  // before the lookup.
  virtual std::string Key(const std::string& path) = 0;
};

// What the decision is given: where the packs and the game live, and the facts about this run it cannot read itself.
struct CoreInputs {
  std::string content_dir;  // <nevr root>/content
  std::string game_dir;     // the folder content.json's data_dir is relative to
  std::string exe_dir;      // PC: the folder of echovr.exe, which holds scripts/
  bool data_read_already = false;  // the game has created its resource manager (and so read its manifests)
  bool signed_only = false;        // PC: the run is -require-signed-plugins (a pack with scripts is refused, signed_only)
  bool game_supported = true;      // PC: echovr.exe is the 631547 build (else a pack with scripts is unsupported_game)
  bool scripts_supported = true;   // Quest: false, a pack with scripts is refused whole (scripts_unsupported)
  bool refuse_placed_bundle = false;  // Quest: a `<manifest>.stock` beside a manifest the pack replaces is bundled_pack_placed
};

// One stock file the pack serves, and how often it was served.
struct Served {
  Kind kind = Kind::kManifest;
  std::string name;        // the stock file's name (the package set, the package, the script)
  std::string key;         // the stock path, in PackFiles::Key's spelling
  std::string serve_path;  // handed to the game's function (HeldFile::serve_path)
  std::string pack_file;   // the pack's file, as the line names it
  mutable std::atomic<unsigned> hits{0};
};

struct Pack {
  std::string id;
  std::vector<std::unique_ptr<Served>> opens;    // the manifests and packages, keyed by stock path
  std::vector<std::unique_ptr<Served>> scripts;  // PC: <exe dir>/scripts/<name>.dll
};

// Checks the installed pack. On kServed `*pack` is its table and `files` holds every pack file that passed; on any other
// outcome nothing is held (ReleaseAll was called) and `*pack` is left as it was. In order, the pack is refused when: more than
// one pack is installed (several_packs); the game has read its data already (loaded_too_late); content.json is missing
// (no_content_json) or breaks a rule (content_json.h); it carries scripts and the run is signed-only (signed_only), the game is
// another build (unsupported_game) or the platform loads no pack script (scripts_unsupported); a pack file does not open
// (file_missing), is empty (file_empty) or is not the sha-256 content.json lists (file_changed); a manifest it replaces carries
// a bundled pack's `.stock` marker (bundled_pack_placed); a stock manifest is not the one it was built from (stock_mismatch).
Decision Decide(const CoreInputs& in, PackFiles& files, std::unique_ptr<Pack>* pack);

// The game's open flags that are a plain read: bit 0, and none of the write, create, truncate or append bits (0x2, 0x4, 0x8,
// 0x10). The same on both builds (echovr.exe CFile::Open, libr15 CSysFile::Open).
bool IsServedRead(std::uint32_t flags) noexcept;

// The entry of `table` whose key is `key`, or nullptr. No allocation, so a hook may call it before the game's function.
const Served* FindKey(const std::vector<std::unique_ptr<Served>>& table, const char* key) noexcept;

// Text from content.json or a path, for a quoted log field: at most 300 bytes of printable ASCII; a quote or any other byte
// is '?'.
std::string Printable(const std::string& text);

// The decision's one line, for the content folder `dir`.
std::string FormatDecision(const Decision& d, const std::string& dir, LineLevel* level);

// One served file's line: counts the serve, and writes `<kind> served ...` (Info the first time the file is served, Debug
// after) or, when the game's function returned `result` != 0, `<kind> open failed ... result=` (Warning). A script, whose load
// comes after the rewrite, passes 0. Written into buf[cap], NUL-terminated and cut at cap; no allocation.
LineLevel FormatServed(const Served& served, const char* id, const char* game_path, std::uint64_t result, char* buf,
                       std::size_t cap) noexcept;

}  // namespace nevr_content
