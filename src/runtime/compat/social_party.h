#pragma once

// Party state and wire messages for the social facade.
//
// The game's own party code (pnsovr's CNSOVRSocial) runs a party as an Oculus room and is not
// available here, and nothing else in the game handles the SNSParty* messages (the game never
// calls pnsrad's Party export). So the facade holds the party itself: game actions become the
// SNSParty* requests Nakama understands, the server's replies and notifies update this state, and
// every change becomes an Event the facade turns into one of the game's Party*CB callbacks.
//
// Wire facts are from Nakama's evr/sns_party.go and evr_pipeline_party.go (and libpnsrad's
// CNSRADParty for the symbols); the account id on the wire is the user's Discord id.
//
// PURE: std only, header-only, so the bridge, the facade and the tests share one definition.

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace SocialParty {

// ---------------------------------------------------------------------------------------------
// Symbols (the 64-bit message hashes, evr/core_packet.go)
// ---------------------------------------------------------------------------------------------
constexpr std::uint64_t kCreateRequest = 0x0b7bd21332523994ULL;
constexpr std::uint64_t kJoinRequest = 0xb57b22cc5352e00cULL;
constexpr std::uint64_t kLeaveRequest = 0xb77b0be7a94a9fb6ULL;
constexpr std::uint64_t kInviteRequest = 0xcf13f934540b5f5eULL;  // SNSPartySendInviteRequest
constexpr std::uint64_t kLockRequest = 0xc2478aa479f3e16aULL;
constexpr std::uint64_t kUnlockRequest = 0x5a4e99802fa3d704ULL;
constexpr std::uint64_t kInviteListRefreshRequest = 0xd8cbc44959e25da8ULL;
constexpr std::uint64_t kFriendListRefreshRequest = 0xdcfa94680e8d19fcULL;  // SNSFriendListRefreshRequest
constexpr std::uint64_t kKickRequest = 0xfaf57beb59917d64ULL;
constexpr std::uint64_t kPassRequest = 0x518543cd886a6946ULL;
constexpr std::uint64_t kInviteResponse = 0xe3654a09203555a3ULL;  // SNSPartyRespondToInviteRequest

// Server replies and notifies (evrcat -reverse of the SNS names; the game's own symbol table has no
// names for these, so dispatch is by hash).
struct SymbolName {
  std::uint64_t symbol;
  const char* name;  // without the SNS prefix
};

inline const SymbolName* ReplyTable(std::size_t* count) {
  static const SymbolName kTable[] = {
      {0x0b7ac20124523993ULL, "PartyCreateSuccess"},   {0x0b6fd60b2b423885ULL, "PartyCreateFailure"},
      {0xb57a32de4552e00bULL, "PartyJoinSuccess"},     {0xb56f26d44a42e11dULL, "PartyJoinFailure"},
      {0xcc38103e64879e53ULL, "PartyJoinNotify"},      {0xb77a1bf5bf4a9fb1ULL, "PartyLeaveSuccess"},
      {0xb76f0fffb05a9ea7ULL, "PartyLeaveFailure"},    {0x05315abefc8f804bULL, "PartyLeaveNotify"},
      {0xfaf46bf94f917d63ULL, "PartyKickSuccess"},     {0xfae17ff340817c75ULL, "PartyKickFailure"},
      {0x28cb04891f93dc81ULL, "PartyKickNotify"},      {0x518453df9e6a6941ULL, "PartyPassSuccess"},
      {0x519147d5917a6857ULL, "PartyPassFailure"},     {0x9d946c88d5a8aca5ULL, "PartyPassNotify"},
      {0xc2469ab66ff3e16dULL, "PartyLockSuccess"},     {0xc2538ebc60e3e07bULL, "PartyLockFailure"},
      {0x93a6b1a6cd4ef8ddULL, "PartyLockNotify"},      {0x5a4f899239a3d703ULL, "PartyUnlockSuccess"},
      {0x5a5a9d9836b3d615ULL, "PartyUnlockFailure"},   {0xd8cfd3795010481fULL, "PartyUnlockNotify"},
      {0x218f721f09026dabULL, "PartyInviteNotify"},    {0x685a5fb8447b1155ULL, "PartyInviteListResponse"},
      {0xdee671b237a5278dULL, "PartyUpdateSuccess"},   {0xdef365b838b5269bULL, "PartyUpdateFailure"},
      {0x23c834cb3bc6ecf5ULL, "PartyUpdateNotify"},    {0x4edffb9fc8cc8731ULL, "PartyUpdateMemberSuccess"},
      {0x4ecaef95c7dc8627ULL, "PartyUpdateMemberFailure"}, {0x451eb6ca40dde289ULL, "PartyUpdateMemberNotify"},
  };
  *count = sizeof(kTable) / sizeof(kTable[0]);
  return kTable;
}

inline const char* ReplyName(std::uint64_t symbol) {
  std::size_t count = 0;
  const SymbolName* table = ReplyTable(&count);
  for (std::size_t i = 0; i < count; ++i)
    if (table[i].symbol == symbol) return table[i].name;
  return nullptr;
}

inline std::uint64_t ReplySymbol(const char* name) {
  std::size_t count = 0;
  const SymbolName* table = ReplyTable(&count);
  for (std::size_t i = 0; i < count; ++i)
    if (name != nullptr && std::strcmp(table[i].name, name) == 0) return table[i].symbol;
  return 0;
}

/// The name of one of our own requests, for logs (the game's table mislabels the create hash).
inline const char* RequestName(std::uint64_t symbol) {
  switch (symbol) {
    case kCreateRequest: return "PartyCreateRequest";
    case kJoinRequest: return "PartyJoinRequest";
    case kLeaveRequest: return "PartyLeaveRequest";
    case kInviteRequest: return "PartyInviteRequest";
    case kLockRequest: return "PartyLockRequest";
    case kUnlockRequest: return "PartyUnlockRequest";
    case kInviteListRefreshRequest: return "PartyInviteListRefreshRequest";
    case kKickRequest: return "PartyKickRequest";
    case kPassRequest: return "PartyPassRequest";
    case kInviteResponse: return "PartyInviteResponse";
    case kFriendListRefreshRequest: return "FriendListRefreshRequest";
    default: return nullptr;
  }
}

// ---------------------------------------------------------------------------------------------
// Wire building
// ---------------------------------------------------------------------------------------------
struct Message {
  std::uint64_t symbol = 0;
  std::string payload;
};

using Uuid = std::array<std::uint8_t, 16>;

inline void AppendLe(std::string& out, std::uint64_t value, int bytes) {
  for (int i = 0; i < bytes; ++i) out.push_back(static_cast<char>((value >> (8 * i)) & 0xFF));
}

/// The frame the game and the bridge exchange: marker(8) + symbol(8) + payload length(8) + payload.
inline std::string Frame(const Message& message) {
  static const std::uint8_t kMarker[8] = {0xf6, 0x40, 0xbb, 0x78, 0xa2, 0xe7, 0x8c, 0xbb};
  std::string out(reinterpret_cast<const char*>(kMarker), sizeof(kMarker));
  AppendLe(out, message.symbol, 8);
  AppendLe(out, message.payload.size(), 8);
  out += message.payload;
  return out;
}

/// SHA-1, for the UUIDv5 Nakama derives from an EvrId token.
inline std::array<std::uint8_t, 20> Sha1(const std::string& data) {
  std::uint32_t h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
  std::string msg = data;
  const std::uint64_t bits = static_cast<std::uint64_t>(data.size()) * 8;
  msg.push_back(static_cast<char>(0x80));
  while (msg.size() % 64 != 56) msg.push_back('\0');
  for (int i = 7; i >= 0; --i) msg.push_back(static_cast<char>((bits >> (8 * i)) & 0xFF));
  const auto rol = [](std::uint32_t v, int n) { return (v << n) | (v >> (32 - n)); };
  for (std::size_t chunk = 0; chunk < msg.size(); chunk += 64) {
    std::uint32_t w[80];
    for (int i = 0; i < 16; ++i) {
      const auto* p = reinterpret_cast<const std::uint8_t*>(msg.data()) + chunk + i * 4;
      w[i] = (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
             (static_cast<std::uint32_t>(p[2]) << 8) | static_cast<std::uint32_t>(p[3]);
    }
    for (int i = 16; i < 80; ++i) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int i = 0; i < 80; ++i) {
      std::uint32_t f, k;
      if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999u; }
      else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1u; }
      else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDCu; }
      else { f = b ^ c ^ d; k = 0xCA62C1D6u; }
      const std::uint32_t temp = rol(a, 5) + f + e + k + w[i];
      e = d; d = c; c = rol(b, 30); b = a; a = temp;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
  }
  std::array<std::uint8_t, 20> out{};
  for (int i = 0; i < 5; ++i)
    for (int j = 0; j < 4; ++j) out[i * 4 + j] = static_cast<std::uint8_t>((h[i] >> (24 - 8 * j)) & 0xFF);
  return out;
}

