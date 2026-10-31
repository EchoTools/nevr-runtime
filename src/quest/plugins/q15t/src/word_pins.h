// Code-word pins: the instruction words a wrapper's behaviour was derived from, read from the loaded game library before any
// slot is swapped. There is no MD5 or other whole-file hash gate anywhere in a module: the plugin's InitEx build-id check, these
// word pins and the slot-target checks (import_hook.h) are the whole gate, so a rebuilt or repacked binary that still carries the
// same code is not refused. A pin that fails refuses the module (nothing swapped) with one ERROR line naming the pin: its ELF
// vaddr, the word found and the word expected.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>

#include "import_hook.h"
#include "plugin_log.h"
#include "safe_read.h"

namespace q15t {

struct WordPin {
  std::uintptr_t va;   // ELF vaddr in the pinned build (disassembly VA - 0x100000); read at base + va
  std::uint32_t want;  // the instruction word
};

// True when every pin reads back as expected. At the first pin that is unreadable or differs: one ERROR line
// "<module> refused: code word pin mismatch, <effect>" with pin / found / expected, and false.
inline bool CheckWordPins(const char* module, std::uintptr_t base, const WordPin* pins, std::size_t n,
                          const char* effect = "nothing changed") {
  for (std::size_t i = 0; i < n; ++i) {
    std::uint32_t have = 0;
    const bool ok = SafeRead(base + pins[i].va, &have, sizeof have);
    if (ok && have == pins[i].want) continue;
    char pin[24], found[24], want[24], msg[160];
    std::snprintf(pin, sizeof pin, "0x%llx", static_cast<unsigned long long>(pins[i].va));
    if (ok)
      std::snprintf(found, sizeof found, "0x%08x", have);
    else
      std::snprintf(found, sizeof found, "unreadable");
    std::snprintf(want, sizeof want, "0x%08x", pins[i].want);
    std::snprintf(msg, sizeof msg, "%s refused: code word pin mismatch, %s", module, effect);
    const NevrLogField f[] = {{"pin", pin}, {"found", found}, {"expected", want}};
    Log(NEVR_LOG_ERROR, msg, f, 3);
    return false;
  }
  return true;
}

// The ERROR line of a refused slot-target check (import_hook.h: the slot must hold base + the real function's vaddr): names the pin
// (the slot's ELF vaddr), the word found there now and the word expected, with the install status and reason.
inline void LogSlotRefusal(const char* message, std::uintptr_t base, std::uintptr_t slot_vaddr, std::uintptr_t real_vaddr,
                           const InstallResult& r) {
  std::uintptr_t word = 0;
  const bool ok = SafeRead(base + slot_vaddr, &word, sizeof word);
  char pin[24], real[24], found[24], want[24];
  std::snprintf(pin, sizeof pin, "0x%llx", static_cast<unsigned long long>(slot_vaddr));
  std::snprintf(real, sizeof real, "0x%llx", static_cast<unsigned long long>(real_vaddr));
  if (ok)
    std::snprintf(found, sizeof found, "0x%llx", static_cast<unsigned long long>(word));
  else
    std::snprintf(found, sizeof found, "unreadable");
  std::snprintf(want, sizeof want, "0x%llx", static_cast<unsigned long long>(base + real_vaddr));
  const NevrLogField f[] = {{"slot_vaddr", pin}, {"real_vaddr", real}, {"status", StatusName(r.status)}, {"reason", r.reason},
                            {"pin", pin},        {"found", found},      {"expected", want}};
  Log(NEVR_LOG_ERROR, message, f, 7);
}

}  // namespace q15t
