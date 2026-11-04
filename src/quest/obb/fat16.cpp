#include "quest/obb/fat16.h"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace nevr_quest::obb {

namespace {

constexpr std::size_t kSector = 512;
constexpr std::size_t kEntryBytes = 32;
constexpr std::size_t kRunBytes = 1u << 20;
constexpr int kMaxDepth = 16;
// Microsoft's FAT specification: fewer than 4085 data clusters is FAT12, fewer than 65525 is FAT16.
constexpr std::uint32_t kFat12Limit = 4085;
constexpr std::uint32_t kFat16Limit = 65525;
constexpr std::uint32_t kFirstReservedMark = 0xFFF0;
constexpr std::uint32_t kBadClusterMark = 0xFFF7;
constexpr std::uint32_t kEndOfChain = 0xFFF8;
constexpr std::uint8_t kAttrDirectory = 0x10;
constexpr std::uint8_t kAttrVolumeLabel = 0x08;
constexpr std::uint8_t kAttrLongName = 0x0F;
constexpr std::uint8_t kLastLongEntry = 0x40;
constexpr std::size_t kMaxLongParts = 20;

std::uint32_t Le16(const std::uint8_t* p) { return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8); }
std::uint32_t Le32(const std::uint8_t* p) { return Le16(p) | (Le16(p + 2) << 16); }

Status BadBoot(const std::string& why) { return Status::Fail("fat_bad_boot_sector", why); }

std::uint8_t ShortNameChecksum(const std::uint8_t* name11) {
  std::uint8_t sum = 0;
  for (int i = 0; i < 11; ++i) sum = static_cast<std::uint8_t>(((sum & 1) ? 0x80 : 0) + (sum >> 1) + name11[i]);
  return sum;
}

// UTF-16 to UTF-8; an unpaired surrogate is refused.
bool AppendUtf8(std::string* out, const std::u16string& in) {
  for (std::size_t i = 0; i < in.size(); ++i) {
    std::uint32_t c = in[i];
    if (c >= 0xD800 && c <= 0xDBFF) {
      if (i + 1 >= in.size() || in[i + 1] < 0xDC00 || in[i + 1] > 0xDFFF) return false;
      c = 0x10000 + ((c - 0xD800) << 10) + (in[i + 1] - 0xDC00u);
      ++i;
    } else if (c >= 0xDC00 && c <= 0xDFFF) {
      return false;
    }
    if (c < 0x80) {
      out->push_back(static_cast<char>(c));
    } else if (c < 0x800) {
      out->push_back(static_cast<char>(0xC0 | (c >> 6)));
      out->push_back(static_cast<char>(0x80 | (c & 0x3F)));
    } else if (c < 0x10000) {
      out->push_back(static_cast<char>(0xE0 | (c >> 12)));
      out->push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
      out->push_back(static_cast<char>(0x80 | (c & 0x3F)));
    } else {
      out->push_back(static_cast<char>(0xF0 | (c >> 18)));
      out->push_back(static_cast<char>(0x80 | ((c >> 12) & 0x3F)));
      out->push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
      out->push_back(static_cast<char>(0x80 | (c & 0x3F)));
    }
  }
  return true;
}

// A name that is written to disk by the extractor: one path component, nothing that climbs or splits a path.
bool SafeName(const std::string& name) {
  if (name.empty() || name == "." || name == "..") return false;
  return name.find_first_of(std::string("/\\\0", 3)) == std::string::npos;
}

struct LongName {
  bool active = false;
  std::uint8_t expected_ordinal = 0;  // the ordinal the next long-name entry must carry, plus one
  std::uint8_t checksum = 0;
  std::size_t parts = 0;
  std::u16string chunks[kMaxLongParts + 1];
  void Reset() { active = false; }
};

}  // namespace

