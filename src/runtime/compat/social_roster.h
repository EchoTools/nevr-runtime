#pragma once

// The friend roster the social facade answers the game with.
//
// The game's own friends code (pnsovr's CNSOVRSocial, slots 44-55) keeps a count, an online count,
// and parallel id / name / status-string arrays, with the online friends first: FriendStatus(i) is
// "online" (2) for i < OnlineFriendCount and "offline" (0) after it. With no provider social object
// the facade has to hold that state itself. Nakama tells the client about its friends with one
// SNSFriendListResponse (aggregate counts) followed by one SNSFriendStatusNotify per confirmed
// friend (account id + status code, no name), and the ws bridge feeds both into this roster.
//
// PURE: std only, header-only, so the bridge, the facade and the tests all share one definition.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace nevr_social_roster {

// SNSFriendStatusNotify.StatusCode as Nakama's friendStatusCode sends it.
constexpr std::uint8_t kStatusOnline = 0;
constexpr std::uint8_t kStatusBusy = 1;
constexpr std::uint8_t kStatusOffline = 2;

/// A friend's presence from SNSFriendPresenceNotify (nevr social level 1; nakama builds it from the
/// friend's match and party): the party the game may join from the friends list (0 when none or not
/// joinable for us) and the text shown under the name.
struct Presence {
  std::uint64_t partyId = 0;
  bool joinable = false;
  std::string text;
};

struct Entry {
  std::uint64_t id = 0;
  std::string name;
  bool online = false;
  Presence presence;
};

/// SNSFriendPresenceNotify: Header(8) FriendID(8) PartyID(8) Joinable(1) StatusCode(1) Reserved(6)
/// TextLen(2) Text (nakama server/evr/sns_friends.go).
constexpr std::uint64_t kFriendPresenceNotify = 0xbdd8dd00c5e97a63ULL;

inline bool ParsePresenceNotify(const std::uint8_t* payload, std::size_t len, std::uint64_t* id, Presence* out) {
  if (payload == nullptr || len < 34 || id == nullptr || out == nullptr) return false;
  const auto u64 = [&](std::size_t off) {
    std::uint64_t v = 0;
    for (int i = 7; i >= 0; --i) v = (v << 8) | payload[off + i];
    return v;
  };
  const std::size_t textLen = static_cast<std::size_t>(payload[32]) | (static_cast<std::size_t>(payload[33]) << 8);
  if (34 + textLen > len) return false;
  *id = u64(8);
  out->partyId = u64(16);
  out->joinable = payload[24] != 0;
  out->text.assign(reinterpret_cast<const char*>(payload + 34), textLen);
  return true;
}

struct Snapshot {
  std::vector<Entry> entries;  // online friends first, then by id
  std::uint32_t online = 0;
};

/// SNSFriendStatusNotify payload (the bytes after the 24-byte frame header):
/// Header(8) + FriendID(8) + StatusCode(1) + 7 reserved.
inline bool ParseStatusNotify(const std::uint8_t* payload, std::size_t len, std::uint64_t* id,
                              std::uint8_t* status) {
  if (payload == nullptr || id == nullptr || status == nullptr || len < 17) return false;
  std::uint64_t value = 0;
  for (int i = 7; i >= 0; --i) value = (value << 8) | payload[8 + i];
  *id = value;
  *status = payload[16];
  return true;
}

/// SNSFriendListResponse payload: Header(8) + NOffline, NBusy, NOnline, NSent, NRecv, Reserved (u32
/// each). Returns the number of confirmed friends the server is about to describe.
inline bool ParseListResponse(const std::uint8_t* payload, std::size_t len, std::uint32_t* confirmed) {
  if (payload == nullptr || confirmed == nullptr || len < 20) return false;
  const auto word = [&](std::size_t off) {
    return static_cast<std::uint32_t>(payload[off]) | (static_cast<std::uint32_t>(payload[off + 1]) << 8) |
           (static_cast<std::uint32_t>(payload[off + 2]) << 16) |
           (static_cast<std::uint32_t>(payload[off + 3]) << 24);
  };
  *confirmed = word(8) + word(12) + word(16);  // offline + busy + online
  return true;
}

