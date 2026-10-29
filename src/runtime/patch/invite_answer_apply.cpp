#include "runtime/patch/invite_answer_apply.h"

#include <string>
#include <utility>

#include "core/logging.h"
#include "runtime/patch/invite_answer_text.h"
#include "runtime/patch/invite_join_game.h"
#include "runtime/patch/invite_service.h"
#include "runtime/patch/ui_text_writer.h"

namespace nevr_invite_apply {

namespace {

nevr_invite_text::Kept g_kept;

}  // namespace

void OnFrame() {
  if (!nevr_invite_service::Active()) return;
  const std::vector<nevr_invite::Answer> answers = nevr_invite_service::TakeAnswers();
  if (answers.empty()) return;
  g_kept.Add(answers);
  nevr_invite_text::Apply(
      answers, nevr_ui_text::Ready(), [](nevr_ui_text::Request request) { return nevr_ui_text::Post(std::move(request)); },
      [](nevr_invite::LogLevel level, const std::string& line) {
        Log(level == nevr_invite::LogLevel::Warning ? EchoVR::LogLevel::Warning : EchoVR::LogLevel::Info, "%s", line.c_str());
      });
  // #1401: a resolved code starts the join of its match. The game's thread is the one this runs on (frame/tick.cpp).
  nevr_invite_join_game::OfferResolved(answers);
}

void KeepForTest(bool on) { g_kept.Enable(on); }

std::vector<nevr_invite::Answer> TakeKeptForTest() { return g_kept.Take(); }

}  // namespace nevr_invite_apply
