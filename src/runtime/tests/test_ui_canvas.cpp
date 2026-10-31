// #1535 / #1599: the UI canvas codec (ui_canvas.h). Canvases are built here through the codec's own types and the
// written bytes are checked against the format independently (descriptor counts, alignment, where each table and
// payload lands); stock canvases are game data and are not in the repository, so the stock round trip runs over a
// directory named by NEVR_UI_CANVAS_CORPUS (one canvas per file) and is skipped without it.

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "runtime/patch/ui_canvas.h"

namespace {

namespace uc = nevr_ui_canvas;

std::uint64_t U64(const std::vector<std::uint8_t>& b, std::size_t at) {
  std::uint64_t v;
  std::memcpy(&v, b.data() + at, sizeof(v));
  return v;
}

void PutU64(std::vector<std::uint8_t>* b, std::size_t at, std::uint64_t v) { std::memcpy(b->data() + at, &v, sizeof(v)); }

uc::Element MakeElement(std::uint64_t name, std::uint32_t type, std::int32_t parent, std::int32_t index) {
  uc::Element e;
  e.set_name(name);
  e.set_type(type);
  e.set_parent(parent);
  e.set_index(index);
  return e;
}

// A canvas with every table filled: a group, a sprite under it, a text with a slot, one behaviour with a payload
// whose length is not a multiple of 8 (so the tables after it are aligned), two named rects, one pair.
uc::Canvas Sample() {
  uc::Canvas c;  // the header starts zeroed: every descriptor at rest with capacity 0
  c.elements.push_back(MakeElement(0x1111, uc::kGroup, -1, 0));
  uc::Element sprite = MakeElement(0x2222, uc::kSprite, 0, 1);
  sprite.set_texture(0xABCDEF);
  sprite.set_uv({0.0f, 0.0f, 1.0f, 1.0f});
  c.elements.push_back(sprite);
  uc::Element text = MakeElement(0x3333, uc::kText, 0, 2);
  const std::int64_t slot = c.AddTextSlot("HELLO", 17);
  text.set_text_slot(static_cast<std::uint32_t>(slot));
  text.set_text_capacity(17);
  c.elements.push_back(text);
  uc::Behaviour b;
  b.payload = {1, 2, 3, 4, 5};
  b.set_element(1);
  c.behaviours.push_back(b);
  c.rects.push_back({0xAAAA, {}});
  c.rects.push_back({0xBBBB, {}});
  c.pairs.push_back({});
  return c;
}

}  // namespace

TEST(UiCanvas, EncodeWritesEveryTableWhereTheFormatSays) {
  const uc::Canvas c = Sample();
  const std::vector<std::uint8_t> b = uc::Encode(c);
  // Counts at +48 of each descriptor, capacity raised to the count, byte size = capacity x stride.
  EXPECT_EQ(U64(b, 80 + 48), 3u);
  EXPECT_EQ(U64(b, 136 + 48), 1u);
  EXPECT_EQ(U64(b, 192 + 48), 2u);
  EXPECT_EQ(U64(b, 256 + 48), 1u);
  EXPECT_EQ(U64(b, 312 + 48), 17u);
  EXPECT_EQ(U64(b, 368 + 48), 1u);
  EXPECT_EQ(U64(b, 80 + 8), 3u * 224u);
  EXPECT_EQ(U64(b, 80 + 40), 3u);
  // Elements right after the header; the info 8-aligned after them; its payload right after the infos.
  const std::size_t info = 432 + 3 * 224;  // already 8-aligned
  EXPECT_EQ(U64(b, 432), 0x1111u);
  EXPECT_EQ(U64(b, info + 16 + 48), 5u);  // the info's payload descriptor carries the payload length
  const std::size_t payload = info + 88;
  EXPECT_EQ(b[payload], 1);
  EXPECT_EQ(b[payload + 4], 5);
  // The rects 8-aligned after the 5-byte payload.
  const std::size_t rects = (payload + 5 + 7) / 8 * 8;
  EXPECT_EQ(U64(b, rects), 0xAAAAu);
  EXPECT_EQ(U64(b, rects + 24), 0xBBBBu);
  // Offsets (4-aligned, already), then the 17-byte pool, then the pair 8-aligned, and nothing after it.
  const std::size_t offsets = rects + 48;
  const std::size_t pool = offsets + 4;
  EXPECT_EQ(std::string(reinterpret_cast<const char*>(b.data() + pool)), "HELLO");
  const std::size_t pairs = (pool + 17 + 7) / 8 * 8;
  EXPECT_EQ(b.size(), pairs + 24);
}

