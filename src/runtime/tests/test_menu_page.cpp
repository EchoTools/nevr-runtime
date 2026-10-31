// #906 / ADR 0017 D4: the main-menu cards are added to a copy of the player's own page, in memory.
// menu_page.h is the byte logic, menu_cards.h the cards and the pin decision. The fixture below writes a page
// resource the way the format says (its own descriptor writer, not the production one), so the tests compare the
// production insert with a page built natively with the cards in it.

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "runtime/patch/menu_cards.h"
#include "runtime/patch/menu_page.h"

namespace {

using nevr_menu_page::Card;
using nevr_menu_page::Status;

struct Row16 {
  uint64_t key;
  uint64_t value;
};
struct Row72 {
  uint64_t key;
  std::string text;
};
struct Element {
  uint64_t owner = 0;
  std::vector<Row16> slot24;
  std::vector<Row16> slot80;
  std::vector<Row72> slot416;
  uint64_t slot304 = 0;  // a populated slot of unknown stride: the format refuses it
};
struct Page {
  std::vector<Element> elements;
  std::vector<Row16> slot88;
  std::vector<Row16> slot144;
};

void Put64(std::vector<uint8_t>& b, size_t at, uint64_t v) { std::memcpy(b.data() + at, &v, 8); }

void Desc(std::vector<uint8_t>& b, size_t at, uint64_t count, uint64_t stride) {
  ASSERT_LE(at + 56, b.size());
  std::memset(b.data() + at, 0, 56);
  Put64(b, at + 0x08, count * stride);
  const uint32_t flags = 1;
  std::memcpy(b.data() + at + 0x1C, &flags, 4);
  Put64(b, at + 0x20, 0x20);
  Put64(b, at + 0x28, count);
  Put64(b, at + 0x30, count);
}

void Append(std::vector<uint8_t>& b, const std::vector<Row16>& rows) {
  for (const Row16& r : rows) {
    const size_t at = b.size();
    b.resize(at + 16);
    Put64(b, at, r.key);
    Put64(b, at + 8, r.value);
  }
}

// The file: a top descriptor and the 328-byte page records, then per page the element array, each element's
// nested payloads in slot order, and the page's tail arrays.
std::vector<uint8_t> Build(const std::vector<Page>& pages) {
  std::vector<uint8_t> b(56 + pages.size() * 328, 0);
  Desc(b, 0, pages.size(), 328);
  for (size_t p = 0; p < pages.size(); ++p) {
    const size_t rec = 56 + p * 328;
    const Page& page = pages[p];
    Desc(b, rec + 32, page.elements.size(), 472);
    Desc(b, rec + 88, page.slot88.size(), 16);
    Desc(b, rec + 144, page.slot144.size(), 16);
    Desc(b, rec + 200, 0, 16);
    Desc(b, rec + 272, 0, 16);
    const size_t elementsAt = b.size();
    b.resize(b.size() + page.elements.size() * 472, 0);
    for (size_t e = 0; e < page.elements.size(); ++e) {
      const Element& el = page.elements[e];
      const size_t at = elementsAt + e * 472;
      Put64(b, at, el.owner);
      Put64(b, at + 16, 0x2fd38a995f615b9eULL);
      Desc(b, at + 24, el.slot24.size(), 16);
      Desc(b, at + 80, el.slot80.size(), 16);
      Desc(b, at + 136, 0, 16);
      Desc(b, at + 192, 0, 16);
      Desc(b, at + 248, 0, 16);
      Desc(b, at + 304, el.slot304, 0);
      Desc(b, at + 360, 0, 0);
      Desc(b, at + 416, el.slot416.size(), 72);
    }
    for (const Element& el : page.elements) {
      Append(b, el.slot24);
      Append(b, el.slot80);
      for (const Row72& r : el.slot416) {
        const size_t at = b.size();
        b.resize(at + 72, 0);
        Put64(b, at, r.key);
        std::memcpy(b.data() + at + 8, r.text.data(), r.text.size());
      }
    }
    Append(b, page.slot88);
    Append(b, page.slot144);
  }
  return b;
}

Element NativeCard(const Card& c) {
  using nevr_menu_cards::kSymbols;
  Element el;
  el.owner = c.owner;
  el.slot80 = {{kSymbols.sprite, c.tile}};
  el.slot416 = {{kSymbols.text, c.text}, {kSymbols.subtext, c.subtext}, {kSymbols.tip, c.tip}};
  return el;
}

Element Plain(uint64_t owner) {
  Element el;
  el.owner = owner;
  el.slot24 = {{0x11, 0x22}, {0x33, 0x44}};
  el.slot80 = {{0x55, 0x66}};
  el.slot416 = {{0x77, "stock text"}};
  return el;
}

std::vector<Card> TwoCards() { return nevr_menu_cards::Cards(); }

// Three pages with different shapes: one with elements and tail rows, one empty, one with a single element.
std::vector<Page> StockPages() {
  Page p0;
  p0.elements = {Plain(0xA1), Plain(0xA2)};
  p0.slot88 = {{1, 2}};
  p0.slot144 = {{3, 4}, {5, 6}};
  Page p1;
  Page p2;
  p2.elements = {Plain(0xC1)};
  p2.slot88 = {{7, 8}};
  return {p0, p1, p2};
}

std::vector<uint8_t> WithCards(std::vector<Page> pages, size_t page, const std::vector<Card>& cards) {
  for (const Card& c : cards) pages[page].elements.push_back(NativeCard(c));
  return Build(pages);
}

Status Insert(const std::vector<uint8_t>& in, uint64_t page, const std::vector<Card>& cards,
              std::vector<uint8_t>* out) {
  return nevr_menu_page::InsertCards(in.data(), in.size(), page, cards.data(), cards.size(), nevr_menu_cards::kSymbols,
                                     out);
}

}  // namespace

