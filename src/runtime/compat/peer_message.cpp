// peer_message.cpp: the PC half of nEVR's control messages on the game's broadcaster (#2365). See peer_message.h.
#include "runtime/compat/peer_message.h"

#include <cstdint>
#include <cstring>
#include <exception>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "core/pch.h"
#include "abi/echovr.h"
#include "abi/echovr_functions.h"
#include "core/logging.h"
#include "nevr/pc/nevr_common.h"
#include "runtime/combat/combat.h"
#include "runtime/compat/peer_message_rules.h"
#include "runtime/compat/peer_message_targets.h"
#include "runtime/lifecycle/cli.h"     // g_isServer
#include "runtime/lifecycle/config.h"  // g_pGame
#include "runtime/lifecycle/readable_memory.h"

namespace nevr_peer_message {
namespace {

constexpr const Targets& kT = kPcTargets;
constexpr std::uint64_t kServerPollUs = 250000;
constexpr std::uint64_t kPeerSelf = 0xFFFFFFFFFFFFFFFCULL;
constexpr std::uint64_t kPeerInvalid = 0xFFFFFFFFFFFFFFFFULL;

using AuthorityPeerFn = std::uint64_t* (*)(void* netGame, std::uint64_t* out);

// All state below is touched on the game thread only (Poll, and the listener the broadcaster calls from its dispatch),
// except the source, which the game server sets once at start.
std::mutex g_sourceLock;
std::function<MatchView()> g_source;

void* g_listenedBroadcaster = nullptr;
std::uint16_t g_listenHandle = 0;
bool g_authorityChecked = false;
AuthorityPeerFn g_authorityPeer = nullptr;
std::uint64_t g_lastServerPollUs = 0;
std::string g_session;
std::set<std::uint64_t> g_greeted;  // the server's peers sent capabilities this session
bool g_broadcasterCompared = false;
std::uint64_t g_lastAuthority = kPeerInvalid;
RefusalLimiter g_refusals;
PingBook g_pings;

const char* RoleName() { return g_isServer ? "server" : "client"; }

std::uint64_t NowMs() { return GetTickCount64(); }

template <typename T>
bool ReadField(std::uintptr_t base, std::size_t offset, T* out) {
  if (base == 0 || !nevr_crash_recovery::IsReadableMemory(reinterpret_cast<const void*>(base + offset), sizeof(T))) return false;
  std::memcpy(out, reinterpret_cast<const void*>(base + offset), sizeof(T));
  return true;
}

std::uintptr_t NetGame() {
  std::uintptr_t game = 0;
  return ReadField(reinterpret_cast<std::uintptr_t>(g_pGame), kT.contextNetGame, &game) ? game : 0;
}

std::uintptr_t Broadcaster(std::uintptr_t netGame) {
  std::uintptr_t bc = 0;
  return ReadField(netGame, kT.netGameBroadcaster, &bc) ? bc : 0;
}

// The client's authority peer: the game's own GetAuthorityPeer, called only after its bytes matched. Empty while there
// is no server (the game answers Invalid) or on the server itself (Self).
std::optional<std::uint64_t> AuthorityPeer(std::uintptr_t netGame) {
  if (!g_authorityChecked) {
    g_authorityChecked = true;
    void* fn = nevr::ResolveVA_Checked(reinterpret_cast<std::uintptr_t>(EchoVR::g_GameBaseAddress), kT.authorityPeer);
    if (fn != nullptr && nevr::ValidatePrologue(fn, kPcAuthorityPeerPrologue.data(), kPcAuthorityPeerPrologue.size())) {
      g_authorityPeer = reinterpret_cast<AuthorityPeerFn>(fn);
    } else {
      Log(EchoVR::LogLevel::Warning,
          "[NEVR.PEER] authority peer lookup not used va=0x%llx reason=prologue_mismatch (a client then takes no peer "
          "message: it cannot tell the server from another peer)",
          static_cast<unsigned long long>(kT.authorityPeer));
    }
  }
  if (g_authorityPeer == nullptr || netGame == 0) return std::nullopt;
  // GetAuthorityPeer loads the flags word through NetGame+0x2da0 without a check; before multiplayer begins that pointer
  // can be unset, so it is probed first.
  std::uintptr_t flags = 0;
  std::uint64_t flagWord = 0;
  if (!ReadField(netGame, kT.netGameFlags, &flags) || !ReadField(flags, 0, &flagWord)) return std::nullopt;
  std::uint64_t peer = kPeerInvalid;
  g_authorityPeer(reinterpret_cast<void*>(netGame), &peer);
  if (peer == kPeerInvalid || peer == kPeerSelf) return std::nullopt;
  return peer;
}

MatchView ReadMatch() {
  std::lock_guard<std::mutex> lock(g_sourceLock);
  return g_source ? g_source() : MatchView{};
}

bool Send(std::uintptr_t broadcaster, std::uint64_t peer, Kind kind, const std::string& bytes, const char* detail) {
  if (bytes.empty() || bytes.size() > kMaxEnvelopeBytes || broadcaster == 0 || EchoVR::BroadcasterSend == nullptr) {
    Log(EchoVR::LogLevel::Warning, "[NEVR.PEER] %s not sent role=%s peer=%llu bytes=%zu reason=%s", KindName(kind), RoleName(),
        static_cast<unsigned long long>(peer), bytes.size(), bytes.empty() ? "encode_failed" : broadcaster == 0 ? "no_broadcaster" : "oversize");
    return false;
  }
  // Reliable (`dest` ~0: the game's reliable queue, #2365), one item, no trailing buffer. Send reads the item only.
  const std::int32_t result = EchoVR::BroadcasterSend(
      reinterpret_cast<EchoVR::Broadcaster*>(broadcaster), static_cast<EchoVR::SymbolId>(kMessageSymbol), 0,
      const_cast<char*>(bytes.data()), bytes.size(), nullptr, 0, peer, ~0ULL, 1.0f, static_cast<EchoVR::SymbolId>(-1));
  Log(EchoVR::LogLevel::Info, "[NEVR.PEER] %s sent role=%s peer=%llu bytes=%zu %s result=%d", KindName(kind), RoleName(),
      static_cast<unsigned long long>(peer), bytes.size(), detail, result);
  return true;
}

void SendPing(std::uintptr_t broadcaster, std::uint64_t peer) {
  const std::uint64_t seq = g_pings.Next(NowMs());
  const std::string detail = "seq=" + std::to_string(seq);
  Send(broadcaster, peer, Kind::kPing, EncodePing(seq), detail.c_str());
}

void Refuse(std::uint64_t sender, std::uint64_t size, Refusal why, Kind kind) {
  std::uint64_t suppressed = 0;
  if (!g_refusals.Take(NowMs(), &suppressed)) return;
  Log(EchoVR::LogLevel::Warning,
      "[NEVR.PEER] message refused role=%s peer=%llu bytes=%llu kind=%s reason=%s suppressed=%llu (nothing acts on it)",
      RoleName(), static_cast<unsigned long long>(sender), static_cast<unsigned long long>(size), KindName(kind),
      RefusalName(why), static_cast<unsigned long long>(suppressed));
}

void Take(const void* payload, std::uint64_t size, std::uint64_t sender) {
  const Decoded d = Decode(payload, static_cast<std::size_t>(size));
  if (d.refusal != Refusal::kNone) {
    Refuse(sender, size, d.refusal, Kind::kNone);
    return;
  }
  const std::uintptr_t netGame = NetGame();
  const std::uintptr_t bc = Broadcaster(netGame);
  const Role role = g_isServer ? Role::kServer : Role::kClient;
  std::optional<std::uint64_t> authority;
  std::vector<std::uint64_t> peers;
  if (role == Role::kClient) authority = AuthorityPeer(netGame);
  else peers = ReadMatch().peers;
  const Refusal admit = Admit(role, d.message.kind, sender, authority, peers);
  if (admit != Refusal::kNone) {
    Refuse(sender, size, admit, d.message.kind);
    return;
  }
  const char* from = role == Role::kClient ? "authority" : "match_peer";
  switch (d.message.kind) {
    case Kind::kCapabilities:
      Log(EchoVR::LogLevel::Info, "[NEVR.PEER] capabilities received role=%s peer=%llu from=%s bytes=%llu gear=%s", RoleName(),
          static_cast<unsigned long long>(sender), from, static_cast<unsigned long long>(size), GearText(d.message.gear).c_str());
      SendPing(bc, sender);
      break;
    case Kind::kPing: {
      Log(EchoVR::LogLevel::Info, "[NEVR.PEER] ping received role=%s peer=%llu from=%s bytes=%llu seq=%llu", RoleName(),
          static_cast<unsigned long long>(sender), from, static_cast<unsigned long long>(size),
          static_cast<unsigned long long>(d.message.seq));
      const std::string detail = "seq=" + std::to_string(d.message.seq);
      Send(bc, sender, Kind::kPong, EncodePong(d.message.seq), detail.c_str());
      break;
    }
    case Kind::kPong: {
      const std::optional<std::uint64_t> rtt = g_pings.Answer(d.message.seq, NowMs());
      Log(EchoVR::LogLevel::Info, "[NEVR.PEER] pong received role=%s peer=%llu from=%s bytes=%llu seq=%llu rtt_ms=%lld%s",
          RoleName(), static_cast<unsigned long long>(sender), from, static_cast<unsigned long long>(size),
          static_cast<unsigned long long>(d.message.seq), rtt ? static_cast<long long>(*rtt) : -1LL,
          rtt ? "" : " (no ping of this seq is outstanding)");
      break;
    }
    case Kind::kNone:
      break;
  }
}

// The broadcaster calls this for every NEVRProtobufMessageV1 it receives, on the game thread, as
// (instance, &method, payload copy, size, a3, sender peer) (InvokeListeners 0x140f80860, call site 0x140f80c82). The
// payload copy lives for this call only. Nothing may escape into the game's dispatch.
void Listener(void*, void*, const void* payload, std::uint64_t size, std::uint64_t, std::uint64_t sender) {
  try {
    Take(payload, size, sender);
  } catch (const std::exception& e) {
    Log(EchoVR::LogLevel::Warning, "[NEVR.PEER] message handling failed role=%s peer=%llu bytes=%llu what_bytes=%zu",
        RoleName(), static_cast<unsigned long long>(sender), static_cast<unsigned long long>(size), std::strlen(e.what()));
  }
}

void EnsureListening(std::uintptr_t broadcaster) {
  if (broadcaster == 0 || reinterpret_cast<void*>(broadcaster) == g_listenedBroadcaster || EchoVR::BroadcasterListen == nullptr) {
    return;
  }
  EchoVR::DelegateProxy proxy = {};
  proxy.method[0] = 0xFFFFFFFFFFFFFFFFULL;  // no member function: the proxy is called directly (server/constants.h DELEGATE_PROXY_INVALID_METHOD)
  proxy.instance = nullptr;
  proxy.proxyFunc = reinterpret_cast<void*>(&Listener);
  g_listenHandle = EchoVR::BroadcasterListen(reinterpret_cast<EchoVR::Broadcaster*>(broadcaster),
                                             static_cast<EchoVR::SymbolId>(kMessageSymbol), TRUE, &proxy, FALSE);
  g_listenedBroadcaster = reinterpret_cast<void*>(broadcaster);
  Log(EchoVR::LogLevel::Info,
      "[NEVR.PEER] listener registered role=%s symbol=0x%016llx broadcaster=%p handle=%u (nEVR control messages are taken "
      "on the game's broadcaster)",
      RoleName(), static_cast<unsigned long long>(kMessageSymbol), g_listenedBroadcaster, g_listenHandle);
}

void PollServer(std::uintptr_t broadcaster, std::uint64_t nowUs) {
  if (nowUs - g_lastServerPollUs < kServerPollUs) return;
  g_lastServerPollUs = nowUs;
  const MatchView view = ReadMatch();
  if (!view.valid) return;
  if (!g_broadcasterCompared && view.lobbyBroadcaster != 0) {
    g_broadcasterCompared = true;
    Log(EchoVR::LogLevel::Info, "[NEVR.PEER] server broadcaster net_game=%p lobby=%p same=%s",
        reinterpret_cast<void*>(broadcaster), reinterpret_cast<void*>(view.lobbyBroadcaster),
        view.lobbyBroadcaster == broadcaster ? "yes" : "no");
  }
  if (view.session != g_session) {
    if (!g_greeted.empty() || !g_session.empty()) {
      Log(EchoVR::LogLevel::Info, "[NEVR.PEER] session changed from=%s to=%s greeted_peers_forgotten=%zu",
          g_session.empty() ? "none" : g_session.c_str(), view.session.empty() ? "none" : view.session.c_str(), g_greeted.size());
    }
    g_session = view.session;
    g_greeted.clear();
  }
  if (g_session.empty()) return;
  const std::vector<std::string> gear = GearFromFeatures(nevr_combat::ServerFeatures());
  for (const std::uint64_t peer : view.peers) {
    if (!g_greeted.insert(peer).second) continue;
    const std::string detail = "gear=" + GearText(gear) + " session=" + g_session;
    Send(broadcaster, peer, Kind::kCapabilities, EncodeCapabilities(gear), detail.c_str());
    SendPing(broadcaster, peer);
  }
}

void PollClient(std::uintptr_t netGame) {
  const std::optional<std::uint64_t> authority = AuthorityPeer(netGame);
  const std::uint64_t now = authority.value_or(kPeerInvalid);
  if (now == g_lastAuthority) return;
  Log(EchoVR::LogLevel::Info, "[NEVR.PEER] authority peer changed from=%lld to=%lld (messages are taken from it only)",
      g_lastAuthority == kPeerInvalid ? -1LL : static_cast<long long>(g_lastAuthority),
      now == kPeerInvalid ? -1LL : static_cast<long long>(now));
  g_lastAuthority = now;
}

}  // namespace

void SetMatchViewSource(std::function<MatchView()> source) {
  std::lock_guard<std::mutex> lock(g_sourceLock);
  g_source = std::move(source);
}

void Poll(std::uint64_t nowUs) {
  const std::uintptr_t netGame = NetGame();
  const std::uintptr_t broadcaster = Broadcaster(netGame);
  if (broadcaster == 0) return;
  EnsureListening(broadcaster);
  if (g_isServer) PollServer(broadcaster, nowUs);
  else PollClient(netGame);
}

}  // namespace nevr_peer_message
