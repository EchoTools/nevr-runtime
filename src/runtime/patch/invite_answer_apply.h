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

/// Test builds only (the scenario endpoint): keep a bounded copy of each answer the frame applied, and hand them over once.
/// The endpoint cannot read the service's own list, because this applier is its reader.
void KeepForTest(bool on);
std::vector<nevr_invite::Answer> TakeKeptForTest();

}  // namespace nevr_invite_apply
