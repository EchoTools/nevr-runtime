#pragma once

// #1535 / #1599: reading and writing a UI canvas resource (CUICanvasResource), in memory.
//
// The FRIENDS panel and its friend profile are built from canvases the game delivers (the player's own data), never
// from stored ones: a resource transform hands this codec the stock bytes, the panel code changes the decoded canvas
// and the codec writes it back. decode(encode(c)) round-trips, and encode(decode(stock)) equals the stock bytes for
// every stock canvas (tests: src/runtime/tests/test_ui_canvas.cpp).
//
// The layout is EchoConstruct's structural codec (src/codecs/ui_canvas.cpp, C-Luddy) as the friends panel prototype's
// Python port carried it:
//
//   432-byte header. Six array descriptors in it, each 56 bytes with the byte size at +8, the capacity at +40 and the
//   count at +48 (a resource on disk carries no pointer: +0 is 0): elements at +80 (224 bytes each), behaviour infos
//   at +136 (88 bytes), named rects at +192 (24 bytes: a symbol and 16 bytes), text offsets at +256 (4 bytes), the
//   text pool at +312 (1 byte) and pairs at +368 (24 bytes, carried as they are).
//   Then, front to back: the elements; when there are behaviours, 8-aligned, the infos and after them each info's
//   payload (its length is the descriptor at +16 of the info, count at +64); then the named rects (8-aligned), the
//   text offsets (4-aligned), the pool and the pairs (8-aligned), each table aligned only when it is not empty.
//
// Element fields (224 bytes), each checked against every element of the 354 stock canvases (8414 elements):
//   every type: +0 name (CSymbol64), +8 type (1 text, 2 group, 3 sprite, 4 mask, 8 child canvas), +20 the rect resolved
//   in the parent (4 floats: anchor x parent size + offset; the canvas's authored size for a top-level element; 8408 of
//   8414 stock elements hold exactly that, 6 hold a stale value), +36 anchors (4 floats, fractions of the parent), +52
//   offsets (4 floats: x0, y0, x1, y1 in authored units), +68 pivot (2 floats, 0.5 in most), +76 position (2 floats,
//   the offsets' x0, y0), +84 size (2 floats), +92 parent index (-1: none; a parent always comes before its children),
//   +96 own index, +100 the number of children (8413 of 8414), +104 the position among the parent's children from 0
//   (all 8414), +120 colour (4 floats);
//   group: +136 1 on the stock button groups (play_btn and the pop-up buttons), 0 on the rest; meaning not known;
//   sprite: +136 texture (CSymbol64), +144 UV rect (4 floats);
//   text: +136 font (lonemono_bold in 1743 of 1771 texts; lonemono, lonemono_black), +144 text slot, +148 capacity,
//   +156 alignment (2 floats), +164 font size, +176 a second colour (200/255 grey, opaque, in 1727), +192 the symbol
//   radstr, +200 and +208 -1 (no string-table reference), +216 the symbol StringTableEntry.
//   Words at +12, +16, +108, +112 and +168 are zero in nearly every stock element; new elements leave them zero.
// A behaviour info (88 bytes), the same in all 6508 stock infos: +0 type, +8 name, +16 the payload's array descriptor
// (56 bytes: +24 byte size, +40 0x100000000, +48 0x20, +56 capacity, +64 count; no pointer), +72 the element it drives
// (8 bytes), +80 0xffff.
//
// Pure byte logic, header only, free of Windows, Android and game headers, no exceptions: the Quest library compiles
// it as it is (#1599).

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

#include "nevr/symbol_hash.h"

