// Which libr15 identity a plugin is shown (docs/adr/0021-quest-libr15-patched-at-build-time.md, section 12).
//
// When the running libr15 is our patched file, its live GNU build-ID note is the derived ID (patch::PatchedBuildId()).
// A plugin's addresses are properties of the STOCK file, and the patched file differs from it only in the bytes the
// patcher reports, so a plugin is shown the stock ID. Header-only and pure (no allocation, no I/O, no global): the
// loader, the loader's record and the GOT backend's alias all decide through this one function.
#pragma once

#include <cstring>
#include <string>

#include "quest/patch/libr15_patch_table.h"

namespace nevr_quest::plugin {

// What the process recorded about the patch: `Attach` logged `libr15_patch status=verified`, and the patched build ID it
// verified. Not verified: `verifiedBuildId` is empty.
struct Libr15PatchState {
  bool verified = false;
  std::string verifiedBuildId;
};

struct PluginView {
  const char* buildId;  // the ID plugins see: a pointer into the caller's `liveNote` or to kLibR15StockBuildId
  bool patched;         // true when `buildId` is the stock ID standing for our verified patched file
};

// The plugin identity is the stock ID when the running libr15 is our patched file and the live note otherwise. "Our
// patched file" needs BOTH: (a) the process recorded `verified`, and (b) the live note, read now by the caller, still
// equals the ID that verification covered. With either missing the identity is the live note, so a file that only
// carries the derived ID in its note, or whose note changed after the check, gets no mapping.
inline PluginView ResolvePluginView(const char* liveNote, bool verified, const char* verifiedBuildId) {
  const char* const live = liveNote != nullptr ? liveNote : "";
  if (verified && live[0] != '\0' && verifiedBuildId != nullptr && std::strcmp(live, verifiedBuildId) == 0) {
    return PluginView{patch::kLibR15StockBuildId, true};
  }
  return PluginView{live, false};
}

}  // namespace nevr_quest::plugin
