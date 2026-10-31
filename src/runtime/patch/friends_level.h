#pragma once

// #1535 / #1599: the main-menu level's changes for the FRIENDS panel, made in memory on the tables the game delivers.
//
// The panel is the SEASON INFO actor drawing the panel canvas (friends_canvas.h rebuilds ui_mnu_season_info itself, so
// no canvas id changes); the friend profile is the SUPPORT & SOCIAL pop-up actor. Per table of the level (each a
// resource named after the level):
//   CR15PointerMeshCR    the panel's pointer mesh 1.35 x 2.40 m (stock 2.1 x 1.05), the UPDATE NEWS and SUPPORT & SOCIAL
//                        cards' 0 x 0 (they draw nothing), the pop-up's 20% wider (stock 3.8 x 2.982);
//   CTransformCR         the panel placed beside the cards, the two cards moved far below the view;
//   CActorDataResource   the same, in the actors' local transforms;
//   CR15UIPage2ElementCR the panel's page element id is friends_panel (SEASON INFO's would make the home page script
//                        open the season page on its clicks);
//   CScriptCR            the panel's one ui_mnu_button_pointermesh instance (the whole panel one button) becomes one
//                        per row and one for INVITE FRIENDS, each with its own element, highlight, click event and
//                        tooltip; the pop-up gets two for its ARENA / COMBAT switch.
// Each edit checks the stock values it changes and refuses a table that does not hold them; the result is checked again.
//
// Flat component tables: a 56-byte header (+8 the entries' byte size, +40 capacity, +48 count), then the entries; an
// entry's +8 is its actor. CScriptCR's entries are 720 bytes with twelve inline-array descriptors (at +48 + 56k: +8 byte
// size, +40 capacity, +48 count); after the entries come their inline arrays, entry by entry, descriptor by descriptor.
// A script instance's settings (inline arrays, ui_mnu_button_pointermesh):
//   +328 symbol settings (key, -1, value): element 0x6ee9c71670a8d362, highlight 0x63ecc1157cad8562, label
//        0x6ee997457faf8030 (SEASON INFO's: the symbol <invalid>, none), click event 0x63becd132df98137;
//   +496 string settings (key, -1, 64 bytes): tooltip 0x63ee96162dae8535, and 0x60e8cd412ba7863e (empty on SEASON INFO);
//   +552 actor settings (key, -1, actor, 0): 0x65e992167cfa8534, the instance's own actor.
// CActorDataResource: a 784-byte header of 13 array descriptors (count at +48 of each; two of 64 bytes, six of 56,
// five of 64), then the arrays in order: headers (16 bytes), components (64 bytes each, then each one's u16 actor
// list), 8-aligned node ids (u64, the actor ids), names, prefab ids (u64), local transforms (48 bytes: rotation x, y, z,
// w, position x, y, z, scale, padding).
//
// Pure byte logic, header only, no platform headers, no exceptions (the Quest library compiles it). Tests:
// src/runtime/tests/test_friends_level.cpp.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "nevr/symbol_hash.h"
#include "runtime/patch/menu_cards.h"

