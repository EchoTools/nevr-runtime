// The checks every combat module's code-word table (combat/pinned_words.h `pinned::Pins`) must pass, for a host test that installs the module on a libr15
// image built in memory (combat_libr15_image.h): the table's content is pinned by a fingerprint, every word flipped in turn refuses the install before
// anything is touched and names the pin, the word found and the word expected at Error, and a word on an unreadable page is refused as `unreadable`. The
// module's test supplies how to prepare a fresh world (a new image holding every pinned word and the slot, the capture started), how to install and what
// "the slot was not touched" means. The pins are written into an image by `PutPins` (combat_pins.h).
#pragma once

#include <sys/mman.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "quest/combat/combat_install.h"
#include "quest/combat/pinned_words.h"
#include "quest/tests/combat_libr15_image.h"
#include "quest/tests/combat_log_capture.h"
#include "quest/tests/combat_pins.h"
#include "quest/tests/test_check.h"

namespace combat_test {

// FNV-1a over every (vaddr, word) of the table, so a test pins the table's content in one number.
inline std::uint64_t Fingerprint(const nevr_quest::combat::pinned::Word* words, std::size_t n) {
  std::uint64_t h = 1469598103934665603ULL;
  auto mix = [&h](std::uint64_t v, int bytes) {
    for (int i = 0; i < bytes; ++i) {
      h ^= (v >> (8 * i)) & 0xff;
      h *= 1099511628211ULL;
    }
  };
  for (std::size_t i = 0; i < n; ++i) {
    mix(words[i].vaddr, 8);
    mix(words[i].word, 4);
  }
  return h;
}

// `prepare()` starts the test's world afresh; `install()` runs the module on it; `untouched()` is whether the slot still holds the game's own word. `event`
// is the module's install event, `image` the test's image (the one `prepare` builds).
template <class Prepare, class Install, class Untouched>
void EveryPinIsRefused(const char* event, const nevr_quest::combat::pinned::Pins& pins, Image& image, Prepare prepare, Install install,
                       Untouched untouched) {
  for (std::size_t i = 0; i < pins.wordCount; ++i) {
    prepare();
    const nevr_quest::combat::pinned::Word& pin = pins.words[i];
    const std::uint32_t good = image.Get<std::uint32_t>(pin.vaddr);
    QCHECK(good == pin.word);
    const std::uint32_t flipped = good ^ 0x00010000U;
    image.Put<std::uint32_t>(pin.vaddr, flipped);
    const nevr_quest::combat::ModuleResult r = install();
    QCHECK(!r.installed && std::strcmp(r.status, "pinned_word_mismatch") == 0);
    QCHECK(untouched());
    const std::vector<Line> lines = Lines(event);
    QCHECK(lines.size() == 1U);
    if (lines.size() == 1U) {
      char at[19];
      char found[19];
      char want[19];
      QCHECK(lines[0].level == sentinel::LogLevel::kError);
      QCHECK(Has(lines[0].text, std::string("\"reason\":\"pinned_word_mismatch\"")));
      QCHECK(Has(lines[0].text, std::string("\"pin\":\"") + pin.what + "\""));
      QCHECK(Has(lines[0].text, std::string("\"vaddr\":\"") + sentinel::HexString(at, pin.vaddr) + "\""));
      QCHECK(Has(lines[0].text, std::string("\"found\":\"") + sentinel::HexString(found, flipped) + "\""));
      QCHECK(Has(lines[0].text, std::string("\"expected\":\"") + sentinel::HexString(want, good) + "\""));
    }
    image.Put<std::uint32_t>(pin.vaddr, good);
  }
  {  // a word on an unreadable page: refused as `unreadable`, never a signal
    prepare();
    const std::uintptr_t page = (image.Base() + static_cast<std::uintptr_t>(pins.words[0].vaddr)) & ~static_cast<std::uintptr_t>(0xfff);
    QCHECK(mprotect(reinterpret_cast<void*>(page), 0x1000, PROT_NONE) == 0);
    const nevr_quest::combat::ModuleResult r = install();
    mprotect(reinterpret_cast<void*>(page), 0x1000, PROT_READ | PROT_WRITE);
    QCHECK(!r.installed && std::strcmp(r.status, "pinned_word_mismatch") == 0);
    QCHECK(untouched());
    const std::vector<Line> lines = Lines(event);
    QCHECK(lines.size() == 1U && Has(lines[0].text, "\"found\":\"unreadable\""));
  }
}

}  // namespace combat_test