namespace nevr_ui_canvas {

inline constexpr std::size_t kHeaderBytes = 432;
inline constexpr std::size_t kElementBytes = 224;
inline constexpr std::size_t kInfoBytes = 88;
inline constexpr std::size_t kRectBytes = 24;
inline constexpr std::size_t kPairBytes = 24;

inline constexpr std::uint32_t kText = 1;
inline constexpr std::uint32_t kGroup = 2;
inline constexpr std::uint32_t kSprite = 3;
inline constexpr std::uint32_t kMask = 4;
inline constexpr std::uint32_t kChildCanvas = 8;

namespace detail {
template <typename T>
T Get(const std::uint8_t* at) {
  T v;
  std::memcpy(&v, at, sizeof(v));
  return v;
}
template <typename T>
void Put(std::uint8_t* at, T v) {
  std::memcpy(at, &v, sizeof(v));
}
inline std::size_t Align(std::size_t at, std::size_t a) { return (at + a - 1) / a * a; }
}  // namespace detail

struct Rect {
  float x0, y0, x1, y1;
};

struct Element {
  std::array<std::uint8_t, kElementBytes> raw{};

  std::uint64_t name() const { return detail::Get<std::uint64_t>(raw.data() + 0); }
  void set_name(std::uint64_t v) { detail::Put(raw.data() + 0, v); }
  std::uint32_t type() const { return detail::Get<std::uint32_t>(raw.data() + 8); }
  void set_type(std::uint32_t v) { detail::Put(raw.data() + 8, v); }
  std::int32_t parent() const { return detail::Get<std::int32_t>(raw.data() + 92); }
  void set_parent(std::int32_t v) { detail::Put(raw.data() + 92, v); }
  std::int32_t index() const { return detail::Get<std::int32_t>(raw.data() + 96); }
  void set_index(std::int32_t v) { detail::Put(raw.data() + 96, v); }
  std::uint64_t texture() const { return detail::Get<std::uint64_t>(raw.data() + 136); }
  void set_texture(std::uint64_t v) { detail::Put(raw.data() + 136, v); }
  std::uint32_t text_slot() const { return detail::Get<std::uint32_t>(raw.data() + 144); }
  void set_text_slot(std::uint32_t v) { detail::Put(raw.data() + 144, v); }
  std::uint32_t text_capacity() const { return detail::Get<std::uint32_t>(raw.data() + 148); }
  void set_text_capacity(std::uint32_t v) { detail::Put(raw.data() + 148, v); }
  std::uint32_t font_size() const { return detail::Get<std::uint32_t>(raw.data() + 164); }
  void set_font_size(std::uint32_t v) { detail::Put(raw.data() + 164, v); }

  Rect anchors() const { return detail::Get<Rect>(raw.data() + 36); }
  Rect offsets() const { return detail::Get<Rect>(raw.data() + 52); }
  std::array<float, 4> colour() const { return detail::Get<std::array<float, 4>>(raw.data() + 120); }
  void set_colour(const std::array<float, 4>& rgba) { detail::Put(raw.data() + 120, rgba); }
  // A sprite's UV rect shares +144 with a text's slot and capacity.
  void set_uv(const Rect& uv) { detail::Put(raw.data() + 144, uv); }
  void set_alignment(float x, float y) {
    detail::Put(raw.data() + 156, x);
    detail::Put(raw.data() + 160, y);
  }

  std::array<float, 2> pivot() const { return detail::Get<std::array<float, 2>>(raw.data() + 68); }
  void set_pivot(const std::array<float, 2>& p) { detail::Put(raw.data() + 68, p); }

  // Anchors (fractions of the parent) and offsets (authored units), and the position that follows from them: the
  // pivot point of the offsets (+76: offset x0 + pivot x (x1 - x0), the same for y; 7808 of 8414 stock elements
  // exactly, the rest within the stale rects). The size (+84) is the rect resolved in the parent: RefreshLayout.
  void set_layout(const Rect& anchor, const Rect& rect) {
    const std::array<float, 2> p = pivot();
    detail::Put(raw.data() + 36, anchor);
    detail::Put(raw.data() + 52, rect);
    detail::Put(raw.data() + 76, rect.x0 + p[0] * (rect.x1 - rect.x0));
    detail::Put(raw.data() + 80, rect.y0 + p[1] * (rect.y1 - rect.y0));
  }
};

struct Behaviour {
  std::array<std::uint8_t, kInfoBytes> raw{};
  std::vector<std::uint8_t> payload;

