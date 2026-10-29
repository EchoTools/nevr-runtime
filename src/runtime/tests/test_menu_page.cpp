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