TEST(UiCanvas, DecodeReadsBackWhatEncodeWrote) {
  const uc::Canvas c = Sample();
  const std::vector<std::uint8_t> b = uc::Encode(c);
  uc::Canvas d;
  std::string why;
  ASSERT_TRUE(uc::Decode(b.data(), b.size(), &d, &why)) << why;
  ASSERT_EQ(d.elements.size(), 3u);
  EXPECT_EQ(d.elements[1].texture(), 0xABCDEFu);
  EXPECT_EQ(d.Text(d.elements[2]), "HELLO");
  ASSERT_EQ(d.behaviours.size(), 1u);
  EXPECT_EQ(d.behaviours[0].payload, (std::vector<std::uint8_t>{1, 2, 3, 4, 5}));
  EXPECT_EQ(d.behaviours[0].element(), 1u);
  EXPECT_EQ(d.Find(0x3333), 2);
  EXPECT_EQ(d.Find(0x9999), -1);
  EXPECT_EQ(uc::Encode(d), b);
  EXPECT_TRUE(uc::Problems(d).empty());
}

TEST(UiCanvas, AnEmptyCanvasIsTheHeaderAlone) {
  uc::Canvas c;
  const std::vector<std::uint8_t> b = uc::Encode(c);
  EXPECT_EQ(b.size(), 432u);
  uc::Canvas d;
  EXPECT_TRUE(uc::Decode(b.data(), b.size(), &d, nullptr));
}

TEST(UiCanvas, DecodeRefusesWhatIsNotACanvas) {
  const std::vector<std::uint8_t> good = uc::Encode(Sample());
  uc::Canvas d;
  std::string why;
  EXPECT_FALSE(uc::Decode(good.data(), 100, &d, &why));
  EXPECT_FALSE(uc::Decode(good.data(), good.size() - 1, &d, &why));  // the last table runs past the end
  std::vector<std::uint8_t> extra = good;
  extra.push_back(0);
  EXPECT_FALSE(uc::Decode(extra.data(), extra.size(), &d, &why));
  EXPECT_EQ(why, "bytes after the last table");
  std::vector<std::uint8_t> pointer = good;
  PutU64(&pointer, 80, 0x1000);  // a descriptor that carries a pointer is not a resource at rest
  EXPECT_FALSE(uc::Decode(pointer.data(), pointer.size(), &d, &why));
  std::vector<std::uint8_t> size = good;
  PutU64(&size, 80 + 8, 1);  // a byte size that does not match the capacity
  EXPECT_FALSE(uc::Decode(size.data(), size.size(), &d, &why));
}

TEST(UiCanvas, AddTextSlotRefusesATextWithoutRoomForItsNul) {
  uc::Canvas c;
  EXPECT_EQ(c.AddTextSlot("ABCD", 4), -1);
  EXPECT_EQ(c.AddTextSlot("ABC", 4), 0);
  EXPECT_EQ(c.pool.size(), 4u);
}

TEST(UiCanvas, ProblemsNamesEachBrokenRule) {
  uc::Canvas c = Sample();
  c.elements[1].set_index(5);                   // index is not its position
  c.elements[0].set_parent(0);                  // its own parent
  c.elements[1].set_texture(0);                 // sprite without texture
  c.behaviours[0].set_element(9);               // drives no element
  uc::Element second_reader = c.elements[2];    // a slot read twice
  second_reader.set_index(3);
  c.elements.push_back(second_reader);
  const std::vector<std::string> p = uc::Problems(c);
  const auto has = [&p](const std::string& s) {
    for (const auto& line : p)
      if (line.find(s) != std::string::npos) return true;
    return false;
  };
  EXPECT_TRUE(has("element 1 carries index 5"));
  EXPECT_TRUE(has("element 0's parent is not an element"));
  EXPECT_TRUE(has("element 1 is a sprite without a texture"));
  EXPECT_TRUE(has("behaviour 0 drives no element"));
  EXPECT_TRUE(has("text slot 0 read 2 times"));
}

