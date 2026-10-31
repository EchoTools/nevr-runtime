// #1535 / #1599: the FRIENDS panel and friend profile canvases, built from the canvases the game delivers
// (friends_canvas.h). Synthetic canvases test the builders' structure; the stock canvases (game data, not in the
// repository) are read from NEVR_UI_CANVAS_CORPUS (a directory of canvas files named 0x<NAME>.bin) when it is set.

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "nevr/symbol_hash.h"
#include "runtime/patch/friends_canvas.h"
#include "runtime/patch/ui_canvas.h"

namespace {

namespace uc = nevr_ui_canvas;
namespace fc = nevr_friends_canvas;

fc::Pictures TestPictures() {
  fc::Pictures p;
  std::uint64_t next = 0x1000;
  for (auto& row : p.rows)
    for (auto& t : row) t = next++;
  for (auto& t : p.profile) t = 0x8000000000000000ULL + next++;  // a negative one, for the signed order
  return p;
}

// A delivered SEASON INFO canvas as far as the panel needs: the three behaviours, one named rect.
uc::Canvas SyntheticSeason() {
  uc::Canvas c;
  c.behaviours.push_back(uc::MakeBehaviour(fc::kButtonStates, 0x0E7F320C56265C6CULL, 0, std::vector<std::uint8_t>(304, 7)));
  c.behaviours.push_back(uc::MakeBehaviour(fc::kPointerInteract, fc::kPointerInteract, 0, uc::Words({fc::kPointerInteract, 0})));
  c.behaviours.push_back(uc::MakeBehaviour(fc::kDynLoadTexture, fc::kDynLoadTexture, 0, uc::Words({fc::kDynLoadTexture, 0xBEEF})));
  c.rects.push_back({0xBEEF, {}});
  return c;
}

const uc::Element& Named(const uc::Canvas& c, const char* name) {
  const std::int64_t at = c.Find(EchoVR::CSymbol64Hash(name));
  EXPECT_GE(at, 0) << name;
  return c.elements[static_cast<std::size_t>(at < 0 ? 0 : at)];
}

std::vector<const uc::Behaviour*> On(const uc::Canvas& c, std::int64_t element) {
  std::vector<const uc::Behaviour*> out;
  for (const auto& b : c.behaviours)
    if (static_cast<std::int64_t>(b.element()) == element) out.push_back(&b);
  return out;
}

std::uint64_t PayloadWord(const uc::Behaviour& b, std::size_t i) {
  std::uint64_t v = 0;
  if (b.payload.size() >= 8 * (i + 1)) std::memcpy(&v, b.payload.data() + 8 * i, 8);
  return v;
}

}  // namespace

TEST(FriendsCanvas, PanelRefusesACanvasWithoutTheBehavioursItTakes) {
  uc::Canvas empty, out;
  std::string why;
  EXPECT_FALSE(fc::BuildFriendsPanel(empty, TestPictures(), &out, &why));
  EXPECT_FALSE(why.empty());
}

