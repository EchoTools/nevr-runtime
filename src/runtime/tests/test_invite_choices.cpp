// #906, docs/adr/0017: what the CREATE GAME wizard's cards chose (patch/invite_choices.h), as pure logic. Which events
// are the wizard's, how a card changes the choices, what a request plan makes of them, and how the observer reads an
// event out of a game space through a reader that can refuse. No game and no hook: the game space below is a few
// buffers in this process.

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "nevr/symbol_hash.h"
#include "runtime/patch/invite_choices.h"
#include "runtime/patch/invite_wizard.h"

namespace {

using nevr_invite::Choices;
using nevr_invite::Kind;
using nevr_invite::Metric;
using nevr_invite::Plan;
using nevr_invite::Skip;
using nevr_invite_choices::Apply;
using nevr_invite_choices::Event;
using nevr_invite_choices::Read;
using nevr_invite_choices::State;

Event Card(uint64_t owner, uint64_t card) { return Event{nevr_invite_choices::kEvtCardClicked, owner, card}; }

Choices After(std::initializer_list<Event> events) {
  Choices c;
  for (const Event& e : events) c = Apply(c, e);
  return c;
}

// The names of the four combat levels, as the symbol cache has them (test_invite_wizard reads the real cache).
const char* LevelNamer(std::uint64_t symbol) {
  if (symbol == nevr_invite::kLevelFission) return "mpl_combat_fission";
  if (symbol == nevr_invite::kLevelGauss) return "mpl_combat_gauss";
  if (symbol == nevr_invite::kLevelDyson) return "mpl_combat_dyson";
  if (symbol == nevr_invite::kLevelCombustion) return "mpl_combat_combustion";
  return nullptr;
}

Metric CreateMetric() {
  Metric m;
  m.kind = Kind::Create;
  m.region = "us-east";
  m.expires_text = "3 HOURS";
  return m;
}

// ---- which events are the wizard's ----

TEST(InviteChoices, TheSixEventSymbolsAreTheWizardsAndNoOtherIs) {
  for (const char* name : {"evt_card_clicked", "evt_btn_clicked", "evt_text_submitted", "evt_link_button_clicked",
                           "evt_back_btn_clicked", "evt_submission_canceled"}) {
    EXPECT_TRUE(nevr_invite_choices::IsWizardEventSymbol(EchoVR::CSymbol64Hash(name))) << name;
    EXPECT_NE(std::string(nevr_invite_choices::EventName(EchoVR::CSymbol64Hash(name))), "") << name;
  }
  for (const char* name : {"evt_card_hovered", "evt_btn_pressed", "", "evt_card_clicked "}) {
    EXPECT_FALSE(nevr_invite_choices::IsWizardEventSymbol(EchoVR::CSymbol64Hash(name))) << name;
  }
  EXPECT_FALSE(nevr_invite_choices::IsWizardEventSymbol(0));
}

TEST(InviteChoices, TheEightPageOwnersAreTheWizardsAndEachHasAName) {
  const uint64_t owners[] = {nevr_invite_choices::kPageJoinCode,   nevr_invite_choices::kPageLink,
                             nevr_invite_choices::kPageExperience, nevr_invite_choices::kPageArenaMode,
                             nevr_invite_choices::kPageCombatMode, nevr_invite_choices::kPageMap,
                             nevr_invite_choices::kPageSettings,   nevr_invite_choices::kPageShare};
  std::vector<std::string> names;
  for (uint64_t owner : owners) {
    EXPECT_TRUE(nevr_invite_choices::IsWizardPage(owner));
    names.emplace_back(nevr_invite_choices::PageName(owner));
    EXPECT_NE(names.back(), "");
  }
  std::sort(names.begin(), names.end());
  EXPECT_EQ(std::adjacent_find(names.begin(), names.end()), names.end()) << "two pages share a name";
  EXPECT_FALSE(nevr_invite_choices::IsWizardPage(0));
  EXPECT_FALSE(nevr_invite_choices::IsWizardPage(nevr_invite_choices::kCardLeft));
  EXPECT_STREQ(nevr_invite_choices::PageName(0x1234), "");
}

TEST(InviteChoices, TheThreeCardsAreNumberedLeftMiddleRight) {
  EXPECT_EQ(nevr_invite_choices::CardIndex(nevr_invite_choices::kCardLeft), 1);
  EXPECT_EQ(nevr_invite_choices::CardIndex(nevr_invite_choices::kCardMiddle), 2);
  EXPECT_EQ(nevr_invite_choices::CardIndex(nevr_invite_choices::kCardRight), 3);
  EXPECT_EQ(nevr_invite_choices::CardIndex(0), 0);
  EXPECT_EQ(nevr_invite_choices::CardIndex(nevr_invite_choices::kPageMap), 0);
}

// ---- how a card changes the choices ----

TEST(InviteChoices, TheExperiencePageSetsTheExperience) {
  using namespace nevr_invite_choices;
  EXPECT_EQ(After({Card(kPageExperience, kCardLeft)}).experience, "lobby");
  EXPECT_EQ(After({Card(kPageExperience, kCardMiddle)}).experience, "arena");
  EXPECT_EQ(After({Card(kPageExperience, kCardRight)}).experience, "combat");
}

TEST(InviteChoices, AnArenaModeCardSetsTheMode) {
  using namespace nevr_invite_choices;
  EXPECT_EQ(After({Card(kPageExperience, kCardMiddle), Card(kPageArenaMode, kCardLeft)}).mode, "custom");
  EXPECT_EQ(After({Card(kPageExperience, kCardMiddle), Card(kPageArenaMode, kCardMiddle)}).mode, "ai");
  EXPECT_EQ(After({Card(kPageArenaMode, kCardRight)}).mode, "") << "the arena page has two cards";
}

TEST(InviteChoices, ACombatModeCardSetsTheModeAndAMapCardTheMap) {
  using namespace nevr_invite_choices;
  EXPECT_EQ(After({Card(kPageCombatMode, kCardLeft)}).mode, "payload");
  EXPECT_EQ(After({Card(kPageCombatMode, kCardMiddle)}).mode, "capture");
  EXPECT_EQ(After({Card(kPageCombatMode, kCardRight)}).mode, "random");
  EXPECT_EQ(After({Card(kPageMap, kCardLeft)}).map, 1);
  EXPECT_EQ(After({Card(kPageMap, kCardMiddle)}).map, 2);
  EXPECT_EQ(After({Card(kPageMap, kCardRight)}).map, 0) << "the map page has two cards";
}

TEST(InviteChoices, ANewExperienceClearsTheModeAndTheMap) {
  using namespace nevr_invite_choices;
  const Choices c = After({Card(kPageExperience, kCardRight), Card(kPageCombatMode, kCardLeft), Card(kPageMap, kCardMiddle),
                           Card(kPageExperience, kCardMiddle)});
  EXPECT_EQ(c.experience, "arena");
  EXPECT_EQ(c.mode, "");
  EXPECT_EQ(c.map, 0);
}

TEST(InviteChoices, ANewCombatModeClearsTheMapButAnArenaModeKeepsIt) {
  using namespace nevr_invite_choices;
  const Choices combat = After({Card(kPageMap, kCardLeft), Card(kPageCombatMode, kCardMiddle)});
  EXPECT_EQ(combat.map, 0);
  EXPECT_EQ(combat.mode, "capture");
  const Choices arena = After({Card(kPageMap, kCardLeft), Card(kPageArenaMode, kCardLeft)});
  EXPECT_EQ(arena.map, 1);
}

TEST(InviteChoices, OnlyACardEventOnAChoicePageChangesTheChoices) {
  using namespace nevr_invite_choices;
  Choices start = After({Card(kPageExperience, kCardRight), Card(kPageCombatMode, kCardLeft), Card(kPageMap, kCardLeft)});
  for (uint64_t symbol : {kEvtButtonClicked, kEvtTextSubmitted, kEvtLinkButtonClicked, kEvtBackButtonClicked,
                          kEvtSubmissionCanceled, uint64_t{0}}) {
    const Choices c = Apply(start, Event{symbol, kPageExperience, kCardLeft});
    EXPECT_EQ(c.experience, "combat");
    EXPECT_EQ(c.mode, "payload");
    EXPECT_EQ(c.map, 1);
  }
  for (uint64_t owner : {kPageJoinCode, kPageLink, kPageSettings, kPageShare, uint64_t{0}, uint64_t{42}}) {
    const Choices c = Apply(start, Card(owner, kCardLeft));
    EXPECT_EQ(c.experience, "combat");
    EXPECT_EQ(c.mode, "payload");
    EXPECT_EQ(c.map, 1);
  }
}

TEST(InviteChoices, ACardThatIsNoneOfTheThreeLeavesThePageUnset) {
  using namespace nevr_invite_choices;
  const Choices c = After({Card(kPageExperience, kCardRight), Card(kPageExperience, 0x1234)});
  EXPECT_EQ(c.experience, "");
}

TEST(InviteChoices, TheStateKeepsTheLastChoicesAndHandsOutCopies) {
  using namespace nevr_invite_choices;
  State state;
  EXPECT_EQ(state.Snapshot().experience, "");
  state.Observe(Card(kPageExperience, kCardMiddle));
  state.Observe(Card(kPageArenaMode, kCardMiddle));
  Choices copy = state.Snapshot();
  EXPECT_EQ(copy.experience, "arena");
  EXPECT_EQ(copy.mode, "ai");
  copy.mode = "changed";
  EXPECT_EQ(state.Snapshot().mode, "ai");
}

// ---- what a request plan makes of them ----

TEST(InviteChoices, TheChoicesTheCardsLeaveBuildAnInviteCreateRequest) {
  using namespace nevr_invite_choices;
  State state;
  state.Observe(Card(kPageExperience, kCardRight));
  state.Observe(Card(kPageCombatMode, kCardMiddle));
  state.Observe(Card(kPageMap, kCardMiddle));
  const Plan plan = nevr_invite::PlanRequest(CreateMetric(), state.Snapshot(), LevelNamer);
  ASSERT_EQ(plan.skip, Skip::None);
  ASSERT_TRUE(plan.request.has_value());
  EXPECT_EQ(plan.request->rpc, "invite/create");
  const nlohmann::json body = nlohmann::json::parse(plan.request->body);
  EXPECT_EQ(body["experience"], "combat");
  EXPECT_EQ(body["mode"], "capture");
  EXPECT_EQ(body["map"], "mpl_combat_combustion") << "capture point, the middle card";
  EXPECT_EQ(body["region"], "") << "the row's text is display text; no region is sent";
}

TEST(InviteChoices, NoChoicesIsAnIncompleteCreate) {
  const Plan plan = nevr_invite::PlanRequest(CreateMetric(), Choices{});
  EXPECT_EQ(plan.skip, Skip::Incomplete);
  EXPECT_FALSE(plan.request.has_value());
}

TEST(InviteChoices, ALobbyNeedsNoModeAndAnArenaNeedsOne) {
  using namespace nevr_invite_choices;
  EXPECT_EQ(nevr_invite::PlanRequest(CreateMetric(), After({Card(kPageExperience, kCardLeft)})).skip, Skip::None);
  EXPECT_EQ(nevr_invite::PlanRequest(CreateMetric(), After({Card(kPageExperience, kCardMiddle)})).skip,
            Skip::Incomplete);
  EXPECT_EQ(nevr_invite::PlanRequest(CreateMetric(),
                                     After({Card(kPageExperience, kCardMiddle), Card(kPageArenaMode, kCardLeft)}))
                .skip,
            Skip::None);
}

TEST(InviteChoices, ACombatPayloadOrCaptureNeedsAMapAndRandomDoesNot) {
  using namespace nevr_invite_choices;
  const Event combat = Card(kPageExperience, kCardRight);
  EXPECT_EQ(nevr_invite::PlanRequest(CreateMetric(), After({combat, Card(kPageCombatMode, kCardLeft)})).skip,
            Skip::Incomplete);
  EXPECT_EQ(nevr_invite::PlanRequest(CreateMetric(), After({combat, Card(kPageCombatMode, kCardMiddle)})).skip,
            Skip::Incomplete);
  EXPECT_EQ(nevr_invite::PlanRequest(CreateMetric(),
                                     After({combat, Card(kPageCombatMode, kCardLeft), Card(kPageMap, kCardLeft)}), LevelNamer)
                .skip,
            Skip::None);
  EXPECT_EQ(nevr_invite::PlanRequest(CreateMetric(), After({combat, Card(kPageCombatMode, kCardRight)}), LevelNamer).skip,
            Skip::None);
}

// ---- reading an event out of the game's memory ----

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
    SetEvent(nevr_invite_choices::kEvtCardClicked, 3, nevr_invite_choices::kCardLeft);
    names[3] = nevr_invite_choices::kPageExperience;
  }
  static void Put(uint8_t* base, size_t at, uint64_t v) { std::memcpy(base + at, &v, sizeof(v)); }
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
  Read Run(Event* out) const {
    return nevr_invite_choices::ReadEvent(Reader(), reinterpret_cast<uintptr_t>(space),
                                          reinterpret_cast<uintptr_t>(event), out);
  }
};

