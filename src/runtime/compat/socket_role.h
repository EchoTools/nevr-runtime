#pragma once
// Which of the game's sockets to the bridge is config, login or matchmaker (#520).
//
// The game opens one socket per service. The bridge cannot assume an order: if the login socket connects
// first and the roles follow the order, the login socket is labelled config and gets no LoginRequest, the
// config socket is labelled login and gets it, and the game stalls at login. So the role comes from what
// the socket asked for:
//
//   * the redirect that points a game service at the bridge gives each service its own request target
//     (TagBridgeUrl): /config for the config service, /login for the login service;
//   * the bridge reads the target of every socket it accepts (RoleFromRequestTarget);
//   * a socket on the matchmaker listener is a matchmaker;
//   * a socket whose target names no role keeps the connect order, over the slots nothing is expected to
//     take (RoleTable::Admit).
//
// Platform neutral and pure: no sockets, no logging, no game state. The caller (ws_bridge.cpp) holds
// g_pairsMutex around a RoleTable, which has no lock of its own.

#include <atomic>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "runtime/compat/evr_codec.h"

namespace nevr_socket_role {

enum class Role { Config, Login, Matchmaker };

// The bridge listener that accepted a socket. The matchmaker listener is the port patched into
// pnsradmatchmaking.dll; every other service socket arrives on the service listener.
enum class Listener { Service, Matchmaker };

// How a socket's role was decided, for the log line that records each admission.
enum class Basis {
  Path,          // the request target named the role
  Listener,      // the matchmaker listener accepted it
  Order,         // no recognised target: the connect order
  LoginClaimed,  // it asked for /login while a login session exists (N91): it shares that session
  SlotReserved,  // no recognised target, and the slots left are ones a role-tagged socket is expected to take
};

inline const char* RoleName(Role role) {
  switch (role) {
    case Role::Config: return "config";
    case Role::Login: return "login";
    case Role::Matchmaker: return "matchmaker";
  }
  return "unknown";
}

inline const char* BasisName(Basis basis) {
  switch (basis) {
    case Basis::Path: return "path";
    case Basis::Listener: return "listener";
    case Basis::Order: return "order";
    case Basis::LoginClaimed: return "login-claimed";
    case Basis::SlotReserved: return "slot-reserved";
  }
  return "unknown";
}

inline const char* ListenerName(Listener listener) {
  return listener == Listener::Matchmaker ? "matchmaker" : "service";
}

// The request targets the redirect gives the two services the bridge tells apart by path.
inline constexpr std::string_view kConfigPath = "/config";
inline constexpr std::string_view kLoginPath = "/login";

// The path of a request target: everything before a query or a fragment.
inline std::string_view PathOf(std::string_view target) {
  const std::size_t end = target.find_first_of("?#");
  return end == std::string_view::npos ? target : target.substr(0, end);
}

// The role a request target names; nullopt when it names none (a pathless socket, or a path that is not
// one of ours).
inline std::optional<Role> RoleFromRequestTarget(std::string_view target) {
  const std::string_view path = PathOf(target);
  if (path == kConfigPath) return Role::Config;
  if (path == kLoginPath) return Role::Login;
  return std::nullopt;
}

// The role of the game service whose address the game reads under `keyName` (the JSON key the service
// redirect sees); nullopt for any other key. The config and login services are the ones whose sockets
// share the bridge's service listener.
inline std::optional<Role> RoleForServiceKey(const char* keyName) {
  if (keyName == nullptr) return std::nullopt;
  const std::string_view key(keyName);
  if (key == "config_host" || key == "configservice_host") return Role::Config;
  if (key == "login_host" || key == "loginservice_host") return Role::Login;
  return std::nullopt;
}

// Which roles the redirect has given a request target. A role in this set has a tagged socket on its way, so
// a socket with no target must not take that role's slot by connect order. Process-wide: the redirect runs in
// the game's config reader and the bridge in its own threads.
class Advertisement {
 public:
  void Note(Role role) {
    if (role == Role::Config) config_.store(true, std::memory_order_release);
    if (role == Role::Login) login_.store(true, std::memory_order_release);
  }
  bool Has(Role role) const {
    if (role == Role::Config) return config_.load(std::memory_order_acquire);
    if (role == Role::Login) return login_.load(std::memory_order_acquire);
    return false;
  }
  void Reset() {
    config_.store(false, std::memory_order_release);
    login_.store(false, std::memory_order_release);
  }