TEST(MenuPage, DecodesAPageResourceWithoutAnyLeftoverByte) {
  const std::vector<uint8_t> stock = Build(StockPages());
  nevr_menu_page::Decoded d;
  ASSERT_EQ(nevr_menu_page::Decode(stock.data(), stock.size(), &d), Status::Ok);
  ASSERT_EQ(d.pages.size(), 3u);
  EXPECT_EQ(d.pages[0].elementCount, 2u);
  EXPECT_EQ(d.pages[1].elementCount, 0u);
  EXPECT_EQ(d.pages[2].elementCount, 1u);
  EXPECT_EQ(d.owners, (std::vector<uint64_t>{0xA1, 0xA2, 0xC1}));
}

TEST(MenuPage, InsertEqualsAPageBuiltNativelyWithTheCards) {
  const std::vector<Card> cards = TwoCards();
  for (size_t page = 0; page < 3; ++page) {
    const std::vector<uint8_t> stock = Build(StockPages());
    std::vector<uint8_t> out;
    ASSERT_EQ(Insert(stock, page, cards, &out), Status::Ok) << "page " << page;
    EXPECT_EQ(out, WithCards(StockPages(), page, cards)) << "page " << page;
    EXPECT_GT(out.size(), stock.size());
  }
}

TEST(MenuPage, TheCardsAreTheLastElementsOfTheirPageInOrder) {
  const std::vector<uint8_t> stock = Build(StockPages());
  std::vector<uint8_t> out;
  ASSERT_EQ(Insert(stock, 0, TwoCards(), &out), Status::Ok);
  nevr_menu_page::Decoded d;
  ASSERT_EQ(nevr_menu_page::Decode(out.data(), out.size(), &d), Status::Ok);
  EXPECT_EQ(d.pages[0].elementCount, 4u);
  EXPECT_EQ(d.owners, (std::vector<uint64_t>{0xA1, 0xA2, nevr_menu_cards::kCreateOwner, nevr_menu_cards::kJoinOwner,
                                             0xC1}));
}

TEST(MenuPage, ACardCarriesItsTileAndItsThreeStrings) {
  const std::vector<uint8_t> stock = Build(StockPages());
  std::vector<uint8_t> out;
  ASSERT_EQ(Insert(stock, 1, {Card{0xBEEF, 0x1234, "ONE", "TWO", "THREE"}}, &out), Status::Ok);
  const char text[] = "ONE";
  const std::vector<uint8_t> row(text, text + 3);
  EXPECT_NE(std::search(out.begin(), out.end(), row.begin(), row.end()), out.end());
  EXPECT_EQ(out, WithCards(StockPages(), 1, {Card{0xBEEF, 0x1234, "ONE", "TWO", "THREE"}}));
}