/// uuid.NewV5(uuid.Nil, name), which is what Nakama's EvrId.UUID() returns for the token.
inline Uuid UuidV5Nil(const std::string& name) {
  const auto digest = Sha1(std::string(16, '\0') + name);
  Uuid out{};
  std::memcpy(out.data(), digest.data(), 16);
  out[6] = static_cast<std::uint8_t>((out[6] & 0x0F) | 0x50);
  out[8] = static_cast<std::uint8_t>((out[8] & 0x3F) | 0x80);
  return out;
}

/// The UUID a party member goes by in Kick, Pass and invite responses. Nakama keys these by the
/// EvrId of the platform the game forces (OVR-ORG) with the Discord id as the account id.
inline Uuid MemberUuid(std::uint64_t accountId) { return UuidV5Nil("OVR-ORG-" + std::to_string(accountId)); }

/// 0x28-byte payload: RoutingID(8) LocalUserUUID(16) SessionGUID(8) last(8).
inline Message Standard(std::uint64_t symbol, const Uuid& self, std::uint64_t last) {
  Message m;
  m.symbol = symbol;
  AppendLe(m.payload, 0, 8);
  m.payload.append(reinterpret_cast<const char*>(self.data()), self.size());
  AppendLe(m.payload, 0, 8);
  AppendLe(m.payload, last, 8);
  return m;
}

