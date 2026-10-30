#pragma once
// ADR 0017 (#966), plan slice S2: the observer on libr15's NR15Logger::MetricLog. The game calls it with the wizard's click
// metric as JSON (x0 = the text, x1 = its length). The observer copies the text, runs the shared PC parse
// (src/runtime/patch/invite_wizard.h) and logs what the click asks for, then calls the game's own function exactly once with
// the arguments it was given. No request is sent in this slice: the submit is off until the invite service lands (S3).
//
// The install is reached through the integration sequence (src/quest/integration/ctor_sequence.cpp), which gates it.
#include <cstddef>

#include "quest/sentinel/got_hook.h"

namespace nevr_invite {
enum class Skip;  // defined in runtime/patch/invite_wizard.h; the observer's .cpp includes it
}

namespace nevr_quest::wizard_metric {

struct Totals {
  unsigned long long calls = 0;    // entries into the observer while its handler is armed
  unsigned long long copies = 0;   // copies attempted (the length was in range)
  unsigned long long wizard = 0;   // lines that parse as a CREATE GAME or a JOIN VIA CODE click
  unsigned long long submits = 0;  // plans handed to the submit stub (always 0 while the submit is off)
};

// Installs the observer on the stock MetricLog slot. Refuses, with one invite_observer_off line, when the image is not
// found, when the function's first 16 bytes are not the pinned prologue, or when the slot does not take the hook. The
// handler is armed only after the prologue check passes, so a refusal leaves every call to the game unchanged.
bool Install(sentinel::ImageLookup lookup = sentinel::FindLoadedImage) noexcept;

// Copies `length` bytes from `source` to `destination` through a pipe: the kernel reads the source, so an unmapped byte
// fails the write with EFAULT and the game's thread never faults on it. False for a null pointer, a zero length, a length
// above `capacity`, or any transfer that does not complete.
bool CopyGameBytes(const char* source, std::size_t length, char* destination, std::size_t capacity) noexcept;

// True when the 16 bytes at `code` are the pinned MetricLog prologue (sentinel::pinned::kMetricLogCode).
bool PrologueMatches(const unsigned char* code) noexcept;

// The reason token a not-sent line carries for a plan's skip: the PC tokens of invite_wizard.h (NotSentLine), one per Skip.
const char* SkipToken(nevr_invite::Skip skip) noexcept;

// Test support: ArmForTest arms the handler without installing the hook; ResetForTest clears the counters.
void ArmForTest() noexcept;
void ResetForTest() noexcept;
Totals CurrentTotals() noexcept;

}  // namespace nevr_quest::wizard_metric