TEST(MenuPage, RefusesAPageThatDoesNotExist) {
  const std::vector<uint8_t> stock = Build(StockPages());
  std::vector<uint8_t> out;
  EXPECT_EQ(Insert(stock, 3, TwoCards(), &out), Status::PageOutOfRange);
  EXPECT_TRUE(out.empty());
}

TEST(MenuPage, RefusesNothingToInsert) {
  const std::vector<uint8_t> stock = Build(StockPages());
  std::vector<uint8_t> out;
  EXPECT_EQ(Insert(stock, 0, {}, &out), Status::NoCards);
}

TEST(MenuPage, RefusesAnOwnerThatAlreadyOccursAnywhereInTheBuffer) {
  const std::vector<uint8_t> stock = Build(StockPages());
  std::vector<uint8_t> out;
  EXPECT_EQ(Insert(stock, 0, {Card{0xA2, 1, "A", "", ""}}, &out), Status::DuplicateOwner);  // an element owner
  EXPECT_EQ(Insert(stock, 0, {Card{0x66, 1, "A", "", ""}}, &out), Status::DuplicateOwner);  // inside a row
  EXPECT_EQ(Insert(stock, 0, {Card{9, 1, "A", "", ""}, Card{9, 2, "B", "", ""}}, &out), Status::DuplicateOwner);
}

TEST(MenuPage, RefusesAStringTheRowCannotHoldOrTheFontCannotDraw) {
  const std::vector<uint8_t> stock = Build(StockPages());
  std::vector<uint8_t> out;
  EXPECT_EQ(Insert(stock, 0, {Card{9, 1, std::string(64, 'x'), "", ""}}, &out), Status::BadText);
  EXPECT_EQ(Insert(stock, 0, {Card{9, 1, "", "", "caf\xc3\xa9"}}, &out), Status::BadText);
  EXPECT_EQ(Insert(stock, 0, {Card{9, 1, "line\nbreak", "", ""}}, &out), Status::BadText);
  EXPECT_EQ(Insert(stock, 0, {Card{9, 1, std::string(63, 'x'), "", ""}}, &out), Status::Ok);
}

TEST(MenuPage, RefusesABufferThatIsNotExactlyAPageResource) {
  const std::vector<uint8_t> stock = Build(StockPages());
  std::vector<uint8_t> out;

  std::vector<uint8_t> longer = stock;
  longer.push_back(0);
  EXPECT_EQ(Insert(longer, 0, TwoCards(), &out), Status::TrailingBytes);

  std::vector<uint8_t> shorter(stock.begin(), stock.end() - 1);
  EXPECT_EQ(Insert(shorter, 0, TwoCards(), &out), Status::Truncated);

  EXPECT_EQ(Insert({}, 0, TwoCards(), &out), Status::Truncated);

  std::vector<uint8_t> withPointer = stock;
  Put64(withPointer, 0, 1);  // a pointer in the top descriptor
  EXPECT_EQ(Insert(withPointer, 0, TwoCards(), &out), Status::BadDescriptor);

  std::vector<uint8_t> wrongFlags = stock;
  wrongFlags[56 + 32 + 0x1C] = 2;
  EXPECT_EQ(Insert(wrongFlags, 0, TwoCards(), &out), Status::BadDescriptor);

  std::vector<uint8_t> usedBelowCount = stock;
  Put64(usedBelowCount, 56 + 32 + 0x30, 1);  // count 2, used 1
  EXPECT_EQ(Insert(usedBelowCount, 0, TwoCards(), &out), Status::BadDescriptor);

  std::vector<uint8_t> wrongSize = stock;
  Put64(wrongSize, 56 + 32 + 0x08, 471);
  EXPECT_EQ(Insert(wrongSize, 0, TwoCards(), &out), Status::BadDescriptor);

  std::vector<uint8_t> hugeCount = stock;
  Put64(hugeCount, 0x28, ~uint64_t{0});
  Put64(hugeCount, 0x30, ~uint64_t{0});
  Put64(hugeCount, 0x08, ~uint64_t{0} * 328);
  EXPECT_EQ(Insert(hugeCount, 0, TwoCards(), &out), Status::BadDescriptor);
  EXPECT_TRUE(out.empty());
}

