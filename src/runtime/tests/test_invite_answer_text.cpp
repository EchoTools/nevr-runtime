// #1278, #906, docs/adr/0017 ("Show the result"): what the game service's answer becomes in the wizard's text, as pure logic
// (patch/invite_answer_text.h). The answer goes in, a list of texts for the UI text writer comes out; the posting, the log
// lines and the record the scenario endpoint reads are tested here with a fake writer and a captured log. No game, no thread.

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "runtime/patch/invite_answer_text.h"

namespace {

using nevr_invite::Answer;
using nevr_invite::Op;
using nevr_invite::Result;
using nevr_invite_text::Field;
using nevr_ui_text::Check;
using nevr_ui_text::Request;

constexpr const char* kUuid = "123e4567-e89b-12d3-a456-426614174000";

Answer Refused(Op op, Result result) {
  Answer answer;
  answer.op = op;
  answer.sent = true;
  answer.status = 404;
  answer.result = result;
  return answer;
}

Answer Created(const std::string& code, const std::string& expiresAt) {
  Answer answer;
  answer.op = Op::Create;
  answer.sent = true;
  answer.status = 200;
  answer.result = Result::Ok;
  answer.body = nlohmann::json{{"code", code}, {"match_id", kUuid}, {"expires_at", expiresAt}}.dump();
  return answer;
}

const nevr_invite_text::Item* Find(const std::vector<nevr_invite_text::Item>& items, Field field) {
  for (const nevr_invite_text::Item& item : items) {
    if (item.field == field) return &item;
  }
  return nullptr;
}

// ---- what an answer says ----

TEST(InviteAnswerText, ARefusedResolveIsWrittenIntoTheEntryPagesTitle) {
  const auto items = nevr_invite_text::TextsFor(Refused(Op::Resolve, Result::Invalid));
  ASSERT_EQ(items.size(), 1u);
  EXPECT_EQ(items[0].field, Field::EntryTitle);
  EXPECT_EQ(items[0].request.text, "INVALID CODE");
  EXPECT_EQ(items[0].request.target.actor, 0xad97e58a7b13425fULL);   // the entry field's actor (ui_text.yaml)
  EXPECT_EQ(items[0].request.target.element, 0xa8452de53657bd6eULL);  // its title_text
}

TEST(InviteAnswerText, EveryRefusalReadsAsTheWizardsWordsForIt) {
  const std::pair<Result, const char*> cases[] = {
      {Result::Invalid, "INVALID CODE"},      {Result::Expired, "CODE EXPIRED"},        {Result::Closed, "CODE CLOSED"},
      {Result::Denied, "NOT ALLOWED"},        {Result::Limit, "TOO MANY CODES"},        {Result::Rate, "TRY AGAIN LATER"},
      {Result::Unavailable, "NO CONNECTION"}, {Result::ServerError, "SERVER ERROR"},    {Result::NoServer, "NO SERVER AVAILABLE"},
  };
  for (const auto& [result, text] : cases) {
    const auto items = nevr_invite_text::TextsFor(Refused(Op::Resolve, result));
    ASSERT_EQ(items.size(), 1u) << text;
    EXPECT_EQ(items[0].request.text, text);
  }
}

TEST(InviteAnswerText, ASentFalseAnswerReadsAsNoConnection) {
  Answer answer;  // no token: nothing went out, the answer is the default
  answer.op = Op::Resolve;
  const auto items = nevr_invite_text::TextsFor(answer);
  ASSERT_EQ(items.size(), 1u);
  EXPECT_EQ(items[0].request.text, "NO CONNECTION");
}

TEST(InviteAnswerText, AResolvedCodeWritesNothingYet) {
  Answer ok;
  ok.op = Op::Resolve;
  ok.sent = true;
  ok.status = 200;
  ok.result = Result::Ok;
  ok.body = nlohmann::json{{"match_id", kUuid}}.dump();
  EXPECT_TRUE(nevr_invite_text::TextsFor(ok).empty());  // the join is the next unit (D3)
}

TEST(InviteAnswerText, ACreatedCodeAndItsExpiryAreWrittenIntoTheSharePage) {
  const auto items = nevr_invite_text::TextsFor(Created("K7M2Q", "2026-10-21T05:38:59.63Z"));
  ASSERT_EQ(items.size(), 3u);
  const auto* code = Find(items, Field::Code);
  const auto* expires = Find(items, Field::Expires);
  ASSERT_NE(code, nullptr);
  ASSERT_NE(expires, nullptr);
  EXPECT_EQ(code->request.text, "K7M2Q");
  EXPECT_EQ(code->request.target.actor, 0xa9bd0cb4e6f2004cULL);
  EXPECT_EQ(code->request.target.element, 0x35e290123dfa9c72ULL);  // "codetext", in the symbol cache
  EXPECT_EQ(expires->request.text, "EXPIRES: 05:38 UTC");
  EXPECT_EQ(expires->request.target.actor, 0x0ec8e16986ef88c8ULL);
  EXPECT_EQ(expires->request.target.element, 0xd38c12d22d3552ebULL);
}

TEST(InviteAnswerText, TheLinkTextOfACreatedCodeReadsComingSoon) {
  const auto items = nevr_invite_text::TextsFor(Created("K7M2Q", "2026-10-21T05:38:59.63Z"));
  const auto* link = Find(items, Field::Link);
  ASSERT_NE(link, nullptr);
  EXPECT_EQ(link->request.text, "<coming soon>");
  EXPECT_EQ(link->request.target.actor, 0xc3f9f167b2a06043ULL);
  EXPECT_EQ(link->request.target.element, 0x3657cd7405999810ULL);  // "link_text", in the symbol cache
  EXPECT_EQ(link->request.text.find("readyatdawn"), std::string::npos);
}

TEST(InviteAnswerText, ARefusedCreateLeavesTheLinkAlone) {
  EXPECT_EQ(Find(nevr_invite_text::TextsFor(Refused(Op::Create, Result::Limit)), Field::Link), nullptr);
}

TEST(InviteAnswerText, AnUnreadableExpiryIsADashNotAMissingLine) {
  const auto items = nevr_invite_text::TextsFor(Created("K7M2Q", "tomorrow"));
  const auto* expires = Find(items, Field::Expires);
  ASSERT_NE(expires, nullptr);
  EXPECT_EQ(expires->request.text, "EXPIRES: -");
}

TEST(InviteAnswerText, ARefusedCreateShowsTheReasonWhereTheCodeWouldBeAndClearsTheExpiry) {
  const auto items = nevr_invite_text::TextsFor(Refused(Op::Create, Result::Limit));
  ASSERT_EQ(items.size(), 2u);
  const auto* code = Find(items, Field::Code);
  const auto* expires = Find(items, Field::Expires);
  ASSERT_NE(code, nullptr);
  ASSERT_NE(expires, nullptr);
  EXPECT_EQ(code->request.text, "TOO MANY CODES");
  EXPECT_EQ(expires->request.text, "");  // an empty text clears the element
}

TEST(InviteAnswerText, ACreateWhoseBodyHoldsNoCodeIsAServerErrorNotABlankPage) {
  Answer ok = Created("K7M2Q", "2026-10-21T05:38:59.63Z");
  ok.body = "{}";
  const auto items = nevr_invite_text::TextsFor(ok);
  const auto* code = Find(items, Field::Code);
  ASSERT_NE(code, nullptr);
  EXPECT_EQ(code->request.text, "SERVER ERROR");
  ASSERT_NE(Find(items, Field::Expires), nullptr);
  EXPECT_EQ(Find(items, Field::Expires)->request.text, "");

  ok.body = nlohmann::json{{"code", "k7m2o"}, {"match_id", kUuid}}.dump();  // outside the alphabet
  EXPECT_EQ(Find(nevr_invite_text::TextsFor(ok), Field::Code)->request.text, "SERVER ERROR");
}

TEST(InviteAnswerText, EveryTextIsSomethingTheWriterWillAccept) {
  std::vector<Answer> answers = {Created("K7M2Q", "2026-10-21T05:38:59.63Z"), Created("K7M2Q", "x")};
  for (const Op op : {Op::Create, Op::Resolve}) {
    for (const Result r : {Result::Invalid, Result::Expired, Result::Closed, Result::Denied, Result::Limit, Result::Rate,
                           Result::Unavailable, Result::ServerError, Result::NoServer}) {
      answers.push_back(Refused(op, r));
    }
  }
  for (const Answer& answer : answers) {
    for (const nevr_invite_text::Item& item : nevr_invite_text::TextsFor(answer)) {
      EXPECT_EQ(nevr_ui_text::CheckRequest(item.request), Check::Ok) << item.request.text;
    }
  }
}

TEST(InviteAnswerText, TheFourTargetsAreDistinctAndNamed) {
  EXPECT_FALSE(nevr_invite_text::kCodeTarget == nevr_invite_text::kLinkTarget);
  EXPECT_FALSE(nevr_invite_text::kExpiresTarget == nevr_invite_text::kLinkTarget);
  EXPECT_FALSE(nevr_invite_text::kEntryTitleTarget == nevr_invite_text::kLinkTarget);
  EXPECT_FALSE(nevr_invite_text::kCodeTarget == nevr_invite_text::kExpiresTarget);
  EXPECT_FALSE(nevr_invite_text::kCodeTarget == nevr_invite_text::kEntryTitleTarget);
  EXPECT_FALSE(nevr_invite_text::kExpiresTarget == nevr_invite_text::kEntryTitleTarget);
  EXPECT_STREQ(nevr_invite_text::FieldName(Field::Code), "code");
  EXPECT_STREQ(nevr_invite_text::FieldName(Field::Expires), "expires");
  EXPECT_STREQ(nevr_invite_text::FieldName(Field::Link), "link");
  EXPECT_STREQ(nevr_invite_text::FieldName(Field::EntryTitle), "entry_title");
}

// ---- posting ----

struct Rig {
  std::vector<Request> posted;
  std::vector<std::pair<nevr_invite::LogLevel, std::string>> lines;
  Check answer = Check::Ok;

