#include "quest/integration/social_gate.h"

#include <exception>

#include <nlohmann/json.hpp>

namespace nevr_quest::integration {

bool SocialRequested(const std::string* fileText) noexcept {
  if (fileText == nullptr || fileText->size() > nevr_quest::kMaxConfigBytes) return false;
  try {
    const nlohmann::json doc = nlohmann::json::parse(*fileText, nullptr, /*allow_exceptions=*/true);
    if (!doc.is_object()) return false;
    const auto features = doc.find("features");
    if (features == doc.end() || !features->is_object()) return false;
    const auto social = features->find("social");
    return social != features->end() && social->is_boolean() && social->get<bool>();
  } catch (const std::exception&) {
    return false;  // nlohmann::json::exception derives from std::exception
  }
}

bool SocialEffective(bool requested, const nevr_quest::Features& effective, const char** reason) noexcept {
  const char* why = "ok";
  bool on = true;
  if (!requested) {
    why = "not_requested";
    on = false;
  } else if (!effective.login) {
    why = "login_not_enabled";
    on = false;
  }
  if (reason != nullptr) *reason = why;
  return on;
}

}  // namespace nevr_quest::integration