TEST(MenuPage, RefusesAnElementSlotWhoseStrideItDoesNotKnow) {
  std::vector<Page> pages = StockPages();
  pages[0].elements[0].slot304 = 1;
  const std::vector<uint8_t> odd = Build(pages);
  std::vector<uint8_t> out;
  EXPECT_EQ(Insert(odd, 0, TwoCards(), &out), Status::UnobservedSlot);
}

TEST(MenuPage, RefusesAnOversizeBufferBeforeReadingIt) {
  nevr_menu_page::Decoded d;
  const uint8_t byte = 0;
  EXPECT_EQ(nevr_menu_page::Decode(&byte, nevr_menu_page::kMaxResourceBytes + 1, &d), Status::TooLarge);
}

TEST(MenuCards, TheSymbolsAreTheGamesOwn) {
  // The literals are the symbols as the game's page and card canvas hold them (the echovr-modkit author's table;
  // the tile names and mnu_master also sit in this repository's symbol cache).
  EXPECT_EQ(nevr_menu_cards::kSymbols.stateDefault, 0x2fd38a995f615b9eULL);
  EXPECT_EQ(nevr_menu_cards::kSymbols.sprite, 0x41d2d42d082a131bULL);
  EXPECT_EQ(nevr_menu_cards::kSymbols.text, 0xc8c33e482e7606a4ULL);
  EXPECT_EQ(nevr_menu_cards::kSymbols.subtext, 0x2fc49a9d4a714f9eULL);
  EXPECT_EQ(nevr_menu_cards::kSymbols.tip, 0xc8e8d0b1a884ebfaULL);
  EXPECT_EQ(nevr_menu_cards::kTileCreate, 0x3e325fc287e68085ULL);
  EXPECT_EQ(nevr_menu_cards::kTileJoin, 0xd90e94db565624faULL);
  EXPECT_EQ(EchoVR::CSymbol64Hash("mnu_master"), nevr_menu_cards::kPageName);
}

// The resource loader's callback names a resource by its class symbol (the word the game keeps at +0x28 of the
// resource), not by the type id of the package manifest row. A live run logged 0x75e5993f80940cd0 for the page
// ("CR15UIPage2CR" in the symbol table) while the transform was registered under the manifest's id and never ran.
TEST(MenuCards, PageIsKnownByTheClassSymbolTheLoaderCallbackReports) {
  EXPECT_EQ(nevr_menu_cards::kPageType, 0x75e5993f80940cd0ULL);
  EXPECT_EQ(nevr_menu_cards::kPageType, EchoVR::CSymbol64Hash("CR15UIPage2CR"));
}

TEST(MenuCards, TheTwoCardsAreCreateAndJoin) {
  const std::vector<Card> cards = nevr_menu_cards::Cards();
  ASSERT_EQ(cards.size(), 2u);
  EXPECT_EQ(cards[0].text, "CREATE GAME");
  EXPECT_EQ(cards[0].tile, nevr_menu_cards::kTileCreate);
  EXPECT_EQ(cards[1].text, "JOIN VIA CODE");
  EXPECT_EQ(cards[1].tile, nevr_menu_cards::kTileJoin);
}

TEST(MenuCards, ThePinDecidesWhetherTheCardsGoIn) {
  // The pinned page is a real RAD file the repository cannot hold; a fixture and a stand-in pin carry the decision.
  const std::vector<uint8_t> stock = Build(StockPages());
  // StockPages() has 3 pages and the cards go to page 24: grow the fixture so that page exists.
  std::vector<Page> pages(25);
  pages[24].elements = {Plain(0xA1)};
  const std::vector<uint8_t> menu = Build(pages);

  const nevr_menu_cards::Result patched = nevr_menu_cards::Patch(menu.data(), menu.size(), "pin", "pin", TwoCards());
  EXPECT_EQ(patched.verdict, nevr_menu_cards::Verdict::Patched);
  EXPECT_EQ(patched.bytes, WithCards(pages, 24, TwoCards()));

  const nevr_menu_cards::Result other = nevr_menu_cards::Patch(menu.data(), menu.size(), "other", "pin", TwoCards());
  EXPECT_EQ(other.verdict, nevr_menu_cards::Verdict::NotStock);
  EXPECT_TRUE(other.bytes.empty());

  const nevr_menu_cards::Result refused = nevr_menu_cards::Patch(stock.data(), stock.size(), "pin", "pin", TwoCards());
  EXPECT_EQ(refused.verdict, nevr_menu_cards::Verdict::Refused);
  EXPECT_EQ(refused.status, Status::PageOutOfRange);
  EXPECT_TRUE(refused.bytes.empty());
}

