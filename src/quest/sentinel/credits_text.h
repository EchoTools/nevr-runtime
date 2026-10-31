// The project section of the in-game CREDITS page on Quest (#970): the Quest twin of the PC patch in
// src/runtime/patch/credits_text.h (N148).
//
// libr15's CR15CreditsUICS::InitCreditsUICI and CR15CreditsUICS::OnComponentDisabled each store the address of the credits
// blob into the component instance's cursor (+0x10). In the build-time patched libr15 (docs/adr/0021 section 8) both
// loads are `adrp xN, <slot page>; ldr xN, [xN, #<slot lo12>]` through ONE 8-byte data slot (the table row
// `credits.pairs`, quest/patch/libr15_patch_table.h), so the address the page starts from is a pointer in writable memory.
// Attach stores the game's own blob there (base + kBlobVaddr); Install builds `block + the game's own bytes + NUL` in
// anonymous memory, makes it read-only, and stores its address in the slot with one atomic store. No code is written and no
// protection of the image changes. A refused install leaves the slot as Attach set it: the page shows the game's text.
//
// Not measured on Quest (see the PR that added this file): that the blob's length and hash match the PC blob
// (src/runtime/patch/credits_text.h kBlobLength, kBlobSha256). The process side logs the hash it reads and does not refuse
// on it.
#pragma once

#include <sys/types.h>

#include <cstddef>
#include <cstdint>

#include "got_hook.h"
#include "libr15_attach.h"

namespace nevr_quest::credits_text {

/// The blob the two loads start from, pinned vaddr (disassembly 0x32a3de1). Attach stores base + this in the slot.
inline constexpr std::uint64_t kBlobVaddr = 0x31a3de1;

/// What Install runs against; every field is the production value when left at its default. A host test replaces them.
struct InstallOptions {
  sentinel::ImageLookup lookup = sentinel::FindLoadedImage;
  /// The rendered block; nullptr is this build's (nevr_credits_block::kText, kSize).
  const char* block = nullptr;
  std::size_t blockSize = 0;
  libr15_patch::MapFn map = nullptr;          // nullptr: mmap
  libr15_patch::ProtectFn protect = nullptr;  // nullptr: mprotect (of the anonymous buffer only)
};

/// The process side (credits_text.cpp): refuses unless Attach verified libr15, validates the block, builds the buffer in
/// anonymous memory, makes it read-only and stores its address in the credits slot, and logs one `credits_text` line. False,
/// with the slot as Attach left it, on any refusal.
bool InstallCreditsText(const InstallOptions& options);
bool InstallCreditsText();

}  // namespace nevr_quest::credits_text
