// #1857, docs/standards/logging.md ([NEVR.PLUGINMGR]): what a main-menu card press looks like as a component event
// (patch/menu_press.h), as pure logic. The measurement behind the plugin manager's press hook: which event symbol, which
// owner and which component each call of CncaGameSpace::SendComponentEvent carries. No game and no hook: the game space
// below is a few buffers in this process.

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <utility>

#include "nevr/symbol_hash.h"
#include "runtime/patch/invite_choices.h"
#include "runtime/patch/menu_press.h"

namespace {

using nevr_menu_press::Press;
using nevr_menu_press::Sender;

constexpr uint64_t kNewCardOwner = 0xf67c7e3e9088c0feULL;

// A game space, its actor table and an event, as buffers in this process, plus a reader that serves only what was
// registered. `refuse` makes one address unreadable, the way a freed page is.
struct FakeGame {
  alignas(8) uint8_t event[nevr_invite_choices::kEventBytes] = {};
  alignas(8) uint8_t space[0x400] = {};
  alignas(8) uint8_t table[0x100] = {};
  alignas(8) uint64_t names[16] = {};
  uintptr_t refuse = 0;
  mutable int reads = 0;

  FakeGame() {
    Put(space, nevr_invite_choices::kSpaceActorTableOffset, reinterpret_cast<uintptr_t>(table));
    Put(table, nevr_invite_choices::kTableNamesOffset, reinterpret_cast<uintptr_t>(names));
    Put(table, nevr_invite_choices::kTableCapacityOffset, 16);
    SetEvent(EchoVR::CSymbol64Hash("evt_something_else"), 5, 0x77);
    names[5] = kNewCardOwner;
  }
  static void Put(uint8_t* base, size_t at, uint64_t v) { std::memcpy(base + at, &v, sizeof(v)); }
  void SetWords(uint64_t target, uint64_t value) {
    Put(event, nevr_menu_press::kEventTargetOffset, target);
    Put(event, nevr_menu_press::kEventValueOffset, value);
  }
  void SetEvent(uint64_t symbol, uint64_t sender, uint64_t component) {
    Put(event, nevr_invite_choices::kEventSymbolOffset, symbol);
    Put(event, nevr_invite_choices::kEventSenderOffset, sender);
    Put(event, nevr_invite_choices::kEventComponentOffset, component);
  }
  bool Serves(uintptr_t address, size_t n) const {
    const std::pair<const void*, size_t> regions[] = {{event, sizeof(event)}, {space, sizeof(space)},
                                                      {table, sizeof(table)}, {names, sizeof(names)}};
    for (const auto& r : regions) {
      const uintptr_t from = reinterpret_cast<uintptr_t>(r.first);
      if (address >= from && address + n <= from + r.second) return true;
    }
    return false;
  }
  auto Reader() const {
    return [this](uintptr_t address, void* out, size_t n) {
      ++reads;
      if (address == refuse || !Serves(address, n)) return false;
      std::memcpy(out, reinterpret_cast<const void*>(address), n);
      return true;
    };
  }
  Sender Run(Press* out) const {
    return nevr_menu_press::ReadPress(Reader(), reinterpret_cast<uintptr_t>(space), reinterpret_cast<uintptr_t>(event),
                                      out);
  }
};

// ---- reading a press out of the game's memory ----

TEST(MenuPressRead, AnyEventSymbolIsReadWithItsOwnerAndComponent) {
  FakeGame game;
  Press e;
  ASSERT_EQ(game.Run(&e), Sender::Resolved);
  EXPECT_EQ(e.symbol, EchoVR::CSymbol64Hash("evt_something_else"));
  EXPECT_EQ(e.owner, kNewCardOwner);
  EXPECT_EQ(e.component, 0x77u);
}

TEST(MenuPressRead, TheWizardsOwnSymbolIsReadTheSameWay) {
  FakeGame game;
  game.SetEvent(nevr_invite_choices::kEvtCardClicked, 5, nevr_invite_choices::kCardLeft);
  Press e;
  ASSERT_EQ(game.Run(&e), Sender::Resolved);
  EXPECT_EQ(e.symbol, nevr_invite_choices::kEvtCardClicked);
  EXPECT_EQ(e.owner, kNewCardOwner);
}

TEST(MenuPressRead, ASenderOfFFFFIsNoSenderButTheSymbolAndComponentAreKept) {
  FakeGame game;
  game.SetEvent(0xabcdULL, 0x1234ffffULL, 0x99);
  Press e;
  EXPECT_EQ(game.Run(&e), Sender::NoSender);
  EXPECT_EQ(e.symbol, 0xabcdULL);
  EXPECT_EQ(e.component, 0x99u);
  EXPECT_EQ(e.owner, 0u);
}

TEST(MenuPressRead, OnlyTheLow16BitsOfTheSenderIndexTheTable) {
  FakeGame game;
  game.SetEvent(0xabcdULL, 0xabcd0000ULL | 5, 0x99);
  Press e;
  ASSERT_EQ(game.Run(&e), Sender::Resolved);
  EXPECT_EQ(e.owner, kNewCardOwner);
}

TEST(MenuPressRead, AnIndexAtOrPastTheCapacityIsOutOfRange) {
  FakeGame game;
  Press e;
  game.SetEvent(0xabcdULL, 16, 0x99);
  EXPECT_EQ(game.Run(&e), Sender::OutOfRange);
  EXPECT_EQ(e.owner, 0u);
  game.SetEvent(0xabcdULL, 15, 0x99);
  game.names[15] = 0x1111222233334444ULL;
  EXPECT_EQ(game.Run(&e), Sender::Resolved);
  EXPECT_EQ(e.owner, 0x1111222233334444ULL);
}

TEST(MenuPressRead, AMissingTableOrNamesArrayIsRefusedNotFollowed) {
  Press e;
  FakeGame game;
  FakeGame::Put(game.space, nevr_invite_choices::kSpaceActorTableOffset, 0);
  EXPECT_EQ(game.Run(&e), Sender::NoTable);
  FakeGame again;
  FakeGame::Put(again.table, nevr_invite_choices::kTableNamesOffset, 0);
  EXPECT_EQ(again.Run(&e), Sender::NoTable);
}

TEST(MenuPressRead, MemoryThatCannotBeReadIsRefusedAtEveryStep) {
  Press e;
  const size_t offsets[] = {nevr_invite_choices::kEventSymbolOffset, nevr_invite_choices::kEventSenderOffset,
                            nevr_invite_choices::kEventComponentOffset, nevr_menu_press::kEventTargetOffset,
                            nevr_menu_press::kEventValueOffset};
  for (size_t offset : offsets) {
    FakeGame game;
    game.refuse = reinterpret_cast<uintptr_t>(game.event) + offset;
    EXPECT_EQ(game.Run(&e), Sender::Unreadable) << "event+" << offset;
  }
  {
    FakeGame game;
    game.refuse = reinterpret_cast<uintptr_t>(game.space) + nevr_invite_choices::kSpaceActorTableOffset;
    EXPECT_EQ(game.Run(&e), Sender::Unreadable);
  }
  {
    FakeGame game;
    game.refuse = reinterpret_cast<uintptr_t>(game.table) + nevr_invite_choices::kTableNamesOffset;
    EXPECT_EQ(game.Run(&e), Sender::Unreadable);
  }
  {
    FakeGame game;
    game.refuse = reinterpret_cast<uintptr_t>(game.table) + nevr_invite_choices::kTableCapacityOffset;
    EXPECT_EQ(game.Run(&e), Sender::Unreadable);
  }
  {
    FakeGame game;
    game.refuse = reinterpret_cast<uintptr_t>(game.names) + 5 * sizeof(uint64_t);
    EXPECT_EQ(game.Run(&e), Sender::Unreadable);
  }
}

TEST(MenuPressRead, TheTargetAndTheValueWordsAreReadWithTheRest) {
  FakeGame game;
  game.SetEvent(0xabcdULL, 5, 0x99);
  game.SetWords(0x1111, 0x2222);
  Press e;
  ASSERT_EQ(game.Run(&e), Sender::Resolved);
  EXPECT_EQ(e.target, 0x1111u);
  EXPECT_EQ(e.value, 0x2222u);
  EXPECT_EQ(e.component, 0x99u);
}

TEST(MenuPressRead, TheTargetAndTheValueAreKeptWhenTheSenderDoesNotResolve) {
  FakeGame game;
  game.SetEvent(0xabcdULL, 0x1234ffffULL, 0x99);
  game.SetWords(0x1111, 0x2222);
  Press e;
  EXPECT_EQ(game.Run(&e), Sender::NoSender);
  EXPECT_EQ(e.target, 0x1111u);
  EXPECT_EQ(e.value, 0x2222u);
}

TEST(MenuPressRead, TheWordOffsetsAreTheOnesTheGameReads) {
  // SComponentEvent (echovr.exe 0x14050fb60): word 1 is the target space id, word 4 the value.
  EXPECT_EQ(nevr_menu_press::kEventTargetOffset, 0x08u);
  EXPECT_EQ(nevr_menu_press::kEventValueOffset, 0x20u);
}

TEST(MenuPressRead, ANullEventOrSpaceIsRefused) {
  FakeGame game;
  Press e;
  EXPECT_EQ(nevr_menu_press::ReadPress(game.Reader(), reinterpret_cast<uintptr_t>(game.space), 0, &e),
            Sender::Unreadable);
  EXPECT_EQ(nevr_menu_press::ReadPress(game.Reader(), 0, reinterpret_cast<uintptr_t>(game.event), &e),
            Sender::Unreadable);
}

// ---- the log line ----

TEST(MenuPressLine, ACardPressIsOneTaggedLineWithTheSymbolOwnerAndComponent) {
  Press e;
  e.symbol = 0x1122334455667788ULL;
  e.owner = kNewCardOwner;
  e.component = 0x77;
  e.target = 0x1234;
  e.value = 0x5;
  EXPECT_EQ(nevr_menu_press::PressLine(e, Sender::Resolved, 42),
            "[NEVR.PLUGINMGR] component event read=resolved event=0x1122334455667788 owner=0xf67c7e3e9088c0fe "
            "component=0x77 target=0x1234 value=0x5 seen=42");
}

TEST(MenuPressLine, AnUnresolvedSenderSaysWhyAndKeepsWhatWasRead) {
  Press e;
  e.symbol = 0xabcd;
  e.component = 0x99;
  EXPECT_EQ(nevr_menu_press::PressLine(e, Sender::NoSender, 7),
            "[NEVR.PLUGINMGR] component event read=no_sender event=0xabcd owner=0x0 component=0x99 target=0x0 value=0x0 "
            "seen=7");
}

TEST(MenuPressLine, EverySenderOutcomeHasItsOwnName) {
  std::string names;
  for (Sender s : {Sender::Unreadable, Sender::NoSender, Sender::NoTable, Sender::OutOfRange, Sender::Resolved}) {
    const std::string name = nevr_menu_press::SenderName(s);
    EXPECT_NE(name, "unknown");
    EXPECT_EQ(names.find("," + name + ","), std::string::npos) << name << " is named twice";
    names += "," + name + ",";
  }
}

}  // namespace
