/* The CDN's settings from config.yaml (`assets.cdn_url`, `assets.manifest`, `assets.required_only`, `assets.labels.<label>`;
 * docs/adr/0045-cdn-texture-delivery.md decision 9), read by one function for both runtimes: the PC's start
 * (runtime/lifecycle/cdn_start.cpp) and the Quest's configuration (quest/sentinel/quest_config.cpp, which adds `assets.whole_game`, a key
 * of the Quest's own). Header-only: it uses nevr::NevrConfig's reading methods, so only a target that links nevr_config.cpp includes it.
 *
 * Every key is optional. An absent key leaves the field as the code's default (an empty cdn_url or manifest, no label, required_only
 * off), and `from_config` stays false while no key was taken. A key that is wrong is one warning that names the key and never the value
 * (except a label name that is short printable ASCII, config_word.h), and the default stays. A warning is returned, not logged: each
 * runtime logs it its own way.
 */
#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "cdn/cdn_service.h"
#include "core/config_word.h"
#include "core/nevr_config.h"

namespace nevr_cdn {

inline constexpr const char* kAssetsCdnUrlKey = "assets.cdn_url";
inline constexpr const char* kAssetsManifestKey = "assets.manifest";
inline constexpr const char* kAssetsRequiredOnlyKey = "assets.required_only";
inline constexpr const char* kAssetsLabelsPath = "assets.labels";

// Bounds on values read from a file, so a line of the file cannot make a URL the core requests (and logs in cdn_manifest's `url`) or a
// table of labels arbitrarily large. None is a device or game limit.
inline constexpr std::size_t kMaxCdnBaseBytes = 200;
inline constexpr std::size_t kMaxManifestPathBytes = 128;
inline constexpr std::size_t kMaxLabelBytes = 48;
inline constexpr std::size_t kMaxAssetLabels = 32;
// The bytes of printable ASCII without a space (0x21 to 0x7e) bound a base URL; see config_word.h.

struct ConfigRead {
  Settings settings;
  std::vector<std::string> warnings;  // each "config file key=<key> rejected reason=<token> ..."
};

// The CDN host's base: https, a host, no query, fragment or credentials, no control or space character, ending in '/'.
inline bool NormalizeCdnBase(const std::string& in, std::string* out) {
  static const std::string kScheme = "https://";
  if (in.size() <= kScheme.size() || in.size() > kMaxCdnBaseBytes || in.compare(0, kScheme.size(), kScheme) != 0) return false;
  for (const char c : in) {
    const auto u = static_cast<unsigned char>(c);
    if (u <= nevr::kLastNonPrintableAscii || u >= nevr::kFirstNonAscii || c == '?' || c == '#' || c == '@') return false;
  }
  const std::size_t hostEnd = in.find('/', kScheme.size());
  if ((hostEnd == std::string::npos ? in.size() : hostEnd) == kScheme.size()) return false;  // no host
  *out = in.back() == '/' ? in : in + "/";
  return true;
}

// A manifest path below the host: relative, plain characters, no "..". Its name decides nothing: the core reads a zstd frame or plain
// JSON by the body's own bytes (cdn_manifest.h ParseManifestBody).
inline bool IsManifestPath(const std::string& p) {
  if (p.empty() || p.size() > kMaxManifestPathBytes || p[0] == '/') return false;
  if (p.find("..") != std::string::npos) return false;
  for (const char c : p) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-' || c == '/';
    if (!ok) return false;
  }
  return true;
}

// A label name as the manifest and the plan spell it: lower case, digits and '_'.
inline bool IsAssetLabel(const std::string& s) {
  if (s.empty() || s.size() > kMaxLabelBytes) return false;
  for (const char c : s) {
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return false;
  }
  return true;
}

inline ConfigRead ReadSettings(const nevr::NevrConfig& cfg) {
  ConfigRead r;
  Settings& a = r.settings;
  const auto warn = [&r](const std::string& key, const std::string& rest) { r.warnings.push_back("config file key=" + key + " rejected " + rest); };
  if (cfg.Kind(kAssetsCdnUrlKey) != nevr::NodeKind::kAbsent) {
    const std::optional<std::string> value = cfg.GetString(kAssetsCdnUrlKey);
    std::string base;
    if (value && NormalizeCdnBase(*value, &base)) {
      a.cdn_url = base;
      a.from_config = true;
    } else {
      warn(kAssetsCdnUrlKey, "reason=not_an_https_address (default stays)");
    }
  }
  if (cfg.Kind(kAssetsManifestKey) != nevr::NodeKind::kAbsent) {
    const std::optional<std::string> value = cfg.GetString(kAssetsManifestKey);
    if (value && IsManifestPath(*value)) {
      a.manifest = *value;
      a.from_config = true;
    } else {
      warn(kAssetsManifestKey, "reason=not_a_relative_path (default stays)");
    }
  }
  if (cfg.Kind(kAssetsRequiredOnlyKey) != nevr::NodeKind::kAbsent) {
    const std::optional<bool> value = cfg.GetBool(kAssetsRequiredOnlyKey);
    if (value) {
      a.required_only = *value;
      a.from_config = true;
    } else {
      warn(kAssetsRequiredOnlyKey, "reason=not_a_boolean (default stays)");
    }
  }
  const nevr::NodeKind labelsKind = cfg.Kind(kAssetsLabelsPath);
  if (labelsKind == nevr::NodeKind::kMapping) {
    std::size_t seen = 0;
    for (const std::string& label : cfg.MappingKeys(kAssetsLabelsPath)) {
      if (++seen > kMaxAssetLabels) {
        warn(kAssetsLabelsPath, "reason=too_many_labels more=" + std::to_string(seen - kMaxAssetLabels) + " (ignored)");
        break;
      }
      if (!IsAssetLabel(label)) {
        warn(kAssetsLabelsPath, "reason=bad_label_name name=" + nevr::DescribeRejectedWord(label) + " (ignored)");
        continue;
      }
      const std::optional<std::string> state = cfg.GetString(std::string(kAssetsLabelsPath) + "." + label);
      LabelState parsed;
      if (state && ParseLabelState(*state, &parsed)) {
        a.labels.emplace_back(label, *state);
        a.from_config = true;
      } else {
        warn(kAssetsLabelsPath, "reason=bad_state label=" + label + " (the label keeps its default; allowed: on, off, required)");
      }
    }
  } else if (labelsKind != nevr::NodeKind::kAbsent) {
    warn(kAssetsLabelsPath, "reason=not_a_mapping (default stays)");
  }
  return r;
}

}  // namespace nevr_cdn
