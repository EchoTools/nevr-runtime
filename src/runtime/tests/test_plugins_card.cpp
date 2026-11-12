// #1857, docs/standards/logging.md ([NEVR.PLUGINMGR]): the press hook's decision, as pure logic (patch/plugins_card.h): is this
// call of CncaGameSpace::SendComponentEvent a press on the PLUGINS card, and the one Info line it writes. No game and no hook:
// the event below is a buffer in this process, read by the same ReadPress the hook calls.

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>

#include "runtime/patch/invite_choices.h"
#include "runtime/patch/menu_press.h"
#include "runtime/patch/plugins_card.h"

namespace {

using nevr_menu_press::Press;
using nevr_menu_press::Sender;

constexpr uint64_t kOtherComponent = 0x35ec861316abbb34ULL;  // PLAY's

// One event as the hook reads it. The sender is "none" (0xffff), so no game space or actor table is needed: the decision
// has to hold whether or not the sender resolves.
struct FakeEvent {
  alignas(8) uint8_t event[nevr_invite_choices::kEventBytes] = {};
  FakeEvent(uint64_t symbol, uint64_t component) {
    Put(nevr_invite_choices::kEventSymbolOffset, symbol);
    Put(nevr_invite_choices::kEventSenderOffset, 0xffffULL);
    Put(nevr_invite_choices::kEventComponentOffset, component);
  }
  void Put(size_t at, uint64_t v) { std::memcpy(event + at, &v, sizeof(v)); }
  Press Read(Sender* sender = nullptr) const {
    Press press;
    const auto reader = [this](uintptr_t address, void* out, size_t n) {
      const uintptr_t from = reinterpret_cast<uintptr_t>(event);
      if (address < from || address + n > from + sizeof(event)) return false;
      std::memcpy(out, reinterpret_cast<const void*>(address), n);
      return true;
    };
    const Sender s = nevr_menu_press::ReadPress(reader, 0, reinterpret_cast<uintptr_t>(event), &press);
    if (sender != nullptr) *sender = s;
    return press;
  }
};

TEST(PluginsCardConstants, TheCardHasAnIdOfItsOwn) {
  // The clone was made from TUTORIALS and shared its id, so a press on PLUGINS started the tutorial (#1857 measurement).
  EXPECT_NE(nevr_plugins_card::kPluginsCardComponent, nevr_plugins_card::kTutorialsCardComponent);
  EXPECT_EQ(nevr_plugins_card::kTutorialsCardComponent, 0x35ec861316abbb35ULL);
  EXPECT_EQ(nevr_plugins_card::kPluginsCardComponent, 0x35ec861316abbbf1ULL);
}

TEST(PluginsCardPress, ACardClickWithTheCardsComponentIsAPress) {
  FakeEvent e(nevr_invite_choices::kEvtCardClicked, nevr_plugins_card::kPluginsCardComponent);
  Sender sender = Sender::Resolved;
  const Press press = e.Read(&sender);
  EXPECT_EQ(sender, Sender::NoSender);  // the decision does not wait for the sender
  EXPECT_TRUE(nevr_plugins_card::IsPluginsPress(press));
}

TEST(PluginsCardPress, AClickOnTheTutorialsCardIsNotAPress) {
  FakeEvent e(nevr_invite_choices::kEvtCardClicked, nevr_plugins_card::kTutorialsCardComponent);
  EXPECT_FALSE(nevr_plugins_card::IsPluginsPress(e.Read()));
}

TEST(PluginsCardPress, AClickOnAnyOtherCardIsNotAPress) {
  FakeEvent e(nevr_invite_choices::kEvtCardClicked, kOtherComponent);
  EXPECT_FALSE(nevr_plugins_card::IsPluginsPress(e.Read()));
}

TEST(PluginsCardPress, AnotherEventWithTheCardsComponentIsNotAPress) {
  // evt_btn_clicked and every other symbol: only a card click is a press on the card.
  FakeEvent e(nevr_invite_choices::kEvtButtonClicked, nevr_plugins_card::kPluginsCardComponent);
  EXPECT_FALSE(nevr_plugins_card::IsPluginsPress(e.Read()));
}

TEST(PluginsCardPress, AnEventThatCouldNotBeReadIsNotAPress) {
  Press nothing;  // what ReadPress leaves for an unreadable event
  EXPECT_FALSE(nevr_plugins_card::IsPluginsPress(nothing));
}

TEST(PluginsCardLine, OnePressIsOneTaggedInfoLineWithItsIdentifiers) {
  Press press;
  press.symbol = nevr_invite_choices::kEvtCardClicked;
  press.component = nevr_plugins_card::kPluginsCardComponent;
  EXPECT_EQ(nevr_plugins_card::PluginsPressLine(press, 12),
            "[NEVR.PLUGINMGR] plugins card pressed component=0x35ec861316abbbf1 seen=12 "
            "(no page opens yet; the press is only recorded)");
}

}  // namespace
