// Builders for the store-OBB host tests (src/quest/obb): a FAT16 image made here byte by byte, and an OBB made from it with
// nettle's own Twofish and PBKDF2 (an oracle independent of the code under test). Nothing here reads the game.
#pragma once

#include <nettle/cbc.h>
#include <nettle/pbkdf2.h>
#include <nettle/twofish.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace obb_test {

inline void Put16(std::vector<std::uint8_t>& b, std::size_t at, std::uint32_t v) {
  b[at] = static_cast<std::uint8_t>(v);
  b[at + 1] = static_cast<std::uint8_t>(v >> 8);
}
inline void Put32(std::vector<std::uint8_t>& b, std::size_t at, std::uint32_t v) {
  Put16(b, at, v & 0xFFFFu);
  Put16(b, at + 2, v >> 16);
}
inline std::uint32_t Get16(const std::vector<std::uint8_t>& b, std::size_t at) {
  return static_cast<std::uint32_t>(b[at]) | (static_cast<std::uint32_t>(b[at + 1]) << 8);
}

// ── FAT16 ───────────────────────────────────────────────────────────────────────────────────────────────────────────────
struct Node {
  enum class Naming { kLong, kShortUpper, kShortLower };
  std::string name;
  bool dir = false;
  std::vector<std::uint8_t> data;
  std::vector<Node> kids;
  Naming naming = Naming::kLong;
};

inline Node File(const std::string& name, std::vector<std::uint8_t> data, Node::Naming naming = Node::Naming::kLong) {
  Node n;
  n.name = name;
  n.data = std::move(data);
  n.naming = naming;
  return n;
}
inline Node Dir(const std::string& name, std::vector<Node> kids, Node::Naming naming = Node::Naming::kLong) {
  Node n;
  n.name = name;
  n.dir = true;
  n.kids = std::move(kids);
  n.naming = naming;
  return n;
}

struct FatSpec {
  std::uint32_t clusters = 4200;  // data clusters: 4085..65524 is FAT16
  std::uint32_t sectors_per_cluster = 1;
  bool fragmented = false;        // hand out clusters in a scattered order, so chains are not contiguous runs
  std::vector<Node> root;
};

struct FatLayout {
  std::uint32_t reserved = 1, fats = 2, fat_sectors = 0, root_entries = 512, root_sectors = 32;
  std::uint32_t clusters = 0, sectors_per_cluster = 1, total_sectors = 0;
  std::size_t fat_offset = 0, root_offset = 0, data_offset = 0;
  std::size_t FatEntryOffset(std::uint32_t cluster) const { return fat_offset + cluster * 2u; }
  std::size_t ClusterOffset(std::uint32_t cluster) const { return data_offset + static_cast<std::size_t>(cluster - 2) * sectors_per_cluster * 512u; }
};

struct BuiltImage {
  std::vector<std::uint8_t> bytes;
  FatLayout layout;
  std::map<std::string, std::uint32_t> first_cluster;  // '/'-joined path -> first cluster (0: empty file)
  std::map<std::string, std::size_t> entry_offset;     // path -> byte offset of the 8.3 entry
};

inline std::uint8_t LfnChecksum(const std::uint8_t* name11) {
  std::uint8_t sum = 0;
  for (int i = 0; i < 11; ++i) sum = static_cast<std::uint8_t>(((sum & 1) ? 0x80 : 0) + (sum >> 1) + name11[i]);
  return sum;
}

inline std::vector<std::uint16_t> ToUtf16(const std::string& s) {
  std::vector<std::uint16_t> out;
  for (std::size_t i = 0; i < s.size();) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    if (c < 0x80) {
      out.push_back(c);
      i += 1;
    } else if ((c & 0xE0) == 0xC0) {
      out.push_back(static_cast<std::uint16_t>(((c & 0x1F) << 6) | (static_cast<unsigned char>(s[i + 1]) & 0x3F)));
      i += 2;
    } else {
      out.push_back(static_cast<std::uint16_t>(((c & 0x0F) << 12) | ((static_cast<unsigned char>(s[i + 1]) & 0x3F) << 6) |
                                               (static_cast<unsigned char>(s[i + 2]) & 0x3F)));
      i += 3;
    }
  }
  return out;
}

