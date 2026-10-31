#include "runtime/patch/invite_answer_apply.h"

#include <atomic>
#include <string>
#include <utility>

#include "core/logging.h"
#include "runtime/patch/invite_answer_text.h"
#include "runtime/patch/invite_join.h"
#include "runtime/patch/invite_join_game.h"
#include "runtime/patch/invite_service.h"
#include "runtime/patch/invite_wizard.h"
#include "runtime/patch/ui_text_writer.h"

namespace nevr_invite_apply {

namespace {

nevr_invite_text::Kept g_kept;
std::atomic<bool> g_shareJoin{false};  // set by the metric observer, taken by the frame
std::string g_lastCreatedMatch;        // the match of the last created invite; game-loop thread only

void Say(nevr_invite::LogLevel level, const std::string& line) {
  Log(level == nevr_invite::LogLevel::Warning ? EchoVR::LogLevel::Warning : EchoVR::LogLevel::Info, "%s", line.c_str());
}

// The JOIN on the share page: the match the player just created is the one to join (#1731).
void OfferShareJoin() {
  if (g_lastCreatedMatch.empty()) {
    Say(nevr_invite::LogLevel::Info, "[NEVR.INVITE] share join not started reason=no_created_match (no invite was created in this run)");
    return;
  }
  Say(nevr_invite::LogLevel::Info,
      "[NEVR.INVITE] share join requested lobby=" + nevr_invite_join::UpperUuid(g_lastCreatedMatch));
  nevr_invite_join_game::OfferMatch(g_lastCreatedMatch);
}

}  // namespace

void RequestShareJoin() noexcept { g_shareJoin.store(true, std::memory_order_release); }

void OnFrame() {
  if (!nevr_invite_service::Active()) return;
  if (g_shareJoin.exchange(false, std::memory_order_acq_rel)) OfferShareJoin();
  // #1415: nothing waits, so this frame costs one atomic load and takes no service lock.
  if (!nevr_invite_service::AnswersPending()) return;
  const std::vector<nevr_invite::Answer> answers = nevr_invite_service::TakeAnswers();
  if (answers.empty()) return;
  g_kept.Add(answers);
  for (const nevr_invite::Answer& answer : answers) {
    if (answer.op != nevr_invite::Op::Create || answer.result != nevr_invite::Result::Ok) continue;
    if (const std::optional<nevr_invite::Created> created = nevr_invite::ParseCreated(answer.body)) g_lastCreatedMatch = created->match_id;
  }
  nevr_invite_text::Apply(
      answers, nevr_ui_text::Ready(), [](nevr_ui_text::Request request) { return nevr_ui_text::Post(std::move(request)); },
      Say);
  // #1401: a resolved code starts the join of its match. The game's thread is the one this runs on (frame/tick.cpp).
  nevr_invite_join_game::OfferResolved(answers);
}

void KeepForTest(bool on) { g_kept.Enable(on); }

std::vector<nevr_invite::Answer> TakeKeptForTest() { return g_kept.Take(); }

void ResetShareJoinForTest() {
  g_shareJoin.store(false);
  g_lastCreatedMatch.clear();
}

}  // namespace nevr_invite_apply
