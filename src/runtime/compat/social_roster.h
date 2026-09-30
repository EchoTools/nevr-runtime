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
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace SocialRoster {

// SNSFriendStatusNotify.StatusCode as Nakama's friendStatusCode sends it.
constexpr std::uint8_t kStatusOnline = 0;
constexpr std::uint8_t kStatusBusy = 1;
constexpr std::uint8_t kStatusOffline = 2;

struct Entry {
  std::uint64_t id = 0;
  std::string name;
  bool online = false;
};

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

  /// Replaces a friend's display name (empty names are ignored).
  void SetName(std::uint64_t id, const std::string& name) {
    if (name.empty()) return;
    std::lock_guard<std::mutex> guard(mutex_);
    if (!current_) return;
    std::vector<Entry> next = current_->entries;
    for (Entry& entry : next) {
      if (entry.id == id) entry.name = name;
    }
    PublishLocked(std::move(next));
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

  static void Upsert(std::vector<Entry>& list, std::uint64_t id, bool online) {
    for (Entry& entry : list) {
      if (entry.id == id) {
        entry.online = online;
        return;
      }
    }
    // Nakama sends no display name, so the account id stands in until a name is known.
    list.push_back(Entry{id, std::to_string(id), online});
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
  bool pendingActive_ = false;
};

/// The process-wide roster the bridge fills and the facade reads.
inline Roster& Global() {
  static Roster roster;
  return roster;
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

}  // namespace SocialRoster
