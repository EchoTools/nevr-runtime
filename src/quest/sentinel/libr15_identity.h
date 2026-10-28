/* Which libr15.so the running process expects (docs/adr/0021 section 7).
 *
 * The APK carries libr15.so as the build-time patcher wrote it (src/quest/patch): its GNU build-ID note is derived from the
 * stock ID, the patch table and any extra rows, so the live note is NOT the stock ID. Two IDs therefore exist:
 *
 *   kLibR15StockBuildId       the store file's ID (pinned_targets.h); the patcher's input, the ID plugins declare, the
 *                             one the pinned tests find in the store APK;
 *   LibR15PatchedBuildId()    the ID of the patcher's output; everything checked at run time against the live note (every
 *                             GotTarget, the hook files, the integration, the hardware dump) compares with this one.
 *
 * LibR15PatchedBuildId() recomputes the ID from the compiled-in table with the same function the patcher calls
 * (nevr_quest::patch::PatchedBuildId), so no generated constant can drift from the patcher's. A translation unit that calls
 * it links libr15_identity.cpp, src/quest/patch/libr15_patch_identity.cpp and src/quest/data/sha256.cpp.
 */
#pragma once

#include <vector>

#include "quest/patch/libr15_patch_table.h"

namespace sentinel::pinned {

// The word-patch rows this build was compiled with (ADR section 11). Empty in the public build: no address or word of a
// row that came through --extra-rows is in this repository.
const std::vector<nevr_quest::patch::WordPatchRow>& LibR15ExtraRows();

// The ID of the patched libr15 this build expects, 40 lowercase hex characters, computed once. The pointer is stable for
// the life of the process.
const char* LibR15PatchedBuildId();

}  // namespace sentinel::pinned
