// The login_host fallback rule: the pure part, with no game call and no pool.
//
// The game reads its login endpoint with CJson::TString("login_host", fallback, 0). The fallback is
// a built-in wss:// URL with no query, and BeginLogIn and VerifyServerLoginConnection both pass it
// on to the login WebSocket. A community patch of the same call substitutes a URL that carries a
// build tuple as its query (name-comment-version-run). The rule here appends that tuple to a
// fallback that has none, so the stock fallback and the patched one both reach the login socket
// with one query. The rule is applied in ServiceRedirector::ApplyFallback, which runs in the
// TString handler before the original (tstring_thunks.cpp).
//
// Header-only and free of exceptions, so the pure rules can be tested without the pool.
#pragma once

#include <string>
#include <string_view>

namespace nevr_quest::redirect {

inline constexpr char kLoginHostKey[] = "login_host";

// The build identity the rule appends: the same four fields the login's build tuple carries.
struct BuildIdentity {
  std::string name;
  std::string comment;
  std::string version;
  std::string run;

  // The rule is off until the identity has a version.
  bool Valid() const { return !version.empty(); }
};

// True for a fallback that starts with ws:// or wss://.
inline bool IsWsUrl(std::string_view url) {
  return url.substr(0, 5) == "ws://" || url.substr(0, 6) == "wss://";
}

// True when the URL already carries a query.
inline bool HasQuery(std::string_view url) { return url.find('?') != std::string_view::npos; }

// Keeps [A-Za-z0-9._]; every other character becomes '_'. An empty field becomes "unknown", so the
// tuple keeps four fields and a '-' separator can never be ambiguous.
inline std::string SanitizeField(std::string_view field) {
  if (field.empty()) return "unknown";
  std::string out;
  out.reserve(field.size());
  for (const char c : field) {
    const bool keep = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' ||
                      c == '_';
    out.push_back(keep ? c : '_');
  }
  return out;
}

// fallback + "?" + name-comment-version-run, every field sanitized.
inline std::string Compose(std::string_view fallback, const BuildIdentity& id) {
  std::string out;
  out.reserve(fallback.size() + 1 + id.name.size() + id.comment.size() + id.version.size() + id.run.size() + 4);
  out.append(fallback);
  out.push_back('?');
  out.append(SanitizeField(id.name));
  out.push_back('-');
  out.append(SanitizeField(id.comment));
  out.push_back('-');
  out.append(SanitizeField(id.version));
  out.push_back('-');
  out.append(SanitizeField(id.run));
  return out;
}

// True when the rule applies to this TString call: the key is exactly login_host, the fallback is a
// ws:// or wss:// URL, and the fallback has no query yet.
inline bool NeedsVersion(const char* key, const char* fallback) {
  if (key == nullptr || fallback == nullptr) return false;
  if (std::string_view(key) != std::string_view(kLoginHostKey)) return false;
  const std::string_view url(fallback);
  return IsWsUrl(url) && !HasQuery(url);
}

}  // namespace nevr_quest::redirect
