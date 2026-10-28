#include "libr15_identity.h"

#include <cstddef>
#include <string>

#include "pinned_targets.h"
#include "quest/patch/libr15_patch_identity.h"
#ifdef NEVR_LIBR15_EXTRA_ROWS
#include "hook_log.h"
#include "quest/patch/libr15_extra_rows.h"
#endif

#ifdef NEVR_LIBR15_EXTRA_ROWS
// Generated at configure time from the file named by NEVR_LIBR15_EXTRA_ROWS (src/quest/cmake/libr15_extra_rows.cmake).
namespace nevr_libr15_extra_rows_data {
extern const unsigned char kBytes[];
extern const std::size_t kSize;
extern const char kSha256[];
}  // namespace nevr_libr15_extra_rows_data
#endif

namespace sentinel::pinned {
namespace {

constexpr bool SameText(const char* a, const char* b) {
  while (*a != '\0' && *a == *b) {
    ++a;
    ++b;
  }
  return *a == *b;
}

// The sentinel's stock ID and the patch table's are one value.
static_assert(SameText(kLibR15StockBuildId, nevr_quest::patch::kLibR15StockBuildId),
              "pinned_targets.h kLibR15StockBuildId must equal quest/patch/libr15_patch_table.h kLibR15StockBuildId");

}  // namespace

#ifdef NEVR_LIBR15_EXTRA_ROWS
namespace {

std::vector<nevr_quest::patch::WordPatchRow> ParseEmbeddedRows() {
  std::vector<nevr_quest::patch::WordPatchRow> rows;
  nevr_quest::patch::Refusal refusal;
  const std::string text(reinterpret_cast<const char*>(nevr_libr15_extra_rows_data::kBytes), nevr_libr15_extra_rows_data::kSize);
  if (!nevr_quest::patch::ParseExtraRows(text, nevr_quest::patch::PublicSpec(), &rows, &refusal)) {
    // Configure parsed this same text with this same function, so this is a corrupted build. Never fall back to the public
    // rows: this build's libr15 is not the public one, and a silent public identity would hide that.
    sentinel::LogFields(sentinel::LogLevel::kError, "libr15_extra_rows",
                        {{"status", "unparsable"}, {"reason", refusal.code.c_str()}, {"sha256", nevr_libr15_extra_rows_data::kSha256}});
    __builtin_trap();
  }
  return rows;
}

}  // namespace
#endif

const std::vector<nevr_quest::patch::WordPatchRow>& LibR15ExtraRows() {
  // Built on first use: a namespace-scope vector would be a load-time constructor.
#ifdef NEVR_LIBR15_EXTRA_ROWS
  static const std::vector<nevr_quest::patch::WordPatchRow> rows = ParseEmbeddedRows();
#else
  static const std::vector<nevr_quest::patch::WordPatchRow> rows;
#endif
  return rows;
}

const char* LibR15ExtraRowsSha256() {
#ifdef NEVR_LIBR15_EXTRA_ROWS
  return nevr_libr15_extra_rows_data::kSha256;
#else
  return "";
#endif
}

const char* LibR15PatchedBuildId() {
  static const std::string id = nevr_quest::patch::PatchedBuildId(LibR15ExtraRows());
  return id.c_str();
}

}  // namespace sentinel::pinned
