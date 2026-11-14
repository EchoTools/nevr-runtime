#pragma once

// The PC half's check of one game function's entry before the function is called (ADR 0048 decision 9; logging.md Rule 6). Pure:
// the caller passes the image base, so a test drives the check on a buffer. The entry is the stock head, or a hook's jmp in front
// of it whose tail is still stock (prologue_head.h).

#include <cstddef>
#include <cstdint>

#include "core/map_session_pin.h"
#include "runtime/hook/prologue_head.h"

namespace nevr_map_session_pin_entry {

// True when the function at base+rva has its stock entry. Otherwise `refusal` receives the record's fields for `reason` and `name`
// (the function's VA and both byte runs), and the function returns false.
inline bool CheckEntry(const std::uint8_t* base, std::uintptr_t rva, const std::uint8_t* head, std::size_t n, const char* reason,
                       const char* name, nevr_map_session_pin::PinFields& refusal) {
  const std::uint8_t* entry = base + rva;
  return nevr_map_session_pin::CheckGameBytes(entry, static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(entry)), head, n,
                                              nevr_prologue_head::EntryOk, reason, name, refusal);
}

}  // namespace nevr_map_session_pin_entry
