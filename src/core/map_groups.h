#pragma once

// The lobby groups a client keeps and asks for (docs/adr/0048 decisions 15 and 16; ADR 0045 decision 8). Pure and header-only: no
// Env, no file, no Log(), so the Quest host groups that compile the session router and the PC build share the same rules with no
// extra source to list (src/core/maps.cmake, src/quest/host_tests/groups/*.cmake).
//
//   - A lobby session's group, as the game's GUID text, becomes the opaque group id the manifest path and its signed `guild`
//     field use (GroupIdOfSession). The game's text (FormatGuid) and the service's group id text are the same characters up to
//     letter case, so the id is that text in lower case; it is never parsed as a UUID.
//   - A session success asks the channel for its group (RequestSessionGroup). The request is the channel's to answer; until a
//     channel sets one, nothing is requested. The fetch records the group in the cache index, latest first (RememberLatestGroup,
//     called from the fetch), and the index keeps as many groups as the device's storage share holds (KeptGroupCount).

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/map_format.h"

namespace nevr_maps {

// The group id of a session's group text. kNil: the nil GUID (a session with no group, which is no group to fetch). kNotPathSafe:
// the text after lower-casing is not an id IsPathSafeGroupId accepts (empty, too long, or a separator). `id` is set only on kOk.
enum class SessionGroup { kOk, kNil, kNotPathSafe };

inline SessionGroup GroupIdOfSession(std::string_view guid_text, std::string* id) {
  // The nil GUID is the game's text of sixteen zero bytes: 36 characters, '0' everywhere but the four hyphens.
  constexpr std::size_t kGuidTextBytes = 36;
  constexpr std::size_t kGuidHyphens[] = {8, 13, 18, 23};
  bool nil = guid_text.size() == kGuidTextBytes;
  std::string lower;
  lower.reserve(guid_text.size());
  for (std::size_t i = 0; i < guid_text.size(); ++i) {
    const char c = guid_text[i];
    const bool hyphen = std::find(std::begin(kGuidHyphens), std::end(kGuidHyphens), i) != std::end(kGuidHyphens);
    if (c != (hyphen ? '-' : '0')) nil = false;
    lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  }
  if (nil) return SessionGroup::kNil;
  if (!IsPathSafeGroupId(lower)) return SessionGroup::kNotPathSafe;
  if (id != nullptr) *id = lower;
  return SessionGroup::kOk;
}

// The bytes a group takes in the cache index beside its stored manifest: an upper bound of its index entry's JSON text, the
// fixed syntax of `{"id":"","bytes":<n>,"maps":[]}` (kGroupRecordSyntaxBytes), its id, each map key with its quotes and comma,
// and the decimal digits of its bytes (at most 20 for a 64-bit count). It is a format bound, not a limit: the limit is the
// storage budget KeptGroupCount is given.
inline constexpr std::uint64_t kGroupRecordSyntaxBytes = 28;
inline constexpr std::uint64_t kGroupRecordQuotedKeyBytes = 3;  // `,"<key>"` beyond the key's own text
inline constexpr std::uint64_t kDecimalDigitsOfU64 = 20;

inline std::uint64_t GroupRecordBytes(const std::string& id, const std::vector<std::string>& maps) {
  std::uint64_t bytes = kGroupRecordSyntaxBytes + kDecimalDigitsOfU64 + id.size();
  for (const std::string& map : maps) bytes += kGroupRecordQuotedKeyBytes + map.size();
  return bytes;
}

// Which groups the cache keeps, latest first: cost[i] is group i's bytes (its stored manifest plus its record, GroupRecordBytes).
// The latest group is always kept. Each older group is kept while its cost fits what remains of `budget`; once one does not fit,
// it and every older group go (the oldest are dropped first). Returns how many of the list's first groups are kept.
inline std::size_t KeptGroupCount(const std::vector<std::uint64_t>& cost, std::uint64_t budget) {
  std::uint64_t total = 0;
  std::size_t kept = 0;
  for (std::size_t i = 0; i < cost.size(); ++i) {
    const std::uint64_t remaining = budget - std::min(total, budget);
    if (i != 0 && cost[i] > remaining) break;
    total += cost[i];
    kept = i + 1;
  }
  return kept;
}

// The requested group becomes the latest the list keeps: an entry already there moves to the front with its fields, and a new one
// is inserted first with only its id. Group needs a string member `id` and to be default constructible.
template <typename Group>
void RememberLatestGroup(std::vector<Group>& groups, const std::string& id) {
  Group group;
  group.id = id;
  for (auto it = groups.begin(); it != groups.end(); ++it) {
    if (it->id != id) continue;
    group = std::move(*it);
    groups.erase(it);
    break;
  }
  groups.insert(groups.begin(), std::move(group));
}

// The channel's request for a group (the worker answers it). Set once by the channel; the default is none, and then nothing is
// requested. Any thread may call RequestSessionGroup.
using SessionGroupRequestFn = void (*)(const std::string& group);

inline std::atomic<SessionGroupRequestFn>& SessionGroupRequestSlot() {
  static std::atomic<SessionGroupRequestFn> slot{nullptr};
  return slot;
}

inline void SetSessionGroupRequest(SessionGroupRequestFn fn) {
  SessionGroupRequestSlot().store(fn, std::memory_order_release);
}

enum class SessionGroupRequest { kRequested, kNoRequestFn, kNil, kNotPathSafe };

// A lobby session success named `guid_text`. `id` is set when the group is a valid id (kRequested, kNoRequestFn).
inline SessionGroupRequest RequestSessionGroup(std::string_view guid_text, std::string* id) {
  std::string group;
  switch (GroupIdOfSession(guid_text, &group)) {
    case SessionGroup::kNil:
      return SessionGroupRequest::kNil;
    case SessionGroup::kNotPathSafe:
      return SessionGroupRequest::kNotPathSafe;
    case SessionGroup::kOk:
      break;
  }
  if (id != nullptr) *id = group;
  const SessionGroupRequestFn fn = SessionGroupRequestSlot().load(std::memory_order_acquire);
  if (fn == nullptr) return SessionGroupRequest::kNoRequestFn;
  fn(group);
  return SessionGroupRequest::kRequested;
}

// The one record a session success writes about its group, the same on both platforms: `[NEVR.MAPS] session group <outcome>
// group=<id or none>`, the outcome being requested, no_channel, nil or not_path_safe. Remote text is not written (only a valid id).
inline std::string SessionGroupRecord(SessionGroupRequest request, const std::string& group) {
  const char* outcome = "requested";
  switch (request) {
    case SessionGroupRequest::kRequested:
      break;
    case SessionGroupRequest::kNoRequestFn:
      outcome = "no_channel";
      break;
    case SessionGroupRequest::kNil:
      outcome = "nil";
      break;
    case SessionGroupRequest::kNotPathSafe:
      outcome = "not_path_safe";
      break;
  }
  const bool valid = request == SessionGroupRequest::kRequested || request == SessionGroupRequest::kNoRequestFn;
  return std::string("[NEVR.MAPS] session group ") + outcome + " group=" + (valid && !group.empty() ? group : std::string("none"));
}

}  // namespace nevr_maps
