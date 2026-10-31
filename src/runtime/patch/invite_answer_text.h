#pragma once
// #1278 (PR-D slice D2), #906, docs/adr/0017-invite-codes-for-the-create-game-wizard.md ("Show the result"): what the game
// service's answer becomes in the wizard's text, as pure logic.
//
// An answer from the invite worker (patch/invite_worker.h) goes in; the texts for the UI text writer (patch/ui_text.h) come
// out:
//   - a created code: the code and `EXPIRES: HH:MM UTC` in the share page's code and expiry elements, and `<coming soon>`
//     in its link element (the stock text is a dead domain; the link becomes `echo.taxi/<code>` with #1726);
//   - a refused create (or a body that holds no code): the reason where the code would be, the expiry cleared;
//   - a refused resolve: the reason in the entry page's title;
//   - a resolved code: nothing; starting the join is the next unit (D3).
//
// No game, no thread, no clock. invite_answer_apply.cpp is the platform glue (the service, the writer, the log, the frame);
// src/runtime/tests/test_invite_answer_text.cpp pins everything below on the host with a fake writer and a captured log.
//
// The share page's code, expiry and link actors and elements are the echovr-modkit author's findings (rad-links, MIT, by Dualgame),
// read from the PC build's resources; the entry title is the pair a live run of the writer already proved (tools/scenario/
// scenarios/ui_text.yaml). None of the share page's ids has been written from this tree on a running game: a wrong id is a
// request the writer drops with its cause (`[NEVR.UI] text dropped ... reason=no_actor|no_canvas`), never a crash.
//
// A text is never logged, only its field and size: a code is a bearer secret (docs/standards/logging.md, [NEVR.INVITE]).
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "runtime/patch/invite_match_api.h"
#include "runtime/patch/invite_wizard.h"
#include "runtime/patch/invite_worker.h"
#include "runtime/patch/ui_text.h"

