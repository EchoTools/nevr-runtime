// The scratch base of a Quest host test (ORDER.md item 2, DESIGN.md P1q). A test does not remove a tree: each folder it makes has
// a name of its own, made new (mkdtemp, or create_directory that fails on an existing name), and nothing clears a folder.
//
// NEVR_TEST_TMP is the run root the recipe made for this run (run-root make); the recipe removes it once when the run ends.
// Unset (a ctest run by hand, outside a recipe), the base is one run folder made once per process under the fallback (the
// build tree's scratch folder), so a hand run works and nothing is removed.
#ifndef NEVR_QUEST_TESTS_RUN_SCRATCH_H_
#define NEVR_QUEST_TESTS_RUN_SCRATCH_H_

#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>

namespace nevr_test_scratch {

// The base folder of this process: NEVR_TEST_TMP when it is set and not empty; else one run folder (run-XXXXXX) made once
// under fallback, memoised for the process. When that folder cannot be made, the fallback itself.
inline std::string Base(const std::string& fallback) {
  const char* root = std::getenv("NEVR_TEST_TMP");
  if (root != nullptr && *root != '\0') return std::string(root);
  static const std::string made = [&fallback]() {
    std::error_code ec;
    std::filesystem::create_directories(fallback, ec);
    std::string tmpl = fallback + "/run-XXXXXX";
    const char* d = ::mkdtemp(&tmpl[0]);
    return d != nullptr ? std::string(d) : std::string();
  }();
  return made.empty() ? fallback : made;
}

// A new folder under Base(fallback), named <name>-XXXXXX: made fresh each call, never cleared. Empty when it cannot be made.
inline std::string NewFolder(const std::string& fallback, const std::string& name) {
  const std::string base = Base(fallback);
  std::error_code ec;
  std::filesystem::create_directories(base, ec);
  std::string tmpl = base + "/" + name + "-XXXXXX";
  const char* d = ::mkdtemp(&tmpl[0]);
  return d != nullptr ? std::string(d) : std::string();
}

}  // namespace nevr_test_scratch

#endif  // NEVR_QUEST_TESTS_RUN_SCRATCH_H_