class Roster {
 public:
  /// A fresh list is starting: up to `confirmed` SNSFriendStatusNotify messages follow (the server
  /// skips a friend it cannot resolve, so fewer is possible). The previous roster stays visible
  /// until the first new entry arrives, so a refresh never flashes an empty list; an announced
  /// count of zero empties it at once.
  void BeginList(std::uint32_t confirmed) {
    std::lock_guard<std::mutex> guard(mutex_);
    pending_.clear();
    pendingActive_ = confirmed != 0;
    if (confirmed == 0) PublishLocked(std::vector<Entry>());
  }

  /// One friend's id and status. During a list it is collected; outside one it updates the live
  /// roster (a friend coming online, or a new friend).
  void Notify(std::uint64_t id, std::uint8_t status) {
    std::lock_guard<std::mutex> guard(mutex_);
    const bool online = status == kStatusOnline || status == kStatusBusy;
    if (pendingActive_) {
      Upsert(pending_, id, online);
      PublishLocked(pending_);
      return;
    }
    std::vector<Entry> next = current_ ? current_->entries : std::vector<Entry>();
    Upsert(next, id, online);
    PublishLocked(std::move(next));
  }

  /// Remembers a friend's display name (empty names are ignored) and applies it to the roster. The
  /// name is kept even when the friend is not in the roster yet or a refresh is rebuilding it: one
  /// tab open fires several overlapping refreshes, and each restarts the roster from a partial list.
  void SetName(std::uint64_t id, const std::string& name) {
    if (name.empty()) return;
    std::lock_guard<std::mutex> guard(mutex_);
    names_[id] = name;
    if (!current_) return;
    std::vector<Entry> next = current_->entries;
    for (Entry& entry : next) {
      if (entry.id == id) entry.name = name;
    }
    PublishLocked(std::move(next));
  }

  /// Remembers a friend's presence and applies it to the roster, as SetName does for names.
  void SetPresence(std::uint64_t id, const Presence& presence) {
    std::lock_guard<std::mutex> guard(mutex_);
    presences_[id] = presence;
    if (!current_) return;
    std::vector<Entry> next = current_->entries;
    for (Entry& entry : next) {
      if (entry.id == id) entry.presence = presence;
    }
    PublishLocked(std::move(next));
  }

  /// The friend's presence text (slot 52); "" when none arrived. Lives as long as NameAt's strings.
  const char* StatusTextAt(std::uint32_t index) const {
    const auto snap = Snap();
    return index < snap->entries.size() ? snap->entries[index].presence.text.c_str() : "";
  }

  /// The friend's party when the game may join it (slots 54/55); 0 otherwise.
  std::uint64_t PartyIdAt(std::uint32_t index) const {
    const auto snap = Snap();
    if (index >= snap->entries.size() || !snap->entries[index].online || !snap->entries[index].presence.joinable) return 0;
    return snap->entries[index].presence.partyId;
  }

  void Clear() {
    std::lock_guard<std::mutex> guard(mutex_);
    pending_.clear();
    pendingActive_ = false;
    PublishLocked(std::vector<Entry>());
  }

  std::uint32_t Count() const { return static_cast<std::uint32_t>(Snap()->entries.size()); }
  std::uint32_t Online() const { return Snap()->online; }
  std::uint32_t Offline() const { return Count() - Online(); }

  bool IdAt(std::uint32_t index, std::uint64_t* out) const {
    const auto snap = Snap();
    if (index >= snap->entries.size()) return false;
    if (out != nullptr) *out = snap->entries[index].id;
    return true;
  }

  /// True when `id` is in the published roster (used to log only the profile replies that concern a friend).
  bool Contains(std::uint64_t id) const {
    const auto snap = Snap();
    for (const Entry& entry : snap->entries)
      if (entry.id == id) return true;
    return false;
  }