TEST(MenuCards, TheStockPinIsAWellFormedDigest) {
  const std::string pin = nevr_menu_cards::kStockPageSha256;
  EXPECT_EQ(pin.size(), 64u);
  EXPECT_EQ(pin.find_first_not_of("0123456789abcdef"), std::string::npos);
}

// ---- #1679: where the four buttons stand (menu_slots.h) -------------------------------------------------------------
// The level's transform table is written here the way the format says (its own writer, not the production one): a
// 56-byte descriptor and 176-byte entries, the owner at +0x08, the pose at +0x20 and a marker in the tail that no
// placement may touch.
namespace {

namespace slots = nevr_menu_slots;

struct Actor {
  uint64_t owner = 0;
  float rotation[4] = {0.0f, 1.0f, 0.0f, 0.0f};
  float position[3] = {0.0f, 0.0f, 0.0f};
  float scale[3] = {1.0f, 1.0f, 1.0f};
  uint8_t marker = 0;
};

Actor At(uint64_t owner, float x, float rotationW, uint8_t marker) {
  Actor a;
  a.owner = owner;
  a.rotation[3] = rotationW;
  a.position[0] = x;
  a.position[1] = -0.275f;
  a.position[2] = 5.5f;
  a.marker = marker;
  return a;
}

std::vector<uint8_t> Table(const std::vector<Actor>& actors) {
  std::vector<uint8_t> b(slots::kDescriptorBytes + actors.size() * slots::kEntryBytes, 0);
  Desc(b, 0, actors.size(), slots::kEntryBytes);
  for (size_t i = 0; i < actors.size(); ++i) {
    const size_t e = slots::kDescriptorBytes + i * slots::kEntryBytes;
    Put64(b, e, 0xabfe651c8d260515ULL);
    Put64(b, e + slots::kOwnerOffset, actors[i].owner);
    std::memcpy(b.data() + e + slots::kPoseOffset, actors[i].rotation, 16);
    std::memcpy(b.data() + e + slots::kPoseOffset + 16, actors[i].position, 12);
    std::memcpy(b.data() + e + slots::kPoseOffset + 28, actors[i].scale, 12);
    Put64(b, e + 0x68, 0xabfe651c8d260515ULL);
    b[e + 0x48] = actors[i].marker;  // just past the pose: a placement that writes too far changes it
    b[e + 0x90] = actors[i].marker;
  }
  return b;
}

// The stock main-menu level in miniature: three other actors, and the four buttons at the stock slots (Join, Play,
// Tutorial, Create from the left) with the arc's rotations.
std::vector<Actor> StockActors() {
  return {At(0x1111, 0.0f, 0.0f, 1),
          At(nevr_menu_cards::kJoinOwner, 2.15f, -0.174f, 2),
          At(nevr_menu_cards::kPlayOwner, 0.73f, -0.061f, 3),
          At(0x2222, 9.0f, 0.0f, 4),
          At(nevr_menu_cards::kTutorialOwner, -0.73f, 0.061f, 5),
          At(nevr_menu_cards::kCreateOwner, -2.15f, 0.174f, 6),
          At(0x3333, -9.0f, 0.0f, 7)};
}

std::string Names(const std::vector<slots::Placement>& p) {
  std::vector<slots::Placement> bySlot = p;
  std::sort(bySlot.begin(), bySlot.end(), [](const slots::Placement& a, const slots::Placement& b) { return a.slot < b.slot; });
  std::string s;
  for (const slots::Placement& x : bySlot) s += std::string(s.empty() ? "" : ",") + nevr_menu_cards::ButtonName(x.owner);
  return s;
}

}  // namespace

