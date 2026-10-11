#pragma once
// The role of a game socket on the bridge port (#513).
//
// The game opens its config and login sockets to the same bridge port at the same moment, so the order they are
// accepted in is not the order the game meant: when the config socket was accepted second it was taken for the
// login, the login went out on it, and the game's real login socket never got its answer. The redirects for the
// config and login services carry the service in the bridge URL's path (ws://127.0.0.1:<port>/config,
// /login), and the bridge takes a socket's role from its upgrade request's path. Connect order stays the role of
// a socket whose path names none (the other services, which open after the login).
//
// Pure: no sockets, no logging.

#include <atomic>
#include <cstring>
#include <string_view>

namespace nevr_bridge_role {

enum class Role { Unknown, Config, Login };

// The path the bridge URL for the game's `keyName` lookup carries: "/config" for the config service keys,
// "/login" for the login service keys, "" for any other key (and null).
inline const char* PathForServiceKey(const char* keyName) {
  if (keyName == nullptr) return "";
  if (std::strcmp(keyName, "config_host") == 0 || std::strcmp(keyName, "configservice_host") == 0) return "/config";
  if (std::strcmp(keyName, "login_host") == 0 || std::strcmp(keyName, "loginservice_host") == 0) return "/login";
  return "";
}

// The role a socket's upgrade request names: "/config" or "/login", alone or followed by '/' or a query.
inline Role RoleFromRequestPath(std::string_view target) {
  const auto names = [&](std::string_view path) {
    if (target.substr(0, path.size()) != path) return false;
    return target.size() == path.size() || target[path.size()] == '/' || target[path.size()] == '?';
  };
  if (names("/config")) return Role::Config;
  if (names("/login")) return Role::Login;
  return Role::Unknown;
}

// The bridge's index for a socket that has just connected: 0 config, 1 login, 2 and up the sockets after the
// login (they share the login session). A named role takes its own index and moves `counter` past it. A socket
// with no role takes the next index by connect order, as every socket did before #513, and so does a login
// socket while a login session is still open (`loginSessionOpen`): it shares that session, as before.
inline int ConnectionIndexFor(Role role, bool loginSessionOpen, std::atomic<int>& counter) {
  if (role == Role::Unknown || (role == Role::Login && loginSessionOpen)) return counter++;
  const int index = role == Role::Config ? 0 : 1;
  int next = counter.load();
  while (next <= index && !counter.compare_exchange_weak(next, index + 1)) {
  }
  return index;
}

}  // namespace nevr_bridge_role
