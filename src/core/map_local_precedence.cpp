#include "core/map_local_precedence.h"

namespace nevr_maps {

LocalPrecedence DecideLocalPrecedence(const std::vector<std::string>& cdn_names, const std::set<std::string>& local_names) {
  for (const std::string& name : cdn_names) {
    if (!name.empty() && local_names.count(name) != 0) return LocalPrecedence{true, name};
  }
  return LocalPrecedence{};
}

std::set<std::string> CollectLocalNames(const LocalLevelNamesFn& provider, const std::vector<std::string>& local_map_ids) {
  std::set<std::string> names;
  if (provider) {
    for (const std::string& name : provider()) {
      if (!name.empty()) names.insert(name);
    }
  }
  for (const std::string& id : local_map_ids) {
    if (!id.empty()) names.insert(id);
  }
  return names;
}

}  // namespace nevr_maps