  std::uint64_t type() const { return detail::Get<std::uint64_t>(raw.data() + 0); }
  std::uint64_t name() const { return detail::Get<std::uint64_t>(raw.data() + 8); }
  std::uint32_t element() const { return detail::Get<std::uint32_t>(raw.data() + 72); }
  void set_element(std::uint32_t v) { detail::Put(raw.data() + 72, v); }
};

// A behaviour made from its fields; Encode writes the payload's length into the descriptor.
inline Behaviour MakeBehaviour(std::uint64_t type, std::uint64_t name, std::uint32_t element,
                               std::vector<std::uint8_t> payload) {
  Behaviour b;
  std::uint8_t* r = b.raw.data();
  detail::Put<std::uint64_t>(r + 0, type);
  detail::Put<std::uint64_t>(r + 8, name);
  detail::Put<std::uint64_t>(r + 16 + 24, 0x100000000ULL);
  detail::Put<std::uint64_t>(r + 16 + 32, 0x20);
  for (const std::size_t k : {8, 40, 48}) detail::Put<std::uint64_t>(r + 16 + k, payload.size());
  detail::Put<std::uint64_t>(r + 72, element);
  detail::Put<std::uint64_t>(r + 80, 0xFFFF);
  b.payload = std::move(payload);
  return b;
}

// A payload of 8-byte words (a symbol list, the shape of the dynamic-texture and pointer behaviours' payloads).
inline std::vector<std::uint8_t> Words(std::initializer_list<std::uint64_t> words) {
  std::vector<std::uint8_t> out(words.size() * 8);
  std::size_t at = 0;
  for (const std::uint64_t w : words) {
    std::memcpy(out.data() + at, &w, 8);
    at += 8;
  }
  return out;
}

struct NamedRect {
  std::uint64_t symbol = 0;
  std::array<std::uint8_t, 16> rect{};
};

struct Canvas {
  std::array<std::uint8_t, kHeaderBytes> header{};
  std::vector<Element> elements;
  std::vector<Behaviour> behaviours;
  std::vector<NamedRect> rects;
  std::vector<std::uint32_t> text_offsets;
  std::vector<std::uint8_t> pool;
  std::vector<std::array<std::uint8_t, kPairBytes>> pairs;

  // Render target pixels (+12, +16) and authored units (+20, +24).
  std::uint32_t render_width() const { return detail::Get<std::uint32_t>(header.data() + 12); }
  std::uint32_t render_height() const { return detail::Get<std::uint32_t>(header.data() + 16); }
  std::uint32_t authored_width() const { return detail::Get<std::uint32_t>(header.data() + 20); }
  std::uint32_t authored_height() const { return detail::Get<std::uint32_t>(header.data() + 24); }
  void set_authored_width(std::uint32_t v) { detail::Put(header.data() + 20, v); }

  // A new pool run of `capacity` bytes holding `text`; returns its slot, or -1 when the text does not fit (it needs its
  // NUL inside the run).
  std::int64_t AddTextSlot(const std::string& text, std::uint32_t capacity) {
    if (text.size() >= capacity) return -1;
    const auto slot = static_cast<std::int64_t>(text_offsets.size());
    text_offsets.push_back(static_cast<std::uint32_t>(pool.size()));
    pool.insert(pool.end(), text.begin(), text.end());
    pool.insert(pool.end(), capacity - text.size(), 0);
    return slot;
  }

  // The text an element shows (up to its NUL), or "" when its slot is not in the tables.
  std::string Text(const Element& e) const {
    if (e.text_slot() >= text_offsets.size()) return "";
    const std::size_t at = text_offsets[e.text_slot()];
    std::string out;
    for (std::size_t i = at; i < pool.size() && i < at + e.text_capacity() && pool[i] != 0; ++i)
      out.push_back(static_cast<char>(pool[i]));
    return out;
  }

