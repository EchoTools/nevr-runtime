// #906, #896, docs/adr/0017: the UI text writer (patch/ui_text.h) as pure logic. A request names an actor of the menu
// gamespace, one of its canvas text elements and the text; the writer finds the actor in whichever gamespace holds it,
// asks the game for the canvas component and sets the text through the game's own setter. No game here: the game space,
// the actor table and the two game calls are a few buffers and functions in this process.

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <functional>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

#include "runtime/patch/ui_text.h"

namespace {

using nevr_ui_text::Check;
using nevr_ui_text::Outcome;
using nevr_ui_text::Pending;
using nevr_ui_text::Report;
using nevr_ui_text::Request;
using nevr_ui_text::Target;

constexpr uint64_t kActor = 0xad97e58a7b13425fULL;
constexpr uint64_t kElement = 0xa8452de53657bd6eULL;

Request Req(const std::string& text, uint64_t actor = kActor, uint64_t element = kElement) {
  return Request{Target{actor, element}, text};
}

// One gamespace: an actor table whose names array holds `names`, as the game lays it out.
struct FakeSpace {
  alignas(8) uint8_t space[0x400] = {};
  alignas(8) uint8_t table[0x100] = {};
  std::vector<uint64_t> names;
  void Names(std::initializer_list<uint64_t> list) {
    names.clear();
    for (const uint64_t n : list) names.push_back(n);
  }

  static void Put(uint8_t* base, size_t at, uint64_t v) { std::memcpy(base + at, &v, sizeof(v)); }
  void Build() {
    Put(space, nevr_ui_text::kSpaceActorTableOffset, reinterpret_cast<uintptr_t>(table));
    Put(table, nevr_ui_text::kTableNamesOffset, reinterpret_cast<uintptr_t>(names.data()));
    Put(table, nevr_ui_text::kTableCountOffset, names.size());
  }
  uintptr_t Address() { return reinterpret_cast<uintptr_t>(space); }
};

// The platform the writer runs on: a game with gamespaces, a canvas finder and a setter that record what they were asked.
struct FakePlatform {
  uintptr_t game = 0x1000;
  std::vector<FakeSpace*> spaces;
  void Use(std::initializer_list<FakeSpace*> list) {
    spaces.clear();
    for (FakeSpace* s : list) spaces.push_back(s);
  }
  uintptr_t refuse = 0;                  // an address the reader cannot read
  bool canvas = true;                    // FindCanvas succeeds
  uintptr_t canvasAddress = 0xc0ffee;    // what it hands back (0: a finder that says yes and gives nothing)
  bool setter = true;                    // SetText reports the element exists
  int gameSpaceCalls = 0;
  int findCalls = 0;
  int setCalls = 0;
  uint64_t lastIdent = 0;
  uintptr_t lastFindSpace = 0;
  uint64_t lastElement = 0;
  std::string lastText;
  std::function<void()> onFind;          // runs inside FindCanvas: a Post from another thread's point of view

