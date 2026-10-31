#pragma once

// #1535 / #1599: the FRIENDS panel and the friend profile pop-up, built in memory from the canvases the game delivers.
//
// The main menu's SEASON INFO card (canvas ui_mnu_season_info) becomes the panel and its SUPPORT & SOCIAL pop-up
// (canvas menu_popup_panel_image_social) becomes the friend profile. Each builder takes the stock canvas as the game
// delivered it and returns the new one; every element is made from its fields (ui_canvas.h), and what is taken from the
// delivered canvas is its header, its named-rect table and its behaviours' payloads (the player's own data, in memory).
// Nothing derived from the game's data is stored or shipped.
//
// Elements other code writes to (CSymbol64 of the name):
//   panel: friend<i>_name, friend<i>_status, friend<i>_mark (texts), friend<i>_avatar (the picture), friend<i>_btn (a
//   row's button group, with its highlight friend<i>_hl), play_btn / play_hl / play_text (INVITE FRIENDS), for i in
//   0..kRows-1; background_image (a hidden picture the home page's status script writes to).
//   profile: headertext (the name), bodytext (status), linktext (party), bodytext2 (drawn transparent), social (the picture, the
//   pop-up's own sprite: an added sprite crashes the level on load), the pop-up's three buttons (lefttext INVITE,
//   middletext CLOSE, righttext UNFRIEND), tab_arena / tab_combat (button groups, highlights tab_<mode>_hl, labels
//   tab_<mode>_txt), stat<i>_label and stat<i>_value for i in 0..kStats-1.
//
// Pure logic, header only, no platform headers, no exceptions (the Quest library compiles it as it is). Tests:
// src/runtime/tests/test_friends_canvas.cpp.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "nevr/symbol_hash.h"
#include "runtime/patch/ui_canvas.h"

