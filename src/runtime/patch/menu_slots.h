#pragma once

// #1679: where the main menu's buttons stand, in memory.
//
// The game lays the main menu's cards out by the transform of each card's owner actor, not by the order of the page's
// element array: the level's CTransformCR (class symbol `CTransformCR`, the name `mnu_master` on PC and
// `mnu_master_lowspec` on Quest, the same 52,680 bytes in both) holds one 176-byte entry per actor, and the four buttons'
// owners (menu_cards.h: Join, Play, Tutorial, Create) have entries of their own at four fixed slots on an arc in front
// of the player. Measured on a PC client screenshot of the stock page with the cards added (the array then read Tutorials,
// Play, Create, Join): left to right the buttons read JOIN VIA CODE, PLAY, TUTORIALS, CREATE GAME, which is the order of
// decreasing x of those four entries. The array order moved nothing.
//
// So the order is changed by giving the four owners the poses (rotation, position, scale: 40 bytes at +0x20 of the
// entry) of the four slots in the order wanted. A card keeps the rotation of its slot, so the arc still faces the
// player. Nothing else is written: not the owners, not the parent links, not any other entry, and the buffer keeps its
// size. The result is decoded again and anything that does not hold is refused, never patched.
//
// Layout, from the game's own bytes (tools/echomod CTransformCR, checked against the 299 entries of the stock level): a
// 56-byte descriptor (u64 data size at +0x08 = count * 176, u64 count at +0x28 and again at +0x30), then count entries;
// per entry u64 node at +0x00, u64 owning actor at +0x08, then the pose at +0x20: rotation x y z w, position x y z, scale
// x y z (ten little-endian floats).
//
// Pure byte logic, header only, free of Windows and game headers (like menu_page.h), so it is unit-tested without a game
// (src/runtime/tests/test_menu_slots.cpp). The process sides are menu_cards.cpp (PC) and
// src/quest/sentinel/menu_cards_hook.cpp (Quest), which call it through menu_cards.h.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

