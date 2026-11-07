#include "runtime/content/content_json.h"

#include <set>

#include <nlohmann/json.hpp>

#include "runtime/content/pack_rules.h"

namespace nevr_content {

namespace {

bool Refuse(std::string* reason, std::string* detail, const char* token, const std::string& what) {
  *reason = token;
  *detail = what;
  return false;
}

const std::string* StringAt(const nlohmann::json& object, const char* key) {
  const auto it = object.find(key);
  return it != object.end() && it->is_string() ? &it->get_ref<const std::string&>() : nullptr;
}

// A version is shown in the log: 1 to 64 printable ASCII characters, no quote or backslash.
bool IsShowableVersion(const std::string& text) {
  if (text.empty() || text.size() > 64) return false;
  for (const char c : text) {
    if (c < 0x21 || c > 0x7e || c == '"' || c == '\\') return false;
  }
  return true;
}

// `<package>_<n>` with n written in canonical decimal: the number, or -1.
long long PackageNumber(const std::string& name, const std::string& package) {
  if (name.size() <= package.size() + 1 || name.compare(0, package.size(), package) != 0 || name[package.size()] != '_') {
    return -1;
  }
  const std::string digits = name.substr(package.size() + 1);
  if (digits.size() > 9 || (digits.size() > 1 && digits.front() == '0')) return -1;
  long long n = 0;
  for (const char c : digits) {
    if (c < '0' || c > '9') return -1;
    n = n * 10 + (c - '0');
  }
  return n;
}

}  // namespace

bool IsRelativePlainPath(const std::string& path) {
  if (path.empty() || path.size() > 200) return false;
  std::size_t start = 0;
  while (true) {
    const std::size_t slash = path.find('/', start);
    const std::string part = path.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
    if (!IsPlainName(part)) return false;  // an empty part, ".", ".." and any name with a dot first are not plain
    if (slash == std::string::npos) return true;
    start = slash + 1;
  }
}

std::string LeafOf(const std::string& path) {
  const std::size_t slash = path.rfind('/');
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

bool ParseContentJson(const std::string& text, const std::string& folder_id, Descriptor* out, std::string* reason,
                      std::string* detail) {
  detail->clear();
  if (text.size() > kMaxContentJsonBytes) return Refuse(reason, detail, "too_large", std::to_string(text.size()) + " bytes");
  const nlohmann::json doc = nlohmann::json::parse(text, nullptr, false);
  if (doc.is_discarded() || !doc.is_object()) return Refuse(reason, detail, "not_json", "");
  const auto format = doc.find("format");
  if (format == doc.end() || !format->is_number_unsigned() || format->get<std::uint64_t>() != 1) {
    return Refuse(reason, detail, "bad_format", "format must be 1");
  }

  Descriptor d;
  const std::string* id = StringAt(doc, "id");
  if (id == nullptr || !IsPackId(*id)) return Refuse(reason, detail, "bad_id", id ? *id : "");
  if (*id != folder_id) return Refuse(reason, detail, "id_mismatch", *id + " in folder " + folder_id);
  d.id = *id;
  const std::string* version = StringAt(doc, "version");
  if (version == nullptr || !IsShowableVersion(*version)) return Refuse(reason, detail, "bad_version", "");
  d.version = *version;

  const auto files = doc.find("files");
  if (files == doc.end() || !files->is_object() || files->empty()) return Refuse(reason, detail, "no_files", "");
  if (files->size() > kMaxPackFiles) return Refuse(reason, detail, "too_many_files", std::to_string(files->size()));
  std::set<std::string> folded;  // two paths that differ only in case are one file on Windows
  for (const auto& [path, sha] : files->items()) {
    if (!IsRelativePlainPath(path) || !sha.is_string() || !IsLowerHex(sha.get_ref<const std::string&>(), 64)) {
      return Refuse(reason, detail, "bad_file_entry", path);
    }
    std::string lower = path;
    for (char& c : lower) {
      if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    if (!folded.insert(lower).second) return Refuse(reason, detail, "bad_file_entry", path + " (differs from another only in case)");
    d.files[path] = sha.get<std::string>();
  }
  const auto listed = [&](const std::string& path) { return d.files.count(path) != 0; };

  const std::string* data_dir = StringAt(doc, "data_dir");
  if (data_dir == nullptr || !IsRelativePlainPath(*data_dir)) return Refuse(reason, detail, "bad_data_dir", data_dir ? *data_dir : "");
  d.data_dir = *data_dir;

  const auto manifests = doc.find("manifests");
  if (manifests == doc.end() || !manifests->is_array() || manifests->empty()) return Refuse(reason, detail, "no_manifests", "");
  std::set<std::string> sets;
  std::set<std::string> served;  // every pack path served in place of a game file, once
  for (const nlohmann::json& m : *manifests) {
    if (!m.is_object()) return Refuse(reason, detail, "bad_manifest", "");
    ManifestEntry e;
    const std::string* package = StringAt(m, "package");
    const std::string* stock = StringAt(m, "stock_sha256");
    const std::string* file = StringAt(m, "file");
    const auto count = m.find("stock_packages");
    if (package == nullptr || !IsLowerHex(*package, 16)) return Refuse(reason, detail, "bad_manifest", package ? *package : "");
    if (stock == nullptr || !IsLowerHex(*stock, 64) || count == m.end() || !count->is_number_unsigned() ||
        count->get<std::uint64_t>() < 1 || count->get<std::uint64_t>() > 4096 || file == nullptr) {
      return Refuse(reason, detail, "bad_manifest", *package);
    }
    if (!sets.insert(*package).second) return Refuse(reason, detail, "duplicate_manifest", *package);
    if (!listed(*file)) return Refuse(reason, detail, "unlisted_file", *file);
    if (!served.insert(*file).second) return Refuse(reason, detail, "duplicate_package", *file);
    e.package = *package;
    e.stock_sha256 = *stock;
    e.stock_packages = static_cast<std::uint32_t>(count->get<std::uint64_t>());
    e.file = *file;
    const auto packages = m.find("packages");
    if (packages != m.end()) {
      if (!packages->is_array()) return Refuse(reason, detail, "bad_package", *package);
      std::set<long long> numbers;
      for (const nlohmann::json& p : *packages) {
        if (!p.is_string()) return Refuse(reason, detail, "bad_package", *package);
        const std::string& rel = p.get_ref<const std::string&>();
        // An added package's number is at least the stock count, so it can never stand in for a stock package.
        const long long n = PackageNumber(LeafOf(rel), *package);
        if (n < static_cast<long long>(e.stock_packages)) return Refuse(reason, detail, "bad_package", rel);
        if (!listed(rel)) return Refuse(reason, detail, "unlisted_file", rel);
        if (!numbers.insert(n).second || !served.insert(rel).second) return Refuse(reason, detail, "duplicate_package", rel);
        e.packages.push_back(rel);
      }
    }
    d.manifests.push_back(std::move(e));
  }

  const auto scripts = doc.find("scripts");
  if (scripts != doc.end()) {
    if (!scripts->is_array()) return Refuse(reason, detail, "bad_scripts", "");
    std::set<std::string> names;
    for (const nlohmann::json& s : *scripts) {
      if (!s.is_string()) return Refuse(reason, detail, "bad_script", "");
      const std::string& rel = s.get_ref<const std::string&>();
      const std::string leaf = LeafOf(rel);
      const bool shaped = rel == "scripts/" + leaf && leaf.size() == 20 && IsLowerHex(leaf.substr(0, 16), 16) &&
                          leaf.compare(16, 4, ".dll") == 0;
      if (!shaped) return Refuse(reason, detail, "bad_script", rel);
      if (!listed(rel)) return Refuse(reason, detail, "unlisted_file", rel);
      if (!names.insert(leaf).second) return Refuse(reason, detail, "duplicate_script", rel);
      d.scripts.push_back(rel);
    }
  }

  *out = std::move(d);
  reason->clear();
  return true;
}

}  // namespace nevr_content
