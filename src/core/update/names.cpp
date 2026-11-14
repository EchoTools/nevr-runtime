#include "core/update/names.h"

namespace nevr_update_names {
namespace {

char Upper(char c) noexcept { return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c; }

bool EqualsUpper(std::string_view text, std::string_view upper) noexcept {
  if (text.size() != upper.size()) return false;
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (Upper(text[i]) != upper[i]) return false;
  }
  return true;
}

bool IsAlnum(char c) noexcept { return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }

// The shared rule: 1 to 64 characters from `allowed`, no leading or trailing '.', not a device name.
template <typename Allowed>
bool IsName(std::string_view name, Allowed allowed) noexcept {
  if (name.empty() || name.size() > kMaxNameLength) return false;
  if (name.front() == '.' || name.back() == '.') return false;
  for (const char c : name) {
    if (!allowed(c)) return false;
  }
  return !IsDeviceName(name);
}

}  // namespace

bool IsDeviceName(std::string_view name) noexcept {
  const std::string_view base = name.substr(0, name.find('.'));
  for (const std::string_view device : {std::string_view("CON"), std::string_view("PRN"), std::string_view("AUX"),
                                        std::string_view("NUL"), std::string_view("CONIN$"), std::string_view("CONOUT$")}) {
    if (EqualsUpper(base, device)) return true;
  }
  if (base.size() == 4 && (EqualsUpper(base.substr(0, 3), "COM") || EqualsUpper(base.substr(0, 3), "LPT")) &&
      base[3] >= '0' && base[3] <= '9') {
    return true;
  }
  return false;
}

bool IsVersionName(std::string_view version) noexcept {
  return IsName(version, [](char c) { return IsAlnum(c) || c == '.' || c == '+' || c == '-'; });
}

bool IsPluginName(std::string_view name) noexcept {
  return IsName(name, [](char c) { return IsAlnum(c) || c == '.' || c == '_' || c == '-'; });
}

bool ParseMember(std::string_view path, Member* out) noexcept {
  if (out == nullptr) return false;
  if (path == kRuntimeMember) {
    out->runtime = true;
    out->plugin.clear();
    return true;
  }
  const std::string_view prefix(kPluginPrefix);
  const std::string_view suffix(kPluginSuffix);
  if (path.size() <= prefix.size() + suffix.size() || path.substr(0, prefix.size()) != prefix ||
      path.substr(path.size() - suffix.size()) != suffix) {
    return false;
  }
  const std::string_view name = path.substr(prefix.size(), path.size() - prefix.size() - suffix.size());
  if (!IsPluginName(name)) return false;
  out->runtime = false;
  out->plugin.assign(name.data(), name.size());
  return true;
}

bool IsQuestRuntimeMember(std::string_view path) noexcept { return path == std::string_view(kQuestRuntimeMember); }

std::string MemberPath(const Member& member) {
  if (member.runtime) return kRuntimeMember;
  return std::string(kPluginPrefix) + member.plugin + kPluginSuffix;
}

}  // namespace nevr_update_names