/// 0x20-byte payload (friend list subscribe/refresh): RoutingID(8) LocalUserUUID(16) SessionGUID(8).
inline Message Short(std::uint64_t symbol, const Uuid& self) {
  Message m;
  m.symbol = symbol;
  AppendLe(m.payload, 0, 8);
  m.payload.append(reinterpret_cast<const char*>(self.data()), self.size());
  AppendLe(m.payload, 0, 8);
  return m;
}

/// 0x30-byte payload: LocalUserUUID(16) TargetUserUUID(16) SessionGUID(8) Param(4) Reserved(4).
inline Message Targeted(std::uint64_t symbol, const Uuid& self, const Uuid& target, std::uint32_t param) {
  Message m;
  m.symbol = symbol;
  m.payload.append(reinterpret_cast<const char*>(self.data()), self.size());
  m.payload.append(reinterpret_cast<const char*>(target.data()), target.size());
  AppendLe(m.payload, 0, 8);
  AppendLe(m.payload, param, 4);
  AppendLe(m.payload, 0, 4);
  return m;
}

// ---------------------------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------------------------
enum class EventKind {
  kCreated,
  kJoined,
  kJoinFailed,       // code
  kUpdated,
  kHostChanged,
  kLeft,
  kKicked,
  kMemberJoined,     // index
  kMemberUpdated,    // index
  kMemberLeft,       // id, name
  kInviteReceived,
  kInviteFailed,     // id, name, code
};

struct Event {
  EventKind kind = EventKind::kUpdated;
  std::uint32_t index = 0;
  std::uint64_t id = 0;
  std::string name;
  std::uint32_t code = 0;
};

inline Event MakeEvent(EventKind kind, std::uint32_t index = 0, std::uint64_t id = 0,
                       std::string name = std::string(), std::uint32_t code = 0) {
  Event event;
  event.kind = kind;
  event.index = index;
  event.id = id;
  event.name = std::move(name);
  event.code = code;
  return event;
}

struct Member {
  std::uint64_t id = 0;
  std::string name;
};

struct Invite {
  std::uint64_t partyId = 0;
  std::uint64_t senderId = 0;
  std::string senderName;
  std::uint64_t sentTime = 0;
};

