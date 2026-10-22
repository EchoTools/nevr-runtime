// plugin_tree.cpp — see plugin_tree.h (ADR 0010, #741).

#include "runtime/ext/plugin_tree.h"

#include <algorithm>
#include <deque>
#include <set>
#include <utility>

namespace nevr_plugin_tree {

using nevr_plugin_signature::Dependencies;
using nevr_plugin_signature::Reason;
using nevr_plugin_signature::Verdict;

namespace {
constexpr size_t kMaxFiles = 64;

struct Pending {
  std::string name;  // lowercase file name in the plugins directory
  std::string via;
};

TreeVerdict Refuse(const std::string& file, const std::string& via, Reason reason, const std::string& detail) {
  TreeVerdict t;
  t.accepted = false;
  t.file = file;
  t.via = via;
  t.verdict.reason = reason;
  t.verdict.detail = detail;
  return t;
}
}  // namespace

namespace {
template <typename String>
String WithDefaultExtensionOf(const String& name) {
  using Char = typename String::value_type;
  const size_t slash = name.find_last_of(static_cast<Char>('\\'));
  const size_t start = slash == String::npos ? 0 : slash + 1;
  if (name.find(static_cast<Char>('.'), start) != String::npos) return name;
  String out = name;
  for (const char* c = ".dll"; *c != '\0'; ++c) out.push_back(static_cast<Char>(*c));
  return out;
}

std::string Lower(std::string s) {
  for (char& c : s) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
  }
  return s;
}
}  // namespace

std::string WithDefaultExtension(const std::string& name) { return WithDefaultExtensionOf(name); }
std::wstring WithDefaultExtension(const std::wstring& name) { return WithDefaultExtensionOf(name); }

bool SuppliedByTheSystem(const std::string& file) {
  const std::string lower = Lower(file);
  if (lower.compare(0, 7, "api-ms-") == 0 || lower.compare(0, 7, "ext-ms-") == 0) return true;  // API sets: no file
  if (GetModuleHandleA(lower.c_str()) != nullptr) return true;
  char system[MAX_PATH] = {};
  const UINT n = GetSystemDirectoryA(system, MAX_PATH);
  if (n == 0 || n >= MAX_PATH) return false;
  const DWORD attributes = GetFileAttributesA((std::string(system) + "\\" + lower).c_str());
  return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

HeldFiles::~HeldFiles() {
  for (HANDLE h : handles_) CloseHandle(h);
}

void HeldFiles::Add(HANDLE handle) { handles_.push_back(handle); }

std::string TreeVerdict::Describe() const {
  if (accepted) return "accepted";
  std::string out = file;
  if (!via.empty()) out += " (" + via + ")";
  return out + ": " + verdict.Describe();
}

TreeVerdict VerifyPluginTree(const std::string& pluginDir, HANDLE root, const nevr_plugin_signature::Policy& policy,
                             int64_t nowUnix, HeldFiles* held) {
  std::deque<std::pair<HANDLE, Pending>> work;  // an open file and how it was reached
  work.push_back({root, Pending{"", ""}});
  std::set<std::string> seen;
  size_t checked = 0;

  while (!work.empty()) {
    const HANDLE file = work.front().first;
    const Pending current = work.front().second;
    work.pop_front();

    Dependencies deps;
    std::string error;
    if (!nevr_plugin_signature::ListDependencies(file, &deps, &error)) {
      return Refuse(current.name.empty() ? "the plugin" : current.name, current.via, Reason::MalformedSignature,
                    "cannot list its dependencies: " + error);
    }
    const std::pair<const std::vector<std::string>*, const char*> kinds[] = {
        {&deps.imports, "import"}, {&deps.delayImports, "delay-load"}, {&deps.forwarders, "forwarder"}};
    for (const auto& kind : kinds) {
      for (const std::string& written : *kind.first) {
        // A name with a path in it is resolved relative to something this walk cannot know, so it cannot be checked.
        if (written.find_first_of("\\/:") != std::string::npos) {
          return Refuse(written, kind.second, Reason::Unreadable, "a dependency named by a path cannot be checked");
        }
        // The loader appends ".dll" to a name with no extension; the file it maps is the one to check.
        const std::string name = WithDefaultExtension(written);
        if (seen.count(name) != 0) continue;
        const bool isDelayLoad = kind.first == &deps.delayImports;
        HANDLE h = CreateFileA((pluginDir + name).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
          const DWORD code = GetLastError();
          if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) {
            // A delay-load is resolved when it is first called, by a LoadLibrary the late-load filter sees then; an
            // optional one (nvapi64.dll on a machine without it) must not refuse a plugin that loads fine. It is not
            // marked seen: the same name as a static import or forwarder is still held to the rule below.
            if (isDelayLoad) continue;
            seen.insert(name);
            if (SuppliedByTheSystem(name)) continue;
            return Refuse(name, kind.second, Reason::Unreadable,
                          "not in the plugins directory and not supplied by the system: a file put there before "
                          "the load would be mapped unchecked");
          }
          return Refuse(name, kind.second, Reason::Unreadable,
                        "cannot open the file to check it: error " + std::to_string(code));
        }
        seen.insert(name);
        if (++checked > kMaxFiles) {
          CloseHandle(h);
          return Refuse(name, kind.second, Reason::Unreadable, "the plugin's dependency tree is over 64 files");
        }
        held->Add(h);
        const Verdict v = nevr_plugin_signature::VerifyPluginFile(h, policy, nowUnix);
        if (!v.accepted()) {
          TreeVerdict t;
          t.accepted = false;
          t.file = name;
          t.via = kind.second;
          t.verdict = v;
          return t;
        }
        work.push_back({h, Pending{name, kind.second}});
      }
    }
  }
  return TreeVerdict{};
}

}  // namespace nevr_plugin_tree