TEST(UiCanvas, SetLayoutPutsThePositionAtThePivotOfTheOffsets) {
  uc::Element e;
  float pos[2];
  e.set_layout({0.0f, 0.0f, 0.0f, 0.0f}, {10.0f, 20.0f, 110.0f, 70.0f});  // pivot 0, 0
  std::memcpy(pos, e.raw.data() + 76, sizeof(pos));
  EXPECT_EQ(pos[0], 10.0f);
  EXPECT_EQ(pos[1], 20.0f);
  e.set_pivot({0.5f, 0.5f});
  e.set_layout({0.0f, 0.0f, 0.0f, 0.0f}, {10.0f, 20.0f, 110.0f, 70.0f});
  std::memcpy(pos, e.raw.data() + 76, sizeof(pos));
  EXPECT_EQ(pos[0], 60.0f);
  EXPECT_EQ(pos[1], 45.0f);
  EXPECT_EQ(e.offsets().x1, 110.0f);
}

TEST(UiCanvas, RefreshLayoutResolvesRectsCountsChildrenAndOrdersSiblings) {
  uc::Canvas c;
  c.header[20] = 0;  // authored 1000 x 500 (+20, +24)
  const std::uint32_t w = 1000, h = 500;
  std::memcpy(c.header.data() + 20, &w, 4);
  std::memcpy(c.header.data() + 24, &h, 4);
  uc::ElementSpec root;
  root.offsets = {100, 50, 600, 450};
  const std::int32_t r = uc::Append(&c, root);
  uc::ElementSpec centred;  // anchored at the parent's centre, 40 x 20 around it
  centred.parent = r;
  centred.anchors = {0.5f, 0.5f, 0.5f, 0.5f};
  centred.offsets = {-20, -10, 20, 10};
  uc::Append(&c, centred);
  uc::ElementSpec stretched;  // the parent's full width, 30 high
  stretched.parent = r;
  stretched.anchors = {0.0f, 0.0f, 1.0f, 0.0f};
  stretched.offsets = {0, 0, 0, 30};
  uc::Append(&c, stretched);
  uc::Append(&c, uc::ElementSpec{});  // a second root
  ASSERT_TRUE(uc::RefreshLayout(&c));
  const auto rect = [&c](std::size_t i) {
    uc::Rect out;
    std::memcpy(&out, c.elements[i].raw.data() + 20, sizeof(out));
    return out;
  };
  const auto word = [&c](std::size_t i, std::size_t at) {
    std::uint32_t v;
    std::memcpy(&v, c.elements[i].raw.data() + at, 4);
    return v;
  };
  EXPECT_EQ(rect(0).x0, 100.0f);
  EXPECT_EQ(rect(1).x0, 230.0f);  // 0.5 x 500 - 20
  EXPECT_EQ(rect(1).y1, 210.0f);  // 0.5 x 400 + 10
  EXPECT_EQ(rect(2).x1, 500.0f);  // 1.0 x 500 + 0
  EXPECT_EQ(word(0, 100), 2u);    // two children
  EXPECT_EQ(word(1, 104), 0u);    // first child
  EXPECT_EQ(word(2, 104), 1u);    // second child
  EXPECT_EQ(word(3, 104), 1u);    // second root
  c.elements[1].set_parent(2);    // a parent after its child
  EXPECT_FALSE(uc::RefreshLayout(&c));
}

TEST(UiCanvas, MakeElementWritesATextsFixedFields) {
  uc::ElementSpec s;
  s.type = uc::kText;
  s.capacity = 33;
  s.font_size = 48;
  const uc::Element e = uc::MakeElement(s);
  std::uint64_t radstr, entry, none;
  std::memcpy(&radstr, e.raw.data() + 192, 8);
  std::memcpy(&none, e.raw.data() + 200, 8);
  std::memcpy(&entry, e.raw.data() + 216, 8);
  EXPECT_EQ(radstr, 0x41D2D53C1E30130CULL);
  EXPECT_EQ(none, ~std::uint64_t{0});
  EXPECT_EQ(entry, 0x89423FB49877EEADULL);
  EXPECT_EQ(e.texture(), uc::kFontBold);
  EXPECT_EQ(e.font_size(), 48u);
}