/// What the facade mirrors into the game-visible object after each change.
struct View {
  std::uint64_t partyId = 0;
  std::uint64_t ownerId = 0;
  std::uint64_t selfId = 0;
  std::string selfName;
  bool creating = false;
  bool joining = false;
  bool locked = false;
  std::vector<Member> members;  // [0] is the local user once the party exists
  std::vector<Invite> invites;  // arrival order; the game indexes them newest first
};

class State {
 public:
  void SetSelf(std::uint64_t accountId, const std::string& name = std::string()) {
    std::lock_guard<std::mutex> guard(mutex_);
    self_ = accountId;
    selfName_ = name;
  }

  /// A friend-list or UI action asked to invite `target`: create the party first if there is none.
  std::vector<Message> SendInvite(std::uint64_t target) {
    std::lock_guard<std::mutex> guard(mutex_);
    std::vector<Message> out;
    if (partyId_ != 0) {
      out.push_back(Standard(kInviteRequest, SelfUuid(), target));
    } else {
      pendingInvites_.push_back(target);
      if (!creating_) {
        creating_ = true;
        out.push_back(Standard(kCreateRequest, SelfUuid(), 0));
      }
    }
    return out;
  }

  /// The game asked for a party to exist (Update's "create" flag): create one unless there is one or a
  /// create is already in flight.
  std::vector<Message> CreateParty() {
    std::lock_guard<std::mutex> guard(mutex_);
    std::vector<Message> out;
    if (partyId_ != 0 || creating_ || joining_) return out;
    creating_ = true;
    out.push_back(Standard(kCreateRequest, SelfUuid(), 0));
    return out;
  }

  /// Accept the invite the game lists at `index` (newest first).
  std::vector<Message> Accept(std::uint32_t index) {
    std::lock_guard<std::mutex> guard(mutex_);
    std::vector<Message> out;
    Invite invite;
    if (!TakeInvite(index, &invite)) return out;
    joining_ = true;
    out.push_back(Targeted(kInviteResponse, SelfUuid(), MemberUuid(invite.senderId), 1));
    return out;
  }

  std::vector<Message> Dismiss(std::uint32_t index) {
    std::lock_guard<std::mutex> guard(mutex_);
    std::vector<Message> out;
    Invite invite;
    if (!TakeInvite(index, &invite)) return out;
    out.push_back(Targeted(kInviteResponse, SelfUuid(), MemberUuid(invite.senderId), 0));
    return out;
  }

  std::vector<Message> Leave() {
    std::lock_guard<std::mutex> guard(mutex_);
    std::vector<Message> out;
    if (partyId_ == 0 && !creating_ && !joining_) return out;
    if (partyId_ != 0) out.push_back(Standard(kLeaveRequest, SelfUuid(), 0));
    ClearParty();
    events_.push_back(MakeEvent(EventKind::kLeft));
    return out;
  }

  std::vector<Message> Kick(std::uint32_t index) {
    std::lock_guard<std::mutex> guard(mutex_);
    std::vector<Message> out;
    if (partyId_ == 0 || ownerId_ != self_ || index == 0 || index >= members_.size()) return out;
    const Member member = members_[index];
    out.push_back(Targeted(kKickRequest, SelfUuid(), MemberUuid(member.id), 0));
    members_.erase(members_.begin() + index);
    events_.push_back(MakeEvent(EventKind::kMemberLeft, 0, member.id, member.name));
    return out;
  }

  std::vector<Message> Pass(std::uint32_t index) {
    std::lock_guard<std::mutex> guard(mutex_);
    std::vector<Message> out;
    if (partyId_ == 0 || ownerId_ != self_ || index >= members_.size() || members_[index].id == self_) return out;
    out.push_back(Targeted(kPassRequest, SelfUuid(), MemberUuid(members_[index].id), 0));
    ownerId_ = members_[index].id;
    events_.push_back(MakeEvent(EventKind::kHostChanged));
    return out;
  }

  std::vector<Message> SetLocked(bool locked) {
    std::lock_guard<std::mutex> guard(mutex_);
    std::vector<Message> out;
    if (partyId_ == 0) return out;
    out.push_back(Standard(locked ? kLockRequest : kUnlockRequest, SelfUuid(), 0));
    return out;
  }

