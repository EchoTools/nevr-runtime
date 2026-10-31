// #1535 / #1599: the main-menu level's changes for the FRIENDS panel (friends_level.h). The tables are written here in
// the documented format with the four actors; the stock tables (game data, not in the repository) are read from
// NEVR_MNU_MASTER_DIR (<dir>/<TypeWin10>/mnu_master.bin) when it is set.

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "nevr/symbol_hash.h"
#include "runtime/patch/friends_level.h"

namespace {

namespace fl = nevr_friends_level;
using fl::Bytes;
using F2 = std::array<float, 2>;
using F3 = std::array<float, 3>;
using F4 = std::array<float, 4>;

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

// A flat table: one `entry`-byte entry per actor, filled by `fill`.
template <typename Fill>
Bytes FlatTable(std::size_t entry, const std::vector<std::uint64_t>& actors, Fill fill) {
  Bytes b(56 + entry * actors.size(), 0);
  Put<std::uint64_t>(&b, 8, entry * actors.size());
  Put<std::uint64_t>(&b, 40, actors.size());
  Put<std::uint64_t>(&b, 48, actors.size());
  for (std::size_t k = 0; k < actors.size(); ++k) {
    Put<std::uint64_t>(&b, 56 + entry * k + 8, actors[k]);
    fill(&b, 56 + entry * k, actors[k]);
  }
  return b;
}

const std::vector<std::uint64_t> kActors = {0x1111, fl::kNews, fl::kSocial, fl::kPanel, 0x2222, fl::kPopup};

Bytes StockMeshes() {
  return FlatTable(80, kActors, [](Bytes* b, std::size_t at, std::uint64_t actor) {
    std::array<float, 2> size{1.0f, 1.0f};
    if (actor == fl::kPanel) size = fl::kSeasonStockMesh;
    if (actor == fl::kPopup) size = fl::kPopupStockMesh;
    Put(b, at + 32, size);
  });
}

}  // namespace

TEST(FriendsLevel, PointerMeshesSizeThePanelHideTheCardsAndWidenThePopup) {
  Bytes out;
  std::string why;
  ASSERT_TRUE(fl::EditPointerMeshes(StockMeshes(), &out, &why)) << why;
  const auto size = [&out](std::size_t k) { return Get<F2>(out, 56 + 80 * k + 32); };
  EXPECT_EQ(size(3), fl::kPanelMesh);
  EXPECT_FLOAT_EQ(size(5)[0], 3.8f * 1.2f);
  EXPECT_FLOAT_EQ(size(5)[1], 2.982f);
  EXPECT_EQ(size(1), (F2{0.0f, 0.0f}));
  EXPECT_EQ(size(2), (F2{0.0f, 0.0f}));
  EXPECT_EQ(size(0), (F2{1.0f, 1.0f}));  // other actors untouched
}

TEST(FriendsLevel, PointerMeshesRefuseATableThatIsNotStock) {
  Bytes b = StockMeshes(), out;
  Put(&b, 56 + 80 * 3 + 32, 1.35f);  // the panel's mesh already changed
  std::string why;
  EXPECT_FALSE(fl::EditPointerMeshes(b, &out, &why));
  EXPECT_FALSE(fl::EditPointerMeshes(Bytes(10, 0), &out, &why));
}

TEST(FriendsLevel, TransformsPlaceThePanelAndSinkTheCards) {
  const Bytes in = FlatTable(176, kActors, [](Bytes* b, std::size_t at, std::uint64_t) {
    Put(b, at + 0x30, std::array<float, 3>{1.0f, 2.0f, 3.0f});
  });
  Bytes out;
  ASSERT_TRUE(fl::EditTransforms(in, &out, nullptr));
  EXPECT_EQ(Get<F3>(out, 56 + 176 * 3 + 0x30), fl::kPanelPosition);
  EXPECT_EQ(Get<F4>(out, 56 + 176 * 3 + 0x20), fl::kPanelRotation);
  EXPECT_EQ(Get<float>(out, 56 + 176 * 1 + 0x34), fl::kHiddenY);
  EXPECT_EQ(Get<float>(out, 56 + 176 * 1 + 0x30), 1.0f);  // only y moves
  EXPECT_EQ(Get<float>(out, 56 + 176 * 2 + 0x34), fl::kHiddenY);
  EXPECT_EQ(Get<float>(out, 56 + 176 * 0 + 0x34), 2.0f);
}