Status Fat16Volume::Open(const ByteSource& image, std::unique_ptr<Fat16Volume>* out) {
  std::uint8_t boot[kSector];
  if (Status s = image.ReadAt(0, boot, sizeof boot); !s.ok) return s;
  if (boot[510] != 0x55 || boot[511] != 0xAA) {
    return BadBoot("the first sector has no 0x55AA boot signature (a wrong key leaves this)");
  }
  const std::uint32_t bytes_per_sector = Le16(boot + 11);
  if (bytes_per_sector != kSector) {
    return Status::Fail("fat_unsupported_sector_size", "the volume's sector is " + std::to_string(bytes_per_sector) + " bytes; only 512 is read");
  }
  const std::uint32_t spc = boot[13];
  if (spc == 0 || (spc & (spc - 1)) != 0 || spc > 128) return BadBoot("sectors per cluster is " + std::to_string(spc) + ", not a power of two from 1 to 128");
  const std::uint32_t reserved = Le16(boot + 14);
  if (reserved == 0) return BadBoot("no reserved sectors: the boot sector itself is one");
  const std::uint32_t fats = boot[16];
  if (fats == 0) return BadBoot("the volume has no FAT");
  const std::uint32_t root_entries = Le16(boot + 17);
  const std::uint32_t fat_sectors = Le16(boot + 22);
  if (root_entries == 0 || fat_sectors == 0) {
    return Status::Fail("fat_unsupported_fat32", "no fixed root directory or no 16-bit FAT size in the boot sector: a FAT32 volume");
  }
  const std::uint64_t total_sectors = Le16(boot + 19) != 0 ? Le16(boot + 19) : Le32(boot + 32);
  if (total_sectors == 0) return BadBoot("the volume has no sectors");
  if (total_sectors * kSector > image.Size()) {
    return BadBoot("the volume claims " + std::to_string(total_sectors) + " sectors; the image holds " + std::to_string(image.Size() / kSector));
  }
  const std::uint64_t root_sectors = (static_cast<std::uint64_t>(root_entries) * kEntryBytes + kSector - 1) / kSector;
  const std::uint64_t metadata_sectors = reserved + static_cast<std::uint64_t>(fats) * fat_sectors + root_sectors;
  if (metadata_sectors >= total_sectors) return BadBoot("the reserved sectors, FATs and root directory fill the volume");
  const std::uint64_t clusters = (total_sectors - metadata_sectors) / spc;
  if (clusters < kFat12Limit) {
    return Status::Fail("fat_unsupported_fat12", "the volume has " + std::to_string(clusters) + " data clusters; fewer than 4085 is FAT12");
  }
  if (clusters >= kFat16Limit) {
    return Status::Fail("fat_unsupported_fat32", "the volume has " + std::to_string(clusters) + " data clusters; 65525 or more is FAT32");
  }
  if (static_cast<std::uint64_t>(fat_sectors) * (kSector / 2) < clusters + 2) {
    return BadBoot("the FAT (" + std::to_string(fat_sectors) + " sectors) is too small for " + std::to_string(clusters) + " clusters");
  }

  std::unique_ptr<Fat16Volume> v(new Fat16Volume());
  v->image_ = &image;
  v->sectors_per_cluster_ = spc;
  v->cluster_count_ = static_cast<std::uint32_t>(clusters);
  v->root_entries_ = root_entries;
  v->root_offset_ = (reserved + static_cast<std::uint64_t>(fats) * fat_sectors) * kSector;
  v->data_offset_ = v->root_offset_ + root_sectors * kSector;
  std::vector<std::uint8_t> raw(static_cast<std::size_t>(v->cluster_count_ + 2) * 2);
  if (Status s = image.ReadAt(static_cast<std::uint64_t>(reserved) * kSector, raw.data(), raw.size()); !s.ok) return s;
  v->fat_.resize(v->cluster_count_ + 2);
  for (std::size_t i = 0; i < v->fat_.size(); ++i) v->fat_[i] = static_cast<std::uint16_t>(Le16(&raw[i * 2]));
  *out = std::move(v);
  return Status::Ok();
}

std::uint64_t Fat16Volume::ClusterOffset(std::uint32_t cluster) const {
  return data_offset_ + static_cast<std::uint64_t>(cluster - 2) * sectors_per_cluster_ * kSector;
}

Status Fat16Volume::NextCluster(std::uint32_t cluster, std::uint32_t* next, bool* end) const {
  if (cluster < 2 || cluster > cluster_count_ + 1) {
    return Status::Fail("fat_chain_out_of_range", "cluster " + std::to_string(cluster) + " is outside 2.." + std::to_string(cluster_count_ + 1));
  }
  const std::uint32_t v = fat_[cluster];
  *end = false;
  if (v == 0) return Status::Fail("fat_free_cluster_in_chain", "cluster " + std::to_string(cluster) + " links to a free cluster");
  if (v == kBadClusterMark) return Status::Fail("fat_bad_cluster", "cluster " + std::to_string(cluster) + " links to a cluster marked bad");
  if (v >= kEndOfChain) {
    *end = true;
    return Status::Ok();
  }
  if (v >= kFirstReservedMark || v < 2 || v > cluster_count_ + 1) {
    return Status::Fail("fat_chain_out_of_range", "cluster " + std::to_string(cluster) + " links to " + std::to_string(v) + ", which is not a data cluster");
  }
  *next = v;
  return Status::Ok();
}

