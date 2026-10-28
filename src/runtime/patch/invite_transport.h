#pragma once
// #1156 (PR-D slice D1b), docs/adr/0017-invite-codes-for-the-create-game-wizard.md: the address a request goes to, as pure
// logic. The token travels in the request, so where it may be sent is decided here and nowhere else: https always, plain
// http only to the machine itself (a local service under test), never a base with a user, a query or a fragment in it, and an
// RPC name that is a path of lower-case words. The libcurl transport (invite_transport_curl.h) builds its URL through this
// and sends nothing when it refuses.
#include <cstddef>
#include <string>
#include <string_view>

namespace nevr_invite {

enum class UrlCheck { Ok, Empty, BadScheme, PlainHttpToRemote, BadBase, BadRpc };

inline const char* UrlCheckName(UrlCheck check) {
  switch (check) {
    case UrlCheck::Ok:
      return "ok";
    case UrlCheck::Empty:
      return "empty";
    case UrlCheck::BadScheme:
      return "bad_scheme";
    case UrlCheck::PlainHttpToRemote:
      return "plain_http_to_remote";
    case UrlCheck::BadBase:
      return "bad_base";
    case UrlCheck::BadRpc:
      return "bad_rpc";
  }
  return "bad_base";
}

struct Endpoint {
  UrlCheck check = UrlCheck::Empty;
  std::string url;  // Ok only
};

namespace detail {

inline bool IsLoopbackHost(std::string_view host) {
  return host == "127.0.0.1" || host == "localhost" || host == "[::1]";
}

// "word" or "word/word": lower-case letters and single slashes inside, as the service's invite/create names are.
inline bool IsRpcName(std::string_view rpc) {
  if (rpc.empty() || rpc.front() == '/' || rpc.back() == '/') return false;
  char previous = '/';
  for (const char c : rpc) {
    const bool letter = c >= 'a' && c <= 'z';
    if (!letter && c != '/') return false;
    if (c == '/' && previous == '/') return false;
    previous = c;
  }
  return true;
}

}  // namespace detail

/// `base` is the service's HTTP address (config auth.http_uri); `rpc` the RPC name. The URL is
/// `<base>/v2/rpc/<rpc>?unwrap`: Nakama's HTTP form of an RPC, with the answer's payload unwrapped.
inline Endpoint BuildRpcUrl(std::string_view base, std::string_view rpc) {
  Endpoint out;
  while (!base.empty() && base.back() == '/') base.remove_suffix(1);
  if (base.empty()) return out;  // Empty
  constexpr std::string_view kHttps = "https://";
  constexpr std::string_view kHttp = "http://";
  const bool https = base.substr(0, kHttps.size()) == kHttps;
  const bool http = base.substr(0, kHttp.size()) == kHttp;
  if (!https && !http) {
    out.check = UrlCheck::BadScheme;
    return out;
  }
  const std::string_view rest = base.substr(https ? kHttps.size() : kHttp.size());
  std::size_t hostEnd = std::string_view::npos;
  if (!rest.empty() && rest.front() == '[') {  // an IPv6 literal: the host runs to its closing bracket
    const std::size_t close = rest.find(']');
    if (close != std::string_view::npos) hostEnd = close + 1;
  } else {
    hostEnd = rest.find_first_of(":/");
  }
  const std::string_view host = rest.substr(0, hostEnd);
  for (const char c : rest) {
    if (c == '@' || c == '?' || c == '#' || c == '\\' || c <= ' ' || c == 0x7f) {
      out.check = UrlCheck::BadBase;
      return out;
    }
  }
  if (host.empty()) {
    out.check = UrlCheck::BadBase;
    return out;
  }
  if (http && !detail::IsLoopbackHost(host)) {
    out.check = UrlCheck::PlainHttpToRemote;
    return out;
  }
  if (!detail::IsRpcName(rpc)) {
    out.check = UrlCheck::BadRpc;
    return out;
  }
  out.check = UrlCheck::Ok;
  out.url = std::string(base) + "/v2/rpc/" + std::string(rpc) + "?unwrap";
  return out;
}

}  // namespace nevr_invite
