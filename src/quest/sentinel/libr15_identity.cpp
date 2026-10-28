#include "libr15_identity.h"

#include <string>

#include "pinned_targets.h"
#include "quest/patch/libr15_patch_identity.h"

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

const std::vector<nevr_quest::patch::WordPatchRow>& LibR15ExtraRows() {
  // Built on first use: a namespace-scope vector would be a load-time constructor.
  static const std::vector<nevr_quest::patch::WordPatchRow> rows;
  return rows;
}

const char* LibR15PatchedBuildId() {
  static const std::string id = nevr_quest::patch::PatchedBuildId(LibR15ExtraRows());
  return id.c_str();
}

}  // namespace sentinel::pinned
