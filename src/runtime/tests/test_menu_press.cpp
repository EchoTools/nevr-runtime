// #1857, docs/standards/logging.md ([NEVR.PLUGINMGR]): what a main-menu card press looks like as a component event
// (patch/menu_press.h), as pure logic: the cheap head read (symbol, then the component of a card click only) and the
// sender walk that resolves the owner. No game and no hook: the game space below is a few buffers in this process.

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
// The reads one call of ReadHead may make, by what it found.
constexpr int kReadsForANonCardSymbol = 1;  // the symbol alone
constexpr int kReadsForACardClick = 2;      // the symbol and the component

// A game space, its actor table and an event, as buffers in this process, plus a reader that serves only what was
// registered. `refuse` makes one address unreadable, the way a freed page is. `reads` counts every read, `outside`
// the reads that left the event (each one costs the hook a VirtualQuery).
struct FakeGame {
  alignas(kBufferAlign) uint8_t event[nevr_invite_choices::kEventBytes] = {};
  alignas(kBufferAlign) uint8_t space[kSpaceBytes] = {};
  alignas(kBufferAlign) uint8_t table[kTableBytes] = {};
  alignas(kBufferAlign) uint64_t names[kNameSlots] = {};
  uintptr_t refuse = 0;
  mutable int reads = 0;
  mutable int outside = 0;

  FakeGame() {
    Put(space, nevr_invite_choices::kSpaceActorTableOffset, reinterpret_cast<uintptr_t>(table));
    Put(table, nevr_invite_choices::kTableNamesOffset, reinterpret_cast<uintptr_t>(names));
    Put(table, nevr_invite_choices::kTableCapacityOffset, kNameSlots);
    SetEvent(EchoVR::CSymbol64Hash("evt_something_else"), kCardSlot, kComponentFirst);
    names[kCardSlot] = kNewCardOwner;
  }
  static void Put(uint8_t* base, size_t at, uint64_t v) { std::memcpy(base + at, &v, sizeof(v)); }
  void SetEvent(uint64_t symbol, uint64_t sender, uint64_t component) {
    Put(event, nevr_invite_choices::kEventSymbolOffset, symbol);
    Put(event, nevr_invite_choices::kEventSenderOffset, sender);
    Put(event, nevr_invite_choices::kEventComponentOffset, component);
  }
  bool InEvent(uintptr_t address, size_t n) const {
    const uintptr_t from = reinterpret_cast<uintptr_t>(event);
    return address >= from && address + n <= from + sizeof(event);
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
      if (!InEvent(address, n)) ++outside;
      if (address == refuse || !Serves(address, n)) return false;
      std::memcpy(out, reinterpret_cast<const void*>(address), n);
      return true;
    };
  }
  uintptr_t Event() const { return reinterpret_cast<uintptr_t>(event); }
  uintptr_t Space() const { return reinterpret_cast<uintptr_t>(space); }
  bool Head(Press* out) const { return nevr_menu_press::ReadHead(Reader(), Event(), out); }
  Sender Owner(Press* out) const { return nevr_menu_press::ResolveOwner(Reader(), Space(), Event(), out); }
};

// ---- the head: the symbol first, the component only of a card click ----

TEST(MenuPressHead, ANonCardSymbolCostsOneReadAndNothingOutsideTheEvent) {
  FakeGame game;
  Press p;
  ASSERT_TRUE(game.Head(&p));
  EXPECT_EQ(p.symbol, EchoVR::CSymbol64Hash("evt_something_else"));
  EXPECT_EQ(p.component, 0u) << "the component of an event that is not a card click is not read";
  EXPECT_EQ(game.reads, kReadsForANonCardSymbol);
  EXPECT_EQ(game.outside, 0);
}

TEST(MenuPressHead, ACardClickIsReadWithItsComponentAndStillNothingOutsideTheEvent) {
  FakeGame game;
  game.SetEvent(nevr_invite_choices::kEvtCardClicked, kCardSlot, kComponentSecond);
  Press p;
  ASSERT_TRUE(game.Head(&p));
  EXPECT_EQ(p.symbol, nevr_invite_choices::kEvtCardClicked);
  EXPECT_EQ(p.component, kComponentSecond);
  EXPECT_EQ(p.owner, 0u) << "the head never resolves the sender";
  EXPECT_EQ(game.reads, kReadsForACardClick);
  EXPECT_EQ(game.outside, 0);
}

TEST(MenuPressHead, ANullEventOrAnUnreadableWordIsRefused) {
  Press p;
  FakeGame game;
  EXPECT_FALSE(nevr_menu_press::ReadHead(game.Reader(), 0, &p));
  // A null event is refused by the head itself, not only by a reader that happens to fail at address 0.
  int calls = 0;
  const auto anything = [&calls](uintptr_t, void* out, size_t n) {
    ++calls;
    std::memset(out, 0, n);
    return true;
  };
  EXPECT_FALSE(nevr_menu_press::ReadHead(anything, 0, &p));
  EXPECT_EQ(calls, 0);
  FakeGame symbolRefused;
  symbolRefused.refuse = symbolRefused.Event() + nevr_invite_choices::kEventSymbolOffset;
  EXPECT_FALSE(symbolRefused.Head(&p));
  FakeGame componentRefused;
  componentRefused.SetEvent(nevr_invite_choices::kEvtCardClicked, kCardSlot, kComponentFirst);
  componentRefused.refuse = componentRefused.Event() + nevr_invite_choices::kEventComponentOffset;
  EXPECT_FALSE(componentRefused.Head(&p));
}