  bool OnlineAt(std::uint32_t index) const {
    const auto snap = Snap();
    return index < snap->entries.size() && snap->entries[index].online;
  }

  /// A C string the game may keep reading after this returns. It points into a published snapshot
  /// that stays alive for the next kRetired publishes, which is far longer than one UI read.
  const char* NameAt(std::uint32_t index) const {
    const auto snap = Snap();
    return index < snap->entries.size() ? snap->entries[index].name.c_str() : "";
  }

 private:
  static constexpr std::size_t kRetired = 8;

  void Upsert(std::vector<Entry>& list, std::uint64_t id, bool online) const {
    for (Entry& entry : list) {
      if (entry.id == id) {
        entry.online = online;
        return;
      }
    }
    // Nakama sends no display name; use the remembered one, else the account id stands in.
    const auto known = names_.find(id);
    const auto presence = presences_.find(id);
    list.push_back(Entry{id, known != names_.end() ? known->second : std::to_string(id), online,
                         presence != presences_.end() ? presence->second : Presence{}});
  }

  std::shared_ptr<const Snapshot> Snap() const {
    std::lock_guard<std::mutex> guard(mutex_);
    return current_ ? current_ : Empty();
  }

  static std::shared_ptr<const Snapshot> Empty() {
    static const std::shared_ptr<const Snapshot> empty = std::make_shared<const Snapshot>();
    return empty;
  }

  void PublishLocked(std::vector<Entry> entries) {
    std::stable_sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
      if (a.online != b.online) return a.online;
      return a.id < b.id;
    });
    auto next = std::make_shared<Snapshot>();
    next->online = static_cast<std::uint32_t>(
        std::count_if(entries.begin(), entries.end(), [](const Entry& e) { return e.online; }));
    next->entries = std::move(entries);
    if (current_) {
      retired_[retiredNext_] = current_;
      retiredNext_ = (retiredNext_ + 1) % kRetired;
    }
    current_ = std::move(next);
  }

  mutable std::mutex mutex_;
  std::shared_ptr<const Snapshot> current_;
  std::array<std::shared_ptr<const Snapshot>, kRetired> retired_{};
  std::size_t retiredNext_ = 0;
  std::vector<Entry> pending_;
  std::map<std::uint64_t, std::string> names_;
  std::map<std::uint64_t, Presence> presences_;
  bool pendingActive_ = false;
};

// ---------------------------------------------------------------------------------------------
// Recently met (social slots 56-67; docs/design/2026-10-01-social-nakama-proposal.md §2)
// ---------------------------------------------------------------------------------------------

/// SNSRecentlyMetListResponse (nevr social level 1): Count(4), then per person AccountID(8) PartyID(8)
/// Joinable(1) Status(1, 0 online, 2 offline) Reserved(6) NameLen(2) Name TextLen(2) Text (nakama
/// server/evr/sns_recently_met.go). The request, SNSRecentlyMetRefreshRequest, is in social_party.h.
constexpr std::uint64_t kRecentlyMetListResponse = 0xbc3ee692bb03328fULL;

