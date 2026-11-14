#pragma once

// The order in which a test removes what it recorded making (run_root.h, nevr_test::Owned). Header-only, with no platform
// header, so a Linux host test (src/quest/tests/run_root_order_test.cpp) runs the same code the Win32 tests run.

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

namespace nevr_test {

// One record a test made: `entry` is '/'-separated and relative to the test's root; `folder` is true for a folder.
struct OwnedRecord {
  std::string entry;
  bool folder;
};

// The entries to pass to nevr_owned::RemoveOwned, in the order that removes children before their parents. A name recorded
// twice is listed once, at its first record. The order is deepest first; at one depth, files come before folders; within
// that, the latest record comes first. A folder is therefore always listed after everything recorded under it.
inline std::vector<std::string> RemovalOrder(const std::vector<OwnedRecord>& recorded) {
  std::vector<OwnedRecord> unique;
  for (const OwnedRecord& record : recorded) {
    const bool seen = std::any_of(unique.begin(), unique.end(),
                                  [&record](const OwnedRecord& u) { return u.entry == record.entry; });
    if (!seen) unique.push_back(record);
  }
  std::reverse(unique.begin(), unique.end());
  const auto depth = [](const std::string& entry) {
    return static_cast<std::size_t>(std::count(entry.begin(), entry.end(), '/')) + 1;
  };
  std::stable_sort(unique.begin(), unique.end(), [&depth](const OwnedRecord& a, const OwnedRecord& b) {
    const std::size_t da = depth(a.entry);
    const std::size_t db = depth(b.entry);
    if (da != db) return da > db;
    return !a.folder && b.folder;
  });
  std::vector<std::string> order;
  order.reserve(unique.size());
  for (const OwnedRecord& record : unique) order.push_back(record.entry);
  return order;
}

}  // namespace nevr_test