  uintptr_t Game() { return game; }
  uintptr_t GameSpace(uintptr_t g, uint64_t index) {
    ++gameSpaceCalls;
    if (g != game || index >= spaces.size()) return 0;
    return spaces[index]->Address();
  }
  bool Read(uintptr_t address, void* out, size_t n) {
    if (address == refuse) return false;
    std::memcpy(out, reinterpret_cast<const void*>(address), n);
    return true;
  }
  bool FindActor(uintptr_t gs, uint64_t actor, uint64_t* index) { return nevr_ui_text::FindActorIndex(*this, gs, actor, index); }
  std::string ActorTableProbe(uintptr_t) { return " table=0x0"; }
  std::string FirstWriteLine() { return "[NEVR.UI] text first write thread=1 game_loop_thread=1"; }
  bool FindCanvas(uintptr_t gs, uint64_t ident, uintptr_t* cs, uint32_t* handle) {
    ++findCalls;
    lastFindSpace = gs;
    lastIdent = ident;
    if (onFind) onFind();
    if (!canvas) return false;
    *cs = canvasAddress;
    *handle = 7;
    return true;
  }
  bool SetText(uintptr_t cs, uint32_t handle, uint64_t element, const char* text) {
    ++setCalls;
    EXPECT_EQ(cs, 0xc0ffeeu);
    EXPECT_EQ(handle, 7u);
    lastElement = element;
    lastText = text;
    return setter;
  }
};

// ---- what may be written ----

TEST(UiText, ARequestNeedsAnActorAndAnElementAndShortPrintableText) {
  EXPECT_EQ(nevr_ui_text::CheckRequest(Req("ENTER CODE")), Check::Ok);
  EXPECT_EQ(nevr_ui_text::CheckRequest(Req("")), Check::Ok) << "an empty text clears the element";
  EXPECT_EQ(nevr_ui_text::CheckRequest(Req("x", 0, kElement)), Check::NoTarget);
  EXPECT_EQ(nevr_ui_text::CheckRequest(Req("x", kActor, 0)), Check::NoTarget);
  EXPECT_EQ(nevr_ui_text::CheckRequest(Req(std::string(nevr_ui_text::kMaxTextBytes, 'A'))), Check::Ok);
  EXPECT_EQ(nevr_ui_text::CheckRequest(Req(std::string(nevr_ui_text::kMaxTextBytes + 1, 'A'))), Check::TooLong);
  EXPECT_EQ(nevr_ui_text::CheckRequest(Req("line\nbreak")), Check::NotPrintable);
  EXPECT_EQ(nevr_ui_text::CheckRequest(Req(std::string("a\0b", 3))), Check::NotPrintable);
  EXPECT_EQ(nevr_ui_text::CheckRequest(Req("caf\xc3\xa9")), Check::NotPrintable);
  EXPECT_EQ(nevr_ui_text::CheckRequest(Req("\x7f")), Check::NotPrintable);
  EXPECT_EQ(nevr_ui_text::CheckRequest(Req(" ~")), Check::Ok);
  EXPECT_EQ(nevr_ui_text::CheckRequest(Req("\x1f")), Check::NotPrintable) << "the last control character";
}

// ---- finding the actor ----

TEST(UiTextActor, TheActorIsFoundByItsIndexInTheNamesArray) {
  FakeSpace gs;
  gs.Names({1, 2, kActor, 4});
  gs.Build();
  FakePlatform p;
  uint64_t index = 99;
  ASSERT_TRUE(nevr_ui_text::FindActorIndex(p, gs.Address(), kActor, &index));
  EXPECT_EQ(index, 2u);
  EXPECT_FALSE(nevr_ui_text::FindActorIndex(p, gs.Address(), 0x1234, &index));
}

TEST(UiTextActor, ANullTableOrNamesArrayOrAnImplausibleCountFindsNothing) {
  FakePlatform p;
  uint64_t index = 0;
  {
    FakeSpace gs;
    gs.Names({kActor});
    gs.Build();
    FakeSpace::Put(gs.space, nevr_ui_text::kSpaceActorTableOffset, 0);
    EXPECT_FALSE(nevr_ui_text::FindActorIndex(p, gs.Address(), kActor, &index));
  }
  {
    FakeSpace gs;
    gs.Names({kActor});
    gs.Build();
    FakeSpace::Put(gs.table, nevr_ui_text::kTableNamesOffset, 0);
    EXPECT_FALSE(nevr_ui_text::FindActorIndex(p, gs.Address(), kActor, &index));
  }
  {
    FakeSpace gs;
    gs.Names({kActor});
    gs.Build();
    FakeSpace::Put(gs.table, nevr_ui_text::kTableCountOffset, 0xffff);
    EXPECT_FALSE(nevr_ui_text::FindActorIndex(p, gs.Address(), kActor, &index)) << "0xffff is the none index";
  }
  {
    FakeSpace gs;
    gs.Build();
    EXPECT_FALSE(nevr_ui_text::FindActorIndex(p, gs.Address(), kActor, &index)) << "no actors";
  }
}

TEST(UiTextActor, MemoryThatCannotBeReadIsRefusedAtEveryStep) {
  uint64_t index = 0;
  for (int step = 0; step < 4; ++step) {
    FakeSpace gs;
    gs.Names({1, kActor});
    gs.Build();
    FakePlatform p;
    p.refuse = step == 0   ? gs.Address() + nevr_ui_text::kSpaceActorTableOffset
               : step == 1 ? reinterpret_cast<uintptr_t>(gs.table) + nevr_ui_text::kTableNamesOffset
               : step == 2 ? reinterpret_cast<uintptr_t>(gs.table) + nevr_ui_text::kTableCountOffset
                           : reinterpret_cast<uintptr_t>(gs.names.data());
    EXPECT_FALSE(nevr_ui_text::FindActorIndex(p, gs.Address(), kActor, &index)) << "step " << step;
  }
}

// ---- one write ----

TEST(UiTextApply, TheTextIsSetOnTheActorsCanvasElementThroughTheGamesSetter) {
  FakeSpace gs;
  gs.Names({1, 2, 3, kActor});
  gs.Build();
  FakePlatform p;
  p.Use({&gs});
  EXPECT_EQ(nevr_ui_text::Apply(p, Req("HELLO")), Outcome::Written);
  EXPECT_EQ(p.lastFindSpace, gs.Address());
  EXPECT_EQ(p.lastIdent, 0x0000ffffffff0000ULL | 3u) << "the actor's index in the packed identity";
  EXPECT_EQ(p.lastElement, kElement);
  EXPECT_EQ(p.lastText, "HELLO");
  EXPECT_EQ(p.setCalls, 1);
}

TEST(UiTextApply, ItWalksTheGamespacesInOrderAndStopsWhereTheActorIs) {
  FakeSpace first, second, third;
  first.Names({1, 2});
  second.Names({5, kActor});
  third.Names({kActor});
  for (FakeSpace* s : {&first, &second, &third}) s->Build();
  FakePlatform p;
  p.Use({&first, &second, &third});
  EXPECT_EQ(nevr_ui_text::Apply(p, Req("X")), Outcome::Written);
  EXPECT_EQ(p.lastFindSpace, second.Address());
  EXPECT_EQ(p.lastIdent, 0x0000ffffffff0000ULL | 1u);
  EXPECT_EQ(p.gameSpaceCalls, 2) << "the third gamespace is never asked for";
}

TEST(UiTextApply, NoGameIsNoGameAndAnActorInNoGamespaceIsNoActor) {
  FakePlatform none;
  none.game = 0;
  EXPECT_EQ(nevr_ui_text::Apply(none, Req("X")), Outcome::NoGame);
  EXPECT_EQ(none.gameSpaceCalls, 0);

  FakeSpace gs;
  gs.Names({1, 2});
  gs.Build();
  FakePlatform p;
  p.Use({&gs});
  EXPECT_EQ(nevr_ui_text::Apply(p, Req("X")), Outcome::NoActor);
  EXPECT_EQ(p.findCalls, 0);
  EXPECT_EQ(p.setCalls, 0);
  EXPECT_EQ(p.gameSpaceCalls, 2) << "the walk stops at the first index the game has no gamespace for";
}

TEST(UiTextApply, ACanvasThatIsNotThereYetIsNoCanvasAndASetterThatRefusesIsNotWritten) {
  FakeSpace gs;
  gs.Names({kActor});
  gs.Build();
  FakePlatform p;
  p.Use({&gs});
  p.canvas = false;
  EXPECT_EQ(nevr_ui_text::Apply(p, Req("X")), Outcome::NoCanvas);
  EXPECT_EQ(p.setCalls, 0);
  p.canvas = true;
  p.canvasAddress = 0;
  EXPECT_EQ(nevr_ui_text::Apply(p, Req("X")), Outcome::NoCanvas) << "a finder that reports success with no canvas is no canvas";
  EXPECT_EQ(p.setCalls, 0);
  p.canvasAddress = 0xc0ffee;
  p.setter = false;
  EXPECT_EQ(nevr_ui_text::Apply(p, Req("X")), Outcome::NotWritten);
  EXPECT_EQ(p.setCalls, 1);
}

TEST(UiTextApply, OnlyTheFirstSixtyFourGamespacesAreAsked) {
  FakeSpace gs;
  gs.Names({1});
  gs.Build();
  FakePlatform p;
  p.spaces.assign(200, &gs);
  EXPECT_EQ(nevr_ui_text::Apply(p, Req("X")), Outcome::NoActor);
  EXPECT_EQ(p.gameSpaceCalls, static_cast<int>(nevr_ui_text::kMaxGameSpaces));
}

TEST(UiTextApply, ARequestThatFailsTheCheckIsNeverSentToTheGame) {
  FakeSpace gs;
  gs.Names({kActor});
  gs.Build();
  FakePlatform p;
  p.Use({&gs});
  EXPECT_EQ(nevr_ui_text::Apply(p, Req("bad\n")), Outcome::Refused);
  EXPECT_EQ(p.gameSpaceCalls, 0);
  EXPECT_EQ(p.setCalls, 0);
}

// ---- the pending requests ----

TEST(UiTextPending, APostedRequestIsWrittenOnTheNextDrainAndLeavesTheList) {
  FakeSpace gs;
  gs.Names({kActor});
  gs.Build();
  FakePlatform p;
  p.Use({&gs});
  Pending pending;
  ASSERT_EQ(pending.Post(Req("ONE"), 1000), Check::Ok);
  EXPECT_EQ(pending.Size(), 1u);
  const std::vector<Report> reports = pending.Drain(p, 1000);
  ASSERT_EQ(reports.size(), 1u);
  EXPECT_EQ(reports[0].outcome, Outcome::Written);
  EXPECT_TRUE(reports[0].done);
  EXPECT_FALSE(reports[0].dropped);
  EXPECT_EQ(reports[0].attempts, 1);
  EXPECT_EQ(pending.Size(), 0u);
  EXPECT_TRUE(pending.Drain(p, 1001).empty());
}

TEST(UiTextPending, AnEmptyListCostsNoCallIntoTheGame) {
  FakePlatform p;
  Pending pending;
  EXPECT_TRUE(pending.Drain(p, 5).empty());
  EXPECT_EQ(p.gameSpaceCalls, 0);
}

TEST(UiTextPending, ARequestThatFailsTheCheckIsRefusedAtPostAndNotKept) {
  Pending pending;
  EXPECT_EQ(pending.Post(Req("bad\n"), 0), Check::NotPrintable);
  EXPECT_EQ(pending.Post(Req("x", 0, 0), 0), Check::NoTarget);
  EXPECT_EQ(pending.Size(), 0u);
}

TEST(UiTextPending, ANewTextForTheSameTargetReplacesTheOldOne) {
  FakeSpace gs;
  gs.Names({kActor});
  gs.Build();
  FakePlatform p;
  p.Use({&gs});
  Pending pending;
  ASSERT_EQ(pending.Post(Req("OLD"), 0), Check::Ok);
  ASSERT_EQ(pending.Post(Req("NEW"), 0), Check::Ok);
  EXPECT_EQ(pending.Size(), 1u);
  pending.Drain(p, 0);
  EXPECT_EQ(p.lastText, "NEW");
  EXPECT_EQ(p.setCalls, 1);
}

TEST(UiTextPending, AtMostSixteenTargetsWaitAtOnce) {
  Pending pending;
  for (uint64_t i = 1; i <= nevr_ui_text::kMaxPending; ++i) {
    ASSERT_EQ(pending.Post(Req("x", kActor, i), 0), Check::Ok) << i;
  }
  EXPECT_EQ(pending.Post(Req("x", kActor, 1000), 0), Check::Full);
  EXPECT_EQ(pending.Post(Req("y", kActor, 3), 0), Check::Ok) << "a replacement needs no room";
  EXPECT_EQ(pending.Size(), nevr_ui_text::kMaxPending);
}

TEST(UiTextPending, AWriteThatCannotYetBeMadeIsTriedAgainAfterTheRetryInterval) {
  FakeSpace gs;
  gs.Names({kActor});
  gs.Build();
  FakePlatform p;
  p.Use({&gs});
  p.canvas = false;
  Pending pending;
  ASSERT_EQ(pending.Post(Req("LATE"), 1000), Check::Ok);

  std::vector<Report> r = pending.Drain(p, 1000);
  ASSERT_EQ(r.size(), 1u);
  EXPECT_EQ(r[0].outcome, Outcome::NoCanvas);
  EXPECT_FALSE(r[0].done);
  EXPECT_EQ(r[0].attempts, 1);
  EXPECT_EQ(pending.Size(), 1u);

  EXPECT_TRUE(pending.Drain(p, 1000 + nevr_ui_text::kRetryIntervalMs - 1).empty()) << "not yet";
  EXPECT_EQ(p.findCalls, 1);

  p.canvas = true;
  r = pending.Drain(p, 1000 + nevr_ui_text::kRetryIntervalMs);
  ASSERT_EQ(r.size(), 1u);
  EXPECT_EQ(r[0].outcome, Outcome::Written);
  EXPECT_EQ(r[0].attempts, 2);
  EXPECT_TRUE(r[0].done);
  EXPECT_EQ(pending.Size(), 0u);
}

TEST(UiTextPending, ARequestThatNeverCanBeWrittenIsDroppedAfterTheGiveUpTimeAndSaysWhy) {
  FakePlatform p;
  Pending pending;
  ASSERT_EQ(pending.Post(Req("NEVER"), 0), Check::Ok);
  uint64_t now = 0;
  std::vector<Report> last;
  while (now <= nevr_ui_text::kGiveUpMs + nevr_ui_text::kRetryIntervalMs) {
    std::vector<Report> r = pending.Drain(p, now);
    if (!r.empty()) last = r;
    if (!last.empty() && last[0].dropped) break;
    now += nevr_ui_text::kRetryIntervalMs;
  }
  ASSERT_EQ(last.size(), 1u);
  EXPECT_TRUE(last[0].dropped);
  EXPECT_TRUE(last[0].done);
  EXPECT_EQ(last[0].outcome, Outcome::NoActor);
  EXPECT_GE(now, nevr_ui_text::kGiveUpMs);
  EXPECT_EQ(pending.Size(), 0u);
}

// The give-up time is reached, not passed: a request posted at 0 is dropped by the drain made exactly kGiveUpMs later,
// and still waits at the drain one millisecond before.
TEST(UiTextPending, TheGiveUpTimeIsReachedNotPassed) {
  FakePlatform p;
  Pending pending;
  ASSERT_EQ(pending.Post(Req("X"), 0), Check::Ok);
  std::vector<Report> r = pending.Drain(p, nevr_ui_text::kGiveUpMs - 1);
  ASSERT_EQ(r.size(), 1u);
  EXPECT_FALSE(r[0].dropped);
  r = pending.Drain(p, nevr_ui_text::kGiveUpMs - 1 + nevr_ui_text::kRetryIntervalMs);
  ASSERT_EQ(r.size(), 1u);
  EXPECT_TRUE(r[0].dropped);

  Pending exact;
  ASSERT_EQ(exact.Post(Req("X"), 0), Check::Ok);
  r = exact.Drain(p, nevr_ui_text::kGiveUpMs);
  ASSERT_EQ(r.size(), 1u);
  EXPECT_TRUE(r[0].dropped) << "a drain exactly kGiveUpMs after the post gives up";
}

TEST(UiTextPending, ANewerPostMadeWhileADrainIsInTheGameWinsOverTheRetryOfTheOldOne) {
  FakeSpace gs;
  gs.Names({kActor});
  gs.Build();
  FakePlatform p;
  p.Use({&gs});
  p.canvas = false;
  Pending pending;
  ASSERT_EQ(pending.Post(Req("OLD"), 0), Check::Ok);
  p.onFind = [&pending] { pending.Post(Req("NEWER"), 0); };  // the worker posts while the game thread is in the game
  pending.Drain(p, 0);
  EXPECT_EQ(pending.Size(), 1u) << "the retry of OLD did not duplicate the target";
  p.onFind = nullptr;
  p.canvas = true;
  pending.Drain(p, nevr_ui_text::kRetryIntervalMs);
  EXPECT_EQ(p.lastText, "NEWER");
  EXPECT_EQ(p.setCalls, 1);
}

// ---- the log line ----

TEST(UiTextLine, TheLineNamesTheTargetTheSizeTheAttemptsAndNeverTheText) {
  Report written{Req("SECRET-CODE"), Outcome::Written, 2, true, false};
  const std::string line = nevr_ui_text::ReportLine(written);
  EXPECT_EQ(line,
            "[NEVR.UI] text written actor=0xad97e58a7b13425f element=0xa8452de53657bd6e bytes=11 attempts=2");
  EXPECT_EQ(line.find("SECRET"), std::string::npos);
  Report dropped{Req("SECRET-CODE"), Outcome::NoCanvas, 300, true, true};
  const std::string drop = nevr_ui_text::ReportLine(dropped);
  EXPECT_EQ(drop,
            "[NEVR.UI] text dropped actor=0xad97e58a7b13425f element=0xa8452de53657bd6e bytes=11 attempts=300 "
            "reason=no_canvas");
  EXPECT_EQ(drop.find("SECRET"), std::string::npos);
}

TEST(UiTextLine, EveryOutcomeAndCheckHasAName) {
  std::vector<std::string> names;
  for (Outcome o : {Outcome::Written, Outcome::NoGame, Outcome::NoActor, Outcome::NoCanvas, Outcome::NotWritten,
                    Outcome::Refused}) {
    names.emplace_back(nevr_ui_text::OutcomeName(o));
    EXPECT_NE(names.back(), "");
  }
  std::sort(names.begin(), names.end());
  EXPECT_EQ(std::adjacent_find(names.begin(), names.end()), names.end());
  names.clear();
  for (Check c : {Check::Ok, Check::NoTarget, Check::TooLong, Check::NotPrintable, Check::Full}) {
    names.emplace_back(nevr_ui_text::CheckName(c));
    EXPECT_NE(names.back(), "");
  }
  std::sort(names.begin(), names.end());
  EXPECT_EQ(std::adjacent_find(names.begin(), names.end()), names.end());
}

}  // namespace

