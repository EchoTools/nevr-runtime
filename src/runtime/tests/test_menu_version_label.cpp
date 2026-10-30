// What the menu's version text becomes (patch/menu_version_label_rules.h). The hook that applies it is
// patch/menu_version_label.cpp; its prologue check and detour need the game and are proven by a live run.
#include <gtest/gtest.h>

#include <cstdint>
#include <string>

#include "nevr/symbol_hash.h"
#include "runtime/patch/menu_version_label_rules.h"

namespace {

using nevr_menu_version_label::FitLabel;
using nevr_menu_version_label::kLobbyVersionItem;
using nevr_menu_version_label::kMaxLabelChars;
using nevr_menu_version_label::kPublisherLockItem;
using nevr_menu_version_label::PublisherLockReplacement;
using nevr_menu_version_label::ReplacementFor;

// The item symbol is the game's CSymbol64 hash of the item's name, the one the script DLL f0c9097e9973b51b passes.
TEST(MenuVersionLabel, TheItemSymbolIsTheHashOfLobbyVersion) {
  EXPECT_EQ(kLobbyVersionItem, EchoVR::CSymbol64Hash("lobby_version"));
}

TEST(MenuVersionLabel, ATagBuildLabelIsDrawnWhole) {
  EXPECT_EQ(FitLabel("nEVR 5.8.0"), "nEVR 5.8.0");
}

TEST(MenuVersionLabel, ALabelExactlyAtTheWidthIsDrawnWhole) {
  const std::string label(kMaxLabelChars, 'x');
  EXPECT_EQ(FitLabel(label), label);
}

TEST(MenuVersionLabel, ADevBuildLabelLosesItsCommitWhenItDoesNotFit) {
  EXPECT_EQ(FitLabel("nEVR 5.8.1-dev.123+0123456789ab"), "nEVR 5.8.1-dev.123");
}

TEST(MenuVersionLabel, ALabelWithNoCommitThatStillDoesNotFitIsCutToTheWidth) {
  const std::string label = FitLabel("nEVR 5.8.1-dev.123456789012345");
  EXPECT_EQ(label.size(), kMaxLabelChars);
  EXPECT_EQ(label, std::string("nEVR 5.8.1-dev.123456789012345").substr(0, kMaxLabelChars));
}

TEST(MenuVersionLabel, ADevLabelWhoseCommitIsDroppedIsStillCutWhenTooLong) {
  const std::string label = FitLabel("nEVR 5.8.1-dev.1234567890123+abc");
  EXPECT_EQ(label.size(), kMaxLabelChars);
}

TEST(MenuVersionLabel, OnlyTheLobbyVersionItemIsReplaced) {
  const std::string label = "nEVR 5.8.0";
  EXPECT_STREQ(ReplacementFor(kLobbyVersionItem, "2023-04-04T17:47:47+00:00", label), "nEVR 5.8.0");
  EXPECT_EQ(ReplacementFor(EchoVR::CSymbol64Hash("rad_version"), "631547", label), nullptr);
  EXPECT_EQ(ReplacementFor(EchoVR::CSymbol64Hash("publisher_lock"), "rad15_live", label), nullptr);
  EXPECT_EQ(ReplacementFor(0, "x", label), nullptr);
}

// The publisher lock item is drawn as the service's name, whatever the document's value; the literal is the game's hash.
TEST(MenuVersionLabel, ThePublisherLockItemIsDrawnAsTheServiceName) {
  EXPECT_EQ(kPublisherLockItem, EchoVR::CSymbol64Hash("publisher_lock"));
  EXPECT_STREQ(PublisherLockReplacement(kPublisherLockItem, "rad15_live"), "echovrce");
  EXPECT_STREQ(PublisherLockReplacement(kPublisherLockItem, "echotools"), "echovrce");
  EXPECT_EQ(PublisherLockReplacement(kPublisherLockItem, ""), nullptr);
  EXPECT_EQ(PublisherLockReplacement(kPublisherLockItem, nullptr), nullptr);
  EXPECT_EQ(PublisherLockReplacement(kLobbyVersionItem, "rad15_live"), nullptr);
}

// The script writes "" to the item when its page is disabled; the label must not reappear then.
TEST(MenuVersionLabel, TheScriptClearingTheItemIsLeftAlone) {
  EXPECT_EQ(ReplacementFor(kLobbyVersionItem, "", "nEVR 5.8.0"), nullptr);
}

TEST(MenuVersionLabel, ANullTextIsLeftAlone) {
  EXPECT_EQ(ReplacementFor(kLobbyVersionItem, nullptr, "nEVR 5.8.0"), nullptr);
}

TEST(MenuVersionLabel, AnEmptyLabelReplacesNothing) {
  EXPECT_EQ(ReplacementFor(kLobbyVersionItem, "2023-04-04T17:47:47+00:00", std::string()), nullptr);
}

}  // namespace