  /// The friends tab was opened: ask the server for a fresh friend list (it answers with a
  /// FriendListResponse and one FriendStatusNotify per friend, which refill the roster).
  std::vector<Message> RefreshFriends() {
    std::lock_guard<std::mutex> guard(mutex_);
    std::vector<Message> out;
    out.push_back(Short(kFriendListRefreshRequest, SelfUuid()));
    return out;
  }

  std::vector<Message> RefreshInvites() {
    std::lock_guard<std::mutex> guard(mutex_);
    std::vector<Message> out;
    out.push_back(Standard(kInviteListRefreshRequest, SelfUuid(), 0));
    return out;
  }

  /// Feeds one server->game message by its symbol hash. Returns true for a party message; requests to send in reply (the
  /// invites that waited for the party to exist) are appended to `outgoing`.
  bool Feed(std::uint64_t symbol, const std::uint8_t* payload, std::size_t len, std::uint64_t now,
            std::vector<Message>* outgoing) {
    const char* name = ReplyName(symbol);
    if (name == nullptr || payload == nullptr) return false;
    const std::string n(name);
    std::lock_guard<std::mutex> guard(mutex_);
    const auto u64 = [&](std::size_t off) -> std::uint64_t {
      std::uint64_t v = 0;
      if (off + 8 > len) return 0;
      for (int i = 7; i >= 0; --i) v = (v << 8) | payload[off + i];
      return v;
    };
    const auto u8 = [&](std::size_t off) -> std::uint32_t { return off < len ? payload[off] : 0; };
    if (n == "PartyCreateSuccess" && len >= 16) {
      creating_ = false;
      partyId_ = u64(0);
      ownerId_ = u64(8);
      members_.assign(1, Member{self_, std::to_string(self_)});
      events_.push_back(MakeEvent(EventKind::kCreated));
      for (const std::uint64_t target : pendingInvites_) {
        if (outgoing != nullptr) outgoing->push_back(Standard(kInviteRequest, SelfUuid(), target));
      }
      pendingInvites_.clear();
    } else if (n == "PartyCreateFailure") {
      creating_ = false;
      pendingInvites_.clear();
    } else if (n == "PartyJoinSuccess" && len >= 16) {
      joining_ = false;
      partyId_ = u64(0);
      ownerId_ = u64(8);
      members_.assign(1, Member{self_, std::to_string(self_)});
      if (ownerId_ != self_) members_.push_back(Member{ownerId_, std::to_string(ownerId_)});
      events_.push_back(MakeEvent(EventKind::kJoined));
      if (members_.size() > 1) events_.push_back(MakeEvent(EventKind::kMemberJoined, 1));
    } else if (n == "PartyJoinFailure") {
      joining_ = false;
      events_.push_back(MakeEvent(EventKind::kJoinFailed, 0, 0, std::string(), u8(8)));
    } else if (n == "PartyJoinNotify" && len >= 16 && u64(0) == partyId_) {
      const std::uint64_t id = u64(8);
      if (id != self_ && Find(id) < 0) {
        members_.push_back(Member{id, std::to_string(id)});
        events_.push_back(MakeEvent(EventKind::kMemberJoined, static_cast<std::uint32_t>(members_.size() - 1)));
      }
    } else if (n == "PartyLeaveNotify" && len >= 16 && u64(0) == partyId_) {
      RemoveMember(u64(8));
    } else if (n == "PartyKickNotify" && len >= 16 && u64(0) == partyId_) {
      const std::uint64_t id = u64(8);
      if (id == self_) {
        ClearParty();
        events_.push_back(MakeEvent(EventKind::kKicked));
      } else {
        RemoveMember(id);
      }
    } else if (n == "PartyPassNotify" && len >= 16 && u64(0) == partyId_) {
      ownerId_ = u64(8);
      events_.push_back(MakeEvent(EventKind::kHostChanged));
    } else if ((n == "PartyLockNotify" || n == "PartyUnlockNotify" || n == "PartyLockSuccess" ||
                n == "PartyUnlockSuccess") && len >= 8 && u64(0) == partyId_) {
      locked_ = n.find("Unlock") == std::string::npos;
      events_.push_back(MakeEvent(EventKind::kUpdated));
    } else if ((n == "PartyUpdateNotify") && len >= 8 && u64(0) == partyId_) {
      events_.push_back(MakeEvent(EventKind::kUpdated));
    } else if (n == "PartyUpdateMemberNotify" && len >= 16 && u64(0) == partyId_) {
      const int at = Find(u64(8));
      if (at >= 0) events_.push_back(MakeEvent(EventKind::kMemberUpdated, static_cast<std::uint32_t>(at)));
    } else if (n == "PartyInviteNotify" && len >= 16) {
      Invite invite;
      invite.partyId = u64(0);
      invite.senderId = u64(8);
      invite.senderName = std::to_string(invite.senderId);
      invite.sentTime = now;
      invites_.push_back(invite);
      events_.push_back(MakeEvent(EventKind::kInviteReceived));
    } else {
      return true;  // a party message with nothing for the game to see (the success replies)
    }
    return true;
  }