// ---- the Quest actor map (libr15 CncaGameSpace::LocalActor) ----

// One Quest gamespace: [gs+0x370] -> table; [table+0x00] -> entries sorted by their signed 64-bit name, 16 bytes each, the
// actor index as a u16 at +8; [table+0x30] -> how many.
struct FakeQuestSpace {
  alignas(8) uint8_t space[0x400] = {};
  alignas(8) uint8_t table[0x100] = {};
  std::vector<uint8_t> entries;
  // Entries must be given sorted by signed name, as the game keeps them.
  void Set(std::initializer_list<std::pair<uint64_t, uint16_t>> list) {
    // The offsets are the game's, written out (libr15 CncaGameSpace::LocalActor): not the constants under test.
    entries.assign(list.size() * 16, 0);
    size_t at = 0;
    for (const auto& [name, index] : list) {
      std::memcpy(entries.data() + at, &name, sizeof(name));
      std::memcpy(entries.data() + at + 8, &index, sizeof(index));
      at += 16;
    }
    FakeSpace::Put(space, 0x370, reinterpret_cast<uintptr_t>(table));
    FakeSpace::Put(table, 0x00, reinterpret_cast<uintptr_t>(entries.data()));
    FakeSpace::Put(table, 0x30, list.size());
  }
  uintptr_t Address() { return reinterpret_cast<uintptr_t>(space); }
};

