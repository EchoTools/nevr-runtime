/* The Quest's nevr_maps::MapFs (docs/adr/0048-custom-maps-channel.md, "Consequences": the channel's two file seams): the map
 * channel's cache on POSIX files, beside the Quest's nevr_cargo::Env (posix_env.h).
 *
 * A held file is an open(2) read-only descriptor, which is also the engine's handle (posix_env.h). POSIX has no share mode, so
 * "nothing can write it while held" rests on the cache being the app's own (<internalDataPath>/nevr/cache/maps): only this process
 * writes there, and the channel core never writes a file it holds. The stamp (st_dev, st_ino, size, mtime in ns) is what tells a
 * changed file at the next mount (ADR 0048 decision 4).
 *
 * Built without exceptions, like the cargo env it sits beside.
 */
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/map_types.h"

namespace nevr_quest::maps {

class PosixMapFs : public nevr_maps::MapFs {
 public:
  bool StampOf(std::int64_t handle, nevr_maps::FileStamp* out) override;
  std::string FinalPathOf(std::int64_t handle) override;
  bool OpenHeld(const std::string& path, std::int64_t* handle, std::uint64_t* size) override;
  bool ListDir(const std::string& dir, std::vector<std::string>* names) override;
  bool RemoveTree(const std::string& dir) override;
  bool RenameDir(const std::string& from, const std::string& to) override;
};

}  // namespace nevr_quest::maps