  std::vector<Event> DrainEvents() {
    std::lock_guard<std::mutex> guard(mutex_);
    std::vector<Event> out;
    out.swap(events_);
    return out;
  }

  View Snapshot() const {
    std::lock_guard<std::mutex> guard(mutex_);
    View view;
    view.partyId = partyId_;
    view.ownerId = ownerId_;
    view.selfId = self_;
    view.selfName = selfName_;
    view.creating = creating_;
    view.joining = joining_;
    view.locked = locked_;
    view.members = members_;
    view.invites = invites_;
    return view;
  }

 private:
  Uuid SelfUuid() const { return self_ == 0 ? Uuid{} : MemberUuid(self_); }

  int Find(std::uint64_t id) const {
    for (std::size_t i = 0; i < members_.size(); ++i)
      if (members_[i].id == id) return static_cast<int>(i);
    return -1;
  }

  void RemoveMember(std::uint64_t id) {
    const int at = Find(id);
    if (at < 0 || members_[at].id == self_) return;
    const Member member = members_[at];
    members_.erase(members_.begin() + at);
    events_.push_back(MakeEvent(EventKind::kMemberLeft, 0, member.id, member.name));
    if (member.id == ownerId_) {
      // Nakama promotes the oldest remaining member without telling clients; the member list is
      // in join order, so take the first one that is not the local user, else the local user.
      ownerId_ = self_;
      for (const Member& m : members_) {
        if (m.id != self_) { ownerId_ = m.id; break; }
      }
      events_.push_back(MakeEvent(EventKind::kHostChanged));
    }
  }

  bool TakeInvite(std::uint32_t index, Invite* out) {
    if (index >= invites_.size()) return false;
    const std::size_t at = invites_.size() - 1 - index;  // the game lists newest first
    *out = invites_[at];
    invites_.erase(invites_.begin() + static_cast<std::ptrdiff_t>(at));
    return true;
  }

  void ClearParty() {
    partyId_ = 0;
    ownerId_ = 0;
    creating_ = false;
    joining_ = false;
    locked_ = false;
    members_.clear();
    pendingInvites_.clear();
  }

  mutable std::mutex mutex_;
  std::uint64_t self_ = 0;
  std::string selfName_;
  std::uint64_t partyId_ = 0;
  std::uint64_t ownerId_ = 0;
  bool creating_ = false;
  bool joining_ = false;
  bool locked_ = false;
  std::vector<Member> members_;
  std::vector<Invite> invites_;
  std::vector<std::uint64_t> pendingInvites_;
  std::vector<Event> events_;
};

/// Where framed requests go. The ws bridge registers its upstream send at startup; until then (and
/// in tests) sending reports failure. A plain function pointer keeps this header free of the bridge.
using Sender = bool (*)(const std::string& frame);

inline std::atomic<Sender>& SenderSlot() {
  static std::atomic<Sender> slot{nullptr};
  return slot;
}

inline void SetSender(Sender sender) { SenderSlot().store(sender, std::memory_order_release); }

/// Frames and sends each message; true only when every one was accepted.
inline bool Send(const std::vector<Message>& messages) {
  const Sender sender = SenderSlot().load(std::memory_order_acquire);
  if (sender == nullptr) return false;
  bool all = true;
  for (const Message& message : messages) all = sender(Frame(message)) && all;
  return all;
}

/// The process-wide party state the bridge feeds and the facade reads.
inline State& Global() {
  static State state;
  return state;
}

}  // namespace SocialParty
