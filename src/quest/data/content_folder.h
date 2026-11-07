/* A content pack installed as a folder, served on Quest (#2312, docs/reference/content-packs.md): the twin of the PC's
 * runtime/content/content_pack.h, over the same decision (runtime/content/content_core.h).
 *
 * A pack is `<media root>/nevr/content/<id>/` (beside the sentinel's nevr/logs, outside the game's _data tree) with the same
 * content.json format 1 as on PC; its data_dir is relative to the root the engine reads (`_data/5932408047/rad15/android`).
 * The game opens every manifest and every package through CSysFile::Open (libr15 0xf85538); the sentinel's hook on its slot
 * (quest/sentinel/content_pack_hook.h) asks ServedFor and hands the game the pack's file instead.
 *
 * What differs from PC, each for a reason of the platform:
 *   - The held file is handed to the game as /proc/self/fd/<n>, a fresh open of the very file that was hashed. POSIX has no
 *     share-deny mode, so holding a descriptor does not stop a rename over the pack's path; and the game reads with read() on
 *     the descriptor it gets, so a dup of the held one would share the offset the hash pass moved.
 *   - A pack that carries scripts is refused whole (scripts_unsupported): the game loads its script libraries by bare name from
 *     the APK's native library directory, and the pack folder is on shared storage, mounted noexec.
 *   - A manifest the pack replaces with a `<manifest>.stock` beside it is refused (bundled_pack_placed): an APK's bundled pack
 *     (docs/adr/0029-quest-bundled-content-pack.md) has replaced that stock manifest, so it is not the one a pack is built on.
 *   - The service-only libr15 spells the media root with the afk package (quest/sentinel/path_rewrite.h); a game path in that
 *     spelling is turned into the running package's before it is looked up, as the path rewrite turns it below the game's open.
 * Pure but for the file system: no hook, no log call (the caller's sink writes the lines).
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "runtime/content/content_core.h"

namespace nevr_quest::content {

// Below the media root, beside the sentinel's nevr/logs (sentinel_log.cpp): the Quest's nevr root, as on PC.
inline constexpr const char* kContentSubdir = "nevr/content";

// `media_root`/nevr/content.
std::string ContentDir(const std::string& media_root);

// The shared decision's file access on POSIX (content_core.h, PackFiles).
class PosixPackFiles final : public nevr_content::PackFiles {
 public:
  // `running_package`: a path in the afk spelling is keyed in this package's (path_rewrite.h); "" keys paths as they are.
  explicit PosixPackFiles(std::string running_package) : running_package_(std::move(running_package)) {}
  ~PosixPackFiles() override { ReleaseAll(); }

  std::vector<std::string> Folders(const std::string& dir) override;
  bool ReadText(const std::string& path, std::size_t max_bytes, std::string* text) override;
  nevr_content::HeldFile Hold(const std::string& path) override;
  void ReleaseAll() override;
  std::string Sha256Of(const std::string& path) override;
  bool Exists(const std::string& path) override;
  std::string Join(const std::string& dir, const std::string& relative) override;
  std::string Key(const std::string& path) override;

  std::size_t HeldCount() const { return held_.size(); }

 private:
  std::string running_package_;
  std::vector<int> held_;
};

// The streamed sha-256 of the whole of `fd` from offset 0 (pread, `chunk_bytes` at a time; the descriptor's own offset is not
// moved), as 64 lowercase hex; "" when it does not read.
inline constexpr std::size_t kDigestChunkBytes = 1u << 20;
std::string StreamSha256(int fd, std::size_t chunk_bytes);

using LineSink = void (*)(nevr_content::LineLevel level, const char* line);
// Puts the hook on the game's open; nullptr when it is in place, else why not (a token, the GOT status).
using HookInstaller = const char* (*)();

struct Config {
  std::string content_dir;      // ContentDir(<media root>)
  std::string engine_root;      // the root the engine reads (data_wiring.cpp, EngineRoot): content.json's game folder
  std::string running_package;  // PosixPackFiles
  bool data_read_already = false;  // the game's resource manager exists (pinned_targets.h, kResourceManagerPtrVaddr)
  HookInstaller install_hook = nullptr;
  LineSink log = nullptr;
};

// Decides over the installed pack (content_core.h, with the Quest refusals) and, for a pack that passed, makes it the one this
// process serves and puts the hook on; a hook that does not go in refuses the pack (hook_failed). Writes the decision's line to
// `config.log`. Once per process in production; a second call replaces what the first served.
nevr_content::Decision Run(const Config& config);

// For the hook, before the game's function runs: the entry the pack serves for an open of `game_path` with `flags`, or nullptr
// (no pack, not a read, a path it does not replace). No allocation and no destructor (callback_thunk.h, rule 5).
const nevr_content::Served* ServedFor(const char* game_path, unsigned flags) noexcept;

// After the game's function returned `result` for a served open: that file's line (content_core.h, FormatServed) in buf[cap].
nevr_content::LineLevel ServedLine(const nevr_content::Served& served, const char* game_path, int result, char* buf,
                                   std::size_t cap) noexcept;

// Forgets the served pack and closes the files it held (tests).
void Forget();

// Test support: serves `pack` (its keys as given, no file held) with `running_package` for the afk spelling.
void ServeForTest(std::unique_ptr<nevr_content::Pack> pack, const std::string& running_package);

}  // namespace nevr_quest::content