namespace nevr_friends_canvas {

namespace uc = nevr_ui_canvas;

inline constexpr std::uint32_t kRows = 5;
inline constexpr std::uint32_t kStats = 8;
inline constexpr std::uint32_t kSlots = 3;  // per picture: a (the default), b and c (the runtime alternates)

inline constexpr std::uint64_t kSeasonCanvas = EchoVR::CSymbol64Hash("ui_mnu_season_info");
inline constexpr std::uint64_t kPopupCanvas = EchoVR::CSymbol64Hash("menu_popup_panel_image_social");

// The behaviours the builders use, by type.
inline constexpr std::uint64_t kButtonStates = 0x89DBB7C4B48FD526ULL;  // plays button_idle / hover / press
inline constexpr std::uint64_t kPointerInteract = EchoVR::CSymbol64Hash("R15PointerInteractBehavior");
inline constexpr std::uint64_t kDynLoadTexture = EchoVR::CSymbol64Hash("R15DynLoadTextureBehavior");
static_assert(kPointerInteract == 0x54384993F5A47462ULL && kDynLoadTexture == 0xD50027525038EC0AULL,
              "the behaviour types the stock menu canvases carry");

// Shapes drawn from the menu's own textures: a texture and the UV rect of one shape in it (the values the stock
// canvases name them with; tests check them against those canvases).
struct Shape {
  std::uint64_t texture;
  uc::Rect uv;
};
inline constexpr std::uint64_t kBasicShapes = 0x295A0F6086A1B807ULL;  // ui_mnu_tip_bodies' slanted ends
inline constexpr std::uint64_t kFrontendButton = EchoVR::CSymbol64Hash("ui_menu_frontend_button");
// Exact float values (hex literals): the stock elements' bits.
inline constexpr Shape kCapLeft{kBasicShapes, {0x1p-1f, 0x1p-2f, 0x1.3d9366p-1f, 0x1.f64d94p-2f}};
inline constexpr Shape kCapRight{kBasicShapes, {0x1.8p-2f, 0.0f, 0x1.fb26cap-2f, 0x1.ec9b28p-3f}};
inline constexpr Shape kButtonFill{kFrontendButton, {0x1.f4p-2f, 0.0f, 0x1.f4p-1f, 0x1.9p-4f}};
inline constexpr Shape kButtonOutline{kFrontendButton, {0x1.f4p-2f, 0x1.a8p-4f, 0x1.f4p-1f, 0x1.9cp-3f}};
inline constexpr Shape kButtonHighlight{kFrontendButton, {0x1.8p-8f, 0.0f, 0x1.5p-2f, 0x1.9p-4f}};

using Colour = std::array<float, 4>;
inline constexpr Colour kTeal{0.30f, 0.86f, 0.80f, 1.0f};
inline constexpr Colour kCyan{0.27f, 0.80f, 0.82f, 1.0f};
inline constexpr Colour kWhite{0.98f, 0.98f, 0.96f, 1.0f};
inline constexpr Colour kBanner{0.25f, 0.27f, 0.29f, 1.0f};
inline constexpr Colour kRow{0.27f, 0.33f, 0.40f, 1.0f};
inline constexpr Colour kClear{0.5f, 0.5f, 0.78f, 0.0f};
inline constexpr Colour kButtonClear{128.0f / 255.0f, 128.0f / 255.0f, 200.0f / 255.0f, 0.0f};  // play_btn's own
inline constexpr Colour kHighlight{0.612f, 0.631f, 0.655f, 0.461f};
inline constexpr Colour kTabHighlight{0.69f, 0.69f, 0.69f, 0.46f};
inline constexpr Colour kButtonSlate{0.325f, 0.369f, 0.427f, 1.0f};
inline constexpr Colour kRed{0.72f, 0.18f, 0.18f, 1.0f};
inline constexpr uc::Rect kTopLeft{0.0f, 0.0f, 0.0f, 0.0f};

// The pictures: the texture each picture element shows by default, and every texture any of them is set to (the
// canvas's named-rect table must hold each, or setting it is an error that unloads the level).
struct Pictures {
  std::array<std::array<std::uint64_t, kSlots>, kRows> rows{};  // [row][slot]
  std::array<std::uint64_t, kSlots> profile{};
};

// The panel's own textures (its pictures set, friends_package.h): nevr_friend_avatar_<row>_<a|b|c> and
// nevr_friend_profile_avatar_<a|b|c>.
inline std::uint64_t RowPicture(std::uint32_t row, std::uint32_t slot) {
  const std::string name = "nevr_friend_avatar_" + std::to_string(row) + "_" + static_cast<char>('a' + slot);
  return EchoVR::CSymbol64Hash(name.c_str());
}
inline std::uint64_t ProfilePicture(std::uint32_t slot) {
  const std::string name = std::string("nevr_friend_profile_avatar_") + static_cast<char>('a' + slot);
  return EchoVR::CSymbol64Hash(name.c_str());
}
inline Pictures StandardPictures() {
  Pictures p;
  for (std::uint32_t r = 0; r < kRows; ++r)
    for (std::uint32_t s = 0; s < kSlots; ++s) p.rows[r][s] = RowPicture(r, s);
  for (std::uint32_t s = 0; s < kSlots; ++s) p.profile[s] = ProfilePicture(s);
  return p;
}

namespace detail {

inline std::int64_t SignedOrder(std::uint64_t v) { return static_cast<std::int64_t>(v); }

// The rect table with a whole-texture entry for every picture texture; one entry per symbol, in signed order (the
// engine looks entries up by binary search).
inline void AddPictureRects(uc::Canvas* c, const std::vector<std::uint64_t>& textures) {
  for (const std::uint64_t t : textures) {
    uc::NamedRect r;
    r.symbol = t;
    const uc::Rect whole{0.0f, 0.0f, 1.0f, 1.0f};
    std::memcpy(r.rect.data(), &whole, sizeof(whole));
    c->rects.push_back(r);
  }
  std::stable_sort(c->rects.begin(), c->rects.end(),
                   [](const uc::NamedRect& a, const uc::NamedRect& b) { return SignedOrder(a.symbol) < SignedOrder(b.symbol); });
  c->rects.erase(std::unique(c->rects.begin(), c->rects.end(),
                             [](const uc::NamedRect& a, const uc::NamedRect& b) { return a.symbol == b.symbol; }),
                 c->rects.end());
}

inline const uc::Behaviour* FindBehaviour(const uc::Canvas& c, std::uint64_t type) {
  for (const auto& b : c.behaviours)
    if (b.type() == type) return &b;
  return nullptr;
}

struct Builder {
  uc::Canvas* c;
  std::string error;