TEST(UiTextQuestActor, TheActorIsFoundBySigningTheNamesLikeTheGamesBinarySearch) {
  // 0xa9bd... and 0xad97... are negative as signed 64-bit, 0x0ec8... is positive: the game's order is signed.
  const uint64_t a = 0xa9bd0cb4e6f2004cULL, b = 0xad97e58a7b13425fULL, c = 0x0ec8e16986ef88c8ULL;
  ASSERT_LT(static_cast<int64_t>(a), static_cast<int64_t>(b));
  ASSERT_LT(static_cast<int64_t>(b), static_cast<int64_t>(c));
  FakeQuestSpace gs;
  gs.Set({{a, 3}, {b, 9}, {c, 21}});
  FakePlatform p;
  uint64_t index = 0;
  EXPECT_TRUE(nevr_ui_text::FindActorInSortedMap(p, gs.Address(), a, &index));
  EXPECT_EQ(index, 3u);
  EXPECT_TRUE(nevr_ui_text::FindActorInSortedMap(p, gs.Address(), b, &index));
  EXPECT_EQ(index, 9u);
  EXPECT_TRUE(nevr_ui_text::FindActorInSortedMap(p, gs.Address(), c, &index));
  EXPECT_EQ(index, 21u);
  EXPECT_FALSE(nevr_ui_text::FindActorInSortedMap(p, gs.Address(), 0x1234, &index));
}