TEST(FriendsCanvas, PanelHasEveryElementTheRuntimeWritesTo) {
  uc::Canvas panel;
  std::string why;
  const fc::Pictures pictures = TestPictures();
  ASSERT_TRUE(fc::BuildFriendsPanel(SyntheticSeason(), pictures, &panel, &why)) << why;
  EXPECT_TRUE(uc::Problems(panel).empty());
  EXPECT_EQ(panel.authored_width(), fc::kPanelWidth);
  EXPECT_EQ(panel.render_width(), fc::kPanelPixelsW);
  for (std::uint32_t i = 0; i < fc::kRows; ++i) {
    const std::string row = "friend" + std::to_string(i) + "_";
    EXPECT_EQ(Named(panel, (row + "name").c_str()).type(), uc::kText);
    EXPECT_EQ(Named(panel, (row + "status").c_str()).type(), uc::kText);
    EXPECT_EQ(Named(panel, (row + "mark").c_str()).type(), uc::kText);
    // The row button: the two stock button behaviours, the payloads as delivered.
    const std::int64_t btn = panel.Find(EchoVR::CSymbol64Hash((row + "btn").c_str()));
    const auto behaviours = On(panel, btn);
    ASSERT_EQ(behaviours.size(), 2u) << row;
    EXPECT_EQ(behaviours[0]->type(), fc::kButtonStates);
    EXPECT_EQ(behaviours[0]->payload, std::vector<std::uint8_t>(304, 7));
    EXPECT_EQ(behaviours[1]->type(), fc::kPointerInteract);
    EXPECT_EQ(Named(panel, (row + "hl").c_str()).parent(), btn);
    // The picture: a sprite on a resident texture with the dynamic-texture behaviour naming the row's default.
    const std::int64_t av = panel.Find(EchoVR::CSymbol64Hash((row + "avatar").c_str()));
    ASSERT_GE(av, 0);
    EXPECT_EQ(panel.elements[static_cast<std::size_t>(av)].texture(), 0xBEEFu);  // SEASON INFO's own, resident
    const auto pic = On(panel, av);
    ASSERT_EQ(pic.size(), 1u);
    EXPECT_EQ(PayloadWord(*pic[0], 0), fc::kDynLoadTexture);
    EXPECT_EQ(PayloadWord(*pic[0], 1), pictures.rows[i][0]);
  }
  EXPECT_EQ(panel.Text(Named(panel, "play_text")), "INVITE FRIENDS");
  EXPECT_EQ(panel.Text(Named(panel, "header_txt")), "FRIENDS");
  EXPECT_EQ(Named(panel, "background_image").texture(), 0xBEEFu);
}

TEST(FriendsCanvas, PanelRectTableHoldsEveryPictureTextureOnceInSignedOrder) {
  uc::Canvas panel;
  const fc::Pictures pictures = TestPictures();
  ASSERT_TRUE(fc::BuildFriendsPanel(SyntheticSeason(), pictures, &panel, nullptr));
  for (const auto& row : pictures.rows)
    for (const std::uint64_t t : row) {
      int n = 0;
      for (const auto& r : panel.rects) n += r.symbol == t;
      EXPECT_EQ(n, 1) << std::hex << t;
    }
  for (std::size_t i = 1; i < panel.rects.size(); ++i)
    EXPECT_LT(static_cast<std::int64_t>(panel.rects[i - 1].symbol), static_cast<std::int64_t>(panel.rects[i].symbol));
}

TEST(FriendsCanvas, PanelRoundTrips) {
  uc::Canvas panel, back;
  ASSERT_TRUE(fc::BuildFriendsPanel(SyntheticSeason(), TestPictures(), &panel, nullptr));
  const std::vector<std::uint8_t> bytes = uc::Encode(panel);
  ASSERT_TRUE(uc::Decode(bytes.data(), bytes.size(), &back, nullptr));
  EXPECT_EQ(uc::Encode(back), bytes);
}

TEST(FriendsCanvas, ProfileRefusesACanvasThatIsNotThePopup) {
  uc::Canvas out;
  std::string why;
  EXPECT_FALSE(fc::BuildFriendProfile(SyntheticSeason(), TestPictures(), &out, &why));
  EXPECT_NE(why.find("pop-up"), std::string::npos);
}

