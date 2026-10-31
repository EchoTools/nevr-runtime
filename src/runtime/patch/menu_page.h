#pragma once

// #906 / ADR 0017 D4: inserting card elements into a menu page resource (CR15UIPage2CR), in memory.
//
// The main-menu page (mnu_master) is one buffer the resource loader hands to CR15UIPage2CR's serializer. The
// runtime adds the CREATE GAME and JOIN VIA CODE cards to a copy of that buffer, so the player's own data is the
// only source of the page and nothing derived from it is stored or shipped.
//
// Layout, read from the serializers (echovr.exe: CR15UIPage2CR_ReadPageTable 0x14082eea0,
// CR15UIPage2CR_ReadElementArray 0x140830620, ReadRowArray16 0x14026fa90, ReadRowArray72 0x140275f00) and credited to
// the echovr-modkit author's cards_page.py, whose documented byte layout the serializers confirm. Every array is
// a 56-byte descriptor (count at +0x28 and +0x30) followed, in the order the serializer consumes them, by its
// payload; a descriptor carries no pointer, so the buffer is read front to back:
//
//   top descriptor, then count page records of 328 bytes
//   per page record, in order: five descriptors at +32 (elements, 472 bytes each), +88, +144, +200, +272 (16-byte
//     rows). A page's element payload is followed by each element's eight nested payloads, in element order, and
//     then by the +88, +144, +200 and +272 payloads.
//   per element: word0 = owner actor, word1 = 0, word2 = the symbol `default`, then eight descriptors at +24, +80,
//     +136, +192, +248 (16-byte rows), +304, +360 (stride not read: must be empty) and +416 (72-byte rows).
//     A card fills +80 (one row: the symbol `sprite` and a tile symbol) and +416 (three rows: an 8-byte key and
//     a 64-byte NUL-padded string for `text`, `subtext`, `tip`).
//
// Inserting adds two blocks (the elements after the page's last element, their nested payloads before the page's
// +88 payload) and raises the one count in the page's +32 descriptor. The result is decoded again and the
// insertions are removed again before it is returned; anything that does not hold is refused, never patched.
//
// Pure byte logic, header only, free of Windows and game headers (like credits_text.h), so it is unit-tested
// without a game (src/runtime/tests/test_menu_page.cpp). The process side is menu_cards.cpp.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace nevr_menu_page {

inline constexpr size_t kDescriptorBytes = 56;
inline constexpr size_t kPageRecordBytes = 328;
inline constexpr size_t kElementBytes = 472;
inline constexpr size_t kSpriteRowBytes = 16;
inline constexpr size_t kTextRowBytes = 72;
/// No page resource is near this; a bigger buffer is not a page.
inline constexpr size_t kMaxResourceBytes = size_t{64} * 1024 * 1024;
/// A text row holds 64 bytes, the last of them the NUL.
inline constexpr size_t kMaxTextBytes = 63;

/// Descriptor offsets inside a page record.
inline constexpr size_t kPageSlots[5] = {32, 88, 144, 200, 272};
/// Descriptor offsets inside an element (24 + 56 * k).
inline constexpr size_t kElementSlots[8] = {24, 80, 136, 192, 248, 304, 360, 416};
inline constexpr size_t kSpriteSlot = 80;
inline constexpr size_t kTextSlot = 416;

enum class Status {
  Ok,
  NoCards,         // nothing to insert
  TooLarge,        // over kMaxResourceBytes (or the result would be)
  Truncated,       // a descriptor or payload runs past the buffer
  BadDescriptor,   // a descriptor with a pointer, a flag or a count that the format does not have
  UnobservedSlot,  // an element slot whose stride is unknown holds rows
  TrailingBytes,   // the arrays do not account for the whole buffer
  PageOutOfRange,
  DuplicateOwner,  // an owner already occurs in the buffer, or twice among the cards
  BadText,         // a card string over kMaxTextBytes, or not printable ASCII
  VerifyFailed,    // the result does not decode, or removing the insertion does not give the input back
};

