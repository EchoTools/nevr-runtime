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
constexpr size_t kBufferAlign = alignof(uint64_t);
constexpr size_t kSpaceBytes = 0x400;
constexpr size_t kTableBytes = 0x100;
constexpr size_t kNameSlots = 16;
constexpr size_t kLastSlot = kNameSlots - 1;
constexpr uint64_t kCardSlot = 5;
constexpr uint64_t kComponentFirst = 0x77;
constexpr uint64_t kComponentSecond = 0x99;
constexpr uint64_t kOtherSymbol = 0xabcdULL;
constexpr uint64_t kNoSenderWord = 0x1234ffffULL;
constexpr uint64_t kSenderHighBits = 0xabcd0000ULL;
constexpr uint64_t kLastSlotOwner = 0x1111222233334444ULL;
constexpr uint64_t kTargetWord = 0x1111;
constexpr uint64_t kValueWord = 0x2222;
constexpr uint64_t kExpectedTargetOffset = 0x08;
constexpr uint64_t kExpectedValueOffset = 0x20;
constexpr uint64_t kLineSymbol = 0x1122334455667788ULL;
constexpr uint64_t kLineTarget = 0x1234;
constexpr uint64_t kLineValue = 0x5;
constexpr uint64_t kSeenFirst = 42;
constexpr uint64_t kSeenSecond = 7;

// A game space, its actor table and an event, as buffers in this process, plus a reader that serves only what was
// registered. `refuse` makes one address unreadable, the way a freed page is.
struct FakeGame {
  alignas(kBufferAlign) uint8_t event[nevr_invite_choices::kEventBytes] = {};
  alignas(kBufferAlign) uint8_t space[kSpaceBytes] = {};
  alignas(kBufferAlign) uint8_t table[kTableBytes] = {};
  alignas(kBufferAlign) uint64_t names[kNameSlots] = {};
  uintptr_t refuse = 0;
  mutable int reads = 0;

  FakeGame() {
    Put(space, nevr_invite_choices::kSpaceActorTableOffset, reinterpret_cast<uintptr_t>(table));
    Put(table, nevr_invite_choices::kTableNamesOffset, reinterpret_cast<uintptr_t>(names));
    Put(table, nevr_invite_choices::kTableCapacityOffset, kNameSlots);
    SetEvent(EchoVR::CSymbol64Hash("evt_something_else"), kCardSlot, kComponentFirst);
    names[kCardSlot] = kNewCardOwner;
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
  EXPECT_EQ(e.component, kComponentFirst);
}

TEST(MenuPressRead, TheWizardsOwnSymbolIsReadTheSameWay) {
  FakeGame game;
  game.SetEvent(nevr_invite_choices::kEvtCardClicked, kCardSlot, nevr_invite_choices::kCardLeft);
  Press e;
  ASSERT_EQ(game.Run(&e), Sender::Resolved);
  EXPECT_EQ(e.symbol, nevr_invite_choices::kEvtCardClicked);
  EXPECT_EQ(e.owner, kNewCardOwner);
}

TEST(MenuPressRead, ASenderOfFFFFIsNoSenderButTheSymbolAndComponentAreKept) {
  FakeGame game;
  game.SetEvent(kOtherSymbol, kNoSenderWord, kComponentSecond);
  Press e;
  EXPECT_EQ(game.Run(&e), Sender::NoSender);
  EXPECT_EQ(e.symbol, kOtherSymbol);
  EXPECT_EQ(e.component, kComponentSecond);
  EXPECT_EQ(e.owner, 0u);
}

TEST(MenuPressRead, OnlyTheLow16BitsOfTheSenderIndexTheTable) {
  FakeGame game;
  game.SetEvent(kOtherSymbol, kSenderHighBits | kCardSlot, kComponentSecond);
  Press e;
  ASSERT_EQ(game.Run(&e), Sender::Resolved);
  EXPECT_EQ(e.owner, kNewCardOwner);
}

TEST(MenuPressRead, AnIndexAtOrPastTheCapacityIsOutOfRange) {
  FakeGame game;
  Press e;
  game.SetEvent(kOtherSymbol, kNameSlots, kComponentSecond);
  EXPECT_EQ(game.Run(&e), Sender::OutOfRange);
  EXPECT_EQ(e.owner, 0u);
  game.SetEvent(kOtherSymbol, kLastSlot, kComponentSecond);
  game.names[kLastSlot] = kLastSlotOwner;
  EXPECT_EQ(game.Run(&e), Sender::Resolved);
  EXPECT_EQ(e.owner, kLastSlotOwner);
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
    game.refuse = reinterpret_cast<uintptr_t>(game.names) + kCardSlot * sizeof(uint64_t);
    EXPECT_EQ(game.Run(&e), Sender::Unreadable);
  }
}

TEST(MenuPressRead, TheTargetAndTheValueWordsAreReadWithTheRest) {
  FakeGame game;
  game.SetEvent(kOtherSymbol, kCardSlot, kComponentSecond);
  game.SetWords(kTargetWord, kValueWord);
  Press e;
  ASSERT_EQ(game.Run(&e), Sender::Resolved);
  EXPECT_EQ(e.target, kTargetWord);
  EXPECT_EQ(e.value, kValueWord);
  EXPECT_EQ(e.component, kComponentSecond);
}

TEST(MenuPressRead, TheTargetAndTheValueAreKeptWhenTheSenderDoesNotResolve) {
  FakeGame game;
  game.SetEvent(kOtherSymbol, kNoSenderWord, kComponentSecond);
  game.SetWords(kTargetWord, kValueWord);
  Press e;
  EXPECT_EQ(game.Run(&e), Sender::NoSender);
  EXPECT_EQ(e.target, kTargetWord);
  EXPECT_EQ(e.value, kValueWord);
}

TEST(MenuPressRead, TheWordOffsetsAreTheOnesTheGameReads) {
  // SComponentEvent (echovr.exe 0x14050fb60): word 1 is the target space id, word 4 the value.
  EXPECT_EQ(nevr_menu_press::kEventTargetOffset, kExpectedTargetOffset);
  EXPECT_EQ(nevr_menu_press::kEventValueOffset, kExpectedValueOffset);
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
  e.symbol = kLineSymbol;
  e.owner = kNewCardOwner;
  e.component = kComponentFirst;
  e.target = kLineTarget;
  e.value = kLineValue;
  EXPECT_EQ(nevr_menu_press::PressLine(e, Sender::Resolved, kSeenFirst),
            "[NEVR.PLUGINMGR] component event read=resolved event=0x1122334455667788 owner=0xf67c7e3e9088c0fe "
            "component=0x77 target=0x1234 value=0x5 seen=42");
}

TEST(MenuPressLine, AnUnresolvedSenderSaysWhyAndKeepsWhatWasRead) {
  Press e;
  e.symbol = kOtherSymbol;
  e.component = kComponentSecond;
  EXPECT_EQ(nevr_menu_press::PressLine(e, Sender::NoSender, kSeenSecond),
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