TEST(InviteChoicesRead, AWizardEventIsReadWithItsOwnerAndComponent) {
  FakeGame game;
  Event e;
  ASSERT_EQ(game.Run(&e), Read::Wizard);
  EXPECT_EQ(e.symbol, nevr_invite_choices::kEvtCardClicked);
  EXPECT_EQ(e.owner, nevr_invite_choices::kPageExperience);
  EXPECT_EQ(e.component, nevr_invite_choices::kCardLeft);
}

TEST(InviteChoicesRead, AnEventThatIsNotTheWizardsCostsOneRead) {
  FakeGame game;
  game.SetEvent(EchoVR::CSymbol64Hash("evt_something_else"), 3, nevr_invite_choices::kCardLeft);
  Event e;
  EXPECT_EQ(game.Run(&e), Read::NotWizardSymbol);
  EXPECT_EQ(game.reads, 1);
  EXPECT_EQ(e.owner, 0u);
  EXPECT_EQ(e.component, 0u);
}

TEST(InviteChoicesRead, ASenderOfFFFFIsNoSenderWhateverTheUpperBitsHold) {
  FakeGame game;
  game.SetEvent(nevr_invite_choices::kEvtCardClicked, 0x1234ffffULL, nevr_invite_choices::kCardLeft);
  Event e;
  EXPECT_EQ(game.Run(&e), Read::NoSender);
  EXPECT_EQ(e.owner, 0u);
}

