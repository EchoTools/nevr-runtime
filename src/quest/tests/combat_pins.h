// Test helpers for the pinned words and records of the combat modules (src/quest/combat/pinned_words.h) on a libr15 image built in memory: write every pin of the lists,
// and bend each one in turn (flip its lowest bit) while a callback runs, so a test can show that the install refuses on every single one.
#pragma once

#include <cstddef>
#include <cstdint>

#include "quest/combat/pinned_words.h"
#include "quest/tests/combat_libr15_image.h"

namespace combat_test {

inline void PutPins(Image& image, const nevr_quest::combat::pinned::Pins* lists, std::size_t count) {
  for (std::size_t l = 0; l < count; ++l) {
    for (std::size_t i = 0; i < lists[l].wordCount; ++i) image.Put<std::uint32_t>(lists[l].words[i].vaddr, lists[l].words[i].word);
    for (std::size_t i = 0; i < lists[l].quadCount; ++i) image.Put<std::uint64_t>(lists[l].quads[i].vaddr, lists[l].quads[i].value);
  }
}

// For each word and record of the lists: bend it, call `fn(what)`, put it back. Returns how many were bent.
template <typename Fn>
std::size_t ForEachBentPin(Image& image, const nevr_quest::combat::pinned::Pins* lists, std::size_t count, Fn fn) {
  std::size_t bent = 0;
  for (std::size_t l = 0; l < count; ++l) {
    for (std::size_t i = 0; i < lists[l].wordCount; ++i) {
      const auto& w = lists[l].words[i];
      image.Put<std::uint32_t>(w.vaddr, w.word ^ 1U);
      fn(w.what);
      image.Put<std::uint32_t>(w.vaddr, w.word);
      ++bent;
    }
    for (std::size_t i = 0; i < lists[l].quadCount; ++i) {
      const auto& q = lists[l].quads[i];
      image.Put<std::uint64_t>(q.vaddr, q.value ^ 1ULL);
      fn(q.what);
      image.Put<std::uint64_t>(q.vaddr, q.value);
      ++bent;
    }
  }
  return bent;
}

}  // namespace combat_test