  std::int32_t Group(const char* name, std::int32_t parent, const uc::Rect& rect, const Colour& colour,
                     std::array<float, 2> pivot = {0.0f, 0.0f}, bool button = false) {
    uc::ElementSpec s;
    s.name = EchoVR::CSymbol64Hash(name);
    s.type = uc::kGroup;
    s.parent = parent;
    s.offsets = rect;
    s.colour = colour;
    s.pivot = pivot;
    s.button_flag = button;
    return uc::Append(c, s);
  }
  std::int32_t Sprite(const char* name, std::int32_t parent, const uc::Rect& rect, const Shape& shape, const Colour& colour,
                      std::array<float, 2> pivot = {0.0f, 0.0f}) {
    uc::ElementSpec s;
    s.name = EchoVR::CSymbol64Hash(name);
    s.type = uc::kSprite;
    s.parent = parent;
    s.offsets = rect;
    s.texture = shape.texture;
    s.uv = shape.uv;
    s.colour = colour;
    s.pivot = pivot;
    return uc::Append(c, s);
  }
  std::int32_t Text(const char* name, std::int32_t parent, const uc::Rect& rect, const Colour& colour, const std::string& text,
                    std::uint32_t capacity, std::uint32_t font_size, std::array<float, 2> alignment) {
    const std::int64_t slot = c->AddTextSlot(text, capacity);
    if (slot < 0) {
      error = std::string("text does not fit: ") + name;
      return -1;
    }
    uc::ElementSpec s;
    s.name = EchoVR::CSymbol64Hash(name);
    s.type = uc::kText;
    s.parent = parent;
    s.offsets = rect;
    s.colour = colour;
    s.pivot = {0.5f, 0.5f};
    s.text_slot = static_cast<std::uint32_t>(slot);
    s.capacity = capacity;
    s.font_size = font_size;
    s.alignment = alignment;
    return uc::Append(c, s);
  }
  // The stock button behaviours on `element`: the state player (its payload names the button animations) and the
  // pointer interaction, payloads as the delivered canvas has them.
  void ButtonBehaviours(std::int32_t element, const uc::Behaviour& states, const uc::Behaviour& pointer) {
    c->behaviours.push_back(uc::MakeBehaviour(states.type(), states.name(), static_cast<std::uint32_t>(element), states.payload));
    c->behaviours.push_back(uc::MakeBehaviour(pointer.type(), pointer.name(), static_cast<std::uint32_t>(element), pointer.payload));
  }
  void Picture(std::int32_t element, std::uint64_t texture) {
    c->behaviours.push_back(uc::MakeBehaviour(kDynLoadTexture, kDynLoadTexture, static_cast<std::uint32_t>(element),
                                              uc::Words({kDynLoadTexture, texture})));
  }
};

inline bool Fail(std::string* why, const std::string& what) {
  if (why != nullptr) *why = what;
  return false;
}

inline std::string RowName(const char* stem, std::uint32_t row) { return "friend" + std::to_string(row) + "_" + stem; }

}  // namespace detail

// ---- the panel ----

inline constexpr std::uint32_t kPanelWidth = 768, kPanelHeight = 1024;   // authored units
inline constexpr std::uint32_t kPanelPixelsW = 576, kPanelPixelsH = 768;  // render target, the same aspect

// Builds the FRIENDS panel from the delivered SEASON INFO canvas. False, with `why`, when the delivered canvas lacks
// what the panel takes from it (the button behaviours and the season picture's behaviour).
inline bool BuildFriendsPanel(const uc::Canvas& season, const Pictures& pictures, uc::Canvas* out, std::string* why) {
  const uc::Behaviour* states = detail::FindBehaviour(season, kButtonStates);
  const uc::Behaviour* pointer = detail::FindBehaviour(season, kPointerInteract);
  const uc::Behaviour* picture = detail::FindBehaviour(season, kDynLoadTexture);
  if (states == nullptr || pointer == nullptr || picture == nullptr)
    return detail::Fail(why, "the delivered canvas has no button or picture behaviour");
  uc::Canvas c;
  c.header = season.header;
  const std::uint32_t dims[4] = {kPanelPixelsW, kPanelPixelsH, kPanelWidth, kPanelHeight};
  std::memcpy(c.header.data() + 12, dims, sizeof(dims));
  c.rects = season.rects;
  std::vector<std::uint64_t> textures;
  for (const auto& row : pictures.rows) textures.insert(textures.end(), row.begin(), row.end());
  detail::AddPictureRects(&c, textures);

  detail::Builder b{&c, {}};
  const float W = kPanelWidth, H = kPanelHeight;
  const std::int32_t root = b.Group("friends_panel", -1, {0, 0, W, H}, {0.5f, 0.5f, 0.5f, 0.0f});
  // A hidden picture the home page's status script writes to (named as on SEASON INFO), with SEASON INFO's own default.
  const std::int32_t bg = b.Sprite("background_image", root, {0, 0, 0, 0}, {0, {0, 0, 1, 1}}, {1, 1, 1, 0});
  std::uint64_t bg_texture = 0;
  if (picture->payload.size() >= 16) std::memcpy(&bg_texture, picture->payload.data() + 8, 8);
  c.elements[static_cast<std::size_t>(bg)].set_texture(bg_texture);
  b.Picture(bg, bg_texture);
  // Banner: a dark bar with slanted ends, a cyan slanted accent at each end, the title centred.
  b.Sprite("banner_accent_l", root, {8, 18, 40, 102}, kCapLeft, kCyan, {0.5f, 0.5f});
  b.Sprite("banner_cap_l", root, {44, 18, 92, 102}, kCapLeft, kBanner, {0.5f, 0.5f});
  b.Group("banner", root, {90, 18, 678, 102}, kBanner);
  b.Sprite("banner_cap_r", root, {676, 18, 724, 102}, kCapRight, kBanner, {0.5f, 0.5f});
  b.Sprite("banner_accent_r", root, {728, 18, 760, 102}, kCapRight, kCyan, {0.5f, 0.5f});
  b.Text("header_txt", root, {90, 18, 678, 102}, kWhite, "FRIENDS", 33, 60, {0.5f, 0.5f});
  for (std::uint32_t i = 0; i < kRows; ++i) {
    const float y = 122.0f + 146.0f * static_cast<float>(i);
    const std::int32_t row = b.Group(detail::RowName("row", i).c_str(), root, {16, y, W - 16, y + 136}, kRow);
    // The sprite starts on a texture the level loads (SEASON INFO's own picture): the level's archive cannot be
    // changed in memory (the game loads it outside the resource callback), so the panel's own textures are not
    // resident when the canvas loads, and a sprite drawing a texture that is not resident crashes the level. The
    // picture behaviour names the row's default; the runtime sets it once the element exists (it loads on demand).
    const std::int32_t av = b.Sprite(detail::RowName("avatar", i).c_str(), row, {8, 8, 128, 128},
                                     {bg_texture, {0, 0, 1, 1}}, {1, 1, 1, 1}, {0.5f, 0.5f});
    b.Picture(av, pictures.rows[i][0]);
    b.Text(detail::RowName("name", i).c_str(), row, {144, 10, W - 40, 76}, kWhite, "", 33, 46, {0.0f, 0.5f});
    b.Text(detail::RowName("status", i).c_str(), row, {144, 76, 520, 128}, kTeal, "", 17, 34, {0.0f, 0.5f});
    b.Text(detail::RowName("mark", i).c_str(), row, {520, 76, W - 48, 128}, kWhite, "", 9, 34, {1.0f, 0.5f});
    // The row is a button: a clear group over it with the stock button behaviours, and its highlight inside.
    const std::int32_t hit = b.Group(detail::RowName("btn", i).c_str(), row, {0, 0, W - 32, 136}, kButtonClear, {0.5f, 0.5f}, true);
    b.ButtonBehaviours(hit, *states, *pointer);
    b.Group(detail::RowName("hl", i).c_str(), hit, {0, 0, W - 32, 136}, kHighlight);
  }
  const std::int32_t play = b.Group("play_btn", root, {16, 878, W - 16, 990}, kButtonClear, {0.5f, 0.5f}, true);
  b.Sprite("play_btn_sprite", play, {0, 0, W - 32, 112}, kButtonFill, kButtonSlate);
  b.Sprite("play_hl", play, {0, 0, W - 32, 112}, kButtonHighlight, kHighlight);
  b.Sprite("play_outline", play, {0, 0, W - 32, 112}, kButtonOutline, kWhite);
  b.Text("play_text", play, {0, 0, W - 32, 112}, kWhite, "INVITE FRIENDS", 51, 44, {0.5f, 0.5f});
  b.ButtonBehaviours(play, *states, *pointer);
  if (!b.error.empty()) return detail::Fail(why, b.error);
  if (!uc::RefreshLayout(&c)) return detail::Fail(why, "an element names a parent after it");
  uc::RefreshHeaderCounts(&c);  // more elements than the stock canvas: its draw buffers must grow with them
  const std::vector<std::string> problems = uc::Problems(c);
  if (!problems.empty()) return detail::Fail(why, problems.front());
  *out = std::move(c);
  return true;
}

// ---- the profile ----

inline constexpr float kProfileWiden = 1.2f;  // the pop-up is 20% wider than the stock one (its pointer mesh too)
inline constexpr Colour kHiddenText{0.98f, 0.98f, 0.96f, 0.0f};
// The tops of INVITE and UNFRIEND in the left column, under the party line.
inline constexpr float kInviteY = 1150.0f;
inline constexpr float kUnfriendY = 1290.0f;

// Builds the friend profile from the delivered SUPPORT & SOCIAL pop-up canvas. False, with `why`, when the delivered
// canvas is not the pop-up this layout knows (its elements by name, its button behaviours).
inline bool BuildFriendProfile(const uc::Canvas& popup, const Pictures& pictures, uc::Canvas* out, std::string* why) {
  uc::Canvas c = popup;
  const auto at = [&c](const char* name) { return c.Find(EchoVR::CSymbol64Hash(name)); };
  const std::int64_t mask = at("mask"), social = at("social"), status = at("bodytext"), party = at("linktext"),
                     body2 = at("bodytext2"), left = at("LeftButton"), rightbg = at("rightbg"),
                     right_region = at("rightinteractregion");
  if (mask < 0 || social < 0 || status < 0 || party < 0 || body2 < 0 || left < 0 || rightbg < 0 || right_region < 0)
    return detail::Fail(why, "the delivered canvas is not the SUPPORT & SOCIAL pop-up this layout knows");
  const std::int32_t right = c.elements[static_cast<std::size_t>(rightbg)].parent();
  const uc::Behaviour* states = nullptr;
  const uc::Behaviour* pointer = nullptr;
  for (const auto& h : c.behaviours) {
    if (static_cast<std::int64_t>(h.element()) != right_region) continue;
    if (h.type() == kButtonStates) states = &h;
    if (h.type() == kPointerInteract) pointer = &h;
  }
  if (right < 0 || states == nullptr || pointer == nullptr)
    return detail::Fail(why, "the delivered pop-up's right button has no button behaviours");
  const uc::Behaviour states_copy = *states, pointer_copy = *pointer;

  // Wider: every stock element's x by 20%; a frame corner or side (at most 70 wide) keeps its size and moves with
  // its side.
  c.set_authored_width(static_cast<std::uint32_t>(std::lround(static_cast<double>(c.authored_width()) * kProfileWiden)));
  for (auto& e : c.elements) {
    uc::Rect o = e.offsets();
    if (o.x1 - o.x0 <= 70.0f) {
      if (o.x1 > 0.0f) {
        const float w = o.x1 - o.x0;
        o.x0 *= kProfileWiden;
        o.x1 = o.x0 + w;
      }
    } else {
      o.x0 *= kProfileWiden;
      o.x1 *= kProfileWiden;
    }
    e.set_layout(e.anchors(), o);
  }
  std::vector<std::uint64_t> textures(pictures.profile.begin(), pictures.profile.end());
  for (const auto& row : pictures.rows) textures.insert(textures.end(), row.begin(), row.end());
  detail::AddPictureRects(&c, textures);

  auto el = [&c](std::int64_t i) -> uc::Element& { return c.elements[static_cast<std::size_t>(i)]; };
  el(rightbg).set_colour(kRed);  // UNFRIEND is red
  // The avatar is the banner's own sprite under its mask, top left, square, framed by four cyan bars.
  const uc::Rect av{150, 210, 710, 770};
  el(mask).set_layout(kTopLeft, av);
  el(social).set_layout(kTopLeft, {0, 0, av.x1 - av.x0, av.y1 - av.y0});
  el(social).set_colour({1, 1, 1, 1});
  // The sprite keeps its stock texture (the level loads it; the panel's own textures load on demand when the runtime
  // sets the picture), and the picture behaviour names the profile's default.
  el(social).set_uv({0, 0, 1, 1});
  c.behaviours.push_back(uc::MakeBehaviour(kDynLoadTexture, kDynLoadTexture, static_cast<std::uint32_t>(social),
                                           uc::Words({kDynLoadTexture, pictures.profile[0]})));
  detail::Builder b{&c, {}};
  const float T = 8.0f;
  b.Group("avatar_frame_top", -1, {av.x0 - T, av.y0 - T, av.x1 + T, av.y0}, kCyan, {0.5f, 0.5f});
  b.Group("avatar_frame_bottom", -1, {av.x0 - T, av.y1, av.x1 + T, av.y1 + T}, kCyan, {0.5f, 0.5f});
  b.Group("avatar_frame_left", -1, {av.x0 - T, av.y0, av.x0, av.y1}, kCyan, {0.5f, 0.5f});
  b.Group("avatar_frame_right", -1, {av.x1, av.y0, av.x1 + T, av.y1}, kCyan, {0.5f, 0.5f});
  const auto text_at = [&el](std::int64_t i, const uc::Rect& r, const Colour& colour, std::uint32_t size) {
    el(i).set_layout(kTopLeft, r);
    el(i).set_colour(colour);
    el(i).set_font_size(size);
    el(i).set_alignment(0.5f, 0.5f);
  };
  text_at(status, {110, 800, 750, 890}, kTeal, 50);
  text_at(party, {110, 900, 750, 980}, kWhite, 42);
  // The stock pop-up's second body line ("Looking for help instead?...") is written by the page script when the pop-up
  // opens, so a text the runtime writes there does not stay: the line is drawn transparent, and the buttons sit below it.
  text_at(body2, {110, 990, 750, 1050}, kHiddenText, 36);
  // INVITE (the left button) and UNFRIEND (the right one) in the left column under the party line; CLOSE stays at the
  // bottom centre. A button's parts are laid out from its centre, so moving its group moves it whole.
  const std::array<std::pair<std::int64_t, float>, 2> moved = {{{left, kInviteY}, {right, kUnfriendY}}};
  for (const auto& m : moved) {
    const uc::Rect o = el(m.first).offsets();
    const float bw = o.x1 - o.x0, bh = o.y1 - o.y0, x = (150.0f + 710.0f) / 2.0f - bw / 2.0f;
    el(m.first).set_layout(kTopLeft, {x, m.second, x + bw, m.second + bh});
  }
  // Right column: the ARENA / COMBAT switch (no fill: a tab is its label and its highlight, so emptying the label hides
  // it), a cyan rule, eight stat lines.
  const float X0 = 800.0f, X1 = 2390.0f, MID = (X0 + X1) / 2.0f;
  const std::array<std::pair<const char*, std::pair<float, float>>, 2> tabs = {
      {{"arena", {X0, MID - 10.0f}}, {"combat", {MID + 10.0f, X1}}}};
  for (const auto& tab : tabs) {
    const std::string stem = std::string("tab_") + tab.first;
    const float a = tab.second.first, z = tab.second.second;
    const std::int32_t g = b.Group(stem.c_str(), -1, {a, 210, z, 300}, kClear, {0.5f, 0.5f}, true);
    b.ButtonBehaviours(g, states_copy, pointer_copy);
    b.Group((stem + "_hl").c_str(), g, {0, 0, z - a, 90}, kTabHighlight);
    std::string label = tab.first;
    std::transform(label.begin(), label.end(), label.begin(), [](char ch) { return static_cast<char>(ch - 32); });
    b.Text((stem + "_txt").c_str(), g, {0, 0, z - a, 90}, kWhite, label, 17, 42, {0.5f, 0.5f});
  }
  b.Group("stats_rule", -1, {X0, 310, X1, 316}, kCyan, {0.5f, 0.5f});
  for (std::uint32_t i = 0; i < kStats; ++i) {
    const float y = 345.0f + 135.0f * static_cast<float>(i);
    const std::string stem = "stat" + std::to_string(i);
    b.Text((stem + "_label").c_str(), -1, {X0 + 10, y, 1800, y + 120}, kWhite, "", 25, 48, {0.0f, 0.5f});
    b.Text((stem + "_value").c_str(), -1, {1800, y, X1 - 10, y + 120}, kWhite, "", 17, 48, {1.0f, 0.5f});
  }
  if (!b.error.empty()) return detail::Fail(why, b.error);
  if (!uc::RefreshLayout(&c)) return detail::Fail(why, "an element names a parent after it");
  uc::RefreshHeaderCounts(&c);  // more elements than the stock canvas: its draw buffers must grow with them
  const std::vector<std::string> problems = uc::Problems(c);
  if (!problems.empty()) return detail::Fail(why, problems.front());
  *out = std::move(c);
  return true;
}

}  // namespace nevr_friends_canvas