TEST(InviteChoicesRead, OnlyTheLow16BitsOfTheSenderIndexTheTable) {
  FakeGame game;
  game.SetEvent(nevr_invite_choices::kEvtCardClicked, 0xabcd0000ULL | 3, nevr_invite_choices::kCardRight);
  Event e;
  ASSERT_EQ(game.Run(&e), Read::Wizard);
  EXPECT_EQ(e.owner, nevr_invite_choices::kPageExperience);
}

TEST(InviteChoicesRead, AnIndexAtOrPastTheCapacityIsOutOfRange) {
  FakeGame game;
  Event e;
  game.SetEvent(nevr_invite_choices::kEvtCardClicked, 16, nevr_invite_choices::kCardLeft);
  EXPECT_EQ(game.Run(&e), Read::OutOfRange);
  game.SetEvent(nevr_invite_choices::kEvtCardClicked, 15, nevr_invite_choices::kCardLeft);
  game.names[15] = nevr_invite_choices::kPageMap;
  EXPECT_EQ(game.Run(&e), Read::Wizard);
  EXPECT_EQ(e.owner, nevr_invite_choices::kPageMap);
}

TEST(InviteChoicesRead, AnOwnerThatIsNotAWizardPageIsNotTheWizards) {
  FakeGame game;
  game.names[3] = 0x1111222233334444ULL;
  Event e;
  EXPECT_EQ(game.Run(&e), Read::NotWizardPage);
  EXPECT_EQ(e.owner, 0x1111222233334444ULL);
}