TEST(MenuSlots, ReadsTheButtonsFromTheLeftByDecreasingX) {
  const std::vector<uint8_t> in = Table(StockActors());
  const std::vector<uint64_t> owners = nevr_menu_cards::ButtonOrder();
  std::vector<slots::Placement> p;
  ASSERT_EQ(slots::Read(in.data(), in.size(), owners.data(), owners.size(), &p), slots::Status::Ok);
  EXPECT_EQ(Names(p), "join,play,tutorial,create");
  ASSERT_EQ(p.size(), 4u);
  // In the order asked for: Play, Join, Create, Tutorial stand in slots 2, 1, 4, 3.
  EXPECT_EQ(p[0].slot, 2u);
  EXPECT_EQ(p[1].slot, 1u);
  EXPECT_EQ(p[2].slot, 4u);
  EXPECT_EQ(p[3].slot, 3u);
  EXPECT_FLOAT_EQ(p[1].x, 2.15f);
}

TEST(MenuSlots, EachButtonTakesTheWholePoseOfTheSlotItIsPlacedIn) {
  const std::vector<uint8_t> in = Table(StockActors());
  const std::vector<uint64_t> order = nevr_menu_cards::ButtonOrder();
  std::vector<uint8_t> out;
  std::vector<slots::Placement> before, after;
  ASSERT_EQ(slots::Place(in.data(), in.size(), order.data(), order.size(), &out, &before, &after), slots::Status::Ok);
  EXPECT_EQ(Names(before), "join,play,tutorial,create");
  EXPECT_EQ(Names(after), "play,join,create,tutorial");

  // Play stands at the old Join slot with its arc rotation, Join at Play's, Create at Tutorial's, Tutorial at Create's; every
  // other byte of every entry (the owner, the parent, the marker, the three other actors) is as it was.
  std::vector<Actor> want = StockActors();
  const Actor join = want[1], play = want[2], tutorial = want[4], create = want[5];
  auto pose = [](Actor& to, const Actor& from) {
    std::memcpy(to.rotation, from.rotation, sizeof(to.rotation));
    std::memcpy(to.position, from.position, sizeof(to.position));
    std::memcpy(to.scale, from.scale, sizeof(to.scale));
  };
  pose(want[2], join);      // Play takes slot 1
  pose(want[1], play);      // Join takes slot 2
  pose(want[5], tutorial);  // Create takes slot 3
  pose(want[4], create);    // Tutorial takes slot 4
  EXPECT_EQ(out, Table(want));
  EXPECT_EQ(out.size(), in.size());
}

TEST(MenuSlots, PlacingTheOrderTheTableAlreadyHasChangesNothing) {
  const std::vector<uint8_t> in = Table(StockActors());
  const std::vector<uint64_t> order = {nevr_menu_cards::kJoinOwner, nevr_menu_cards::kPlayOwner, nevr_menu_cards::kTutorialOwner,
                                       nevr_menu_cards::kCreateOwner};
  std::vector<uint8_t> out;
  std::vector<slots::Placement> before, after;
  ASSERT_EQ(slots::Place(in.data(), in.size(), order.data(), order.size(), &out, &before, &after), slots::Status::Ok);
  EXPECT_EQ(out, in);
}

TEST(MenuSlots, ASubsetKeepsToTheSlotsItsOwnersHold) {
  // Two owners trade; the actor standing between them in x, and the rest, stay.
  const std::vector<uint8_t> in = Table(StockActors());
  const std::vector<uint64_t> order = {nevr_menu_cards::kCreateOwner, nevr_menu_cards::kPlayOwner};
  std::vector<uint8_t> out;
  std::vector<slots::Placement> before, after;
  ASSERT_EQ(slots::Place(in.data(), in.size(), order.data(), order.size(), &out, &before, &after), slots::Status::Ok);
  std::vector<slots::Placement> p;
  ASSERT_EQ(slots::Read(out.data(), out.size(), order.data(), order.size(), &p), slots::Status::Ok);
  EXPECT_EQ(p[0].slot, 1u);
  EXPECT_FLOAT_EQ(p[0].x, 0.73f);  // Create now stands where Play stood: the left of the two
  EXPECT_FLOAT_EQ(p[1].x, -2.15f);
}