inline const char* StatusName(Status s) {
  switch (s) {
    case Status::Ok: return "ok";
    case Status::NoCards: return "no_cards";
    case Status::TooLarge: return "too_large";
    case Status::Truncated: return "truncated";
    case Status::BadDescriptor: return "bad_descriptor";
    case Status::UnobservedSlot: return "unobserved_slot";
    case Status::TrailingBytes: return "trailing_bytes";
    case Status::PageOutOfRange: return "page_out_of_range";
    case Status::DuplicateOwner: return "duplicate_owner";
    case Status::BadText: return "bad_text";
    case Status::VerifyFailed: return "verify_failed";
  }
  return "unknown";
}

/// One card: the actor that owns it, the tile symbol of its sprite and three strings.
struct Card {
  uint64_t owner;
  uint64_t tile;
  std::string text;
  std::string subtext;
  std::string tip;
};

/// The symbols an element is built from (CSymbol64 of these names; pinned against the hash in test_menu_page.cpp).
struct ElementSymbols {
  uint64_t stateDefault;
  uint64_t sprite;
  uint64_t text;
  uint64_t subtext;
  uint64_t tip;
};

/// Where one page's pieces sit in the buffer.
struct PageInfo {
  size_t elementDescriptorOffset = 0;  // the +32 descriptor
  size_t elementPayloadOffset = 0;     // first element
  uint64_t elementCount = 0;
  size_t tailPayloadOffset = 0;        // the +88 payload: where the nested payloads of new elements go
};

struct Decoded {
  std::vector<PageInfo> pages;
  std::vector<uint64_t> owners;  // word0 of every element of every page, in file order
};

namespace detail {

inline uint64_t Load64(const uint8_t* p) {
  uint64_t v;
  std::memcpy(&v, p, sizeof(v));
  return v;
}
inline uint32_t Load32(const uint8_t* p) {
  uint32_t v;
  std::memcpy(&v, p, sizeof(v));
  return v;
}
inline void Store64(std::vector<uint8_t>& b, size_t at, uint64_t v) { std::memcpy(b.data() + at, &v, sizeof(v)); }
inline void Append64(std::vector<uint8_t>& b, uint64_t v) {
  const size_t at = b.size();
  b.resize(at + sizeof(v));
  Store64(b, at, v);
}

/// Stride of the rows behind an element descriptor; 0 for a slot the serializer reads with a stride we do not know.
inline size_t ElementStride(size_t slot) {
  if (slot == kTextSlot) return kTextRowBytes;
  if (slot == 304 || slot == 360) return 0;
  return kSpriteRowBytes;
}

/// A descriptor as the format has it: no pointer, flags 1, count equal to the used count, size = count * stride.
inline Status ReadDescriptor(const uint8_t* data, size_t size, size_t offset, size_t stride, uint64_t* count,
                             size_t* payloadBytes) {
  if (offset > size || size - offset < kDescriptorBytes) return Status::Truncated;
  const uint8_t* d = data + offset;
  const uint64_t ptr = Load64(d), bytes = Load64(d + 0x08), z10 = Load64(d + 0x10);
  const uint32_t z18 = Load32(d + 0x18), flags = Load32(d + 0x1C);
  const uint64_t n = Load64(d + 0x28), used = Load64(d + 0x30);
  if (ptr != 0 || z10 != 0 || z18 != 0 || flags != 1 || n != used) return Status::BadDescriptor;
  if (stride == 0) {
    if (n != 0 || bytes != 0) return Status::UnobservedSlot;
  } else {
    if (n > kMaxResourceBytes / stride || bytes != n * stride) return Status::BadDescriptor;
  }
  *count = n;
  *payloadBytes = static_cast<size_t>(bytes);
  return Status::Ok;
}

}  // namespace detail

