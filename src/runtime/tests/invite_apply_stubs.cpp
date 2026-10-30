// #1415: stubs for the platform calls of patch/invite_answer_apply.cpp (see invite_apply_stubs.h).
#include <winsock2.h>

#include <cstdarg>
#include <cstdio>
#include <utility>
#include <vector>

#include "core/logging.h"
#include "runtime/patch/invite_answer_apply.h"
#include "runtime/patch/invite_join_game.h"
#include "runtime/patch/invite_worker.h"
#include "runtime/patch/ui_text_writer.h"
#include "runtime/tests/invite_apply_stubs.h"

namespace nevr_invite_apply_test {

Record& Get() {
  static Record record;
  return record;
}

void Reset() { Get() = Record(); }

}  // namespace nevr_invite_apply_test

namespace nevr_ui_text {

bool Ready() { return nevr_invite_apply_test::Get().writer_ready; }

Check Post(Request request) {
  nevr_invite_apply_test::Get().posted.push_back(std::move(request));
  return Check::Ok;
}

}  // namespace nevr_ui_text

namespace nevr_invite_join_game {

void OfferResolved(const std::vector<nevr_invite::Answer>& answers) {
  nevr_invite_apply_test::Record& record = nevr_invite_apply_test::Get();
  ++record.join_offers;
  record.offered_answers += answers.size();
}

void OfferMatch(std::string_view match_id) { nevr_invite_apply_test::Get().matches_offered.emplace_back(match_id); }

}  // namespace nevr_invite_join_game

VOID Log(EchoVR::LogLevel level, const CHAR* format, ...) {
  char buffer[2048];
  std::va_list args;
  va_start(args, format);
  std::vsnprintf(buffer, sizeof(buffer), format, args);
  va_end(args);
  const char* tag = level == EchoVR::LogLevel::Warning ? "warning " : "info ";
  nevr_invite_apply_test::Get().logged.push_back(std::string(tag) + buffer);
}