TEST(FriendsLevel, PageElementBecomesFriendsPanelAndOnlyFromSeasonInfo) {
  const Bytes in = FlatTable(96, kActors, [](Bytes* b, std::size_t at, std::uint64_t actor) {
    Put<std::uint64_t>(b, at + 88, actor == fl::kPanel ? fl::kSeasonPageElement : 0x77);
  });
  Bytes out;
  ASSERT_TRUE(fl::EditPageElements(in, &out, nullptr));
  EXPECT_EQ(Get<std::uint64_t>(out, 56 + 96 * 3 + 88), EchoVR::CSymbol64Hash("friends_panel"));
  EXPECT_EQ(Get<std::uint64_t>(out, 56 + 96 * 0 + 88), 0x77u);
  EXPECT_FALSE(fl::EditPageElements(out, &out, nullptr));  // not SEASON INFO's any more
}

namespace {

// Actor data: headers (1), components (1, listing 3 actor indexes), then the four u64 arrays and the transforms.
Bytes ActorData(const std::vector<std::uint64_t>& actors) {
  const std::size_t n = actors.size();
  Bytes b(784, 0);
  const auto set_count = [&b](std::size_t d, std::uint64_t c) { Put<std::uint64_t>(&b, d + 48, c); };
  set_count(0, 1);
  set_count(64, 1);
  for (const std::size_t d : {128, 184, 240, 296}) set_count(d, n);
  b.resize(b.size() + 16 + 64, 0);
  Put<std::uint64_t>(&b, 784 + 16 + 8 + 48, 3);  // the component lists three actors
  b.resize(b.size() + 6, 0);                       // three u16
  b.resize((b.size() + 7) / 8 * 8, 0);
  const std::size_t ids = b.size();
  b.resize(ids + 24 * n + 48 * n, 0);
  for (std::size_t i = 0; i < n; ++i) Put<std::uint64_t>(&b, ids + 8 * i, actors[i]);
  return b;
}

}  // namespace

TEST(FriendsLevel, ActorDataMovesTheActorsLocalTransforms) {
  const Bytes in = ActorData(kActors);
  Bytes out;
  std::string why;
  ASSERT_TRUE(fl::EditActorData(in, &out, &why)) << why;
  const std::size_t transforms = in.size() - 48 * kActors.size();
  EXPECT_EQ(Get<F4>(out, transforms + 48 * 3), fl::kPanelRotation);
  EXPECT_EQ(Get<F3>(out, transforms + 48 * 3 + 16), fl::kPanelPosition);
  EXPECT_EQ(Get<float>(out, transforms + 48 * 1 + 20), fl::kHiddenY);
  EXPECT_EQ(Get<float>(out, transforms + 48 * 2 + 20), fl::kHiddenY);
  EXPECT_EQ(Get<float>(out, transforms + 48 * 0 + 20), 0.0f);
  const Bytes missing = ActorData({0x1111, fl::kNews, fl::kSocial});
  EXPECT_FALSE(fl::EditActorData(missing, &out, &why));
}

namespace {

// A script table: an entry for 0x1111 with one symbol setting; SEASON INFO's button instance (3 symbols, 2 strings, 1
// actor); a page-element script on the panel (no settings); two pop-up scripts.
Bytes Scripts() {
  struct E {
    std::uint64_t actor, script;
    std::size_t symbols, strings, actors;
  };
  const std::vector<E> entries = {{0x1111, 0xAA, 1, 0, 0}, {fl::kPanel, fl::kButtonScript, 3, 2, 1},
                                  {fl::kPanel, 0x51BBBACDD168CA36ULL, 0, 0, 0}, {fl::kPopup, 0x77A7, 0, 0, 0},
                                  {fl::kPopup, 0x51BB, 0, 0, 0}};
  Bytes b(56 + 720 * entries.size(), 0);
  Put<std::uint64_t>(&b, 8, 720 * entries.size());
  Put<std::uint64_t>(&b, 40, entries.size());
  Put<std::uint64_t>(&b, 48, entries.size());
  Bytes tail;
  for (std::size_t k = 0; k < entries.size(); ++k) {
    const std::size_t e = 56 + 720 * k;
    Put<std::uint64_t>(&b, e + 8, entries[k].actor);
    Put<std::uint64_t>(&b, e + 32, entries[k].script);
    const std::pair<std::size_t, std::pair<std::size_t, std::size_t>> arrays[] = {
        {328, {entries[k].symbols, 24}}, {496, {entries[k].strings, 80}}, {552, {entries[k].actors, 32}}};
    for (const auto& a : arrays) {
      Put<std::uint64_t>(&b, e + a.first + 8, a.second.first * a.second.second);
      Put<std::uint64_t>(&b, e + a.first + 40, a.second.first);
      Put<std::uint64_t>(&b, e + a.first + 48, a.second.first);
      tail.resize(tail.size() + a.second.first * a.second.second, static_cast<std::uint8_t>(k + 1));
    }
  }
  b.insert(b.end(), tail.begin(), tail.end());
  return b;
}

}  // namespace