TEST(UiTextQuestActor, ARefusedReadTheNoneIndexAndAnEmptyMapAreNotFound) {
  FakeQuestSpace gs;
  gs.Set({{0x10, 1}, {0x20, 0xffff}});
  FakePlatform p;
  uint64_t index = 0;
  EXPECT_TRUE(nevr_ui_text::FindActorInSortedMap(p, gs.Address(), 0x10, &index));
  EXPECT_FALSE(nevr_ui_text::FindActorInSortedMap(p, gs.Address(), 0x20, &index)) << "0xffff is the none index";
  p.refuse = reinterpret_cast<uintptr_t>(gs.entries.data());
  EXPECT_FALSE(nevr_ui_text::FindActorInSortedMap(p, gs.Address(), 0x10, &index)) << "the second probe of the binary search";
  p.refuse = gs.Address() + 0x370;
  EXPECT_FALSE(nevr_ui_text::FindActorInSortedMap(p, gs.Address(), 0x10, &index));
  FakeQuestSpace none;
  none.Set({});
  p.refuse = 0;
  EXPECT_FALSE(nevr_ui_text::FindActorInSortedMap(p, none.Address(), 0x10, &index));
}

// ---- one frame of the writer ----

struct Lines {
  static std::vector<std::pair<nevr_ui_text::LogLevel, std::string>>& All() {
    static std::vector<std::pair<nevr_ui_text::LogLevel, std::string>> lines;
    return lines;
  }
  static void Log(nevr_ui_text::LogLevel level, const std::string& line) { All().emplace_back(level, line); }
  static bool Has(const std::string& needle) {
    for (const auto& [level, line] : All()) {
      if (line.find(needle) != std::string::npos) return true;
    }
    return false;
  }
};

