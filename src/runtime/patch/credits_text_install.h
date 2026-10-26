#pragma once

// #938: install the nEVR credits section into the running game (client only). Pure logic is in
// credits_text.h; this is the process side.

#include <cstddef>

namespace nevr_credits_text {

/// Validates `block` (the rendered nEVR section, LF-terminated lines), checks that the game holds
/// the credits blob and instructions this patch knows, builds block + the game's own bytes in
/// memory near the code and points both loads at it. Returns true when the page now starts with
/// the block. On any refusal nothing is written, the game's own credits stay as they were, and one
/// `[NEVR.PATCH] credits_text skipped reason=...` line says why.
bool Install(const char* block, size_t blockLen);

}  // namespace nevr_credits_text