// ---- resolving the owner of the sender (the walk that costs a page query per step) ----

TEST(MenuPressOwner, TheOwnerAndComponentAreResolved) {
  FakeGame game;
  Press e;
  ASSERT_EQ(game.Owner(&e), Sender::Resolved);
  EXPECT_EQ(e.owner, kNewCardOwner);
  EXPECT_EQ(e.component, kComponentFirst);
}

TEST(MenuPressOwner, TheWalkIsFourReadsOutsideTheEvent) {
  FakeGame game;
  Press e;
  ASSERT_EQ(game.Owner(&e), Sender::Resolved);
  // The game space's table pointer, the table's names pointer and capacity, and the one name: one page query each.
  constexpr int kWalkReads = 4;
  EXPECT_EQ(game.outside, kWalkReads);
}

TEST(MenuPressOwner, ASenderOfFFFFIsNoSenderButTheComponentIsKept) {
  FakeGame game;
  game.SetEvent(kOtherSymbol, kNoSenderWord, kComponentSecond);
  Press e;
  EXPECT_EQ(game.Owner(&e), Sender::NoSender);
  EXPECT_EQ(e.component, kComponentSecond);
  EXPECT_EQ(e.owner, 0u);
  EXPECT_EQ(game.outside, 0) << "no sender, so nothing outside the event is read";
}

TEST(MenuPressOwner, OnlyTheLow16BitsOfTheSenderIndexTheTable) {
  FakeGame game;
  game.SetEvent(kOtherSymbol, kSenderHighBits | kCardSlot, kComponentSecond);
  Press e;
  ASSERT_EQ(game.Owner(&e), Sender::Resolved);
  EXPECT_EQ(e.owner, kNewCardOwner);
}

TEST(MenuPressOwner, AnIndexAtOrPastTheCapacityIsOutOfRange) {
  FakeGame game;
  Press e;
  game.SetEvent(kOtherSymbol, kNameSlots, kComponentSecond);
  EXPECT_EQ(game.Owner(&e), Sender::OutOfRange);
  EXPECT_EQ(e.owner, 0u);
  game.SetEvent(kOtherSymbol, kLastSlot, kComponentSecond);
  game.names[kLastSlot] = kLastSlotOwner;
  EXPECT_EQ(game.Owner(&e), Sender::Resolved);
  EXPECT_EQ(e.owner, kLastSlotOwner);
}

TEST(MenuPressOwner, AMissingTableOrNamesArrayIsRefusedNotFollowed) {
  Press e;
  FakeGame game;
  FakeGame::Put(game.space, nevr_invite_choices::kSpaceActorTableOffset, 0);
  EXPECT_EQ(game.Owner(&e), Sender::NoTable);
  FakeGame again;
  FakeGame::Put(again.table, nevr_invite_choices::kTableNamesOffset, 0);
  EXPECT_EQ(again.Owner(&e), Sender::NoTable);
}

TEST(MenuPressOwner, MemoryThatCannotBeReadIsRefusedAtEveryStep) {
  Press e;
  const size_t offsets[] = {nevr_invite_choices::kEventSenderOffset, nevr_invite_choices::kEventComponentOffset};
  for (size_t offset : offsets) {
    FakeGame game;
    game.refuse = game.Event() + offset;
    EXPECT_EQ(game.Owner(&e), Sender::Unreadable) << "event+" << offset;
  }
  {
    FakeGame game;
    game.refuse = game.Space() + nevr_invite_choices::kSpaceActorTableOffset;
    EXPECT_EQ(game.Owner(&e), Sender::Unreadable);
  }
  {
    FakeGame game;
    game.refuse = reinterpret_cast<uintptr_t>(game.table) + nevr_invite_choices::kTableNamesOffset;
    EXPECT_EQ(game.Owner(&e), Sender::Unreadable);
  }
  {
    FakeGame game;
    game.refuse = reinterpret_cast<uintptr_t>(game.table) + nevr_invite_choices::kTableCapacityOffset;
    EXPECT_EQ(game.Owner(&e), Sender::Unreadable);
  }
  {
    FakeGame game;
    game.refuse = reinterpret_cast<uintptr_t>(game.names) + kCardSlot * sizeof(uint64_t);
    EXPECT_EQ(game.Owner(&e), Sender::Unreadable);
  }
}

TEST(MenuPressOwner, ANullSpaceIsRefused) {
  FakeGame game;
  Press e;
  EXPECT_EQ(nevr_menu_press::ResolveOwner(game.Reader(), 0, game.Event(), &e), Sender::Unreadable);
}

TEST(MenuPressOwner, EverySenderOutcomeHasItsOwnName) {
  std::string names;
  for (Sender s : {Sender::Unreadable, Sender::NoSender, Sender::NoTable, Sender::OutOfRange, Sender::Resolved}) {
    const std::string name = nevr_menu_press::SenderName(s);
    EXPECT_NE(name, "unknown");
    EXPECT_EQ(names.find("," + name + ","), std::string::npos) << name << " is named twice";
    names += "," + name + ",";
  }
}

}  // namespace
