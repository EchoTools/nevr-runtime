// #1857, docs/standards/logging.md ([NEVR.PLUGINMGR]): the press hook's decision, as pure logic (patch/plugins_card.h): is this
// call of CncaGameSpace::SendComponentEvent a press on the PLUGINS card, what it costs the game thread to find out, and the
// one Info line it writes. No game and no hook: the event, the game space and the actor table below are buffers in this
// process, read by the same ReadPluginsPress the hook calls.
//
// SendComponentEvent has about 25 callers (event-driven callers, rate not measured), so the cost is part of the behaviour: an event
// that is not a card click reads one word, a card click that is not PLUGINS reads two, and only a PLUGINS press walks the
// sender (four reads outside the event, each a page query in the hook).

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <string>

#include "runtime/patch/invite_choices.h"
#include "runtime/patch/menu_press.h"
#include "runtime/patch/plugins_card.h"

namespace {

using nevr_menu_press::Press;
using nevr_menu_press::Sender;

constexpr uint64_t kOtherComponent = 0x35ec861316abbb34ULL;  // PLAY's
constexpr uint64_t kOtherSymbol = 0xabcdULL;
// The component ids the game gives the TUTORIALS card and its clone, the PLUGINS card, as measured (#1857). Test-local on
// purpose: the test checks the production constants against these, so it must not read the values from them.
constexpr uint64_t kExpectedTutorialsCardId = 0x35ec861316abbb35ULL;
constexpr uint64_t kExpectedPluginsCardId = 0x35ec861316abbbf1ULL;
constexpr size_t kBufferAlign = alignof(uint64_t);
constexpr uint64_t kNoActor = 0xffffULL;
constexpr uint64_t kSeenCount = 12;
constexpr uint64_t kPageActor = 0xf67c7e3e9088c0feULL;
constexpr uint64_t kActorSlot = 3;
constexpr size_t kNameSlots = 8;
constexpr size_t kSpaceBytes = 0x400;
constexpr size_t kTableBytes = 0x100;
constexpr int kWalkReads = 4;             // table, names, capacity, one name: the sender walk outside the event
constexpr int kReadsForANonCardSymbol = 1;
constexpr int kReadsForACardClick = 2;
constexpr int kSenderAndComponentReads = 2;  // the walk's own read of the sender word and the component, inside the event
constexpr int kReadsForAPress = kReadsForACardClick + kSenderAndComponentReads + kWalkReads;

// One event, a game space and an actor table as the hook finds them, and a reader that counts what it is asked.
struct FakeGame {
  alignas(kBufferAlign) uint8_t event[nevr_invite_choices::kEventBytes] = {};
  alignas(kBufferAlign) uint8_t space[kSpaceBytes] = {};
  alignas(kBufferAlign) uint8_t table[kTableBytes] = {};
  alignas(kBufferAlign) uint64_t names[kNameSlots] = {};
  mutable int reads = 0;
  mutable int outside = 0;

