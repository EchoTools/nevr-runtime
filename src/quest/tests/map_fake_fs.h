// A nevr_maps::MapFs over the shared fake Env's files (quest/tests/cargo_fake_env.h), for the map channel's host tests: one file
// system serves both seams, as on a device. A file's stamp is its id, its size and its write count, so a write moves the stamp and
// the same bytes written again move it too. Removing a tree keeps a file that a handle holds open, as Windows does, and then says
// false; renaming a directory with a file open under it is refused.
#pragma once

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "core/map_types.h"
#include "quest/tests/cargo_fake_env.h"

namespace cargo_test {

// The volume number every stamp of this file system carries.
constexpr std::uint64_t kFakeVolume = 7;

class FakeMapFs : public nevr_maps::MapFs {
 public:
  explicit FakeMapFs(FakeEnv& env) : env_(env) {}

  bool stamps_supported = true;  // false: a file system with no file ids (StampOf fails)

  static std::string Parent(const std::string& path) {
    const std::size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? std::string() : path.substr(0, slash);
  }
  static bool Under(const std::string& path, const std::string& dir) {
    return path.size() > dir.size() && path.compare(0, dir.size(), dir) == 0 && path[dir.size()] == '/';
  }

  // Test helpers: a file written with these bytes (its directories made), the same bytes with a new write time.
  void Put(const std::string& path, const std::string& bytes) {
    env_.MakeDirs(Parent(path));
    env_.fs_->files[path] = bytes;
    env_.fs_->Wrote(path);
  }
  void Touch(const std::string& path) { env_.fs_->Wrote(path); }
  bool Has(const std::string& path) const { return env_.fs_->files.count(path) != 0; }
  bool HasDir(const std::string& path) const { return env_.fs_->dirs.count(path) != 0; }

  bool StampOf(std::int64_t handle, nevr_maps::FileStamp* out) override {
    const std::string path = env_.PathOfHandle(handle);
    if (!stamps_supported || path.empty()) return false;
    *out = nevr_maps::FileStamp();
    out->volume = kFakeVolume;
    out->id_lo = env_.fs_->ids[path];
    out->size = env_.fs_->files[path].size();
    out->last_write = env_.fs_->writes[path];
    return true;
  }
  std::string FinalPathOf(std::int64_t handle) override { return env_.PathOfHandle(handle); }
  bool OpenHeld(const std::string& path, std::int64_t* handle, std::uint64_t* size) override {
    return env_.OpenBlob(path, handle, size);
  }
  bool ListDir(const std::string& dir, std::vector<std::string>* names) override {
    if (env_.fs_->dirs.count(dir) == 0) return false;
    std::set<std::string> found;
    for (const auto& file : env_.fs_->files) {
      if (Parent(file.first) == dir) found.insert(file.first.substr(dir.size() + 1));
    }
    for (const std::string& sub : env_.fs_->dirs) {
      if (Parent(sub) == dir) found.insert(sub.substr(dir.size() + 1));
    }
    names->assign(found.begin(), found.end());
    return true;
  }
  bool RemoveTree(const std::string& dir) override {
    if (env_.fs_->files.count(dir) != 0) return false;
    if (env_.fs_->dirs.count(dir) == 0) return true;
    std::set<std::string> held;
    for (const std::string& path : env_.OpenPaths()) {
      if (Under(path, dir)) held.insert(path);
    }
    auto& files = env_.fs_->files;
    for (auto it = files.begin(); it != files.end();) {
      it = Under(it->first, dir) && held.count(it->first) == 0 ? files.erase(it) : std::next(it);
    }
    auto& dirs = env_.fs_->dirs;
    for (auto it = dirs.begin(); it != dirs.end();) {
      const std::string sub = *it;
      const bool keep = std::any_of(held.begin(), held.end(), [&](const std::string& h) { return Under(h, sub); });
      it = (sub == dir || Under(sub, dir)) && !keep ? dirs.erase(it) : std::next(it);
    }
    return held.empty();
  }
  bool RenameDir(const std::string& from, const std::string& to) override {
    auto& files = env_.fs_->files;
    auto& dirs = env_.fs_->dirs;
    if (dirs.count(from) == 0 || dirs.count(to) != 0 || files.count(to) != 0 || dirs.count(Parent(to)) == 0) return false;
    for (const std::string& path : env_.OpenPaths()) {
      if (Under(path, from)) return false;
    }
    std::map<std::string, std::string> moved;
    for (auto it = files.begin(); it != files.end();) {
      if (Under(it->first, from)) {
        const std::string target = to + it->first.substr(from.size());
        moved[target] = it->second;
        // A rename keeps the file: its id and its write time go with it.
        env_.fs_->ids[target] = env_.fs_->ids[it->first];
        env_.fs_->writes[target] = env_.fs_->writes[it->first];
        it = files.erase(it);
      } else {
        ++it;
      }
    }
    files.insert(moved.begin(), moved.end());
    std::set<std::string> moved_dirs;
    for (auto it = dirs.begin(); it != dirs.end();) {
      if (*it == from || Under(*it, from)) {
        moved_dirs.insert(to + it->substr(from.size()));
        it = dirs.erase(it);
      } else {
        ++it;
      }
    }
    dirs.insert(moved_dirs.begin(), moved_dirs.end());
    return true;
  }

 private:
  FakeEnv& env_;
};

}  // namespace cargo_test
