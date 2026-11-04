// A read-only FAT16 volume (the image inside the store OBB is one, written by Android's `jobb`): boot sector, FAT, fixed root
// directory, subdirectories, long names and cluster chains. Anything else is refused with a code, never guessed at:
// FAT12 and FAT32 (decided by the data cluster count, as Microsoft's FAT specification says), a sector size other than 512,
// a boot sector without the 0x55AA mark, a chain that loops, leaves the table or is shorter than the file, a bad cluster.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "quest/obb/byte_source.h"
#include "quest/obb/status.h"

namespace nevr_quest::obb {

struct FatEntry {
  std::string name;  // UTF-8, never "." or "..", never contains '/' or '\\'
  bool is_directory = false;
  std::uint32_t first_cluster = 0;
  std::uint32_t size = 0;
};

class Fat16Volume {
 public:
  using Sink = std::function<Status(const std::uint8_t* bytes, std::size_t length)>;
  using Visitor = std::function<Status(const std::string& path, const FatEntry& entry)>;

  // Reads and checks the boot sector and loads the FAT. `image` must outlive the volume.
  static Status Open(const ByteSource& image, std::unique_ptr<Fat16Volume>* out);

  std::uint32_t cluster_count() const { return cluster_count_; }
  std::uint32_t cluster_bytes() const { return sectors_per_cluster_ * 512u; }

  // The entries of a directory (nullptr: the root), in directory order, with long names resolved.
  Status ReadDirectory(const FatEntry* directory, std::vector<FatEntry>* out) const;
  // Streams the file's bytes (exactly `size` of them) to `sink` in runs of at most 1 MiB.
  Status ReadFile(const FatEntry& file, const Sink& sink) const;
  // Visits every entry below the root, depth first, a directory before its contents. `path` is relative, '/'-separated.
  Status Walk(const Visitor& visitor) const;

 private:
  Fat16Volume() = default;
  Status NextCluster(std::uint32_t cluster, std::uint32_t* next, bool* end) const;
  Status ChainRuns(std::uint32_t first, std::uint64_t clusters_needed, bool until_end,
                   std::vector<std::pair<std::uint32_t, std::uint32_t>>* runs) const;
  Status ReadDirectoryBytes(const FatEntry* directory, std::vector<std::uint8_t>* bytes) const;
  Status WalkDirectory(const FatEntry* directory, const std::string& prefix, int depth, const Visitor& visitor) const;
  std::uint64_t ClusterOffset(std::uint32_t cluster) const;

  const ByteSource* image_ = nullptr;
  std::uint32_t sectors_per_cluster_ = 0;
  std::uint32_t cluster_count_ = 0;
  std::uint32_t root_entries_ = 0;
  std::uint64_t root_offset_ = 0;
  std::uint64_t data_offset_ = 0;
  std::vector<std::uint16_t> fat_;
};

}  // namespace nevr_quest::obb
