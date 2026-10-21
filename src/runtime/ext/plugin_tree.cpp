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
      for (const std::string& name : *kind.first) {
        // A name with a path in it is resolved relative to something this walk cannot know, so it cannot be checked.
        if (name.find_first_of("\\/:") != std::string::npos) {
          return Refuse(name, kind.second, Reason::Unreadable, "a dependency named by a path cannot be checked");
        }
        if (!seen.insert(name).second) continue;
        HANDLE h = CreateFileA((pluginDir + name).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
          const DWORD code = GetLastError();
          if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) continue;  // the OS's, or absent
          return Refuse(name, kind.second, Reason::Unreadable,
                        "cannot open the file to check it: error " + std::to_string(code));
        }
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