class FatBuilder {
 public:
  explicit FatBuilder(const FatSpec& spec) : spec_(spec) {
    L_.clusters = spec.clusters;
    L_.sectors_per_cluster = spec.sectors_per_cluster;
    const std::uint32_t entries = spec.clusters + 2;
    L_.fat_sectors = (entries * 2 + 511) / 512;
    L_.total_sectors = L_.reserved + L_.fats * L_.fat_sectors + L_.root_sectors + spec.clusters * spec.sectors_per_cluster;
    L_.fat_offset = static_cast<std::size_t>(L_.reserved) * 512;
    L_.root_offset = L_.fat_offset + static_cast<std::size_t>(L_.fats) * L_.fat_sectors * 512;
    L_.data_offset = L_.root_offset + static_cast<std::size_t>(L_.root_sectors) * 512;
    img_.assign(static_cast<std::size_t>(L_.total_sectors) * 512, 0);
    img_[0] = 0xEB;
    img_[1] = 0x3C;
    img_[2] = 0x90;
    std::memcpy(&img_[3], "fat32lib", 8);
    Put16(img_, 11, 512);
    img_[13] = static_cast<std::uint8_t>(spec.sectors_per_cluster);
    Put16(img_, 14, L_.reserved);
    img_[16] = static_cast<std::uint8_t>(L_.fats);
    Put16(img_, 17, L_.root_entries);
    if (L_.total_sectors < 65536) Put16(img_, 19, L_.total_sectors);
    img_[21] = 0xF8;
    Put16(img_, 22, L_.fat_sectors);
    Put32(img_, 32, L_.total_sectors);
    std::memcpy(&img_[54], "FAT16   ", 8);
    img_[510] = 0x55;
    img_[511] = 0xAA;
    SetFat(0, 0xFFF8);
    SetFat(1, 0xFFFF);
  }

  BuiltImage Build() {
    std::size_t root_cursor = L_.root_offset;
    WriteDirectory(spec_.root, "", /*dir_cluster=*/0, /*parent=*/0, &root_cursor, L_.root_offset + static_cast<std::size_t>(L_.root_sectors) * 512);
    // The second FAT is a copy of the first.
    std::memcpy(&img_[L_.fat_offset + static_cast<std::size_t>(L_.fat_sectors) * 512], &img_[L_.fat_offset],
                static_cast<std::size_t>(L_.fat_sectors) * 512);
    BuiltImage out;
    out.bytes = img_;
    out.layout = L_;
    out.first_cluster = first_;
    out.entry_offset = entry_at_;
    return out;
  }

 private:
  void SetFat(std::uint32_t cluster, std::uint32_t value) { Put16(img_, L_.FatEntryOffset(cluster), value); }

  std::uint32_t AllocateCluster() {
    for (;;) {
      const std::uint32_t slot = spec_.fragmented ? static_cast<std::uint32_t>((static_cast<std::uint64_t>(next_) * 37u) % spec_.clusters) : next_;
      ++next_;
      const std::uint32_t cluster = 2 + slot;
      if (!used_[cluster]) {
        used_[cluster] = true;
        return cluster;
      }
    }
  }

  // Allocates a chain of `count` clusters and links it; returns the first (0 for an empty chain).
  std::uint32_t Chain(std::uint32_t count, std::vector<std::uint32_t>* clusters) {
    std::uint32_t first = 0, prev = 0;
    for (std::uint32_t i = 0; i < count; ++i) {
      const std::uint32_t c = AllocateCluster();
      if (!first) first = c;
      if (prev) SetFat(prev, c);
      prev = c;
      clusters->push_back(c);
    }
    if (prev) SetFat(prev, 0xFFFF);
    return first;
  }

  void WriteData(const std::vector<std::uint32_t>& clusters, const std::vector<std::uint8_t>& data) {
    const std::size_t cb = static_cast<std::size_t>(spec_.sectors_per_cluster) * 512;
    for (std::size_t i = 0; i < clusters.size(); ++i) {
      const std::size_t take = std::min(cb, data.size() - i * cb);
      std::memcpy(&img_[L_.ClusterOffset(clusters[i])], data.data() + i * cb, take);
    }
  }

