// Which textures the UI keeps at their top mip (patch/ui_texture_mips_rules.h). The hook that applies the decisions is
// patch/ui_texture_mips.cpp; its byte checks and the field reads need the game and are proven by a live run.
#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <iterator>

#include "runtime/patch/ui_texture_mips_rules.h"

namespace {

using nevr_ui_texture_mips::IsUiTextureName;
using nevr_ui_texture_mips::kMaxMipCount;
using nevr_ui_texture_mips::NameForHash;
using nevr_ui_texture_mips::NeedsTopMip;
using nevr_ui_texture_mips::TextureState;

TEST(UiTextureMips, TheUiSystemsTexturePrefixesAreUi) {
  EXPECT_TRUE(IsUiTextureName("ui_banner_0016_primary_msk"));
  EXPECT_TRUE(IsUiTextureName("mnu_news_region_line_top"));
  EXPECT_TRUE(IsUiTextureName("menu_tile_back"));
  EXPECT_TRUE(IsUiTextureName("loading_4"));
  EXPECT_TRUE(IsUiTextureName("splashglow"));
  EXPECT_TRUE(IsUiTextureName("default_splash_texture"));
}

// World, avatar and prop textures keep the engine's streaming.
TEST(UiTextureMips, WorldTexturesAreNotUi) {
  EXPECT_FALSE(IsUiTextureName("mpl_lobby_plastic_matte_a_grey_mid_ly2_triangle"));
  EXPECT_FALSE(IsUiTextureName("mpl_cbt_gauss_s03_scaffolding_rail_b~instance_phys"));
  EXPECT_FALSE(IsUiTextureName("emote_selected_frame"));
  EXPECT_FALSE(IsUiTextureName(""));
}

TEST(UiTextureMips, ThePrefixMustStartTheName) {
  EXPECT_FALSE(IsUiTextureName("grid_ui_overlay"));
  EXPECT_FALSE(IsUiTextureName("ui"));
  EXPECT_FALSE(IsUiTextureName("loading"));
  EXPECT_FALSE(IsUiTextureName("default_splash_texture_2"));
}

TEST(UiTextureMips, ATextureShortOfItsTopMipNeedsForcing) {
  EXPECT_TRUE(NeedsTopMip(TextureState{1, 9, 0}));
  EXPECT_TRUE(NeedsTopMip(TextureState{0, 1, 0}));
}

TEST(UiTextureMips, ACompleteTextureIsLeftAlone) {
  EXPECT_FALSE(NeedsTopMip(TextureState{9, 9, 0}));
}

TEST(UiTextureMips, ATextureThatOwnsItsStreamIsLeftAlone) {
  EXPECT_FALSE(NeedsTopMip(TextureState{1, 9, 1}));
}

// A misread field must not start a read of the texture file.
TEST(UiTextureMips, ANonsenseMipCountIsNotForced) {
  EXPECT_FALSE(NeedsTopMip(TextureState{0, 0, 0}));
  EXPECT_FALSE(NeedsTopMip(TextureState{1, kMaxMipCount + 1, 0}));
  EXPECT_FALSE(NeedsTopMip(TextureState{1, 0xFFFFFFFFu, 0}));
  EXPECT_TRUE(NeedsTopMip(TextureState{1, kMaxMipCount, 0}));
}

struct Entry {
  std::uint64_t hash;
  const char* name;
};
constexpr Entry kTable[] = {{0x0000000000000001ULL, "a"}, {0x00ff000000000000ULL, "b"}, {0x8000000000000000ULL, "high_bit"},
                            {0xfffffffffffffffeULL, "last"}};

TEST(UiTextureMips, ANameIsFoundByItsHashAtEveryPositionOfTheTable) {
  for (const Entry& e : kTable) {
    const char* found = NameForHash(kTable, std::size(kTable), e.hash);
    ASSERT_NE(found, nullptr) << e.name;
    EXPECT_STREQ(found, e.name);
  }
}

// The game's name field is all ones until it is set; no table holds that hash.
TEST(UiTextureMips, AnUnknownHashHasNoName) {
  EXPECT_EQ(NameForHash(kTable, std::size(kTable), 0x2), nullptr);
  EXPECT_EQ(NameForHash(kTable, std::size(kTable), ~0ULL), nullptr);
  EXPECT_EQ(NameForHash(kTable, std::size(kTable), 0), nullptr);
  EXPECT_EQ(NameForHash(kTable, 0, 1), nullptr);
}

}  // namespace