TEST(InviteChoicesRead, AMissingTableOrNamesArrayIsRefusedNotFollowed) {
  FakeGame game;
  Event e;
  FakeGame::Put(game.space, nevr_invite_choices::kSpaceActorTableOffset, 0);
  EXPECT_EQ(game.Run(&e), Read::NoTable);
  FakeGame again;
  FakeGame::Put(again.table, nevr_invite_choices::kTableNamesOffset, 0);
  EXPECT_EQ(again.Run(&e), Read::NoTable);
}

TEST(InviteChoicesRead, MemoryThatCannotBeReadIsRefusedAtEveryStep) {
  Event e;
  {
    FakeGame game;
    game.refuse = reinterpret_cast<uintptr_t>(game.event) + nevr_invite_choices::kEventSymbolOffset;
    EXPECT_EQ(game.Run(&e), Read::Unreadable);
  }
  {
    FakeGame game;
    game.refuse = reinterpret_cast<uintptr_t>(game.event) + nevr_invite_choices::kEventSenderOffset;
    EXPECT_EQ(game.Run(&e), Read::Unreadable);
  }
  {
    FakeGame game;
    game.refuse = reinterpret_cast<uintptr_t>(game.event) + nevr_invite_choices::kEventComponentOffset;
    EXPECT_EQ(game.Run(&e), Read::Unreadable);
  }
  {
    FakeGame game;
    game.refuse = reinterpret_cast<uintptr_t>(game.space) + nevr_invite_choices::kSpaceActorTableOffset;
    EXPECT_EQ(game.Run(&e), Read::Unreadable);
  }
  {
    FakeGame game;
    game.refuse = reinterpret_cast<uintptr_t>(game.table) + nevr_invite_choices::kTableNamesOffset;
    EXPECT_EQ(game.Run(&e), Read::Unreadable);
  }
  {
    FakeGame game;
    game.refuse = reinterpret_cast<uintptr_t>(game.table) + nevr_invite_choices::kTableCapacityOffset;
    EXPECT_EQ(game.Run(&e), Read::Unreadable);
  }
  {
    FakeGame game;
    game.refuse = reinterpret_cast<uintptr_t>(game.names) + 3 * sizeof(uint64_t);
    EXPECT_EQ(game.Run(&e), Read::Unreadable);
  }
}