  FakeGame(uint64_t symbol, uint64_t component, uint64_t sender = kActorSlot) {
    Put(space, nevr_invite_choices::kSpaceActorTableOffset, reinterpret_cast<uintptr_t>(table));
    Put(table, nevr_invite_choices::kTableNamesOffset, reinterpret_cast<uintptr_t>(names));
    Put(table, nevr_invite_choices::kTableCapacityOffset, kNameSlots);
    names[kActorSlot] = kPageActor;
    Put(event, nevr_invite_choices::kEventSymbolOffset, symbol);
    Put(event, nevr_invite_choices::kEventSenderOffset, sender);
    Put(event, nevr_invite_choices::kEventComponentOffset, component);
  }
  static void Put(uint8_t* base, size_t at, uint64_t v) { std::memcpy(base + at, &v, sizeof(v)); }
  bool Within(const void* base, size_t size, uintptr_t address, size_t n) const {
    const uintptr_t from = reinterpret_cast<uintptr_t>(base);
    return address >= from && address + n <= from + size;
  }
  auto Reader() const {
    return [this](uintptr_t address, void* out, size_t n) {
      ++reads;
      const bool inEvent = Within(event, sizeof(event), address, n);
      if (!inEvent) ++outside;
      if (!inEvent && !Within(space, sizeof(space), address, n) && !Within(table, sizeof(table), address, n) &&
          !Within(names, sizeof(names), address, n)) {
        return false;
      }
      std::memcpy(out, reinterpret_cast<const void*>(address), n);
      return true;
    };
  }
  bool Run(Press* press, Sender* sender) const {
    return nevr_plugins_card::ReadPluginsPress(Reader(), reinterpret_cast<uintptr_t>(space),
                                               reinterpret_cast<uintptr_t>(event), press, sender);
  }
};

TEST(PluginsCardConstants, TheCardHasAnIdOfItsOwn) {
  // The clone was made from TUTORIALS and shared its id, so a press on PLUGINS started the tutorial (#1857 measurement).
  EXPECT_NE(nevr_plugins_card::kPluginsCardComponent, nevr_plugins_card::kTutorialsCardComponent);
  EXPECT_EQ(nevr_plugins_card::kTutorialsCardComponent, kExpectedTutorialsCardId);
  EXPECT_EQ(nevr_plugins_card::kPluginsCardComponent, kExpectedPluginsCardId);
}

TEST(PluginsCardPress, ACardClickWithTheCardsComponentIsAPressWithItsOwnerResolved) {
  FakeGame game(nevr_invite_choices::kEvtCardClicked, nevr_plugins_card::kPluginsCardComponent);
  Press press;
  Sender sender = Sender::Unreadable;
  EXPECT_TRUE(game.Run(&press, &sender));
  EXPECT_EQ(sender, Sender::Resolved);
  EXPECT_EQ(press.owner, kPageActor);
  EXPECT_EQ(press.component, nevr_plugins_card::kPluginsCardComponent);
  EXPECT_TRUE(nevr_plugins_card::IsPluginsPress(press));
}

TEST(PluginsCardPress, ThePressHoldsWhetherOrNotTheSenderResolves) {
  FakeGame game(nevr_invite_choices::kEvtCardClicked, nevr_plugins_card::kPluginsCardComponent, kNoActor);
  Press press;
  Sender sender = Sender::Resolved;
  EXPECT_TRUE(game.Run(&press, &sender));
  EXPECT_EQ(sender, Sender::NoSender);
  EXPECT_EQ(press.owner, 0u);
}

TEST(PluginsCardPress, AClickOnTheTutorialsCardIsNotAPress) {
  FakeGame game(nevr_invite_choices::kEvtCardClicked, nevr_plugins_card::kTutorialsCardComponent);
  Press press;
  Sender sender = Sender::Resolved;
  EXPECT_FALSE(game.Run(&press, &sender));
}

TEST(PluginsCardPress, AClickOnAnyOtherCardIsNotAPress) {
  FakeGame game(nevr_invite_choices::kEvtCardClicked, kOtherComponent);
  Press press;
  Sender sender = Sender::Resolved;
  EXPECT_FALSE(game.Run(&press, &sender));
}

TEST(PluginsCardPress, AnotherEventWithTheCardsComponentIsNotAPress) {
  // evt_btn_clicked and every other symbol: only a card click is a press on the card.
  FakeGame game(nevr_invite_choices::kEvtButtonClicked, nevr_plugins_card::kPluginsCardComponent);
  Press press;
  Sender sender = Sender::Resolved;
  EXPECT_FALSE(game.Run(&press, &sender));
}

TEST(PluginsCardPress, TheDecisionChecksTheSymbolAsWellAsTheComponent) {
  Press press;
  press.symbol = nevr_invite_choices::kEvtButtonClicked;
  press.component = nevr_plugins_card::kPluginsCardComponent;
  EXPECT_FALSE(nevr_plugins_card::IsPluginsPress(press));
}

TEST(PluginsCardPress, AnEventThatCouldNotBeReadIsNotAPress) {
  FakeGame game(nevr_invite_choices::kEvtCardClicked, nevr_plugins_card::kPluginsCardComponent);
  Press press;
  Sender sender = Sender::Resolved;
  EXPECT_FALSE(nevr_plugins_card::ReadPluginsPress(game.Reader(), 0, 0, &press, &sender));
  Press nothing;  // what the head leaves for an unreadable event
  EXPECT_FALSE(nevr_plugins_card::IsPluginsPress(nothing));
}

// ---- the cost of asking, per call of SendComponentEvent ----

TEST(PluginsCardCost, AnEventThatIsNotACardClickReadsOneWordAndWalksNothing) {
  FakeGame game(kOtherSymbol, nevr_plugins_card::kPluginsCardComponent);
  Press press;
  Sender sender = Sender::Resolved;
  EXPECT_FALSE(game.Run(&press, &sender));
  EXPECT_EQ(game.reads, kReadsForANonCardSymbol);
  EXPECT_EQ(game.outside, 0) << "a sender walk ran for an event that is not a card click";
}

TEST(PluginsCardCost, ACardClickOnAnotherCardReadsTwoWordsAndWalksNothing) {
  FakeGame game(nevr_invite_choices::kEvtCardClicked, kOtherComponent);
  Press press;
  Sender sender = Sender::Resolved;
  EXPECT_FALSE(game.Run(&press, &sender));
  EXPECT_EQ(game.reads, kReadsForACardClick);
  EXPECT_EQ(game.outside, 0) << "a sender walk ran for a click on another card";
}

TEST(PluginsCardCost, OnlyAPressOnThePluginsCardWalksTheSenderOnce) {
  FakeGame game(nevr_invite_choices::kEvtCardClicked, nevr_plugins_card::kPluginsCardComponent);
  Press press;
  Sender sender = Sender::Unreadable;
  ASSERT_TRUE(game.Run(&press, &sender));
  EXPECT_EQ(game.outside, kWalkReads);
  EXPECT_EQ(game.reads, kReadsForAPress);
}

// ---- the line ----

TEST(PluginsCardLine, OnePressIsOneTaggedInfoLineWithItsIdentifiers) {
  Press press;
  press.symbol = nevr_invite_choices::kEvtCardClicked;
  press.component = nevr_plugins_card::kPluginsCardComponent;
  press.owner = kPageActor;
  char line[nevr_plugins_card::kPluginsLineBytes];
  const size_t length = nevr_plugins_card::PluginsPressLine(line, sizeof(line), press, Sender::Resolved, kSeenCount);
  EXPECT_EQ(std::string(line),
            "[NEVR.PLUGINMGR] plugins card pressed component=0x35ec861316abbbf1 owner=0xf67c7e3e9088c0fe read=resolved "
            "seen=12 (no page opens yet; the press is only recorded)");
  EXPECT_EQ(length, std::string(line).size());
}

TEST(PluginsCardLine, AnUnresolvedSenderSaysWhyAndKeepsTheComponent) {
  Press press;
  press.component = nevr_plugins_card::kPluginsCardComponent;
  char line[nevr_plugins_card::kPluginsLineBytes];
  nevr_plugins_card::PluginsPressLine(line, sizeof(line), press, Sender::NoSender, kSeenCount);
  EXPECT_NE(std::string(line).find("owner=0x0 read=no_sender"), std::string::npos) << line;
}

TEST(PluginsCardLine, TheLongestLineFitsItsBufferWithoutTruncation) {
  Press press;
  press.component = UINT64_MAX;
  press.owner = UINT64_MAX;
  char line[nevr_plugins_card::kPluginsLineBytes];
  const size_t length =
      nevr_plugins_card::PluginsPressLine(line, sizeof(line), press, Sender::OutOfRange, UINT64_MAX);
  EXPECT_LT(length, sizeof(line)) << "the line was cut: " << line;
}

}  // namespace