/// The response's people in its order (online first, newest meeting first); false if it is cut short.
inline bool ParseRecentlyMetResponse(const std::uint8_t* payload, std::size_t len, std::vector<Entry>* out) {
  if (payload == nullptr || out == nullptr || len < 4) return false;
  const auto le = [&](std::size_t off, int bytes) {
    std::uint64_t v = 0;
    for (int i = bytes - 1; i >= 0; --i) v = (v << 8) | payload[off + static_cast<std::size_t>(i)];
    return v;
  };
  const std::uint64_t count = le(0, 4);
  std::size_t at = 4;
  std::vector<Entry> entries;
  for (std::uint64_t i = 0; i < count; ++i) {
    if (at + 32 > len) return false;
    Entry entry;
    entry.id = le(at, 8);
    const std::uint64_t partyId = le(at + 8, 8);
    const bool joinable = payload[at + 16] != 0;
    const std::uint8_t status = payload[at + 17];
    entry.online = status == kStatusOnline || status == kStatusBusy;
    const std::size_t nameLen = static_cast<std::size_t>(le(at + 24, 2));
    at += 26;
    if (at + nameLen + 2 > len) return false;
    entry.name.assign(reinterpret_cast<const char*>(payload + at), nameLen);
    at += nameLen;
    const std::size_t textLen = static_cast<std::size_t>(le(at, 2));
    at += 2;
    if (at + textLen > len) return false;
    entry.presence.text.assign(reinterpret_cast<const char*>(payload + at), textLen);
    at += textLen;
    entry.presence.joinable = joinable && partyId != 0;
    entry.presence.partyId = entry.presence.joinable ? partyId : 0;
    if (entry.name.empty()) entry.name = std::to_string(entry.id);
    entries.push_back(std::move(entry));
  }
  *out = std::move(entries);
  return true;
}

/// The recently-met list the facade answers slots 56-67 with. pnsovr's slot 56 is "busy" from the
/// refresh (slot 57) until its answer is in (0x180091200); the game's refresh node polls it until it
/// is 0. A server that does not know the request never answers, so a refresh also ends after
/// kRefreshSeconds with the list it had.
class RecentList {
 public:
  static constexpr std::uint64_t kRefreshSeconds = 5;

  /// Starts a refresh unless one is in flight; true when the caller should send the request.
  bool BeginRefresh(std::uint64_t now) {
    std::lock_guard<std::mutex> guard(mutex_);
    if (BusyLocked(now)) return false;
    refreshing_ = true;
    started_ = now;
    return true;
  }

  /// The refresh could not be sent (or its answer could not be read): not busy any more.
  void EndRefresh() {
    std::lock_guard<std::mutex> guard(mutex_);
    refreshing_ = false;
  }

  /// Slot 56. True while a refresh waits for its answer, for at most kRefreshSeconds. `timedOut` is
  /// set once when a refresh ends that way.
  bool Refreshing(std::uint64_t now, bool* timedOut = nullptr) {
    std::lock_guard<std::mutex> guard(mutex_);
    const bool busy = BusyLocked(now);
    if (refreshing_ && !busy) {
      refreshing_ = false;
      if (timedOut != nullptr) *timedOut = true;
    }
    return busy;
  }

  /// The server's answer: the list as sent (online first), and the refresh is over.
  void SetList(std::vector<Entry> entries) {
    std::lock_guard<std::mutex> guard(mutex_);
    refreshing_ = false;
    auto next = std::make_shared<Snapshot>();
    std::stable_partition(entries.begin(), entries.end(), [](const Entry& e) { return e.online; });
    next->online = static_cast<std::uint32_t>(
        std::count_if(entries.begin(), entries.end(), [](const Entry& e) { return e.online; }));
    next->entries = std::move(entries);
    if (current_) {
      retired_[retiredNext_] = current_;
      retiredNext_ = (retiredNext_ + 1) % retired_.size();
    }
    current_ = std::move(next);
  }

  std::uint32_t Count() const { return static_cast<std::uint32_t>(Snap()->entries.size()); }
  std::uint32_t Online() const { return Snap()->online; }

  bool IdAt(std::uint32_t index, std::uint64_t* out) const {
    const auto snap = Snap();
    if (index >= snap->entries.size()) return false;
    if (out != nullptr) *out = snap->entries[index].id;
    return true;
  }
  bool OnlineAt(std::uint32_t index) const {
    const auto snap = Snap();
    return index < snap->entries.size() && snap->entries[index].online;
  }
  /// Strings the game may keep reading: they live in a snapshot kept for the next 8 lists.
  const char* NameAt(std::uint32_t index) const {
    const auto snap = Snap();
    return index < snap->entries.size() ? snap->entries[index].name.c_str() : "";
  }
  const char* TextAt(std::uint32_t index) const {
    const auto snap = Snap();
    return index < snap->entries.size() ? snap->entries[index].presence.text.c_str() : "";
  }
  /// Their party, when online and joinable for us (slots 66/67); 0 otherwise.
  std::uint64_t PartyIdAt(std::uint32_t index) const {
    const auto snap = Snap();
    if (index >= snap->entries.size() || !snap->entries[index].online) return 0;
    return snap->entries[index].presence.partyId;
  }

