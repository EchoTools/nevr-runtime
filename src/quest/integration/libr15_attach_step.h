// The constructor's `attach_libr15` step (docs/adr/0021 sections 6, 7 and 12): runs Attach on the live libr15, and when it
// verified the file, hands the rest of the sentinel the two things that follow from that:
//
//   * the plugin identity (docs/adr/0021 section 12): the plugin loader reads the patch state through
//     nevr_quest::plugin::SetLibr15PatchSource, so a plugin built for the stock libr15 is shown the stock ID on our patched file;
//   * the GOT backend's build-ID alias (sentinel::SetBuildIdAlias): a plugin target that carries the stock ID is accepted
//     against the live (patched) note, and the sentinel's own targets, which carry the patched ID, are accepted as they are.
//
// Both are registered exactly once per process, and only for a verified file. A file Attach did not verify registers
// nothing: the plugin identity is then the live note and the backend accepts only the live note, as without the patch.
//
// The step never throws for a refusal and never ends the process by itself: Attach ends it (through the process-end call)
// when a row or a trampoline fails, because the file is ours and a mismatch is a build defect.
#pragma once

#include <string>

#include "libr15_attach.h"
#include "quest/plugin/plugin_identity.h"

namespace nevr_quest::integration {

struct AttachStepOutcome {
  bool verified = false;       // Attach verified this process's libr15 (a second call in one process counts: it stays verified)
  const char* detail = "";     // a fixed token: "verified", "already_verified", or the reason of the refusal
};

// The options this build runs Attach with: the default lookup and seams, and the word-patch rows it was compiled with (none in
// the public build).
libr15_patch::AttachOptions BuildAttachOptions();

// Attach with `options`; on a verified file, registers the plugin identity and the build-ID alias if they are not registered
// yet. Returns the outcome with a token that outlives the call.
AttachStepOutcome RunAttachStep(const libr15_patch::AttachOptions& options);

// What the plugin loader reads (registered with SetLibr15PatchSource): `Attach`'s record.
plugin::Libr15PatchState PatchStateForPlugins();

// What the GOT backend asks at every install (registered with SetBuildIdAlias): the stock build ID when `liveNote` is the
// patched ID Attach verified, else nullptr. The returned string outlives the call.
const char* BuildIdAliasForPlugins(const char* liveNote);

// The token a refusal is logged under, for any Attach result.
const char* AttachDetail(const libr15_patch::AttachResult& result);

// Forgets that the identity was registered, and unregisters nothing else. Tests only.
void ResetAttachStepForTest() noexcept;

}  // namespace nevr_quest::integration