/// Walks the whole buffer the way the serializer does and records where each page's pieces are. Refuses a buffer
/// that does not match the format exactly, including any byte the arrays do not account for.
inline Status Decode(const uint8_t* data, size_t size, Decoded* out) {
  if (size > kMaxResourceBytes) return Status::TooLarge;
  out->pages.clear();
  out->owners.clear();
  uint64_t pageCount = 0;
  size_t tableBytes = 0;
  Status s = detail::ReadDescriptor(data, size, 0, kPageRecordBytes, &pageCount, &tableBytes);
  if (s != Status::Ok) return s;
  size_t cur = kDescriptorBytes;
  if (size - cur < tableBytes) return Status::Truncated;
  cur += tableBytes;
  for (uint64_t page = 0; page < pageCount; ++page) {
    const size_t base = kDescriptorBytes + static_cast<size_t>(page) * kPageRecordBytes;
    PageInfo info;
    for (size_t slot : kPageSlots) {
      uint64_t count = 0;
      size_t bytes = 0;
      const size_t stride = slot == 32 ? kElementBytes : kSpriteRowBytes;
      s = detail::ReadDescriptor(data, size, base + slot, stride, &count, &bytes);
      if (s != Status::Ok) return s;
      if (size - cur < bytes) return Status::Truncated;
      const size_t start = cur;
      cur += bytes;
      if (slot == 88) info.tailPayloadOffset = start;
      if (slot != 32) continue;
      info.elementDescriptorOffset = base + slot;
      info.elementPayloadOffset = start;
      info.elementCount = count;
      for (uint64_t e = 0; e < count; ++e) {
        const size_t element = start + static_cast<size_t>(e) * kElementBytes;
        out->owners.push_back(detail::Load64(data + element));
        for (size_t childSlot : kElementSlots) {
          uint64_t childCount = 0;
          size_t childBytes = 0;
          s = detail::ReadDescriptor(data, size, element + childSlot, detail::ElementStride(childSlot), &childCount,
                                     &childBytes);
          if (s != Status::Ok) return s;
          if (size - cur < childBytes) return Status::Truncated;
          cur += childBytes;
        }
      }
    }
    out->pages.push_back(info);
  }
  return cur == size ? Status::Ok : Status::TrailingBytes;
}

namespace detail {

inline bool PrintableAscii(const std::string& s) {
  return std::all_of(s.begin(), s.end(), [](char c) { return c >= 0x20 && c < 0x7f; });
}

/// One 56-byte descriptor exactly as the format writes it (pointer 0, flags 1, mark 0x20).
inline void AppendDescriptor(std::vector<uint8_t>& b, uint64_t count, size_t stride) {
  Append64(b, 0);
  Append64(b, count * stride);
  Append64(b, 0);
  const uint32_t zero = 0, flags = 1;
  const size_t at = b.size();
  b.resize(at + 8);
  std::memcpy(b.data() + at, &zero, 4);
  std::memcpy(b.data() + at + 4, &flags, 4);
  Append64(b, 0x20);
  Append64(b, count);
  Append64(b, count);
}

inline void AppendText(std::vector<uint8_t>& b, uint64_t key, const std::string& s) {
  Append64(b, key);
  const size_t at = b.size();
  b.resize(at + 64, 0);
  std::memcpy(b.data() + at, s.data(), s.size());
}

/// The 472-byte element of a card.
inline std::vector<uint8_t> ElementBytes(const Card& c, const ElementSymbols& sym) {
  std::vector<uint8_t> b;
  b.reserve(kElementBytes);
  Append64(b, c.owner);
  Append64(b, 0);
  Append64(b, sym.stateDefault);
  for (size_t slot : kElementSlots) {
    if (slot == kSpriteSlot) AppendDescriptor(b, 1, kSpriteRowBytes);
    else if (slot == kTextSlot) AppendDescriptor(b, 3, kTextRowBytes);
    else AppendDescriptor(b, 0, 0);
  }
  return b;
}

/// The nested payloads of a card, in slot order: the sprite row, then the three text rows.
inline void AppendNested(std::vector<uint8_t>& b, const Card& c, const ElementSymbols& sym) {
  Append64(b, sym.sprite);
  Append64(b, c.tile);
  AppendText(b, sym.text, c.text);
  AppendText(b, sym.subtext, c.subtext);
  AppendText(b, sym.tip, c.tip);
}

inline bool Contains(const uint8_t* data, size_t size, uint64_t v) {
  uint8_t needle[8];
  std::memcpy(needle, &v, sizeof(v));
  return std::search(data, data + size, needle, needle + sizeof(needle)) != data + size;
}

}  // namespace detail