 private:
  bool BusyLocked(std::uint64_t now) const { return refreshing_ && now - started_ < kRefreshSeconds; }

  std::shared_ptr<const Snapshot> Snap() const {
    std::lock_guard<std::mutex> guard(mutex_);
    if (current_) return current_;
    static const std::shared_ptr<const Snapshot> empty = std::make_shared<const Snapshot>();
    return empty;
  }

  mutable std::mutex mutex_;
  bool refreshing_ = false;
  std::uint64_t started_ = 0;
  std::shared_ptr<const Snapshot> current_;
  std::array<std::shared_ptr<const Snapshot>, 8> retired_{};
  std::size_t retiredNext_ = 0;
};

inline RecentList& RecentlyMet() {
  static RecentList list;
  return list;
}

/// The process-wide roster the bridge fills and the facade reads.
inline Roster& Global() {
  static Roster roster;
  return roster;
}

/// A server message that changes who is on the friends list (an add, an accept, a removal, a
/// withdrawn or rejected request, a new incoming request). The roster is rebuilt from the server's
/// list on these, because none of them carries presence. `name` is as the game's symbol table
/// returns it; an "SNS" prefix is accepted too.
inline bool IsFriendChangeSymbol(std::uint64_t symbol) {
  switch (symbol) {
    case 0xc237c84c31d3ae05ULL:  // SNSFriendAcceptNotify
    case 0x1bbda7fa06af4627ULL:  // SNSFriendAcceptSuccess
    case 0xe06972f49cd72265ULL:  // SNSFriendRemoveNotify
    case 0xc2bf83a08ea3a955ULL:  // SNSFriendRemoveResponse
    case 0x191aa30801ec6d03ULL:  // SNSFriendWithdrawnNotify
    case 0xb9b86c0ce8e8d0c1ULL:  // SNSFriendRejectNotify
    case 0xca09b0b36bd981b7ULL:  // SNSFriendInviteNotify
    case 0x7f0c6a3ac83c6f77ULL:  // SNSFriendInviteSuccess
      return true;
    default:
      return false;
  }
}

inline bool IsFriendChange(const char* name) {
  if (name == nullptr) return false;
  std::string n(name);
  if (n.rfind("SNS", 0) == 0) n.erase(0, 3);
  return n == "FriendAcceptNotify" || n == "FriendAcceptSuccess" || n == "FriendRemoveNotify" ||
         n == "FriendRemoveResponse" || n == "FriendWithdrawnNotify" || n == "FriendRejectNotify" ||
         n == "FriendInviteNotify" || n == "FriendInviteSuccess";
}

/// Feeds one server->game message into `roster`. `name` is the symbol name exactly as the game's
/// symbol table returns it, which has no "SNS" prefix ("FriendStatusNotify", not
/// "SNSFriendStatusNotify"). Returns true when the message was a roster message.
inline bool Feed(Roster& roster, const char* name, const std::uint8_t* payload, std::size_t len) {
  if (name == nullptr) return false;
  const std::string n(name);
  std::uint64_t id = 0;
  std::uint8_t status = 0;
  std::uint32_t confirmed = 0;
  if (n == "FriendStatusNotify" && ParseStatusNotify(payload, len, &id, &status)) {
    roster.Notify(id, status);
    return true;
  }
  if (n == "FriendListResponse" && ParseListResponse(payload, len, &confirmed)) {
    roster.BeginList(confirmed);
    return true;
  }
  return false;
}

}  // namespace nevr_social_roster
