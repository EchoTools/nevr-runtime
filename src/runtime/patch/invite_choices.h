#pragma once
// #906, docs/adr/0017-invite-codes-for-the-create-game-wizard.md ("Runtime hook", the component-event observation
// point): what the CREATE GAME wizard's cards chose, as pure logic. The game sends a component event for every card
// and button the player presses; the cards of pages 36 to 39 carry the experience, the mode and the map, which the
// click metric does not. This header says which events are the wizard's, how each changes the choices and what a
// request plan makes of them.
//
// No game and no clock in here: invite_choice_hook.cpp reads the event out of the game's memory and calls Observe.
// Memory is reached only through a reader the caller passes, so a test feeds it a fake game space and an unreadable
// one. src/runtime/tests/test_invite_choices.cpp pins everything below on the host.
//
// The page owners, the card ids and the actor-table offsets are the echovr-modkit author's findings (rad-links, MIT,
// by Dualgame), found by use on the final PC build; the event layout is read from CncaGameSpace::SendComponentEvent
// (echovr.exe 0x14050fb60). None of the ids is confirmed by a live click until the run recorded in the PR
// that introduced this file; the code is ours.

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>

#include "nevr/symbol_hash.h"
#include "runtime/patch/invite_wizard.h"

namespace nevr_invite_choices {

// ---- what the wizard sends ----

// The events the wizard's pages send. Everything else that passes through SendComponentEvent is not looked at.
inline constexpr uint64_t kEvtCardClicked = EchoVR::CSymbol64Hash("evt_card_clicked");
inline constexpr uint64_t kEvtButtonClicked = EchoVR::CSymbol64Hash("evt_btn_clicked");
inline constexpr uint64_t kEvtTextSubmitted = EchoVR::CSymbol64Hash("evt_text_submitted");
inline constexpr uint64_t kEvtLinkButtonClicked = EchoVR::CSymbol64Hash("evt_link_button_clicked");
inline constexpr uint64_t kEvtBackButtonClicked = EchoVR::CSymbol64Hash("evt_back_btn_clicked");
inline constexpr uint64_t kEvtSubmissionCanceled = EchoVR::CSymbol64Hash("evt_submission_canceled");

// The actors that own the wizard's pages of mnu_master (34 to 41). A page's events carry its owner's name.
inline constexpr uint64_t kPageJoinCode = 0x2d50833b445fe66cULL;    // 34: the code entry
inline constexpr uint64_t kPageLink = 0xb7eca761e624f927ULL;        // 35
inline constexpr uint64_t kPageExperience = 0x6a9e38e434a28911ULL;  // 36: lobby / arena / combat
inline constexpr uint64_t kPageArenaMode = 0x1726509ab6803498ULL;   // 37
inline constexpr uint64_t kPageCombatMode = 0x04a63327518dd0e4ULL;  // 38
inline constexpr uint64_t kPageMap = 0x5d4b7b11b22d1872ULL;         // 39
inline constexpr uint64_t kPageSettings = 0xeb6ff9ddc99fe9f5ULL;    // 40: CREATE INVITE CODE & LINK
inline constexpr uint64_t kPageShare = 0xe8b448bf5e62ef7dULL;       // 41

// The three card actors of pages 36 to 39 (left, middle, right).
inline constexpr uint64_t kCardLeft = 0x35ec861316acbb37ULL;
inline constexpr uint64_t kCardMiddle = 0x35ec861316acbb34ULL;
inline constexpr uint64_t kCardRight = 0x35ec861316acbb35ULL;

inline bool IsWizardEventSymbol(uint64_t symbol) {
  return symbol == kEvtCardClicked || symbol == kEvtButtonClicked || symbol == kEvtTextSubmitted ||
         symbol == kEvtLinkButtonClicked || symbol == kEvtBackButtonClicked || symbol == kEvtSubmissionCanceled;
}

inline bool IsWizardPage(uint64_t owner) {
  return owner == kPageJoinCode || owner == kPageLink || owner == kPageExperience || owner == kPageArenaMode ||
         owner == kPageCombatMode || owner == kPageMap || owner == kPageSettings || owner == kPageShare;
}

/// A short name for a wizard page, for the log line; "" for an owner that is not one.
inline const char* PageName(uint64_t owner) {
  if (owner == kPageJoinCode) return "join_code";
  if (owner == kPageLink) return "link";
  if (owner == kPageExperience) return "experience";
  if (owner == kPageArenaMode) return "arena_mode";
  if (owner == kPageCombatMode) return "combat_mode";
  if (owner == kPageMap) return "map";
  if (owner == kPageSettings) return "settings";
  if (owner == kPageShare) return "share";
  return "";
}

inline const char* EventName(uint64_t symbol) {
  if (symbol == kEvtCardClicked) return "card";
  if (symbol == kEvtButtonClicked) return "button";
  if (symbol == kEvtTextSubmitted) return "text";
  if (symbol == kEvtLinkButtonClicked) return "link";
  if (symbol == kEvtBackButtonClicked) return "back";
  if (symbol == kEvtSubmissionCanceled) return "cancel";
  return "";
}

/// 1 left, 2 middle, 3 right, 0 for any other component.
inline int CardIndex(uint64_t component) {
  if (component == kCardLeft) return 1;
  if (component == kCardMiddle) return 2;
  if (component == kCardRight) return 3;
  return 0;
}

// ---- the event ----

/// The three words of an event this feature uses. `owner` is the name of the sending actor, 0 when it did not resolve.
struct Event {
  uint64_t symbol = 0;
  uint64_t owner = 0;
  uint64_t component = 0;
};

// SComponentEvent (echovr.exe 0x14050fb60 and its twin 0x14050fed0): word 0 the event symbol, word 2 the sender
// identity (its low 16 bits index the game space's actor table; 0xffff is none), word 3 the component id.
inline constexpr size_t kEventSymbolOffset = 0x00;
inline constexpr size_t kEventSenderOffset = 0x10;
inline constexpr size_t kEventComponentOffset = 0x18;
inline constexpr size_t kEventBytes = 0x28;
inline constexpr uint16_t kNoSender = 0xffff;

// The game space's actor table (UNVERIFIED by a click until the run in the PR): gs+0x370 -> table; table+0x80 -> an
// array of 8-byte names indexed by the sender's low 16 bits; table+0xa8 -> the array's capacity, the bound used here.
inline constexpr size_t kSpaceActorTableOffset = 0x370;
inline constexpr size_t kTableNamesOffset = 0x80;
inline constexpr size_t kTableCapacityOffset = 0xa8;

/// What the observer found in an event, and why it stopped when it stopped.
enum class Read { NotWizardSymbol, Unreadable, NoSender, NoTable, OutOfRange, NotWizardPage, Wizard };

inline const char* ReadName(Read r) {
  switch (r) {
    case Read::NotWizardSymbol: return "not_wizard_symbol";
    case Read::Unreadable: return "unreadable";
    case Read::NoSender: return "no_sender";
    case Read::NoTable: return "no_table";
    case Read::OutOfRange: return "out_of_range";
    case Read::NotWizardPage: return "not_wizard_page";
    case Read::Wizard: return "wizard";
  }
  return "unknown";
}

/// Reads `length` bytes at `address` into `out`; false when the memory is not committed and readable. The hook passes a
/// VirtualQuery-checked copy, a test a fake.
template <typename Reader>
inline Read ReadEvent(Reader&& read, uintptr_t space, uintptr_t event, Event* out) {
  *out = Event{};
  // The event symbol first: almost every event the game sends is not the wizard's, and this is all they cost.
  uint64_t symbol = 0;
  if (event == 0 || !read(event + kEventSymbolOffset, &symbol, sizeof(symbol))) return Read::Unreadable;
  if (!IsWizardEventSymbol(symbol)) return Read::NotWizardSymbol;
  out->symbol = symbol;

  uint64_t sender = 0, component = 0;
  if (!read(event + kEventSenderOffset, &sender, sizeof(sender)) ||
      !read(event + kEventComponentOffset, &component, sizeof(component))) {
    return Read::Unreadable;
  }
  out->component = component;
  const uint16_t index = static_cast<uint16_t>(sender & 0xffff);
  if (index == kNoSender) return Read::NoSender;

  uint64_t table = 0, names = 0, capacity = 0;
  if (space == 0 || !read(space + kSpaceActorTableOffset, &table, sizeof(table))) return Read::Unreadable;
  if (table == 0) return Read::NoTable;
  if (!read(table + kTableNamesOffset, &names, sizeof(names)) ||
      !read(table + kTableCapacityOffset, &capacity, sizeof(capacity))) {
    return Read::Unreadable;
  }
  if (names == 0) return Read::NoTable;
  if (index >= capacity) return Read::OutOfRange;
  uint64_t owner = 0;
  if (!read(static_cast<uintptr_t>(names) + static_cast<uintptr_t>(index) * sizeof(uint64_t), &owner, sizeof(owner))) {
    return Read::Unreadable;
  }
  out->owner = owner;
  return IsWizardPage(owner) ? Read::Wizard : Read::NotWizardPage;
}

// ---- the choices ----

inline constexpr const char* kExperienceLobby = "lobby";
inline constexpr const char* kExperienceArena = "arena";
inline constexpr const char* kExperienceCombat = "combat";
inline constexpr const char* kModeCustom = "custom";
inline constexpr const char* kModeAi = "ai";
inline constexpr const char* kModePayload = "payload";
inline constexpr const char* kModeCapture = "capture";
inline constexpr const char* kModeRandom = "random";

/// The choices after `event`. Only a card event on pages 36 to 39 changes them: a new experience clears the mode and
/// the map, a new combat mode clears the map, and a card that is none of the three leaves the page's choice unset.
inline nevr_invite::Choices Apply(nevr_invite::Choices choices, const Event& event) {
  if (event.symbol != kEvtCardClicked) return choices;
  const int card = CardIndex(event.component);
  if (event.owner == kPageExperience) {
    choices.experience = card == 1 ? kExperienceLobby : card == 2 ? kExperienceArena : card == 3 ? kExperienceCombat : "";
    choices.mode.clear();
    choices.map = 0;
  } else if (event.owner == kPageArenaMode) {
    choices.mode = card == 1 ? kModeCustom : card == 2 ? kModeAi : "";
  } else if (event.owner == kPageCombatMode) {
    choices.mode = card == 1 ? kModePayload : card == 2 ? kModeCapture : card == 3 ? kModeRandom : "";
    choices.map = 0;
  } else if (event.owner == kPageMap) {
    choices.map = card == 1 ? 1 : card == 2 ? 2 : 0;
  }
  return choices;
}

/// The wizard's choices as the last events left them. One writer (the game thread, from the event hook) and any
/// reader (the metric observer), so every access takes the lock.
class State {
 public:
  void Observe(const Event& event) {
    std::lock_guard<std::mutex> lock(mutex_);
    choices_ = Apply(choices_, event);
  }
  nevr_invite::Choices Snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return choices_;
  }

 private:
  mutable std::mutex mutex_;
  nevr_invite::Choices choices_;
};

// ---- the log line (docs/standards/logging.md, tag [NEVR.INVITE]) ----

/// `seen` is how many events the hook has been given since it was installed, the measurement of its call rate.
inline std::string ChoiceLine(const Event& event, const nevr_invite::Choices& choices, uint64_t seen) {
  return std::string("[NEVR.INVITE] wizard event seen page=") + PageName(event.owner) +
         " event=" + EventName(event.symbol) + " card=" + std::to_string(CardIndex(event.component)) + " experience=" + choices.experience +
         " mode=" + choices.mode + " map=" + std::to_string(choices.map) + " seen=" + std::to_string(seen);
}

}  // namespace nevr_invite_choices