Status Fat16Volume::ChainRuns(std::uint32_t first, std::uint64_t clusters_needed, bool until_end,
                              std::vector<std::pair<std::uint32_t, std::uint32_t>>* runs) const {
  runs->clear();
  if (!until_end && clusters_needed == 0) return Status::Ok();
  std::vector<bool> seen(cluster_count_ + 2, false);
  std::uint32_t cluster = first;
  std::uint64_t collected = 0;
  for (;;) {
    if (cluster < 2 || cluster > cluster_count_ + 1) {
      return Status::Fail("fat_chain_out_of_range", "cluster " + std::to_string(cluster) + " is outside 2.." + std::to_string(cluster_count_ + 1));
    }
    if (seen[cluster]) return Status::Fail("fat_chain_loop", "the chain returns to cluster " + std::to_string(cluster));
    seen[cluster] = true;
    if (!runs->empty() && runs->back().first + runs->back().second == cluster) {
      ++runs->back().second;
    } else {
      runs->emplace_back(cluster, 1u);
    }
    ++collected;
    if (!until_end && collected == clusters_needed) return Status::Ok();
    std::uint32_t next = 0;
    bool end = false;
    if (Status s = NextCluster(cluster, &next, &end); !s.ok) return s;
    if (end) {
      if (until_end) return Status::Ok();
      return Status::Fail("fat_chain_too_short", "the chain ends after " + std::to_string(collected) + " clusters; the file needs " + std::to_string(clusters_needed));
    }
    cluster = next;
  }
}

Status Fat16Volume::ReadDirectoryBytes(const FatEntry* directory, std::vector<std::uint8_t>* bytes) const {
  if (directory == nullptr) {
    bytes->resize(static_cast<std::size_t>(root_entries_) * kEntryBytes);
    return image_->ReadAt(root_offset_, bytes->data(), bytes->size());
  }
  std::vector<std::pair<std::uint32_t, std::uint32_t>> runs;
  if (Status s = ChainRuns(directory->first_cluster, 0, /*until_end=*/true, &runs); !s.ok) return s;
  bytes->clear();
  for (const auto& run : runs) {
    const std::size_t at = bytes->size();
    bytes->resize(at + static_cast<std::size_t>(run.second) * cluster_bytes());
    if (Status s = image_->ReadAt(ClusterOffset(run.first), bytes->data() + at, static_cast<std::size_t>(run.second) * cluster_bytes()); !s.ok) return s;
  }
  return Status::Ok();
}