namespace nevr_friends_level {

using Bytes = std::vector<std::uint8_t>;

// The level and its actors (CSymbol64 ids, the tables' +8).
inline constexpr std::uint64_t kLevel = 0xAC360E41E4EDE056ULL;    // mnu_master (PC)
inline constexpr std::uint64_t kQuestLevel = EchoVR::CSymbol64Hash("mnu_master_lowspec");  // the Quest's main menu: the same
                                                // tables, actors and values as PC's (headset data, 2026-10)
inline constexpr std::uint64_t kPanel = 0x9E0BF5FE92ABA570ULL;    // SEASON INFO card: the FRIENDS panel
inline constexpr std::uint64_t kNews = 0x27DFDB284B22A843ULL;     // UPDATE NEWS card: hidden
inline constexpr std::uint64_t kSocial = 0x12BFDA55B0B0AF9FULL;   // SUPPORT & SOCIAL card: hidden
inline constexpr std::uint64_t kPopup = 0x3DC5205E08A99CA4ULL;    // its pop-up: the friend profile

inline constexpr std::uint64_t kPointerMeshType = EchoVR::CSymbol64Hash("CR15PointerMeshCR");
inline constexpr std::uint64_t kTransformType = EchoVR::CSymbol64Hash("CTransformCR");
inline constexpr std::uint64_t kActorDataType = EchoVR::CSymbol64Hash("CActorDataResource");
inline constexpr std::uint64_t kPageElementType = EchoVR::CSymbol64Hash("CR15UIPage2ElementCR");
inline constexpr std::uint64_t kScriptType = EchoVR::CSymbol64Hash("CScriptCR");
// A transform is keyed by the logical type; the game reads each table by Make(<logical>, "Win10") (resource_buffer_key.h).
static_assert(EchoVR::CSymbol64Hash("Win10", kPointerMeshType) == 0x2972B798AA1CF688ULL &&
                  EchoVR::CSymbol64Hash("Win10", kTransformType) == 0x92ABD3E1432BF5E8ULL &&
                  EchoVR::CSymbol64Hash("Win10", kActorDataType) == 0x347869CE492DC7DAULL &&
                  EchoVR::CSymbol64Hash("Win10", kPageElementType) == 0xC1B1FCA890787B30ULL &&
                  EchoVR::CSymbol64Hash("Win10", kScriptType) == 0xD99F6BBD8009C92CULL,
              "the level tables' on-disk types");

// Placement. The panel mirrors the left column (x +4.1, z 4.4, rotation (0, 0.9135, 0, -0.4067)), nearer the cards
// (it is wider), centred on them; the hidden cards go 60 m down.
inline constexpr std::array<float, 2> kPanelMesh{1.35f, 2.40f};
inline constexpr std::array<float, 2> kSeasonStockMesh{2.1f, 1.05f};
inline constexpr std::array<float, 2> kPopupStockMesh{3.8f, 2.982f};
inline constexpr float kPopupWiden = 1.2f;
inline constexpr std::array<float, 3> kPanelPosition{-3.6f, -0.275f, 4.4f};
inline constexpr std::array<float, 4> kPanelRotation{0.0f, 0.9135f, 0.0f, 0.4067f};
inline constexpr float kHiddenY = -60.0f;

inline constexpr std::uint64_t kSeasonPageElement = 0x0D021E1B8759BC99ULL;
inline constexpr std::uint64_t kPanelPageElement = EchoVR::CSymbol64Hash("friends_panel");

// Script instances.
inline constexpr std::uint64_t kButtonScript = EchoVR::CSymbol64Hash("ui_mnu_button_pointermesh");
static_assert(kButtonScript == 0xE2868929D1ED191AULL, "ui_mnu_button_pointermesh");
inline constexpr std::uint64_t kKeyElement = 0x6EE9C71670A8D362ULL, kKeyHighlight = 0x63ECC1157CAD8562ULL,
                               kKeyLabel = 0x6EE997457FAF8030ULL, kKeyEvent = 0x63BECD132DF98137ULL,
                               kKeyTooltip = 0x63EE96162DAE8535ULL, kKeyText2 = 0x60E8CD412BA7863EULL,
                               kKeyActor = 0x65E992167CFA8534ULL;
inline constexpr std::uint64_t kNoLabel = 0x558C21A9B108687AULL;  // "<invalid>"
inline constexpr std::size_t kScriptEntry = 720;
inline constexpr std::array<std::size_t, 12> kInline = {48, 104, 160, 216, 272, 328, 384, 440, 496, 552, 608, 664};

struct Button {
  const char* element;
  const char* highlight;
  const char* event;
  const char* tooltip;
};
inline constexpr std::array<Button, 6> kPanelButtons = {{
    {"friend0_btn", "friend0_hl", "evt_friend0_btn_clicked", "See this friend"},
    {"friend1_btn", "friend1_hl", "evt_friend1_btn_clicked", "See this friend"},
    {"friend2_btn", "friend2_hl", "evt_friend2_btn_clicked", "See this friend"},
    {"friend3_btn", "friend3_hl", "evt_friend3_btn_clicked", "See this friend"},
    {"friend4_btn", "friend4_hl", "evt_friend4_btn_clicked", "See this friend"},
    {"play_btn", "play_hl", "evt_friends_invite_btn_clicked", "Invite friends to your party"},
}};
inline constexpr std::array<Button, 2> kProfileButtons = {{
    {"tab_arena", "tab_arena_hl", "evt_nevr_profile_tab_arena", "Show arena stats"},
    {"tab_combat", "tab_combat_hl", "evt_nevr_profile_tab_combat", "Show combat stats"},
}};

namespace detail {

template <typename T>
T Get(const Bytes& b, std::size_t at) {
  T v{};
  std::memcpy(&v, b.data() + at, sizeof(v));
  return v;
}
template <typename T>
void Put(Bytes* b, std::size_t at, T v) {
  std::memcpy(b->data() + at, &v, sizeof(v));
}
inline bool Fail(std::string* why, const char* what) {
  if (why != nullptr) *why = what;
  return false;
}
inline bool Near(float a, float b) { return a - b < 1e-4f && b - a < 1e-4f; }

// A flat table's entry size and count; false when the header and the length disagree.
inline bool Flat(const Bytes& b, std::size_t* entry, std::size_t* count) {
  if (b.size() < 56) return false;
  const auto bytes = Get<std::uint64_t>(b, 8), capacity = Get<std::uint64_t>(b, 40), n = Get<std::uint64_t>(b, 48);
  if (capacity == 0 || bytes % capacity != 0 || n > capacity) return false;
  *entry = static_cast<std::size_t>(bytes / capacity);
  *count = static_cast<std::size_t>(n);
  return *entry >= 16 && 56 + *entry * *count <= b.size();
}

// The offsets of every entry of `actor`.
inline std::vector<std::size_t> EntriesOf(const Bytes& b, std::size_t entry, std::size_t count, std::uint64_t actor) {
  std::vector<std::size_t> out;
  for (std::size_t k = 0; k < count; ++k)
    if (Get<std::uint64_t>(b, 56 + entry * k + 8) == actor) out.push_back(56 + entry * k);
  return out;
}

}  // namespace detail

// ---- CR15PointerMeshCR (80-byte entries: +32 width, +36 height) ----
inline bool EditPointerMeshes(const Bytes& in, Bytes* out, std::string* why) {
  using namespace detail;
  std::size_t entry = 0, count = 0;
  if (!Flat(in, &entry, &count) || entry < 40) return Fail(why, "not a flat pointer-mesh table");
  Bytes b = in;
  const auto panel = EntriesOf(b, entry, count, kPanel), popup = EntriesOf(b, entry, count, kPopup);
  const auto news = EntriesOf(b, entry, count, kNews), social = EntriesOf(b, entry, count, kSocial);
  if (panel.size() != 1 || popup.size() != 1 || news.size() != 1 || social.size() != 1)
    return Fail(why, "the actors do not have one pointer mesh each");
  if (!Near(Get<float>(b, panel[0] + 32), kSeasonStockMesh[0]) || !Near(Get<float>(b, panel[0] + 36), kSeasonStockMesh[1]) ||
      !Near(Get<float>(b, popup[0] + 32), kPopupStockMesh[0]) || !Near(Get<float>(b, popup[0] + 36), kPopupStockMesh[1]))
    return Fail(why, "the pointer meshes are not the stock sizes");
  Put(&b, panel[0] + 32, kPanelMesh);
  Put(&b, popup[0] + 32, Get<float>(b, popup[0] + 32) * kPopupWiden);
  for (const std::size_t at : {news[0], social[0]}) Put(&b, at + 32, std::array<float, 2>{0.0f, 0.0f});
  *out = std::move(b);
  return true;
}

// ---- CTransformCR (176-byte entries: +0x20 rotation x, y, z, w; +0x30 position x, y, z) ----
inline bool EditTransforms(const Bytes& in, Bytes* out, std::string* why) {
  using namespace detail;
  std::size_t entry = 0, count = 0;
  if (!Flat(in, &entry, &count) || entry < 0x3C) return Fail(why, "not a flat transform table");
  Bytes b = in;
  const auto panel = EntriesOf(b, entry, count, kPanel), news = EntriesOf(b, entry, count, kNews),
             social = EntriesOf(b, entry, count, kSocial);
  if (panel.empty() || news.empty() || social.empty()) return Fail(why, "an actor has no transform");
  for (const std::size_t at : panel) {
    Put(&b, at + 0x20, kPanelRotation);
    Put(&b, at + 0x30, kPanelPosition);
  }
  for (const auto& list : {news, social})
    for (const std::size_t at : list) Put(&b, at + 0x34, kHiddenY);
  *out = std::move(b);
  return true;
}

// The main menu's transform table carries two edits: the four buttons' order (#1679, nevr_menu_cards::PlaceButtons,
// which serves only the pinned stock table) and the panel's (EditTransforms, which moves other actors). Transforms of one
// resource do not chain (resource_override.h: the first that serves wins; the Quest's read-finished hook serves once), so
// the panel's edit is made on the table with the buttons placed. `sha256Hex` is the delivered table's, `pin` the stock
// table's; `buttons_placed` says whether the buttons moved (false on a table that is not the pinned stock one, which the
// panel still edits). False, with `why`, when the panel's edit refuses the table.
inline bool EditTransformsWithButtons(const Bytes& in, const std::string& sha256Hex, const std::string& pin, Bytes* out,
                                      std::string* why, bool* buttons_placed) {
  const nevr_menu_cards::SlotResult r = nevr_menu_cards::PlaceButtons(in.data(), in.size(), sha256Hex, pin);
  const bool placed = r.verdict == nevr_menu_cards::Verdict::Patched;
  if (buttons_placed != nullptr) *buttons_placed = placed;
  return EditTransforms(placed ? r.bytes : in, out, why);
}

// ---- CActorDataResource: the actors' local transforms ----
inline bool EditActorData(const Bytes& in, Bytes* out, std::string* why) {
  using namespace detail;
  constexpr std::size_t kHeader = 784;
  if (in.size() < kHeader) return Fail(why, "shorter than the actor data header");
  // Descriptor offsets: headers 0, components 64 (both 64 bytes), node ids 128, names 184, prefab ids 240, transforms 296.
  const auto count = [&in](std::size_t d) { return Get<std::uint64_t>(in, d + 48); };
  const std::uint64_t headers = count(0), components = count(64), actors = count(128);
  if (count(184) != actors || count(240) != actors || count(296) != actors) return Fail(why, "the actor arrays disagree");
  std::size_t at = kHeader + 16 * headers;
  std::uint64_t indices = 0;
  for (std::uint64_t k = 0; k < components; ++k) {
    if (at + 64 * (k + 1) > in.size()) return Fail(why, "the components run past the end");
    indices += Get<std::uint64_t>(in, at + 64 * k + 8 + 48);
  }
  at += 64 * components + 2 * indices;
  at = (at + 7) / 8 * 8;
  const std::size_t ids = at, transforms = ids + 3 * 8 * actors;
  if (transforms + 48 * actors > in.size()) return Fail(why, "the transforms run past the end");
  const auto index_of = [&](std::uint64_t actor) -> std::int64_t {
    for (std::uint64_t i = 0; i < actors; ++i)
      if (Get<std::uint64_t>(in, ids + 8 * i) == actor) return static_cast<std::int64_t>(i);
    return -1;
  };
  const std::int64_t panel = index_of(kPanel), news = index_of(kNews), social = index_of(kSocial);
  if (panel < 0 || news < 0 || social < 0) return Fail(why, "an actor is not in the actor data");
  Bytes b = in;
  const std::size_t p = transforms + 48 * static_cast<std::size_t>(panel);
  Put(&b, p, kPanelRotation);
  Put(&b, p + 16, kPanelPosition);
  for (const std::int64_t i : {news, social}) Put(&b, transforms + 48 * static_cast<std::size_t>(i) + 16 + 4, kHiddenY);
  *out = std::move(b);
  return true;
}

// ---- CR15UIPage2ElementCR (96-byte entries: +88 the page element id) ----
inline bool EditPageElements(const Bytes& in, Bytes* out, std::string* why) {
  using namespace detail;
  std::size_t entry = 0, count = 0;
  if (!Flat(in, &entry, &count) || entry != 96) return Fail(why, "not a page element table");
  const auto panel = EntriesOf(in, entry, count, kPanel);
  if (panel.size() != 1 || Get<std::uint64_t>(in, panel[0] + 88) != kSeasonPageElement)
    return Fail(why, "the panel's page element is not SEASON INFO's");
  Bytes b = in;
  Put(&b, panel[0] + 88, kPanelPageElement);
  *out = std::move(b);
  return true;
}

// ---- CScriptCR ----

namespace detail {

struct Script {
  Bytes flat;   // 720 bytes
  Bytes inline_bytes;
};

inline bool SplitScripts(const Bytes& b, std::vector<Script>* out) {
  std::size_t entry = 0, count = 0;
  if (!Flat(b, &entry, &count) || entry != kScriptEntry) return false;
  std::size_t pos = 56 + entry * count;
  for (std::size_t k = 0; k < count; ++k) {
    const std::size_t e = 56 + entry * k;
    std::size_t n = 0;
    for (const std::size_t d : kInline)
      if (Get<std::uint64_t>(b, e + d + 48) != 0) n += static_cast<std::size_t>(Get<std::uint64_t>(b, e + d + 8));
    if (pos + n > b.size()) return false;
    out->push_back({Bytes(b.begin() + static_cast<std::ptrdiff_t>(e), b.begin() + static_cast<std::ptrdiff_t>(e + entry)),
                    Bytes(b.begin() + static_cast<std::ptrdiff_t>(pos), b.begin() + static_cast<std::ptrdiff_t>(pos + n))});
    pos += n;
  }
  return pos == b.size();
}

inline Bytes JoinScripts(const Bytes& header, const std::vector<Script>& scripts) {
  Bytes out(header.begin(), header.begin() + 56);
  Put<std::uint64_t>(&out, 8, kScriptEntry * scripts.size());
  Put<std::uint64_t>(&out, 40, scripts.size());
  Put<std::uint64_t>(&out, 48, scripts.size());
  for (const auto& s : scripts) out.insert(out.end(), s.flat.begin(), s.flat.end());
  for (const auto& s : scripts) out.insert(out.end(), s.inline_bytes.begin(), s.inline_bytes.end());
  return out;
}

inline void Append16(Bytes* b, std::uint64_t key, std::uint64_t value) {
  const std::size_t at = b->size();
  b->resize(at + 24);
  Put<std::uint64_t>(b, at, key);
  Put<std::int64_t>(b, at + 8, -1);
  Put<std::uint64_t>(b, at + 16, value);
}
inline void AppendString(Bytes* b, std::uint64_t key, const char* text) {
  const std::size_t at = b->size();
  b->resize(at + 80, 0);
  Put<std::uint64_t>(b, at, key);
  Put<std::int64_t>(b, at + 8, -1);
  std::strncpy(reinterpret_cast<char*>(b->data() + at + 16), text, 63);
}

// One ui_mnu_button_pointermesh instance of `actor` for `button`, from the stock instance `stock` (SEASON INFO's): its
// name, actor and settings replaced, every other byte as the stock instance has it.
inline Script ButtonInstance(const Bytes& stock, std::uint64_t actor, const Button& button) {
  Script s{stock, {}};
  Put<std::uint64_t>(&s.flat, 0, EchoVR::CSymbol64Hash((std::string(button.element) + "_pointermesh").c_str()));
  Put<std::uint64_t>(&s.flat, 8, actor);
  Bytes symbols, strings, actors;
  Append16(&symbols, kKeyElement, EchoVR::CSymbol64Hash(button.element));
  Append16(&symbols, kKeyHighlight, EchoVR::CSymbol64Hash(button.highlight));
  Append16(&symbols, kKeyLabel, kNoLabel);
  Append16(&symbols, kKeyEvent, EchoVR::CSymbol64Hash(button.event));
  AppendString(&strings, kKeyTooltip, button.tooltip);
  AppendString(&strings, kKeyText2, "");
  actors.resize(32, 0);
  Put<std::uint64_t>(&actors, 0, kKeyActor);
  Put<std::int64_t>(&actors, 8, -1);
  Put<std::uint64_t>(&actors, 16, actor);
  for (const std::size_t d : kInline) {
    const Bytes* arr = d == 328 ? &symbols : d == 496 ? &strings : d == 552 ? &actors : nullptr;
    const std::uint64_t n = d == 328 ? 4 : d == 496 ? 2 : d == 552 ? 1 : 0;
    Put<std::uint64_t>(&s.flat, d + 8, arr != nullptr ? arr->size() : 0);
    Put<std::uint64_t>(&s.flat, d + 40, n);
    Put<std::uint64_t>(&s.flat, d + 48, n);
  }
  s.inline_bytes = symbols;
  s.inline_bytes.insert(s.inline_bytes.end(), strings.begin(), strings.end());
  s.inline_bytes.insert(s.inline_bytes.end(), actors.begin(), actors.end());
  return s;
}

}  // namespace detail

inline bool EditScripts(const Bytes& in, Bytes* out, std::string* why) {
  using namespace detail;
  std::vector<Script> scripts;
  if (!SplitScripts(in, &scripts)) return Fail(why, "not a script table");
  std::size_t panel_at = scripts.size(), popup_first = scripts.size(), panel_count = 0;
  for (std::size_t k = 0; k < scripts.size(); ++k) {
    const std::uint64_t actor = Get<std::uint64_t>(scripts[k].flat, 8);
    const std::uint64_t script = Get<std::uint64_t>(scripts[k].flat, 32);
    if (actor == kPanel && script == kButtonScript) {
      panel_at = k;
      ++panel_count;
    }
    if (actor == kPopup && popup_first == scripts.size()) popup_first = k;
  }
  if (panel_count != 1 || popup_first == scripts.size()) return Fail(why, "the panel's button instance or the pop-up is not there");
  // SEASON INFO's instance: three symbol settings, two strings, the actor itself.
  const Bytes stock = scripts[panel_at].flat;
  if (Get<std::uint64_t>(stock, 328 + 48) != 3 || Get<std::uint64_t>(stock, 496 + 48) != 2 ||
      Get<std::uint64_t>(stock, 552 + 48) != 1)
    return Fail(why, "the panel's button instance is not SEASON INFO's");
  std::vector<Script> panel, tabs;
  for (const auto& button : kPanelButtons) panel.push_back(ButtonInstance(stock, kPanel, button));
  for (const auto& button : kProfileButtons) tabs.push_back(ButtonInstance(stock, kPopup, button));
  // Entries stay in actor order: the panel's in place of its one, the switch's before the pop-up's own (as stock
  // actors order them: button instances first).
  std::vector<Script> result;
  for (std::size_t k = 0; k < scripts.size(); ++k) {
    if (k == popup_first) result.insert(result.end(), tabs.begin(), tabs.end());
    if (k == panel_at) {
      result.insert(result.end(), panel.begin(), panel.end());
      continue;
    }
    result.push_back(scripts[k]);
  }
  Bytes b = JoinScripts(in, result);
  std::vector<Script> check;
  if (!SplitScripts(b, &check) || check.size() != scripts.size() + panel.size() + tabs.size() - 1)
    return Fail(why, "the edited script table does not read back");
  *out = std::move(b);
  return true;
}

}  // namespace nevr_friends_level