TEST(InviteChoicesRead, ANullEventOrSpaceIsRefused) {
  FakeGame game;
  Event e;
  EXPECT_EQ(nevr_invite_choices::ReadEvent(game.Reader(), reinterpret_cast<uintptr_t>(game.space), 0, &e),
            Read::Unreadable);
  EXPECT_EQ(nevr_invite_choices::ReadEvent(game.Reader(), 0, reinterpret_cast<uintptr_t>(game.event), &e),
            Read::Unreadable);
}

TEST(InviteChoicesRead, AReadEventFeedsTheChoicesEndToEnd) {
  FakeGame game;
  State state;
  Event e;
  ASSERT_EQ(game.Run(&e), Read::Wizard);
  state.Observe(e);
  EXPECT_EQ(state.Snapshot().experience, "lobby");
}

// ---- the log line ----

TEST(InviteChoices, TheLogLineNamesThePageTheEventTheCardTheChoicesAndTheCount) {
  using namespace nevr_invite_choices;
  const Choices c = After({Card(kPageExperience, kCardMiddle), Card(kPageArenaMode, kCardLeft)});
  EXPECT_EQ(ChoiceLine(Card(kPageArenaMode, kCardLeft), c, 42),
            "[NEVR.INVITE] wizard event seen page=arena_mode event=card card=1 experience=arena mode=custom map=0 "
            "seen=42");
  EXPECT_EQ(ChoiceLine(Event{kEvtButtonClicked, kPageSettings, 0x9999}, c, 7),
            "[NEVR.INVITE] wizard event seen page=settings event=button card=0 experience=arena mode=custom map=0 "
            "seen=7");
}

}  // namespace