 private:
  std::atomic<bool> config_{false};
  std::atomic<bool> login_{false};
};

inline Advertisement& GlobalAdvertisement() {
  static Advertisement advertisement;
  return advertisement;
}

// The bridge's address for a game service: `url` with the service's request target appended, when `url` is
// exactly the bridge's loopback address and `keyName` is the config or login service's key. Any other URL
// or key comes back unchanged. Records the role in `advertised` (the process-wide set by default) when it
// tags, so the bridge knows a tagged socket is expected.
inline std::string TagBridgeUrl(const std::string& url, unsigned bridgePort, const char* keyName,
                                Advertisement& advertised = GlobalAdvertisement()) {
  const std::optional<Role> role = RoleForServiceKey(keyName);
  if (!role || bridgePort == 0) return url;
  if (url != "ws://127.0.0.1:" + std::to_string(bridgePort)) return url;
  advertised.Note(*role);
  return url + std::string(*role == Role::Config ? kConfigPath : kLoginPath);
}

// The role a socket's first frame positively names, for checking the role it was admitted with; nullopt when
// the symbol says nothing (the login connection can send anything). The same symbols the Quest router
// classifies a first frame by (session_router.cpp, ClassifyFirstFrame).
inline std::optional<Role> RoleOfFirstSymbol(uint64_t symbol) {
  switch (symbol) {
    case nevr_evr_codec::kSymConfigRequest:
      return Role::Config;
    case nevr_evr_codec::kSymLoginRequest:
      return Role::Login;
    case nevr_evr_codec::kSymMatchmakerStatusRequest:
    case nevr_evr_codec::kSymFindSessionRequest:
    case nevr_evr_codec::kSymCreateSessionRequest:
    case nevr_evr_codec::kSymJoinSessionRequest:
    case nevr_evr_codec::kSymDirectoryRequest:
    case nevr_evr_codec::kSymPendingSessionCancel:
    case nevr_evr_codec::kSymPlayerSessionsRequest:
    case nevr_evr_codec::kSymLobbyPingResponse:
      return Role::Matchmaker;
    default:
      return std::nullopt;
  }
}

// The slot a socket takes. connIdx is 0 for every config socket, 1 for the login socket, and from 2 up,
// unique, for the sockets that share the login session.
struct Admission {
  Role role = Role::Config;
  int connIdx = -1;
  Basis basis = Basis::Order;
};

inline constexpr int kConfigConnIdx = 0;
inline constexpr int kLoginConnIdx = 1;
inline constexpr int kFirstSharedConnIdx = 2;

// The bridge's record of which slots are taken. Not thread safe: the caller serialises (g_pairsMutex).
class RoleTable {
 public:
  explicit RoleTable(const Advertisement* advertised = nullptr)
      : advertised_(advertised != nullptr ? advertised : &GlobalAdvertisement()) {}

  // Admits one socket. `byTarget` is RoleFromRequestTarget of its request target.
  //   * Matchmaker listener: a matchmaker, whatever it asked for.
  //   * /config: a config socket, as many as arrive; it never shares the login session.
  //   * /login: the login socket; while a login session exists a further /login is not a second login (N91).
  //   * no recognised target: the connect order, config then login, over the slots no tagged socket is
  //     expected to take; with none left it is a matchmaker.
  Admission Admit(Listener listener, std::optional<Role> byTarget) {
    if (listener == Listener::Matchmaker) return Shared(Basis::Listener);
    if (byTarget) {
      switch (*byTarget) {
        case Role::Config:
          configTaken_ = true;
          return {Role::Config, kConfigConnIdx, Basis::Path};
        case Role::Login:
          if (loginClaimed_) return Shared(Basis::LoginClaimed);
          loginClaimed_ = true;
          return {Role::Login, kLoginConnIdx, Basis::Path};
        case Role::Matchmaker:
          return Shared(Basis::Path);
      }
    }
    if (!configTaken_ && !advertised_->Has(Role::Config)) {
      configTaken_ = true;
      return {Role::Config, kConfigConnIdx, Basis::Order};
    }
    if (!loginClaimed_ && !advertised_->Has(Role::Login)) {
      loginClaimed_ = true;
      return {Role::Login, kLoginConnIdx, Basis::Order};
    }
    const bool reserved = (!configTaken_ && advertised_->Has(Role::Config)) ||
                          (!loginClaimed_ && advertised_->Has(Role::Login));
    return Shared(reserved ? Basis::SlotReserved : Basis::Order);
  }

  // The login session ended (#70): the game's next /login is a new login, and the shared numbering starts again.
  void LoginSessionEnded() {
    loginClaimed_ = false;
    nextShared_ = kFirstSharedConnIdx;
  }

  // The index the next socket that shares the login session takes.
  int NextSharedConnIdx() const { return nextShared_; }
  // True while a login socket holds the login slot.
  bool LoginClaimed() const { return loginClaimed_; }

  void Reset() {
    configTaken_ = false;
    loginClaimed_ = false;
    nextShared_ = kFirstSharedConnIdx;
  }

 private:
  Admission Shared(Basis basis) { return {Role::Matchmaker, nextShared_++, basis}; }

  const Advertisement* advertised_;
  bool configTaken_ = false;
  bool loginClaimed_ = false;
  int nextShared_ = kFirstSharedConnIdx;
};

}  // namespace nevr_socket_role