namespace nevr_invite_text {

enum class Field { Code, Expires, Link, EntryTitle };

inline const char* FieldName(Field field) {
  switch (field) {
    case Field::Code:
      return "code";
    case Field::Expires:
      return "expires";
    case Field::Link:
      return "link";
    case Field::EntryTitle:
      return "entry_title";
  }
  return "unknown";
}

// The share page (mnu_master page 41): INVITE CODE, EXPIRES and link actors, each written through its `canvasui` text element.
inline constexpr nevr_ui_text::Target kCodeTarget{0xa9bd0cb4e6f2004cULL, 0x35e290123dfa9c72ULL};     // element "codetext"
inline constexpr nevr_ui_text::Target kExpiresTarget{0x0ec8e16986ef88c8ULL, 0xd38c12d22d3552ebULL};  // its name is not in the symbol cache
inline constexpr nevr_ui_text::Target kLinkTarget{0xc3f9f167b2a06043ULL, 0x3657cd7405999810ULL};     // element "link_text"
/// What the link element reads until the invite link exists. The stock text is `www.readyatdawn.com/...`, a domain we do not own.
inline constexpr const char* kLinkText = "<coming soon>";
// The entry page (page 34): the ENTER CODE field's actor and the title bar above it.
inline constexpr nevr_ui_text::Target kEntryTitleTarget{0xad97e58a7b13425fULL, 0xa8452de53657bd6eULL};  // element "title_text"

inline const nevr_ui_text::Target& TargetOf(Field field) {
  switch (field) {
    case Field::Code:
      return kCodeTarget;
    case Field::Expires:
      return kExpiresTarget;
    case Field::Link:
      return kLinkTarget;
    case Field::EntryTitle:
      break;
  }
  return kEntryTitleTarget;
}

struct Item {
  Field field = Field::Code;
  nevr_ui_text::Request request;
};

namespace detail {

inline Item MakeItem(Field field, std::string text) {
  Item item;
  item.field = field;
  item.request.target = TargetOf(field);
  item.request.text = std::move(text);
  return item;
}

}  // namespace detail

/// The texts one answer asks for, in the order they are written.
inline std::vector<Item> TextsFor(const nevr_invite::Answer& answer) {
  std::vector<Item> items;
  if (answer.op == nevr_invite::Op::Resolve) {
    if (nevr_invite::IsFailure(answer.result)) {
      items.push_back(detail::MakeItem(Field::EntryTitle, nevr_invite::WizardText(answer.result)));
    }
    return items;
  }
  if (answer.result == nevr_invite::Result::Ok) {
    if (const auto created = nevr_invite::ParseCreated(answer.body)) {
      items.push_back(detail::MakeItem(Field::Code, created->code));
      items.push_back(detail::MakeItem(Field::Expires, created->expires_text));
      items.push_back(detail::MakeItem(Field::Link, kLinkText));
      return items;
    }
    // The service said ok and sent no code the client may show: that is its failure, and a blank page says nothing.
    items.push_back(detail::MakeItem(Field::Code, nevr_invite::WizardText(nevr_invite::Result::ServerError)));
    items.push_back(detail::MakeItem(Field::Expires, std::string()));
    return items;
  }
  items.push_back(detail::MakeItem(Field::Code, nevr_invite::WizardText(answer.result)));
  items.push_back(detail::MakeItem(Field::Expires, std::string()));
  return items;
}

using PostFn = std::function<nevr_ui_text::Check(nevr_ui_text::Request)>;
using LogFn = std::function<void(nevr_invite::LogLevel, const std::string&)>;

inline std::string PostedLine(Field field, std::size_t bytes) {
  return nevr_invite_match::PostedText(FieldName(field), bytes);
}

inline std::string NotPostedLine(Field field, std::size_t bytes, const char* reason) {
  return nevr_invite_match::NotPostedText(FieldName(field), bytes, reason);
}

/// Posts the texts of `answers`, oldest first. `writerReady` is the writer's own state: with it off nothing is posted and each
/// text says so. A post the writer refuses is a Warning line with its cause; a post it takes is an Info line.
inline void Apply(const std::vector<nevr_invite::Answer>& answers, bool writerReady, const PostFn& post, const LogFn& log) {
  for (const nevr_invite::Answer& answer : answers) {
    for (Item& item : TextsFor(answer)) {
      const std::size_t bytes = item.request.text.size();
      if (!writerReady) {
        log(nevr_invite::LogLevel::Warning, NotPostedLine(item.field, bytes, "writer_off"));
        continue;
      }
      const nevr_ui_text::Check check = post(std::move(item.request));
      if (check == nevr_ui_text::Check::Ok) {
        log(nevr_invite::LogLevel::Info, PostedLine(item.field, bytes));
      } else {
        log(nevr_invite::LogLevel::Warning, NotPostedLine(item.field, bytes, nevr_ui_text::CheckName(check)));
      }
    }
  }
}

inline constexpr std::size_t kMaxKept = 16;

/// The answers the scenario endpoint reads. The menu applier takes each answer from the service (one reader per answer), so
/// a test build asks it to keep a bounded copy; a release build never does, and no code is held for nobody.
class Kept {
 public:
  void Enable(bool on) {
    std::lock_guard<std::mutex> lock(m_);
    on_ = on;
    if (!on_) kept_.clear();
  }

  void Add(const std::vector<nevr_invite::Answer>& answers) {
    std::lock_guard<std::mutex> lock(m_);
    if (!on_) return;
    for (const nevr_invite::Answer& answer : answers) {
      if (kept_.size() >= kMaxKept) kept_.pop_front();
      kept_.push_back(answer);
    }
  }

  /// Each kept answer once, oldest first.
  std::vector<nevr_invite::Answer> Take() {
    std::lock_guard<std::mutex> lock(m_);
    std::vector<nevr_invite::Answer> out(kept_.begin(), kept_.end());
    kept_.clear();
    return out;
  }

 private:
  std::mutex m_;
  bool on_ = false;
  std::deque<nevr_invite::Answer> kept_;
};

}  // namespace nevr_invite_text