/// Appends `count` cards to page `page` of the buffer. On Ok, `*out` is the new buffer.
inline Status InsertCards(const uint8_t* data, size_t size, uint64_t page, const Card* cards, size_t count,
                          const ElementSymbols& sym, std::vector<uint8_t>* out) {
  if (count == 0) return Status::NoCards;
  Decoded before;
  Status s = Decode(data, size, &before);
  if (s != Status::Ok) return s;
  if (page >= before.pages.size()) return Status::PageOutOfRange;
  for (size_t i = 0; i < count; ++i) {
    const Card& c = cards[i];
    for (const std::string* t : {&c.text, &c.subtext, &c.tip}) {
      if (t->size() > kMaxTextBytes || !detail::PrintableAscii(*t)) return Status::BadText;
    }
    if (detail::Contains(data, size, c.owner)) return Status::DuplicateOwner;
    for (size_t j = 0; j < i; ++j) {
      if (cards[j].owner == c.owner) return Status::DuplicateOwner;
    }
  }

  std::vector<uint8_t> elements, nested;
  for (size_t i = 0; i < count; ++i) {
    const std::vector<uint8_t> e = detail::ElementBytes(cards[i], sym);
    elements.insert(elements.end(), e.begin(), e.end());
    detail::AppendNested(nested, cards[i], sym);
  }
  if (size + elements.size() + nested.size() > kMaxResourceBytes) return Status::TooLarge;

  const PageInfo& info = before.pages[static_cast<size_t>(page)];
  const size_t elementsAt = info.elementPayloadOffset + static_cast<size_t>(info.elementCount) * kElementBytes;
  const size_t nestedAt = info.tailPayloadOffset;
  if (elementsAt > nestedAt) return Status::VerifyFailed;

  // The later offset first, so the earlier one is still where Decode found it.
  std::vector<uint8_t> result(data, data + size);
  result.insert(result.begin() + static_cast<std::ptrdiff_t>(nestedAt), nested.begin(), nested.end());
  result.insert(result.begin() + static_cast<std::ptrdiff_t>(elementsAt), elements.begin(), elements.end());
  const uint64_t newCount = info.elementCount + count;
  detail::Store64(result, info.elementDescriptorOffset + 0x08, newCount * kElementBytes);
  detail::Store64(result, info.elementDescriptorOffset + 0x28, newCount);
  detail::Store64(result, info.elementDescriptorOffset + 0x30, newCount);

  // Verify: the result decodes with the same pages and the cards' owners added, and taking the insertion out again
  // gives the input back byte for byte.
  Decoded after;
  if (Decode(result.data(), result.size(), &after) != Status::Ok) return Status::VerifyFailed;
  if (after.pages.size() != before.pages.size()) return Status::VerifyFailed;
  if (after.pages[static_cast<size_t>(page)].elementCount != newCount) return Status::VerifyFailed;
  std::vector<uint64_t> expectedOwners = before.owners;
  size_t owner = 0;
  for (size_t p = 0; p < static_cast<size_t>(page); ++p) owner += static_cast<size_t>(before.pages[p].elementCount);
  owner += static_cast<size_t>(info.elementCount);
  std::vector<uint64_t> added;
  for (size_t i = 0; i < count; ++i) added.push_back(cards[i].owner);
  expectedOwners.insert(expectedOwners.begin() + static_cast<std::ptrdiff_t>(owner), added.begin(), added.end());
  if (after.owners != expectedOwners) return Status::VerifyFailed;

  std::vector<uint8_t> back = result;
  back.erase(back.begin() + static_cast<std::ptrdiff_t>(nestedAt + elements.size()),
             back.begin() + static_cast<std::ptrdiff_t>(nestedAt + elements.size() + nested.size()));
  back.erase(back.begin() + static_cast<std::ptrdiff_t>(elementsAt),
             back.begin() + static_cast<std::ptrdiff_t>(elementsAt + elements.size()));
  std::memcpy(back.data() + info.elementDescriptorOffset + 0x08, data + info.elementDescriptorOffset + 0x08, 8);
  std::memcpy(back.data() + info.elementDescriptorOffset + 0x28, data + info.elementDescriptorOffset + 0x28, 16);
  if (back.size() != size || std::memcmp(back.data(), data, size) != 0) return Status::VerifyFailed;

  *out = std::move(result);
  return Status::Ok;
}

}  // namespace nevr_menu_page