  static std::string ShortAlias(const Node& n, int serial) {
    // 11 bytes, space padded: a name that is already 8.3 keeps its letters (upper-cased); a long one is NAME~N.EXT.
    std::string base = n.name, ext;
    const std::size_t dot = n.name.rfind('.');
    if (dot != std::string::npos) {
      base = n.name.substr(0, dot);
      ext = n.name.substr(dot + 1);
    }
    for (char& c : base) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    for (char& c : ext) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    if (n.naming == Node::Naming::kLong) {
      std::string clean;
      for (char c : base) {
        if (static_cast<unsigned char>(c) < 0x80 && c != ' ') clean.push_back(c);
      }
      clean = clean.substr(0, 6) + "~" + std::to_string(serial);
      base = clean;
      ext = ext.substr(0, 3);
    }
    std::string s = base;
    s.resize(8, ' ');
    std::string e = ext;
    e.resize(3, ' ');
    return s + e;
  }

  void WriteEntries(std::size_t* cursor, std::size_t limit, const Node& n, std::uint32_t cluster, const std::string& path) {
    const std::string alias = ShortAlias(n, ++serial_);
    std::size_t needed = 1;
    std::vector<std::uint16_t> u16;
    if (n.naming == Node::Naming::kLong) {
      u16 = ToUtf16(n.name);
      needed += (u16.size() + 12) / 13;
    }
    if (*cursor + needed * 32 > limit) std::abort();  // the test's own spec outgrew a directory
    if (n.naming == Node::Naming::kLong) {
      const std::uint8_t sum = LfnChecksum(reinterpret_cast<const std::uint8_t*>(alias.data()));
      const std::size_t parts = (u16.size() + 12) / 13;
      std::vector<std::uint16_t> padded = u16;
      if (padded.size() % 13 != 0) padded.push_back(0);
      padded.resize(parts * 13, 0xFFFF);
      for (std::size_t p = parts; p >= 1; --p) {
        std::uint8_t* e = &img_[*cursor];
        e[0] = static_cast<std::uint8_t>(p | (p == parts ? 0x40 : 0));
        e[11] = 0x0F;
        e[13] = sum;
        const std::uint16_t* chunk = &padded[(p - 1) * 13];
        static const int offs[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
        for (int i = 0; i < 13; ++i) {
          e[offs[i]] = static_cast<std::uint8_t>(chunk[i]);
          e[offs[i] + 1] = static_cast<std::uint8_t>(chunk[i] >> 8);
        }
        *cursor += 32;
      }
    }
    std::uint8_t* e = &img_[*cursor];
    std::memcpy(e, alias.data(), 11);
    e[11] = n.dir ? 0x10 : 0x20;
    if (n.naming == Node::Naming::kShortLower) e[12] = 0x18;  // NT: lower-case base and extension
    Put16(img_, *cursor + 26, cluster);
    Put32(img_, *cursor + 28, n.dir ? 0 : static_cast<std::uint32_t>(n.data.size()));
    entry_at_[path] = *cursor;
    first_[path] = cluster;
    *cursor += 32;
  }

  void WriteDirectory(const std::vector<Node>& kids, const std::string& prefix, std::uint32_t dir_cluster, std::uint32_t parent,
                      std::size_t* cursor, std::size_t limit) {
    (void)dir_cluster;
    (void)parent;
    // Directories other than the root live in clusters: build their bytes first, then store them.
    for (const Node& n : kids) {
      const std::string path = prefix.empty() ? n.name : prefix + "/" + n.name;
      std::vector<std::uint32_t> clusters;
      if (n.dir) {
        // A directory's own entries need its cluster number (for "." and "..") before they are written.
        const std::uint32_t first = AllocateCluster();
        clusters.push_back(first);
        SetFat(first, 0xFFFF);
        WriteEntries(cursor, limit, n, first, path);
        WriteSubdirectory(n, path, first, dir_cluster);
      } else {
        const std::size_t cb = static_cast<std::size_t>(spec_.sectors_per_cluster) * 512;
        const std::uint32_t count = static_cast<std::uint32_t>((n.data.size() + cb - 1) / cb);
        const std::uint32_t first = Chain(count, &clusters);
        WriteData(clusters, n.data);
        WriteEntries(cursor, limit, n, first, path);
      }
    }
  }

  void WriteSubdirectory(const Node& n, const std::string& path, std::uint32_t first, std::uint32_t parent) {
    // One cluster of entries is enough for the tests' directories; the cluster is already allocated and terminated.
    const std::size_t base = L_.ClusterOffset(first);
    const std::size_t limit = base + static_cast<std::size_t>(spec_.sectors_per_cluster) * 512;
    std::size_t cursor = base;
    std::memcpy(&img_[cursor], ".          ", 11);
    img_[cursor + 11] = 0x10;
    Put16(img_, cursor + 26, first);
    cursor += 32;
    std::memcpy(&img_[cursor], "..         ", 11);
    img_[cursor + 11] = 0x10;
    Put16(img_, cursor + 26, parent);
    cursor += 32;
    WriteDirectory(n.kids, path, first, first, &cursor, limit);
  }

  FatSpec spec_;
  FatLayout L_;
  std::vector<std::uint8_t> img_;
  std::map<std::uint32_t, bool> used_;
  std::map<std::string, std::uint32_t> first_;
  std::map<std::string, std::size_t> entry_at_;
  std::uint32_t next_ = 0;
  int serial_ = 0;
};

inline BuiltImage BuildFat16(const FatSpec& spec) { return FatBuilder(spec).Build(); }

// ── OBB ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────
struct ObbSpec {
  std::string key_string = "37c70a1635a1ad7a";
  std::uint8_t salt[8] = {0xdf, 0xc9, 0x0a, 0x5f, 0x1b, 0xbe, 0x68, 0x35};
  std::uint32_t flags = 2;
  std::uint32_t footer_version = 1;
  std::string package_name = "com.readyatdawn.r15";
  std::uint32_t tag = 0x01059983;
};

inline void AppendLe32(std::vector<std::uint8_t>& b, std::uint32_t v) {
  for (int i = 0; i < 4; ++i) b.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
}

// Android's key: PBKDF2-HMAC-SHA1 (nettle's, called here directly), 1024 rounds, 16 bytes.
inline std::vector<std::uint8_t> ReferenceKey(const std::string& key_string, const std::uint8_t* salt) {
  std::vector<std::uint8_t> key(16);
  nettle_pbkdf2_hmac_sha1(key_string.size(), reinterpret_cast<const std::uint8_t*>(key_string.data()), 1024, 8, salt, 16, key.data());
  return key;
}

// dm-crypt twofish-cbc-plain, as nettle's encrypt side does it: one CBC chain per 512-byte sector, IV = sector number LE.
inline std::vector<std::uint8_t> EncryptImage(const std::vector<std::uint8_t>& plain, const std::vector<std::uint8_t>& key) {
  twofish_ctx ctx;
  nettle_twofish_set_key(&ctx, key.size(), key.data());
  std::vector<std::uint8_t> out(plain.size());
  for (std::size_t s = 0; s * 512 < plain.size(); ++s) {
    std::uint8_t iv[16] = {};
    iv[0] = static_cast<std::uint8_t>(s);
    iv[1] = static_cast<std::uint8_t>(s >> 8);
    iv[2] = static_cast<std::uint8_t>(s >> 16);
    iv[3] = static_cast<std::uint8_t>(s >> 24);
    nettle_cbc_encrypt(&ctx, reinterpret_cast<nettle_cipher_func*>(nettle_twofish_encrypt), 16, iv, 512, &out[s * 512], &plain[s * 512]);
  }
  return out;
}

// image bytes (encrypted with the spec's key) followed by the footer.
inline std::vector<std::uint8_t> BuildObb(const std::vector<std::uint8_t>& plain_image, const ObbSpec& spec = ObbSpec()) {
  std::vector<std::uint8_t> obb = EncryptImage(plain_image, ReferenceKey(spec.key_string, spec.salt));
  std::vector<std::uint8_t> footer;
  AppendLe32(footer, spec.footer_version);
  AppendLe32(footer, 1);  // package version
  AppendLe32(footer, spec.flags);
  if (spec.flags & 2) footer.insert(footer.end(), spec.salt, spec.salt + 8);
  AppendLe32(footer, static_cast<std::uint32_t>(spec.package_name.size()));
  footer.insert(footer.end(), spec.package_name.begin(), spec.package_name.end());
  obb.insert(obb.end(), footer.begin(), footer.end());
  AppendLe32(obb, static_cast<std::uint32_t>(footer.size()));
  AppendLe32(obb, spec.tag);
  return obb;
}

// A deterministic, non-repeating byte pattern, so a wrong offset or sector shows.
inline std::vector<std::uint8_t> Pattern(std::size_t n, std::uint32_t seed) {
  std::vector<std::uint8_t> v(n);
  std::uint32_t x = seed * 2654435761u + 1;
  for (std::size_t i = 0; i < n; ++i) {
    x = x * 1664525u + 1013904223u;
    v[i] = static_cast<std::uint8_t>(x >> 24);
  }
  return v;
}

}  // namespace obb_test