namespace {

std::vector<std::filesystem::path> CorpusFiles() {
  std::vector<std::filesystem::path> out;
  const char* dir = std::getenv("NEVR_UI_CANVAS_CORPUS");
  if (dir == nullptr || *dir == '\0') return out;
  for (const auto& entry : std::filesystem::directory_iterator(dir))
    if (entry.is_regular_file()) out.push_back(entry.path());
  return out;
}

std::vector<std::uint8_t> ReadFile(const std::filesystem::path& p) {
  std::ifstream in(p, std::ios::binary);
  return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

template <typename T>
T Field(const uc::Element& e, std::size_t at) {
  T v;
  std::memcpy(&v, e.raw.data() + at, sizeof(v));
  return v;
}

}  // namespace

// RefreshLayout recomputes what the stock canvases store. It writes only the resolved rect, the size, the child count
// and the sibling position; the counts must be the stock ones, the floats within 0.01 of an authored unit (the stock
// values carry the authoring tool's rounding). A few stock elements hold stale values; they are counted.
TEST(UiCanvas, RefreshLayoutLeavesStockCanvasesAsTheyAre) {
  const auto files = CorpusFiles();
  if (files.empty()) GTEST_SKIP() << "NEVR_UI_CANVAS_CORPUS not set";
  std::size_t elements = 0, other_bytes = 0, counts = 0, stale = 0;
  for (const auto& f : files) {
    const std::vector<std::uint8_t> b = ReadFile(f);
    uc::Canvas c;
    ASSERT_TRUE(uc::Decode(b.data(), b.size(), &c, nullptr)) << f.filename().string();
    const std::vector<uc::Element> before = c.elements;
    ASSERT_TRUE(uc::RefreshLayout(&c)) << f.filename().string();
    for (std::size_t i = 0; i < c.elements.size(); ++i) {
      ++elements;
      uc::Element a = c.elements[i], s = before[i];
      if (Field<std::uint32_t>(a, 100) != Field<std::uint32_t>(s, 100) ||
          Field<std::uint32_t>(a, 104) != Field<std::uint32_t>(s, 104))
        ++counts;
      bool near = true;
      for (const std::size_t at : {20, 24, 28, 32, 84, 88})
        near = near && std::abs(Field<float>(a, at) - Field<float>(s, at)) <= 0.01f;
      if (!near) ++stale;
      for (const std::size_t at : {20, 24, 28, 32, 84, 88, 100, 104}) {
        std::memset(a.raw.data() + at, 0, 4);
        std::memset(s.raw.data() + at, 0, 4);
      }
      if (a.raw != s.raw) ++other_bytes;
    }
  }
  std::printf("[ui_canvas] refresh: elements=%zu other_bytes=%zu counts=%zu stale_rect=%zu\n", elements, other_bytes,
              counts, stale);
  EXPECT_EQ(other_bytes, 0u);  // it writes nothing else
  EXPECT_LE(counts, 1u);       // one stock element's child count is off (8413 of 8414)
  EXPECT_LE(stale, 11u);       // 6 stale rects and 5 stale sizes in the corpus
}

// Every stock group, sprite and text, made again from its own fields by MakeElement, is the stock element, but for
// the words new elements leave zero (+12, +16, +108, +112, +168, +172, and a text's +152).
TEST(UiCanvas, MakeElementRebuildsEveryStockElementFromItsFields) {
  const auto files = CorpusFiles();
  if (files.empty()) GTEST_SKIP() << "NEVR_UI_CANVAS_CORPUS not set";
  std::size_t rebuilt = 0, differ = 0, stale = 0;
  for (const auto& f : files) {
    const std::vector<std::uint8_t> b = ReadFile(f);
    uc::Canvas c;
    ASSERT_TRUE(uc::Decode(b.data(), b.size(), &c, nullptr));
    for (const uc::Element& stock : c.elements) {
      const std::uint32_t t = stock.type();
      if (t != uc::kGroup && t != uc::kSprite && t != uc::kText) continue;
      uc::ElementSpec s;
      s.name = stock.name();
      s.type = t;
      s.parent = stock.parent();
      s.anchors = stock.anchors();
      s.offsets = stock.offsets();
      s.pivot = Field<std::array<float, 2>>(stock, 68);
      s.colour = stock.colour();
      s.button_flag = Field<std::uint32_t>(stock, 136) == 1;
      s.texture = stock.texture();
      s.uv = Field<uc::Rect>(stock, 144);
      s.font = stock.texture();
      s.text_slot = stock.text_slot();
      s.capacity = stock.text_capacity();
      s.font_size = stock.font_size();
      s.alignment = Field<std::array<float, 2>>(stock, 156);
      s.second_colour = Field<std::array<float, 4>>(stock, 176);
      uc::Element made = uc::MakeElement(s);
      made.set_index(stock.index());
      uc::Element expect = stock;
      // What RefreshLayout writes, taken from the stock element here.
      for (const std::size_t at : {std::size_t{20}, std::size_t{24}, std::size_t{28}, std::size_t{32}, std::size_t{84},
                                   std::size_t{88}, std::size_t{100}, std::size_t{104}})
        std::memcpy(made.raw.data() + at, stock.raw.data() + at, 4);
      for (const std::size_t at : {std::size_t{12}, std::size_t{16}, std::size_t{108}, std::size_t{112},
                                   std::size_t{168}, std::size_t{172}}) {
        std::memset(made.raw.data() + at, 0, 4);
        std::memset(expect.raw.data() + at, 0, 4);
      }
      if (t == uc::kText) {
        std::memset(made.raw.data() + 152, 0, 4);
        std::memset(expect.raw.data() + 152, 0, 4);
      }
      ++rebuilt;
      // The position is the offsets' pivot point; the stock values were rounded by the authoring tool in a way no
      // float32 ordering reproduces (about 300 of 16062 axes differ in the last bits), and 29 elements hold a stale
      // position. It is compared within 0.01 of an authored unit, and a stale one is counted, not failed.
      const auto pos_made = Field<std::array<float, 2>>(made, 76), pos_stock = Field<std::array<float, 2>>(stock, 76);
      if (std::abs(pos_made[0] - pos_stock[0]) > 0.01f || std::abs(pos_made[1] - pos_stock[1]) > 0.01f) ++stale;
      std::memcpy(made.raw.data() + 76, stock.raw.data() + 76, 8);
      if (made.raw != expect.raw) {
        if (++differ <= 5) {
          for (std::size_t at = 0; at < uc::kElementBytes; at += 4)
            if (std::memcmp(made.raw.data() + at, expect.raw.data() + at, 4) != 0)
              ADD_FAILURE() << f.filename().string() << " element " << stock.index() << " type " << t << " differs at +" << at;
        }
      }
    }
  }
  std::printf("[ui_canvas] rebuilt=%zu differ=%zu stale_position=%zu\n", rebuilt, differ, stale);
  EXPECT_EQ(differ, 0u);
  EXPECT_LE(stale, 29u);
}

// Every stock behaviour info, made again from its type, name, element and payload, is the stock info.
TEST(UiCanvas, MakeBehaviourRebuildsEveryStockInfoFromItsFields) {
  const auto files = CorpusFiles();
  if (files.empty()) GTEST_SKIP() << "NEVR_UI_CANVAS_CORPUS not set";
  std::size_t rebuilt = 0, differ = 0;
  for (const auto& f : files) {
    const std::vector<std::uint8_t> b = ReadFile(f);
    uc::Canvas c;
    ASSERT_TRUE(uc::Decode(b.data(), b.size(), &c, nullptr));
    for (const uc::Behaviour& stock : c.behaviours) {
      const uc::Behaviour made = uc::MakeBehaviour(stock.type(), stock.name(), stock.element(), stock.payload);
      ++rebuilt;
      if (made.raw != stock.raw && ++differ <= 3) ADD_FAILURE() << f.filename().string() << " behaviour differs";
    }
  }
  std::printf("[ui_canvas] behaviours rebuilt=%zu differ=%zu\n", rebuilt, differ);
  EXPECT_EQ(differ, 0u);
}

TEST(UiCanvas, WordsLaysOutSymbolsInOrder) {
  const std::vector<std::uint8_t> w = uc::Words({0x1122334455667788ULL, 7});
  ASSERT_EQ(w.size(), 16u);
  EXPECT_EQ(w[0], 0x88);
  EXPECT_EQ(w[8], 7);
}

// The header counts the game sizes its draw buffers by: RefreshHeaderCounts writes, for every stock canvas, at least
// what the stock header holds (a smaller count crashes the level), and exactly it for most.
TEST(UiCanvas, HeaderCountsAreNeverBelowTheStockOnes) {
  const auto files = CorpusFiles();
  if (files.empty()) GTEST_SKIP() << "NEVR_UI_CANVAS_CORPUS not set";
  std::size_t canvases = 0, exact = 0;
  for (const auto& f : files) {
    const std::vector<std::uint8_t> b = ReadFile(f);
    uc::Canvas c;
    ASSERT_TRUE(uc::Decode(b.data(), b.size(), &c, nullptr));
    const uc::Canvas stock = c;
    uc::RefreshHeaderCounts(&c);
    bool same = true;
    for (const std::size_t at : {std::size_t{8}, std::size_t{40}, std::size_t{44}, std::size_t{48}}) {
      std::uint32_t mine, theirs;
      std::memcpy(&mine, c.header.data() + at, 4);
      std::memcpy(&theirs, stock.header.data() + at, 4);
      EXPECT_GE(mine, theirs) << f.filename().string() << " header +" << at;
      same = same && mine == theirs;
    }
    ++canvases;
    exact += same ? 1 : 0;
  }
  std::printf("[ui_canvas] header counts: canvases=%zu exact=%zu\n", canvases, exact);
  // 253 of 354 match exactly; the rest are above (child canvases and the rarer element types are counted as quads and
  // images, which the stock counts do not always include), never below.
  EXPECT_GE(exact, 253u);
}

TEST(UiCanvas, HeaderCountsCountRootsImagesAndQuads) {
  uc::Canvas c;
  uc::ElementSpec group;
  const std::int32_t g = uc::Append(&c, group);
  uc::ElementSpec sprite;
  sprite.type = uc::kSprite;
  sprite.parent = g;
  uc::Append(&c, sprite);
  uc::ElementSpec text;
  text.type = uc::kText;
  text.capacity = 17;
  uc::Append(&c, text);
  uc::RefreshHeaderCounts(&c);
  std::uint32_t w[4];
  for (int i = 0; i < 4; ++i) std::memcpy(&w[i], c.header.data() + (i == 0 ? 8 : 36 + 4 * i), 4);
  EXPECT_EQ(w[0], 2u);             // two top-level elements
  EXPECT_EQ(w[1], 1u);             // one image
  EXPECT_EQ(w[2], 4u * (2 + 16));  // the group, the sprite and 16 character slots
  EXPECT_EQ(w[3], 6u * (2 + 16));
}

// Every stock canvas: decode, encode, the same bytes. NEVR_UI_CANVAS_CORPUS names a directory of canvas files (for
// example an EchoConstruct corpus's CUICanvasResourceWin10 folder).
TEST(UiCanvas, EveryStockCanvasRoundTrips) {
  const char* dir = std::getenv("NEVR_UI_CANVAS_CORPUS");
  if (dir == nullptr || *dir == '\0') GTEST_SKIP() << "NEVR_UI_CANVAS_CORPUS not set: no stock canvases to read";
  std::size_t files = 0, refused = 0, changed = 0;
  for (const auto& entry : std::filesystem::directory_iterator(dir)) {
    if (!entry.is_regular_file()) continue;
    std::ifstream in(entry.path(), std::ios::binary);
    const std::vector<std::uint8_t> b((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    ++files;
    uc::Canvas c;
    std::string why;
    if (!uc::Decode(b.data(), b.size(), &c, &why)) {
      ++refused;
      ADD_FAILURE() << entry.path().filename().string() << ": " << why;
      continue;
    }
    if (uc::Encode(c) != b) {
      ++changed;
      ADD_FAILURE() << entry.path().filename().string() << ": encode(decode(stock)) differs";
    }
  }
  EXPECT_GT(files, 0u);
  std::printf("[ui_canvas] stock canvases=%zu refused=%zu changed=%zu\n", files, refused, changed);
}