namespace {

bool Corpus(const char* name, uc::Canvas* out) {
  const char* dir = std::getenv("NEVR_UI_CANVAS_CORPUS");
  if (dir == nullptr || *dir == '\0') return false;
  char file[40];
  std::snprintf(file, sizeof(file), "0x%016llX.bin", static_cast<unsigned long long>(EchoVR::CSymbol64Hash(name)));
  std::ifstream in(std::filesystem::path(dir) / file, std::ios::binary);
  const std::vector<std::uint8_t> b((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  return !b.empty() && uc::Decode(b.data(), b.size(), out, nullptr);
}

}  // namespace

TEST(FriendsCanvas, PanelBuildsFromTheStockSeasonCanvas) {
  uc::Canvas season, panel;
  if (!Corpus("ui_mnu_season_info", &season)) GTEST_SKIP() << "NEVR_UI_CANVAS_CORPUS not set";
  std::string why;
  ASSERT_TRUE(fc::BuildFriendsPanel(season, TestPictures(), &panel, &why)) << why;
  EXPECT_TRUE(uc::Problems(panel).empty());
}

TEST(FriendsCanvas, ProfileBuildsFromTheStockPopupCanvas) {
  uc::Canvas popup, profile;
  if (!Corpus("menu_popup_panel_image_social", &popup)) GTEST_SKIP() << "NEVR_UI_CANVAS_CORPUS not set";
  std::string why;
  const fc::Pictures pictures = TestPictures();
  ASSERT_TRUE(fc::BuildFriendProfile(popup, pictures, &profile, &why)) << why;
  EXPECT_TRUE(uc::Problems(profile).empty());
  EXPECT_EQ(profile.authored_width(), 2546u);  // 2122 x 1.2
  // The stock elements stay at their indexes (the pop-up's script finds them by name; nothing is removed).
  for (std::size_t i = 0; i < popup.elements.size(); ++i) EXPECT_EQ(profile.elements[i].name(), popup.elements[i].name());
  EXPECT_EQ(Named(profile, "social").texture(), Named(popup, "social").texture());  // the stock one, resident
  EXPECT_EQ(Named(profile, "rightbg").colour(), fc::kRed);
  // The stock second body line is drawn transparent (the page script writes it on open), and INVITE and UNFRIEND sit
  // below it, inside the pop-up.
  EXPECT_EQ(Named(profile, "bodytext2").colour()[3], 0.0f);
  const uc::Rect body2 = Named(profile, "bodytext2").offsets();
  const uc::Rect invite = Named(profile, "LeftButton").offsets();
  const auto& unfriend = profile.elements[static_cast<std::size_t>(Named(profile, "rightbg").parent())];
  EXPECT_EQ(invite.y0, fc::kInviteY);
  EXPECT_EQ(unfriend.offsets().y0, fc::kUnfriendY);
  EXPECT_GE(invite.y0, body2.y1);
  EXPECT_GE(unfriend.offsets().y0, invite.y1);
  EXPECT_LE(unfriend.offsets().y1, static_cast<float>(profile.authored_height()));
  EXPECT_EQ(profile.Text(Named(profile, "tab_arena_txt")), "ARENA");
  EXPECT_EQ(profile.Text(Named(profile, "tab_combat_txt")), "COMBAT");
  EXPECT_EQ(On(profile, profile.Find(EchoVR::CSymbol64Hash("tab_combat"))).size(), 2u);
  for (std::uint32_t i = 0; i < fc::kStats; ++i)
    EXPECT_EQ(Named(profile, ("stat" + std::to_string(i) + "_value").c_str()).type(), uc::kText);
  uc::Canvas back;
  const std::vector<std::uint8_t> bytes = uc::Encode(profile);
  ASSERT_TRUE(uc::Decode(bytes.data(), bytes.size(), &back, nullptr));
}

// The shapes are the stock elements' texture and UV rect, bit for bit.
TEST(FriendsCanvas, ShapesAreTheStockElementsTexturesAndUvs) {
  uc::Canvas tips, button, group;
  if (!Corpus("ui_mnu_tip_bodies", &tips) || !Corpus("ui_mnu_button_small", &button) ||
      !Corpus("ui_mnu_group_selection", &group))
    GTEST_SKIP() << "NEVR_UI_CANVAS_CORPUS not set";
  const auto same = [](const uc::Element& e, const fc::Shape& s) {
    uc::Rect uv;
    std::memcpy(&uv, e.raw.data() + 144, sizeof(uv));
    return e.texture() == s.texture && std::memcmp(&uv, &s.uv, sizeof(uv)) == 0;
  };
  EXPECT_TRUE(same(tips.elements[6], fc::kCapLeft));
  EXPECT_TRUE(same(tips.elements[7], fc::kCapRight));
  EXPECT_TRUE(same(button.elements[1], fc::kButtonFill));
  EXPECT_TRUE(same(button.elements[3], fc::kButtonOutline));
  EXPECT_TRUE(same(group.elements[54], fc::kButtonHighlight));
}
