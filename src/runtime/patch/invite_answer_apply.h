#pragma once
// #1278 (PR-D slice D2), #906, docs/adr/0017: writes the invite service's answers into the wizard's text. The pure rule is
// patch/invite_answer_text.h; this is its platform: the service's answer list, the UI text writer (patch/ui_text_writer.h)
// and the log, driven once per frame from the dispatcher (frame/tick.cpp).
#include <vector>

#include "runtime/patch/invite_worker.h"

namespace nevr_invite_apply {

/// Takes the answers that have arrived and posts their texts to the writer. Game-loop thread only (frame/tick.cpp gates it).
/// With the invite service off, or with nothing waiting, it returns after one atomic load and takes no service lock.
void OnFrame();

/// The share page's JOIN was clicked (#1731): the next OnFrame offers the match of the last created invite to the join, the one a
/// typed code reaches. Any thread (the metric observer's); the join itself runs on the game-loop thread inside OnFrame.
void RequestShareJoin() noexcept;

/// Test builds only (the scenario endpoint): keep a bounded copy of each answer the frame applied, and hand them over once.
/// The endpoint cannot read the service's own list, because this applier is its reader.
void KeepForTest(bool on);
std::vector<nevr_invite::Answer> TakeKeptForTest();
/// Test builds only: forget the last created match and a pending share-page JOIN.
void ResetShareJoinForTest();

}  // namespace nevr_invite_apply