TEST(UiTextFrame, AWrittenRequestIsLoggedByTargetAndSizeAndTheFirstWriteOnce) {
  Lines::All().clear();
  FakeSpace gs;
  gs.Names({kActor});
  gs.Build();
  FakePlatform p;
  p.Use({&gs});
  Pending pending;
  std::atomic<bool> first{false};
  ASSERT_EQ(pending.Post(Req("<coming soon>"), 0), Check::Ok);
  nevr_ui_text::RunFrame(pending, p, 0, first, &Lines::Log);
  ASSERT_EQ(Lines::All().size(), 2u);
  EXPECT_EQ(Lines::All()[0].second,
            "[NEVR.UI] text written actor=0xad97e58a7b13425f element=0xa8452de53657bd6e bytes=13 attempts=1");
  EXPECT_EQ(Lines::All()[0].first, nevr_ui_text::LogLevel::Info);
  EXPECT_EQ(Lines::All()[1].second, "[NEVR.UI] text first write thread=1 game_loop_thread=1");
  ASSERT_EQ(pending.Post(Req("again"), 1), Check::Ok);
  nevr_ui_text::RunFrame(pending, p, 1, first, &Lines::Log);
  EXPECT_EQ(Lines::All().size(), 3u) << "the first-write line is logged once";
  EXPECT_FALSE(Lines::Has("coming soon")) << "a text is never logged";
}

