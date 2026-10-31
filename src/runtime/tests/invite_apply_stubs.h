#pragma once
// #1415: the platform calls patch/invite_answer_apply.cpp makes, replaced for a host test: the UI text writer, the join of a
// resolved code and the log. The applier and the invite service around it run for real (tests/test_invite_answer_apply.cpp).
// Defined in tests/invite_apply_stubs.cpp.
#include <cstddef>
#include <string>
#include <vector>

#include "runtime/patch/ui_text.h"

namespace nevr_invite_apply_test {

struct Record {
  bool writer_ready = true;                                 // what nevr_ui_text::Ready() returns
  std::vector<nevr_ui_text::Request> posted;                // every text the applier posted, oldest first
  std::size_t join_offers = 0;                              // calls to nevr_invite_join_game::OfferResolved
  std::size_t offered_answers = 0;                          // answers handed to OfferResolved, all calls
  std::vector<std::string> matches_offered;                 // every match handed to OfferMatch (the share page's JOIN), oldest first
  std::vector<std::string> logged;                          // every line the applier logged through Log()
};

/// The stubs' record. Written by the applier on the thread that calls OnFrame (the test's thread).
Record& Get();
void Reset();

}  // namespace nevr_invite_apply_test