TEST(FriendsLevel, ScriptsGiveEachRowAndTheSwitchItsOwnButtonInstance) {
  Bytes out;
  std::string why;
  ASSERT_TRUE(fl::EditScripts(Scripts(), &out, &why)) << why;
  const std::uint64_t count = Get<std::uint64_t>(out, 48);
  ASSERT_EQ(count, 5u - 1u + 6u + 2u);
  std::vector<std::uint64_t> actors, events;
  std::size_t tail = 56 + 720 * count;
  for (std::size_t k = 0; k < count; ++k) {
    const std::size_t e = 56 + 720 * k;
    actors.push_back(Get<std::uint64_t>(out, e + 8));
    if (Get<std::uint64_t>(out, e + 32) == fl::kButtonScript) {
      // settings: element, highlight, label, event; the tooltip; the instance's own actor.
      EXPECT_EQ(Get<std::uint64_t>(out, e + 328 + 48), 4u);
      EXPECT_EQ(Get<std::uint64_t>(out, tail + 0), fl::kKeyElement);
      EXPECT_EQ(Get<std::uint64_t>(out, tail + 24 * 2 + 16), fl::kNoLabel);
      events.push_back(Get<std::uint64_t>(out, tail + 24 * 3 + 16));
      EXPECT_EQ(Get<std::uint64_t>(out, tail + 96), fl::kKeyTooltip);
      EXPECT_EQ(Get<std::uint64_t>(out, tail + 96 + 160 + 16), actors.back());  // its own actor
    }
    for (const std::size_t d : fl::kInline)
      if (Get<std::uint64_t>(out, e + d + 48) != 0) tail += Get<std::uint64_t>(out, e + d + 8);
  }
  EXPECT_EQ(tail, out.size());  // the inline arrays read back to the end
  const std::vector<std::uint64_t> want_actors = {0x1111, fl::kPanel, fl::kPanel, fl::kPanel, fl::kPanel, fl::kPanel,
                                                  fl::kPanel, fl::kPanel, fl::kPopup, fl::kPopup, fl::kPopup, fl::kPopup};
  EXPECT_EQ(actors, want_actors);  // the panel's six in place of its one; the switch's two before the pop-up's own
  ASSERT_EQ(events.size(), 8u);
  EXPECT_EQ(events[0], EchoVR::CSymbol64Hash("evt_friend0_btn_clicked"));
  EXPECT_EQ(events[5], EchoVR::CSymbol64Hash("evt_friends_invite_btn_clicked"));
  EXPECT_EQ(events[6], EchoVR::CSymbol64Hash("evt_nevr_profile_tab_arena"));
  EXPECT_FALSE(fl::EditScripts(out, &out, &why));  // the panel has six button instances now, not SEASON INFO's one
}

// The stock level: every edit applies and its result is the size it should be.
TEST(FriendsLevel, EveryEditAppliesToTheStockLevel) {
  const char* dir = std::getenv("NEVR_MNU_MASTER_DIR");
  if (dir == nullptr || *dir == '\0') GTEST_SKIP() << "NEVR_MNU_MASTER_DIR not set";
  const auto read = [dir](const char* type) {
    std::ifstream in(std::string(dir) + "/" + type + "/mnu_master.bin", std::ios::binary);
    return Bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  };
  Bytes out;
  std::string why;
  EXPECT_TRUE(fl::EditPointerMeshes(read("CR15PointerMeshCRWin10"), &out, &why)) << why;
  EXPECT_TRUE(fl::EditTransforms(read("CTransformCRWin10"), &out, &why)) << why;
  EXPECT_TRUE(fl::EditActorData(read("CActorDataResourceWin10"), &out, &why)) << why;
  EXPECT_TRUE(fl::EditPageElements(read("CR15UIPage2ElementCRWin10"), &out, &why)) << why;
  const Bytes scripts = read("CScriptCRWin10");
  ASSERT_TRUE(fl::EditScripts(scripts, &out, &why)) << why;
  // Eight instances in place of SEASON INFO's one: 7 more entries, 8 x 288 bytes of settings, less its own 264.
  EXPECT_EQ(out.size(), scripts.size() + 7 * 720 + 8 * (96 + 160 + 32) - (72 + 160 + 32));
}