namespace nevr_menu_slots {

inline constexpr size_t kDescriptorBytes = 56;
inline constexpr size_t kEntryBytes = 176;
inline constexpr size_t kOwnerOffset = 0x08;
inline constexpr size_t kPoseOffset = 0x20;
inline constexpr size_t kPoseBytes = 40;
/// The x of the position inside an entry: pose + rotation (16 bytes).
inline constexpr size_t kPositionXOffset = kPoseOffset + 16;
/// No level's transform table is near this; a bigger buffer is not one.
inline constexpr size_t kMaxResourceBytes = size_t{64} * 1024 * 1024;

enum class Status {
  Ok,
  NothingToPlace,   // no owner given
  Truncated,        // too short for its descriptor or for the entries it declares
  BadDescriptor,    // the descriptor disagrees with itself or with the buffer size
  TooLarge,
  MissingOwner,     // an owner asked for has no entry
  DuplicateOwner,   // an owner has two entries, or is asked for twice
  AmbiguousSlots,   // two of the entries stand at the same x (or one has none): no left to right order
  VerifyFailed,     // the result did not decode as intended
};

inline const char* StatusName(Status s) {
  switch (s) {
    case Status::Ok: return "ok";
    case Status::NothingToPlace: return "nothing_to_place";
    case Status::Truncated: return "truncated";
    case Status::BadDescriptor: return "bad_descriptor";
    case Status::TooLarge: return "too_large";
    case Status::MissingOwner: return "missing_owner";
    case Status::DuplicateOwner: return "duplicate_owner";
    case Status::AmbiguousSlots: return "ambiguous_slots";
    case Status::VerifyFailed: return "verify_failed";
  }
  return "unknown";
}

/// Where one owner stands: `slot` counts from the left (1 is the leftmost, the greatest x), `x` is the entry's x.
struct Placement {
  uint64_t owner = 0;
  unsigned slot = 0;
  float x = 0.0f;
};

namespace detail {

inline uint64_t Load64(const uint8_t* p) {
  uint64_t v;
  std::memcpy(&v, p, sizeof(v));
  return v;
}

inline float LoadFloat(const uint8_t* p) {
  float v;
  std::memcpy(&v, p, sizeof(v));
  return v;
}

/// Checks the descriptor and returns the entry count.
inline Status Header(const uint8_t* data, size_t size, size_t* count) {
  if (data == nullptr || size < kDescriptorBytes) return Status::Truncated;
  if (size > kMaxResourceBytes) return Status::TooLarge;
  const uint64_t n = Load64(data + 0x28);
  if (n != Load64(data + 0x30)) return Status::BadDescriptor;
  if (n > (size - kDescriptorBytes) / kEntryBytes) return Status::Truncated;
  if (Load64(data + 0x08) != n * kEntryBytes) return Status::BadDescriptor;
  if (size != kDescriptorBytes + n * kEntryBytes) return Status::BadDescriptor;
  *count = static_cast<size_t>(n);
  return Status::Ok;
}

inline size_t EntryAt(size_t index) { return kDescriptorBytes + index * kEntryBytes; }

/// The entry index of each owner (once each), and the left-to-right rank of those entries.
struct Located {
  std::vector<size_t> entry;     // per owner given, its entry index
  std::vector<size_t> bySlot;    // the owners' indexes into `entry`, leftmost first
};

inline Status Locate(const uint8_t* data, size_t count, const uint64_t* owners, size_t n, Located* out) {
  if (n == 0) return Status::NothingToPlace;
  for (size_t i = 0; i < n; ++i) {
    for (size_t j = i + 1; j < n; ++j) {
      if (owners[i] == owners[j]) return Status::DuplicateOwner;
    }
  }
  out->entry.assign(n, count);
  for (size_t e = 0; e < count; ++e) {
    const uint64_t actor = Load64(data + EntryAt(e) + kOwnerOffset);
    for (size_t i = 0; i < n; ++i) {
      if (actor != owners[i]) continue;
      if (out->entry[i] != count) return Status::DuplicateOwner;
      out->entry[i] = e;
    }
  }
  for (size_t i = 0; i < n; ++i) {
    if (out->entry[i] == count) return Status::MissingOwner;
  }
  out->bySlot.resize(n);
  for (size_t i = 0; i < n; ++i) out->bySlot[i] = i;
  std::vector<float> x(n);
  for (size_t i = 0; i < n; ++i) {
    x[i] = LoadFloat(data + EntryAt(out->entry[i]) + kPositionXOffset);
    if (std::isnan(x[i])) return Status::AmbiguousSlots;
  }
  std::sort(out->bySlot.begin(), out->bySlot.end(), [&x](size_t a, size_t b) { return x[a] > x[b]; });
  for (size_t k = 1; k < n; ++k) {
    if (x[out->bySlot[k - 1]] == x[out->bySlot[k]]) return Status::AmbiguousSlots;
  }
  return Status::Ok;
}

inline std::vector<Placement> Placements(const uint8_t* data, const uint64_t* owners, size_t n, const Located& at) {
  std::vector<Placement> p(n);
  for (size_t i = 0; i < n; ++i) {
    p[i].owner = owners[i];
    p[i].x = LoadFloat(data + EntryAt(at.entry[i]) + kPositionXOffset);
  }
  for (size_t k = 0; k < n; ++k) p[at.bySlot[k]].slot = static_cast<unsigned>(k + 1);
  return p;
}

}  // namespace detail

/// Where each of `owners` stands in the transform table `data`, in the order given (so `Placement::slot` says how many
/// of the owners stand to its left, plus one). Reads only.
inline Status Read(const uint8_t* data, size_t size, const uint64_t* owners, size_t n, std::vector<Placement>* out) {
  size_t count = 0;
  Status s = detail::Header(data, size, &count);
  if (s != Status::Ok) return s;
  detail::Located at;
  s = detail::Locate(data, count, owners, n, &at);
  if (s != Status::Ok) return s;
  *out = detail::Placements(data, owners, n, at);
  return Status::Ok;
}

/// Gives each of `order` the pose of the slot it is named for: the k-th of `order` takes the pose of the k-th slot from
/// the left among the entries those owners hold now. The slots are the ones the table already has, so the arc keeps its
/// shape; only which owner stands at which slot changes. On Ok, `*out` is the new buffer, of the input's size, and
/// `*before` and `*after` are the placements (in `order`'s order) read from the input and read back from the result.
inline Status Place(const uint8_t* data, size_t size, const uint64_t* order, size_t n, std::vector<uint8_t>* out,
                    std::vector<Placement>* before, std::vector<Placement>* after) {
  size_t count = 0;
  Status s = detail::Header(data, size, &count);
  if (s != Status::Ok) return s;
  detail::Located at;
  s = detail::Locate(data, count, order, n, &at);
  if (s != Status::Ok) return s;
  *before = detail::Placements(data, order, n, at);

  std::vector<uint8_t> result(data, data + size);
  for (size_t k = 0; k < n; ++k) {
    const size_t from = detail::EntryAt(at.entry[at.bySlot[k]]) + kPoseOffset;  // the pose of the k-th slot from the left
    const size_t to = detail::EntryAt(at.entry[k]) + kPoseOffset;               // the owner that is to stand there
    std::memcpy(result.data() + to, data + from, kPoseBytes);
  }

  // Verify: the result reads back with the k-th owner in slot k + 1, the four poses are the same four poses, and not one
  // byte outside those poses changed.
  std::vector<Placement> readBack;
  if (Read(result.data(), result.size(), order, n, &readBack) != Status::Ok) return Status::VerifyFailed;
  for (size_t k = 0; k < n; ++k) {
    if (readBack[k].slot != k + 1) return Status::VerifyFailed;
  }
  std::vector<uint8_t> mask(size, 0);
  for (size_t i = 0; i < n; ++i) {
    std::memset(mask.data() + detail::EntryAt(at.entry[i]) + kPoseOffset, 1, kPoseBytes);
  }
  for (size_t b = 0; b < size; ++b) {
    if (mask[b] == 0 && result[b] != data[b]) return Status::VerifyFailed;
  }
  for (size_t k = 0; k < n; ++k) {
    const size_t slotPose = detail::EntryAt(at.entry[at.bySlot[k]]) + kPoseOffset;
    const size_t ownerPose = detail::EntryAt(at.entry[k]) + kPoseOffset;
    if (std::memcmp(result.data() + ownerPose, data + slotPose, kPoseBytes) != 0) return Status::VerifyFailed;
  }
  *after = std::move(readBack);
  *out = std::move(result);
  return Status::Ok;
}

}  // namespace nevr_menu_slots