  // The first element named `name`, or -1.
  std::int64_t Find(std::uint64_t name) const {
    for (std::size_t i = 0; i < elements.size(); ++i)
      if (elements[i].name() == name) return static_cast<std::int64_t>(i);
    return -1;
  }
};

// ---- building elements from their fields ----

inline constexpr std::uint64_t kRadStr = EchoVR::CSymbol64Hash("radstr");
inline constexpr std::uint64_t kStringTableEntry = EchoVR::CSymbol64Hash("StringTableEntry");
inline constexpr std::uint64_t kFontBold = EchoVR::CSymbol64Hash("lonemono_bold");
static_assert(kRadStr == 0x41D2D53C1E30130CULL && kStringTableEntry == 0x89423FB49877EEADULL &&
                  kFontBold == 0xDBCD462F8D9F0512ULL,
              "the text fields hold these symbols in every stock canvas");

// What an element is made of. Index, the resolved rect, the child count and the sibling position are not here: Append
// sets the index and RefreshLayout the other three.
struct ElementSpec {
  std::uint64_t name = 0;
  std::uint32_t type = kGroup;
  std::int32_t parent = -1;
  Rect anchors{0.0f, 0.0f, 0.0f, 0.0f};
  Rect offsets{0.0f, 0.0f, 0.0f, 0.0f};
  std::array<float, 2> pivot{0.5f, 0.5f};
  std::array<float, 4> colour{1.0f, 1.0f, 1.0f, 1.0f};
  bool button_flag = false;  // group: +136 (1 on the stock button groups)
  std::uint64_t texture = 0;  // sprite
  Rect uv{0.0f, 0.0f, 1.0f, 1.0f};
  std::uint64_t font = kFontBold;  // text
  std::uint32_t text_slot = 0;
  std::uint32_t capacity = 0;
  std::uint32_t font_size = 32;
  std::array<float, 2> alignment{0.0f, 0.0f};
  std::array<float, 4> second_colour{200.0f / 255.0f, 200.0f / 255.0f, 200.0f / 255.0f, 1.0f};
};

inline Element MakeElement(const ElementSpec& s) {
  using detail::Put;
  Element e;
  std::uint8_t* r = e.raw.data();
  e.set_name(s.name);
  e.set_type(s.type);
  e.set_pivot(s.pivot);
  e.set_layout(s.anchors, s.offsets);
  Put(r + 20, s.offsets);  // resolved in the parent by RefreshLayout, with the size
  Put(r + 84, std::array<float, 2>{s.offsets.x1 - s.offsets.x0, s.offsets.y1 - s.offsets.y0});
  e.set_parent(s.parent);
  e.set_colour(s.colour);
  if (s.type == kGroup) {
    Put<std::uint32_t>(r + 136, s.button_flag ? 1u : 0u);
  } else if (s.type == kSprite) {
    e.set_texture(s.texture);
    e.set_uv(s.uv);
  } else if (s.type == kText) {
    e.set_texture(s.font);
    e.set_text_slot(s.text_slot);
    e.set_text_capacity(s.capacity);
    e.set_alignment(s.alignment[0], s.alignment[1]);
    e.set_font_size(s.font_size);
    Put(r + 176, s.second_colour);
    Put<std::uint64_t>(r + 192, kRadStr);
    Put<std::uint64_t>(r + 200, ~std::uint64_t{0});
    Put<std::uint64_t>(r + 208, ~std::uint64_t{0});
    Put<std::uint64_t>(r + 216, kStringTableEntry);
  }
  return e;
}

// Appends an element at the end (its index is its position) and returns that index.
inline std::int32_t Append(Canvas* c, const ElementSpec& s) {
  Element e = MakeElement(s);
  const auto at = static_cast<std::int32_t>(c->elements.size());
  e.set_index(at);
  c->elements.push_back(e);
  return at;
}

// Writes the fields that follow from the layout, front to back (a parent comes before its children): each element's
// resolved rect (+20) and size (+84, the resolved rect's width and height: 8409 of 8414 stock elements), its child
// count (+100) and its position among its parent's children (+104). False when an
// element names a parent that is not before it; nothing is changed then.
inline bool RefreshLayout(Canvas* c) {
  using detail::Put;
  const std::size_t n = c->elements.size();
  for (std::size_t i = 0; i < n; ++i) {
    const std::int32_t p = c->elements[i].parent();
    if (p >= static_cast<std::int32_t>(i) || p < -1) return false;
  }
  std::vector<std::uint32_t> children(n, 0);
  std::uint32_t roots = 0;
  for (std::size_t i = 0; i < n; ++i) {
    Element& e = c->elements[i];
    const std::int32_t p = e.parent();
    float pw = static_cast<float>(c->authored_width()), ph = static_cast<float>(c->authored_height());
    if (p >= 0) {
      const Rect pr = detail::Get<Rect>(c->elements[static_cast<std::size_t>(p)].raw.data() + 20);
      pw = pr.x1 - pr.x0;
      ph = pr.y1 - pr.y0;
    }
    const Rect a = e.anchors(), o = e.offsets();
    const Rect resolved{a.x0 * pw + o.x0, a.y0 * ph + o.y0, a.x1 * pw + o.x1, a.y1 * ph + o.y1};
    Put(e.raw.data() + 20, resolved);
    Put(e.raw.data() + 84, std::array<float, 2>{resolved.x1 - resolved.x0, resolved.y1 - resolved.y0});
    Put<std::uint32_t>(e.raw.data() + 104, p >= 0 ? children[static_cast<std::size_t>(p)]++ : roots++);
  }
  for (std::size_t i = 0; i < n; ++i) Put<std::uint32_t>(c->elements[i].raw.data() + 100, children[i]);
  return true;
}

// The header's counts the game sizes a canvas's draw buffers by, which a canvas with more elements than its stock
// one must raise (a canvas whose counts are too small crashes the level as it loads; seen in game). Measured on the
// 354 stock canvases: +8 is the number of top-level elements (all 354); +40 the image elements, sprites, masks and
// child canvases (343 exactly); +44 and +48 four and six per quad, a quad per element that is not a text and per
// character slot of each text (its capacity less the NUL; 299 exactly, and the rest within one or two quads of it,
// stock below this count). The counts written are never below what the elements need.
inline void RefreshHeaderCounts(Canvas* c) {
  std::uint32_t roots = 0, images = 0, quads = 0;
  for (const Element& e : c->elements) {
    if (e.parent() < 0) ++roots;
    const std::uint32_t t = e.type();
    if (t == kSprite || t == kMask || t == kChildCanvas) ++images;
    if (t == kText) {
      quads += e.text_capacity() > 0 ? e.text_capacity() - 1 : 0;
    } else {
      ++quads;
    }
  }
  detail::Put<std::uint32_t>(c->header.data() + 8, roots);
  detail::Put<std::uint32_t>(c->header.data() + 40, images);
  detail::Put<std::uint32_t>(c->header.data() + 44, 4 * quads);
  detail::Put<std::uint32_t>(c->header.data() + 48, 6 * quads);
}

enum class Table { kElements, kInfos, kRects, kOffsets, kPool, kPairs };
inline constexpr std::array<std::size_t, 6> kDescriptor = {80, 136, 192, 256, 312, 368};
inline constexpr std::array<std::size_t, 6> kStride = {kElementBytes, kInfoBytes, kRectBytes, 4, 1, kPairBytes};

// Reads a canvas. False, with `why` set, when the bytes are not one: a descriptor that carries a pointer or a size
// that does not match its capacity, a table that runs past the end, or bytes after the last table.
inline bool Decode(const std::uint8_t* b, std::size_t n, Canvas* out, std::string* why) {
  using detail::Align;
  using detail::Get;
  const auto fail = [why](const char* what) {
    if (why != nullptr) *why = what;
    return false;
  };
  if (b == nullptr || out == nullptr || n < kHeaderBytes) return fail("shorter than the header");
  std::array<std::uint64_t, 6> count{};
  for (std::size_t t = 0; t < 6; ++t) {
    const std::uint8_t* d = b + kDescriptor[t];
    const auto data = Get<std::uint64_t>(d), size = Get<std::uint64_t>(d + 8);
    const auto capacity = Get<std::uint64_t>(d + 40), cnt = Get<std::uint64_t>(d + 48);
    if (data != 0 || cnt > capacity || size != capacity * kStride[t] || cnt > n) return fail("a descriptor is not at rest");
    count[t] = cnt;
  }
  Canvas c;
  std::memcpy(c.header.data(), b, kHeaderBytes);
  std::size_t at = kHeaderBytes;
  const auto room = [&](std::size_t bytes) { return at <= n && bytes <= n - at; };
  if (!room(count[0] * kElementBytes)) return fail("the elements run past the end");
  c.elements.resize(count[0]);
  for (auto& e : c.elements) {
    std::memcpy(e.raw.data(), b + at, kElementBytes);
    at += kElementBytes;
  }
  if (count[1] != 0) {
    at = Align(at, 8);
    if (!room(count[1] * kInfoBytes)) return fail("the behaviour infos run past the end");
    c.behaviours.resize(count[1]);
    for (auto& h : c.behaviours) {
      std::memcpy(h.raw.data(), b + at, kInfoBytes);
      at += kInfoBytes;
    }
    for (auto& h : c.behaviours) {
      const auto bytes = Get<std::uint64_t>(h.raw.data() + 16 + 48);
      if (!room(bytes)) return fail("a behaviour payload runs past the end");
      h.payload.assign(b + at, b + at + bytes);
      at += bytes;
    }
  }
  if (count[2] != 0) at = Align(at, 8);
  if (!room(count[2] * kRectBytes)) return fail("the named rects run past the end");
  c.rects.resize(count[2]);
  for (auto& r : c.rects) {
    r.symbol = Get<std::uint64_t>(b + at);
    std::memcpy(r.rect.data(), b + at + 8, 16);
    at += kRectBytes;
  }
  if (count[3] != 0) at = Align(at, 4);
  if (!room(count[3] * 4)) return fail("the text offsets run past the end");
  c.text_offsets.resize(count[3]);
  for (auto& o : c.text_offsets) {
    o = Get<std::uint32_t>(b + at);
    at += 4;
  }
  if (!room(count[4])) return fail("the text pool runs past the end");
  c.pool.assign(b + at, b + at + count[4]);
  at += count[4];
  if (count[5] != 0) at = Align(at, 8);
  if (!room(count[5] * kPairBytes)) return fail("the pairs run past the end");
  c.pairs.resize(count[5]);
  for (auto& p : c.pairs) {
    std::memcpy(p.data(), b + at, kPairBytes);
    at += kPairBytes;
  }
  if (at != n) return fail("bytes after the last table");
  *out = std::move(c);
  return true;
}

// Writes a canvas: the header as it is, with each descriptor's count set and its capacity (and byte size) raised to the
// count when the count is larger; each behaviour info's payload descriptor set to its payload's length.
inline std::vector<std::uint8_t> Encode(const Canvas& c) {
  using detail::Align;
  using detail::Get;
  using detail::Put;
  const std::array<std::uint64_t, 6> count = {c.elements.size(), c.behaviours.size(), c.rects.size(),
                                              c.text_offsets.size(), c.pool.size(), c.pairs.size()};
  std::size_t at = kHeaderBytes + kElementBytes * c.elements.size();
  std::size_t info_at = 0;
  std::vector<std::size_t> payload_at;
  if (!c.behaviours.empty()) {
    info_at = at = Align(at, 8);
    at += kInfoBytes * c.behaviours.size();
    for (const auto& h : c.behaviours) {
      payload_at.push_back(at);
      at += h.payload.size();
    }
  }
  std::array<std::size_t, 6> tail_at{};
  const std::array<std::size_t, 4> tail_align = {8, 4, 1, 8};
  for (std::size_t t = 2; t < 6; ++t) {
    if (count[t] != 0) at = Align(at, tail_align[t - 2]);
    tail_at[t] = at;
    at += count[t] * kStride[t];
  }
  std::vector<std::uint8_t> out(at, 0);
  std::memcpy(out.data(), c.header.data(), kHeaderBytes);
  for (std::size_t t = 0; t < 6; ++t) {
    std::uint8_t* d = out.data() + kDescriptor[t];
    Put<std::uint64_t>(d + 48, count[t]);
    if (Get<std::uint64_t>(d + 40) < count[t]) {
      Put<std::uint64_t>(d + 40, count[t]);
      Put<std::uint64_t>(d + 8, count[t] * kStride[t]);
    }
  }
  for (std::size_t i = 0; i < c.elements.size(); ++i)
    std::memcpy(out.data() + kHeaderBytes + kElementBytes * i, c.elements[i].raw.data(), kElementBytes);
  for (std::size_t i = 0; i < c.behaviours.size(); ++i) {
    std::uint8_t* o = out.data() + info_at + kInfoBytes * i;
    std::memcpy(o, c.behaviours[i].raw.data(), kInfoBytes);
    const std::uint64_t bytes = c.behaviours[i].payload.size();
    for (const std::size_t k : {8, 40, 48}) Put<std::uint64_t>(o + 16 + k, bytes);
    if (bytes != 0) std::memcpy(out.data() + payload_at[i], c.behaviours[i].payload.data(), bytes);
  }
  for (std::size_t i = 0; i < c.rects.size(); ++i) {
    std::uint8_t* o = out.data() + tail_at[2] + kRectBytes * i;
    Put<std::uint64_t>(o, c.rects[i].symbol);
    std::memcpy(o + 8, c.rects[i].rect.data(), 16);
  }
  for (std::size_t i = 0; i < c.text_offsets.size(); ++i) Put<std::uint32_t>(out.data() + tail_at[3] + 4 * i, c.text_offsets[i]);
  if (!c.pool.empty()) std::memcpy(out.data() + tail_at[4], c.pool.data(), c.pool.size());
  for (std::size_t i = 0; i < c.pairs.size(); ++i)
    std::memcpy(out.data() + tail_at[5] + kPairBytes * i, c.pairs[i].data(), kPairBytes);
  return out;
}

// EchoConstruct's content check (first_content_problem), every problem: an element whose index is not its position,
// a parent that is not an element, a text whose slot or run is outside the tables, a sprite without a texture, a text
// slot read by other than exactly one text, a behaviour that drives no element. Empty: the game can load it.
inline std::vector<std::string> Problems(const Canvas& c) {
  std::vector<std::string> out;
  const auto n = static_cast<std::int64_t>(c.elements.size());
  std::vector<int> used(c.text_offsets.size(), 0);
  for (std::int64_t i = 0; i < n; ++i) {
    const Element& e = c.elements[static_cast<std::size_t>(i)];
    const std::string at = "element " + std::to_string(i);
    if (e.index() != i) out.push_back(at + " carries index " + std::to_string(e.index()));
    if (e.parent() < -1 || e.parent() >= n || e.parent() == i) out.push_back(at + "'s parent is not an element");
    if (e.type() == kText) {
      if (e.text_slot() >= c.text_offsets.size()) {
        out.push_back(at + "'s text slot is past the offset table");
      } else {
        ++used[e.text_slot()];
        if (static_cast<std::uint64_t>(c.text_offsets[e.text_slot()]) + e.text_capacity() > c.pool.size())
          out.push_back(at + "'s text runs past the pool");
      }
    }
    if (e.type() == kSprite && (e.texture() == 0 || e.texture() == ~std::uint64_t{0}))
      out.push_back(at + " is a sprite without a texture");
  }
  for (std::size_t s = 0; s < used.size(); ++s)
    if (used[s] != 1) out.push_back("text slot " + std::to_string(s) + " read " + std::to_string(used[s]) + " times");
  for (std::size_t i = 0; i < c.behaviours.size(); ++i)
    if (static_cast<std::int64_t>(c.behaviours[i].element()) >= n)
      out.push_back("behaviour " + std::to_string(i) + " drives no element");
  return out;
}

}  // namespace nevr_ui_canvas
