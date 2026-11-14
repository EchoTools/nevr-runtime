#pragma once
// The Quest update root's folder, and its comparison with the folder the activity names, as pure rules: no logging, no Android
// headers, so the host tests run the code they state.
//
// The update root (docs/adr/0024-pc-self-update.md, section 2): the folder that holds `update/` is the app's internal files
// folder with `nevr` below it, /data/user/<uid / kAidUserOffset>/<package>/files/nevr. It is not sentinel::FilesDir(), which
// is the external folder (quest/sentinel/sentinel_log.h).
//
// No code in this tree calls these rules yet; quest/tests/update_root_test.cpp runs them.

#include <string>
#include <string_view>

namespace nevr_quest::update_root {

inline constexpr char kRootFolderName[] = "nevr";

// The root below the app's internal files folder. An empty folder gives an empty root.
inline std::string RootFromFilesDir(std::string_view files_dir) {
  if (files_dir.empty()) return std::string();
  std::string root(files_dir);
  root += '/';
  root += kRootFolderName;
  return root;
}

// Whether `root` is the folder the activity names as its internal data path with the root's folder name below it. The
// activity's internalDataPath is the app's files folder (getFilesDir()), so the formula agrees with the platform only when
// this holds (docs/adr/0024-pc-self-update.md, section 2).
inline bool RootMatchesInternalDataPath(std::string_view root, std::string_view internal_data_path) {
  if (root.empty() || internal_data_path.empty()) return false;
  return root == RootFromFilesDir(internal_data_path);
}

}  // namespace nevr_quest::update_root
