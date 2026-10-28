// The libr15 build-time patcher (ADR 0021 sections 2 to 4, 7, 8 and 11): reads the stock libr15.so, refuses on any
// doubt, and returns the patched bytes and a JSON account of every byte that changed. A host tool (it uses
// nlohmann::json and <elf.h>); the sentinel does not include this header.
//
// Pure: the result is a function of the stock bytes, the spec and the extra rows. No clock, no environment, no
// file access (RunCli, in quest_patch_libr15's own translation unit path, is the only function that touches files).
#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <iosfwd>
#include <optional>
#include <string>
#include <vector>

#include "quest/patch/libr15_patch_identity.h"
#include "quest/patch/libr15_patch_table.h"

namespace nevr_quest::patch {

/// What a patch is checked against. PublicSpec() is the shipped one; the host tests build their own around a
/// synthetic ELF (a table at their own addresses, a pin of their own image end).
struct PatchSpec {
  const Row* rows = nullptr;
  std::size_t rowCount = 0;
  BuildIdBytes stockBuildId{};
  std::string stockSha256;                        // empty: the file's digest is not checked
  std::optional<std::uint64_t> expectedImageEnd;  // the pinned end of the writable PT_LOAD; empty: not checked
};

/// The compiled-in public table with the pinned stock build ID, SHA-256 and image end.
PatchSpec PublicSpec();

/// One refusal: a stable `code`, the row `id` it concerns (empty for the file as a whole) and a human `detail`.
struct Refusal {
  std::string code;
  std::string id;
  std::string detail;
  nlohmann::json ToJson() const;
};

/// Reads an extra-rows file (format 1, ADR section 11). False, with `refusal` set, on any rule broken. `rows` is
/// then unspecified. The pinned stock ID and the public row ids come from `spec`.
bool ParseExtraRows(const std::string& fileBytes, const PatchSpec& spec, std::vector<WordPatchRow>* rows,
                    Refusal* refusal);

struct PatchResult {
  bool ok = false;
  Refusal refusal;                    // set when !ok
  std::vector<std::uint8_t> output;   // the patched file, when ok
  std::vector<nlohmann::json> events; // libr15_patch_site (one per site), libr15_patch_word (one per word)
  nlohmann::json summary;             // the final `libr15_patch` record: status "patched" or "refused"
};

/// Patches `stock`. `extra` is the parsed extra-rows file (empty for the public release) and `extraRowsSha256` the
/// digest of the file's bytes as given (empty when there is none).
PatchResult PatchLibr15(const std::vector<std::uint8_t>& stock, const PatchSpec& spec,
                        const std::vector<WordPatchRow>& extra, const std::string& extraRowsSha256);

/// Writes `bytes` to `path` through a temporary file beside it (`path` + ".partial") and checks that the file on disk
/// reads back as `expectedSha256` (lower-case hex). The temporary file is created with O_CREAT | O_EXCL | O_NOFOLLOW: a
/// name that already exists, a link included, is refused and left alone, so the write can never go through a link. The
/// bytes are read back through the descriptor that wrote them, before the rename, so a file that does not read back as
/// the validated bytes never replaces `path`. False, with `why` set, on any failure; `path` then holds what it held before.
bool WriteOutputFile(const std::string& path, const std::vector<std::uint8_t>& bytes, const std::string& expectedSha256,
                     std::string* why);

/// The tool: `--stock <file> --out <file> --report <file> [--extra-rows <file>]`. Prints one JSON line per event and
/// the summary to `out`, a refusal or error as one JSON line to `err`. Exit 0 on success, 1 on a refusal (nothing is
/// written to --out), 2 on a usage or I/O error. The report's summary says what is on disk: "patched" only once --out is
/// in place and has been read back, "refused" for a refusal, "io_error" when --out could not be written, and
/// "incomplete" while --out is being written (what a run that dies there leaves).
int RunCli(const std::vector<std::string>& args, const PatchSpec& spec, std::ostream& out, std::ostream& err);

}  // namespace nevr_quest::patch