TEST(MenuSlots, RefusesWhatItCannotPlaceAndChangesNothing) {
  const std::vector<uint8_t> in = Table(StockActors());
  const std::vector<uint64_t> order = nevr_menu_cards::ButtonOrder();
  std::vector<uint8_t> out;
  std::vector<slots::Placement> before, after;
  auto place = [&](const std::vector<uint8_t>& table, const std::vector<uint64_t>& o) {
    return slots::Place(table.data(), table.size(), o.data(), o.size(), &out, &before, &after);
  };

  EXPECT_EQ(place(in, {}), slots::Status::NothingToPlace);
  EXPECT_EQ(place(in, {order[0], order[0]}), slots::Status::DuplicateOwner);
  EXPECT_EQ(place(in, {order[0], 0x9999}), slots::Status::MissingOwner);

  std::vector<Actor> twice = StockActors();
  twice.push_back(At(nevr_menu_cards::kPlayOwner, 0.1f, 0.0f, 8));
  EXPECT_EQ(place(Table(twice), order), slots::Status::DuplicateOwner);

  std::vector<Actor> tied = StockActors();
  tied[5].position[0] = tied[4].position[0];
  EXPECT_EQ(place(Table(tied), order), slots::Status::AmbiguousSlots);

  EXPECT_EQ(place(std::vector<uint8_t>(in.begin(), in.end() - 1), order), slots::Status::Truncated);
  EXPECT_EQ(place(std::vector<uint8_t>(in.begin(), in.begin() + 20), order), slots::Status::Truncated);
  std::vector<uint8_t> longer = in;
  longer.push_back(0);
  EXPECT_EQ(place(longer, order), slots::Status::BadDescriptor);
  std::vector<uint8_t> lying = in;
  Put64(lying, 0x08, 1);
  EXPECT_EQ(place(lying, order), slots::Status::BadDescriptor);
}

TEST(MenuCards, ThePinDecidesWhetherTheButtonsAreOrdered) {
  const std::vector<uint8_t> table = Table(StockActors());
  const nevr_menu_cards::SlotResult placed = nevr_menu_cards::PlaceButtons(table.data(), table.size(), "pin", "pin");
  ASSERT_EQ(placed.verdict, nevr_menu_cards::Verdict::Patched);
  EXPECT_EQ(nevr_menu_cards::SlotLine(placed.before), "join:1@2.150,play:2@0.730,tutorial:3@-0.730,create:4@-2.150");
  EXPECT_EQ(nevr_menu_cards::SlotLine(placed.after), "play:1@2.150,join:2@0.730,create:3@-0.730,tutorial:4@-2.150");
  EXPECT_EQ(placed.bytes.size(), table.size());

  const nevr_menu_cards::SlotResult other = nevr_menu_cards::PlaceButtons(table.data(), table.size(), "other", "pin");
  EXPECT_EQ(other.verdict, nevr_menu_cards::Verdict::NotStock);
  EXPECT_TRUE(other.bytes.empty());

  const std::vector<uint8_t> noButtons = Table({At(0x1111, 1.0f, 0.0f, 1)});
  const nevr_menu_cards::SlotResult refused = nevr_menu_cards::PlaceButtons(noButtons.data(), noButtons.size(), "pin", "pin");
  EXPECT_EQ(refused.verdict, nevr_menu_cards::Verdict::Refused);
  EXPECT_EQ(refused.status, slots::Status::MissingOwner);
  EXPECT_TRUE(refused.bytes.empty());
  EXPECT_TRUE(refused.after.empty());
}

TEST(MenuCards, TheButtonsAreOrderedPlayJoinCreateTutorial) {
  EXPECT_EQ(nevr_menu_cards::ButtonOrder(),
            (std::vector<uint64_t>{nevr_menu_cards::kPlayOwner, nevr_menu_cards::kJoinOwner, nevr_menu_cards::kCreateOwner,
                                   nevr_menu_cards::kTutorialOwner}));
  EXPECT_EQ(nevr_menu_cards::kPlayOwner, 0xa3d3ef315cb999dbULL);
  EXPECT_EQ(nevr_menu_cards::kTutorialOwner, 0xd763e348e835f249ULL);
}

TEST(MenuCards, TheTransformTableIsKnownByTheClassSymbolTheLoaderCallbackReports) {
  // The headset's loader callback reported class 0x9c72178d05adb686 for the 52,680-byte table under mnu_master_lowspec.
  EXPECT_EQ(nevr_menu_cards::kTransformType, 0x9c72178d05adb686ULL);
  const std::string pin = nevr_menu_cards::kStockTransformSha256;
  EXPECT_EQ(pin.size(), 64u);
  EXPECT_EQ(pin.find_first_not_of("0123456789abcdef"), std::string::npos);
}