Status Fat16Volume::ReadDirectory(const FatEntry* directory, std::vector<FatEntry>* out) const {
  std::vector<std::uint8_t> bytes;
  if (Status s = ReadDirectoryBytes(directory, &bytes); !s.ok) return s;
  out->clear();
  LongName lfn;
  for (std::size_t at = 0; at + kEntryBytes <= bytes.size(); at += kEntryBytes) {
    const std::uint8_t* e = &bytes[at];
    if (e[0] == 0x00) break;  // the end of the directory: nothing after it is read
    if (e[0] == 0xE5) {
      lfn.Reset();
      continue;
    }
    const std::uint8_t attr = e[11];
    if ((attr & 0x3F) == kAttrLongName) {
      const std::uint8_t ordinal = e[0] & 0x1F;
      if (e[0] & kLastLongEntry) {
        if (ordinal < 1 || ordinal > kMaxLongParts) {
          lfn.Reset();
          continue;
        }
        lfn.active = true;
        lfn.parts = ordinal;
        lfn.checksum = e[13];
      } else if (!lfn.active || ordinal + 1 != lfn.expected_ordinal || e[13] != lfn.checksum) {
        lfn.Reset();
        continue;
      }
      static const int kOffsets[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
      std::u16string chunk;
      for (int offset : kOffsets) chunk.push_back(static_cast<char16_t>(Le16(e + offset)));
      lfn.chunks[ordinal] = chunk;
      lfn.expected_ordinal = ordinal;
      continue;
    }
    if (attr & kAttrVolumeLabel) {
      lfn.Reset();
      continue;
    }

    // The 8.3 entry. A long name counts only when its entries run N..1 and their checksum is this entry's.
    FatEntry entry;
    const bool long_name = lfn.active && lfn.expected_ordinal == 1 && lfn.checksum == ShortNameChecksum(e);
    if (long_name) {
      std::u16string joined;
      for (std::size_t part = 1; part <= lfn.parts; ++part) joined += lfn.chunks[part];
      const std::size_t stop = joined.find(u'\0');
      if (stop != std::u16string::npos) joined.resize(stop);
      if (!AppendUtf8(&entry.name, joined)) {
        return Status::Fail("fat_bad_long_name", "a long name holds an unpaired UTF-16 surrogate");
      }
    } else {
      std::string base(reinterpret_cast<const char*>(e), 8), ext(reinterpret_cast<const char*>(e) + 8, 3);
      if (static_cast<std::uint8_t>(base[0]) == 0x05) base[0] = static_cast<char>(0xE5);
      while (!base.empty() && base.back() == ' ') base.pop_back();
      while (!ext.empty() && ext.back() == ' ') ext.pop_back();
      for (char c : base + ext) {
        if (static_cast<unsigned char>(c) >= 0x80) return Status::Fail("fat_bad_short_name", "an 8.3 name with a byte above 0x7F and no long name");
      }
      if (e[12] & 0x08) std::transform(base.begin(), base.end(), base.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      if (e[12] & 0x10) std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      entry.name = ext.empty() ? base : base + "." + ext;
      if (entry.name == "." || entry.name == "..") {
        lfn.Reset();
        continue;  // the directory's own and its parent's entries
      }
    }
    lfn.Reset();
    if (!SafeName(entry.name)) {
      return Status::Fail("fat_unsafe_name", "an entry is named '" + entry.name + "', which is not one path component");
    }
    entry.is_directory = (attr & kAttrDirectory) != 0;
    entry.first_cluster = Le16(e + 26);
    entry.size = entry.is_directory ? 0 : Le32(e + 28);
    out->push_back(std::move(entry));
  }
  return Status::Ok();
}

Status Fat16Volume::ReadFile(const FatEntry& file, const Sink& sink) const {
  if (file.size == 0) return Status::Ok();
  const std::uint64_t needed = (static_cast<std::uint64_t>(file.size) + cluster_bytes() - 1) / cluster_bytes();
  std::vector<std::pair<std::uint32_t, std::uint32_t>> runs;
  if (Status s = ChainRuns(file.first_cluster, needed, /*until_end=*/false, &runs); !s.ok) {
    s.detail = "'" + file.name + "': " + s.detail;
    return s;
  }
  std::vector<std::uint8_t> buffer(kRunBytes);
  std::uint64_t remaining = file.size;
  for (const auto& run : runs) {
    std::uint64_t offset = ClusterOffset(run.first);
    std::uint64_t run_bytes = static_cast<std::uint64_t>(run.second) * cluster_bytes();
    while (run_bytes > 0 && remaining > 0) {
      const std::size_t piece = static_cast<std::size_t>(std::min<std::uint64_t>(std::min<std::uint64_t>(run_bytes, kRunBytes), remaining));
      if (Status s = image_->ReadAt(offset, buffer.data(), piece); !s.ok) return s;
      if (Status s = sink(buffer.data(), piece); !s.ok) return s;
      offset += piece;
      run_bytes -= piece;
      remaining -= piece;
    }
  }
  return Status::Ok();
}

Status Fat16Volume::WalkDirectory(const FatEntry* directory, const std::string& prefix, int depth, const Visitor& visitor) const {
  if (depth > kMaxDepth) {
    return Status::Fail("fat_directory_too_deep", "directories nest deeper than " + std::to_string(kMaxDepth) + " below '" + prefix + "'");
  }
  std::vector<FatEntry> entries;
  if (Status s = ReadDirectory(directory, &entries); !s.ok) {
    if (!prefix.empty()) s.detail = "directory '" + prefix + "': " + s.detail;
    return s;
  }
  for (const FatEntry& entry : entries) {
    const std::string path = prefix.empty() ? entry.name : prefix + "/" + entry.name;
    if (Status s = visitor(path, entry); !s.ok) return s;
    if (entry.is_directory) {
      if (Status s = WalkDirectory(&entry, path, depth + 1, visitor); !s.ok) return s;
    }
  }
  return Status::Ok();
}

Status Fat16Volume::Walk(const Visitor& visitor) const { return WalkDirectory(nullptr, "", 0, visitor); }

}  // namespace nevr_quest::obb