  nevr_invite_text::PostFn Post() {
    return [this](Request request) {
      posted.push_back(request);
      return answer;
    };
  }
  nevr_invite_text::LogFn Log() {
    return [this](nevr_invite::LogLevel level, const std::string& line) { lines.emplace_back(level, line); };
  }
  bool Logged(const std::string& needle) const {
    for (const auto& [level, line] : lines) {
      if (line.find(needle) != std::string::npos) return true;
    }
    return false;
  }
};

TEST(InviteAnswerPost, EachTextIsPostedAndLoggedByFieldAndSizeOnly) {
  Rig rig;
  nevr_invite_text::Apply({Created("K7M2Q", "2026-10-21T05:38:59.63Z")}, true, rig.Post(), rig.Log());
  ASSERT_EQ(rig.posted.size(), 3u);
  EXPECT_EQ(rig.posted[0].text, "K7M2Q");
  EXPECT_EQ(rig.posted[2].text, "<coming soon>");
  ASSERT_EQ(rig.lines.size(), 3u);
  EXPECT_EQ(rig.lines[0].second, "[NEVR.INVITE] text posted field=code bytes=5");
  EXPECT_EQ(rig.lines[1].second, "[NEVR.INVITE] text posted field=expires bytes=18");
  EXPECT_EQ(rig.lines[2].second, "[NEVR.INVITE] text posted field=link bytes=13");
  EXPECT_EQ(rig.lines[0].first, nevr_invite::LogLevel::Info);
  for (const auto& [level, line] : rig.lines) {
    EXPECT_EQ(line.find("K7M2Q"), std::string::npos) << "the code is a bearer secret";
    EXPECT_EQ(line.find("EXPIRES"), std::string::npos);
  }
}

TEST(InviteAnswerPost, AWriterThatIsOffPostsNothingAndSaysWhy) {
  Rig rig;
  nevr_invite_text::Apply({Refused(Op::Resolve, Result::Invalid)}, false, rig.Post(), rig.Log());
  EXPECT_TRUE(rig.posted.empty());
  ASSERT_EQ(rig.lines.size(), 1u);
  EXPECT_EQ(rig.lines[0].second, "[NEVR.INVITE] text not posted field=entry_title bytes=12 reason=writer_off");
  EXPECT_EQ(rig.lines[0].first, nevr_invite::LogLevel::Warning);
}

TEST(InviteAnswerPost, ARefusedPostIsALineWithItsCause) {
  Rig rig;
  rig.answer = Check::Full;
  nevr_invite_text::Apply({Refused(Op::Resolve, Result::Rate)}, true, rig.Post(), rig.Log());
  ASSERT_EQ(rig.lines.size(), 1u);
  EXPECT_EQ(rig.lines[0].second, "[NEVR.INVITE] text not posted field=entry_title bytes=15 reason=full");
  EXPECT_EQ(rig.lines[0].first, nevr_invite::LogLevel::Warning);
}

TEST(InviteAnswerPost, AnAnswerWithNothingToShowIsSilent) {
  Rig rig;
  Answer ok;
  ok.op = Op::Resolve;
  ok.sent = true;
  ok.status = 200;
  ok.result = Result::Ok;
  ok.body = nlohmann::json{{"match_id", kUuid}}.dump();
  nevr_invite_text::Apply({ok}, true, rig.Post(), rig.Log());
  EXPECT_TRUE(rig.posted.empty());
  EXPECT_TRUE(rig.lines.empty());
}

TEST(InviteAnswerPost, AnswersAreAppliedOldestFirst) {
  Rig rig;
  nevr_invite_text::Apply({Refused(Op::Resolve, Result::Invalid), Refused(Op::Resolve, Result::Expired)}, true, rig.Post(),
                          rig.Log());
  ASSERT_EQ(rig.posted.size(), 2u);
  EXPECT_EQ(rig.posted[0].text, "INVALID CODE");
  EXPECT_EQ(rig.posted[1].text, "CODE EXPIRED");  // the writer keeps the newer text for a target
}

// ---- the record the scenario endpoint reads ----

TEST(InviteAnswerKept, NothingIsKeptUnlessAskedAndEachAnswerIsHandedOverOnce) {
  nevr_invite_text::Kept kept;
  kept.Add({Refused(Op::Resolve, Result::Invalid)});
  EXPECT_TRUE(kept.Take().empty());  // off: a code is not held in memory for nobody

  kept.Enable(true);
  kept.Add({Refused(Op::Resolve, Result::Invalid), Created("K7M2Q", "2026-10-21T05:38:59.63Z")});
  const std::vector<Answer> taken = kept.Take();
  ASSERT_EQ(taken.size(), 2u);
  EXPECT_EQ(taken[0].result, Result::Invalid);
  EXPECT_EQ(taken[1].result, Result::Ok);
  EXPECT_TRUE(kept.Take().empty());
}

TEST(InviteAnswerKept, TheRecordIsBoundedAndKeepsTheNewest) {
  nevr_invite_text::Kept kept;
  kept.Enable(true);
  std::vector<Answer> many;
  for (std::size_t i = 0; i < nevr_invite_text::kMaxKept + 4; ++i) {
    Answer answer = Refused(Op::Resolve, Result::Invalid);
    answer.status = static_cast<int>(i);
    many.push_back(answer);
  }
  kept.Add(many);
  const std::vector<Answer> taken = kept.Take();
  ASSERT_EQ(taken.size(), nevr_invite_text::kMaxKept);
  EXPECT_EQ(taken.front().status, 4);
  EXPECT_EQ(taken.back().status, static_cast<int>(nevr_invite_text::kMaxKept + 3));
}

TEST(InviteAnswerKept, TurningItOffForgetsWhatWasHeld) {
  nevr_invite_text::Kept kept;
  kept.Enable(true);
  kept.Add({Created("K7M2Q", "2026-10-21T05:38:59.63Z")});
  kept.Enable(false);
  EXPECT_TRUE(kept.Take().empty());
}

}  // namespace