TEST(UiTextFrame, AMissExplainsItselfWithTheSpacesAndIsDroppedWithItsCause) {
  Lines::All().clear();
  FakeSpace gs;
  gs.Names({0x1});
  gs.Build();
  FakePlatform p;
  p.Use({&gs});
  Pending pending;
  std::atomic<bool> first{false};
  ASSERT_EQ(pending.Post(Req("X"), 0), Check::Ok);
  nevr_ui_text::RunFrame(pending, p, 0, first, &Lines::Log);
  ASSERT_EQ(Lines::All().size(), 3u);
  EXPECT_EQ(Lines::All()[0].second,
            "[NEVR.UI] text waiting actor=0xad97e58a7b13425f element=0xa8452de53657bd6e bytes=1 attempts=1 outcome=no_actor");
  EXPECT_EQ(Lines::All()[1].second.rfind("[NEVR.UI] text probe space=0 gs=0x", 0), 0u);
  EXPECT_NE(Lines::All()[1].second.find(" table=0x0"), std::string::npos);
  EXPECT_EQ(Lines::All()[2].second, "[NEVR.UI] text probe spaces=1");
  nevr_ui_text::RunFrame(pending, p, nevr_ui_text::kGiveUpMs, first, &Lines::Log);
  EXPECT_EQ(Lines::All().back().first, nevr_ui_text::LogLevel::Warning);
  EXPECT_NE(Lines::All().back().second.find("text dropped"), std::string::npos);
  EXPECT_NE(Lines::All().back().second.find("reason=no_actor"), std::string::npos);
}
