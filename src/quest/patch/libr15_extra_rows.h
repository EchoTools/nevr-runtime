// The extra-rows file (ADR 0021 section 11) and the spec a patch is checked against. Split from the patcher so the sentinel of a
// variant build can parse the same file with the same code: this translation unit needs nlohmann::json and nothing else of the
// patcher (no <elf.h>, no stub codec, no relocator). The public sentinel build does not link it.
#pragma once

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
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
  std::vector<std::string> alsoSha256;            // other digests the input may have (only read when stockSha256 is set)
  std::optional<std::uint64_t> expectedImageEnd;  // the pinned end of the writable PT_LOAD; empty: not checked
  std::uint64_t slotAreaGap = 0;                  // bytes left free between the image end and the slot area (PublicSpec: kSlotAreaGap)
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

}  // namespace nevr_quest::patch
