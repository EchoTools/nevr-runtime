// Host tests for the FAT16 reader (src/quest/obb/fat16.cpp, docs/adr/0042) over images built byte by byte in
// obb_test_support.h: directories, long names, short names with the NT case flags, chains that are scattered, clusters of one
// and of four sectors, and every refusal (FAT12, FAT32, the sector size, the boot sector, the chains, the names).

#include <map>
#include <string>

#include "quest/obb/fat16.h"
#include "quest/tests/mini_test.h"
#include "quest/tests/obb_test_support.h"

using namespace nevr_quest::obb;
using namespace obb_test;

namespace {

FatSpec Sample(bool fragmented = false, std::uint32_t spc = 1) {
  FatSpec spec;
  spec.fragmented = fragmented;
  spec.sectors_per_cluster = spc;
  spec.root.push_back(File("README.TXT", Pattern(100, 1), Node::Naming::kShortUpper));
  spec.root.push_back(File("notes.txt", Pattern(10, 2), Node::Naming::kShortLower));
  spec.root.push_back(Dir("_data", {Dir("5932408047", {Dir("rad15", {Dir("android", {
      Dir("manifests", {File("48037dc70b0ecab2", Pattern(3000, 3))}),
      Dir("packages", {File("48037dc70b0ecab2_0", Pattern(20000, 4)), File("empty_file_with_a_long_name.bin", {})}),
  })})})}));
  spec.root.push_back(File("caf\xC3\xA9 menu.txt", Pattern(513, 5)));
  return spec;
}

struct Opened {
  BuiltImage built;
  std::unique_ptr<MemorySource> source;
  std::unique_ptr<Fat16Volume> volume;
  Status status;
};

Opened Open(const FatSpec& spec, void (*damage)(BuiltImage&) = nullptr) {
  Opened o;
  o.built = BuildFat16(spec);
  if (damage) damage(o.built);
  o.source = std::make_unique<MemorySource>(o.built.bytes);
  o.status = Fat16Volume::Open(*o.source, &o.volume);
  return o;
}

std::map<std::string, FatEntry> WalkAll(const Fat16Volume& v, Status* status) {
  std::map<std::string, FatEntry> out;
  *status = v.Walk([&](const std::string& path, const FatEntry& e) {
    out[path] = e;
    return Status::Ok();
  });
  return out;
}

std::vector<std::uint8_t> Read(const Fat16Volume& v, const FatEntry& e, Status* status) {
  std::vector<std::uint8_t> got;
  *status = v.ReadFile(e, [&](const std::uint8_t* b, std::size_t n) {
    got.insert(got.end(), b, b + n);
    return Status::Ok();
  });
  return got;
}

}  // namespace

TEST(OpensAFat16Volume) {
  Opened o = Open(Sample());
  CHECK(o.status.ok);
  CHECK_EQ(o.volume->cluster_count(), 4200u);
  CHECK_EQ(o.volume->cluster_bytes(), 512u);
}

TEST(WalkNamesEveryEntryWithItsFullPath) {
  Opened o = Open(Sample());
  CHECK(o.status.ok);
  Status s;
  const auto all = WalkAll(*o.volume, &s);
  CHECK(s.ok);
  CHECK(all.count("README.TXT") == 1);
  CHECK(all.count("notes.txt") == 1);  // NT flag: lower-case base and extension of a short name
  CHECK(all.count("café menu.txt") == 1);
  CHECK(all.count("_data/5932408047/rad15/android/manifests/48037dc70b0ecab2") == 1);
  CHECK(all.count("_data/5932408047/rad15/android/packages/48037dc70b0ecab2_0") == 1);
  CHECK(all.count("_data/5932408047/rad15/android/packages/empty_file_with_a_long_name.bin") == 1);
  CHECK(all.at("_data").is_directory);
  CHECK(!all.at("README.TXT").is_directory);
  CHECK_EQ(all.at("_data/5932408047/rad15/android/packages/48037dc70b0ecab2_0").size, 20000u);
  CHECK_EQ(all.size(), 12u);  // no "." or ".." entry is reported
}

TEST(FilesReadBackByteForByte) {
  for (bool fragmented : {false, true}) {
    for (std::uint32_t spc : {1u, 4u}) {
      Opened o = Open(Sample(fragmented, spc));
      CHECK(o.status.ok);
      Status s;
      const auto all = WalkAll(*o.volume, &s);
      CHECK(s.ok);
      const std::string base = "_data/5932408047/rad15/android/";
      Status r;
      CHECK(Read(*o.volume, all.at(base + "packages/48037dc70b0ecab2_0"), &r) == Pattern(20000, 4));
      CHECK(r.ok);
      CHECK(Read(*o.volume, all.at(base + "manifests/48037dc70b0ecab2"), &r) == Pattern(3000, 3));
      CHECK(Read(*o.volume, all.at("café menu.txt"), &r) == Pattern(513, 5));  // one byte over a cluster of 512
      CHECK(Read(*o.volume, all.at("notes.txt"), &r) == Pattern(10, 2));
      CHECK(Read(*o.volume, all.at(base + "packages/empty_file_with_a_long_name.bin"), &r).empty());
      CHECK(r.ok);
    }
  }
}

TEST(AScatteredChainIsNotAContiguousRun) {
  Opened o = Open(Sample(/*fragmented=*/true));
  const std::uint32_t first = o.built.first_cluster.at("_data/5932408047/rad15/android/packages/48037dc70b0ecab2_0");
  const std::uint32_t next = static_cast<std::uint32_t>(Get16(o.built.bytes, o.built.layout.FatEntryOffset(first)));
  CHECK(next != first + 1);  // the builder's scatter really breaks the run the reader would otherwise coalesce
}

TEST(AWholeNumberOfClustersReadsToTheEnd) {
  FatSpec spec;
  spec.root.push_back(File("exact.bin", Pattern(1024, 8)));
  Opened o = Open(spec);
  Status s;
  const auto all = WalkAll(*o.volume, &s);
  CHECK(Read(*o.volume, all.at("exact.bin"), &s) == Pattern(1024, 8));
  CHECK(s.ok);
}

TEST(Fat12AndFat32AreRefused) {
  FatSpec small;
  small.clusters = 4000;  // below 4085 clusters: FAT12
  CHECK_EQ(Open(small).status.code, std::string("fat_unsupported_fat12"));
  FatSpec big;
  big.clusters = 65600;  // 65525 or more: FAT32
  CHECK_EQ(Open(big).status.code, std::string("fat_unsupported_fat32"));
  // A boot sector in the FAT32 layout: no fixed root directory and no 16-bit FAT size.
  Opened o = Open(Sample(), [](BuiltImage& b) {
    Put16(b.bytes, 17, 0);
    Put16(b.bytes, 22, 0);
  });
  CHECK_EQ(o.status.code, std::string("fat_unsupported_fat32"));
}

TEST(BootSectorFaultsAreRefused) {
  CHECK_EQ(Open(Sample(), [](BuiltImage& b) { b.bytes[510] = 0; }).status.code, std::string("fat_bad_boot_sector"));
  CHECK_EQ(Open(Sample(), [](BuiltImage& b) { b.bytes[511] = 0; }).status.code, std::string("fat_bad_boot_sector"));
  CHECK_EQ(Open(Sample(), [](BuiltImage& b) { Put16(b.bytes, 11, 1024); }).status.code, std::string("fat_unsupported_sector_size"));
  CHECK_EQ(Open(Sample(), [](BuiltImage& b) { b.bytes[13] = 3; }).status.code, std::string("fat_bad_boot_sector"));
  CHECK_EQ(Open(Sample(), [](BuiltImage& b) { b.bytes[13] = 0; }).status.code, std::string("fat_bad_boot_sector"));
  CHECK_EQ(Open(Sample(), [](BuiltImage& b) { b.bytes[16] = 0; }).status.code, std::string("fat_bad_boot_sector"));
  CHECK_EQ(Open(Sample(), [](BuiltImage& b) { Put16(b.bytes, 14, 0); }).status.code, std::string("fat_bad_boot_sector"));
  // The volume claims more sectors than the image holds.
  CHECK_EQ(Open(Sample(), [](BuiltImage& b) { Put16(b.bytes, 19, 60000); }).status.code, std::string("fat_bad_boot_sector"));
  // A FAT too small for the clusters the volume claims.
  CHECK_EQ(Open(Sample(), [](BuiltImage& b) { Put16(b.bytes, 22, 2); }).status.code, std::string("fat_bad_boot_sector"));
  // An image shorter than a boot sector.
  MemorySource tiny(std::vector<std::uint8_t>(100, 0));
  std::unique_ptr<Fat16Volume> v;
  CHECK_EQ(Fat16Volume::Open(tiny, &v).code, std::string("source_short_read"));
}

namespace {
Status ReadByPath(const std::string& path, void (*damage)(BuiltImage&), std::vector<std::uint8_t>* got = nullptr) {
  Opened o = Open(Sample(), damage);
  CHECK(o.status.ok);
  Status w;
  const auto all = WalkAll(*o.volume, &w);
  const FatEntry& e = all.at(path);
  Status r;
  auto bytes = Read(*o.volume, e, &r);
  if (got) *got = bytes;
  return r;
}
const char kBig[] = "_data/5932408047/rad15/android/packages/48037dc70b0ecab2_0";
}  // namespace

TEST(ALoopedChainIsRefused) {
  // The file's second cluster points back at its first.
  const Status s = ReadByPath(kBig, [](BuiltImage& b) {
    const std::uint32_t first = b.first_cluster.at(kBig);
    Put16(b.bytes, b.layout.FatEntryOffset(first), first);
  });
  CHECK_EQ(s.code, std::string("fat_chain_loop"));
}

TEST(AChainThatLeavesTheTableIsRefused) {
  CHECK_EQ(ReadByPath(kBig, [](BuiltImage& b) {
             Put16(b.bytes, b.layout.FatEntryOffset(b.first_cluster.at(kBig)), 5000);  // above the last cluster (4201)
           }).code,
           std::string("fat_chain_out_of_range"));
  CHECK_EQ(ReadByPath(kBig, [](BuiltImage& b) { Put16(b.bytes, b.layout.FatEntryOffset(b.first_cluster.at(kBig)), 1); }).code,
           std::string("fat_chain_out_of_range"));
  CHECK_EQ(ReadByPath(kBig, [](BuiltImage& b) { Put16(b.bytes, b.layout.FatEntryOffset(b.first_cluster.at(kBig)), 0xFFF0); }).code,
           std::string("fat_chain_out_of_range"));
}

TEST(ABadOrFreeClusterInAChainIsRefused) {
  CHECK_EQ(ReadByPath(kBig, [](BuiltImage& b) { Put16(b.bytes, b.layout.FatEntryOffset(b.first_cluster.at(kBig)), 0xFFF7); }).code,
           std::string("fat_bad_cluster"));
  CHECK_EQ(ReadByPath(kBig, [](BuiltImage& b) { Put16(b.bytes, b.layout.FatEntryOffset(b.first_cluster.at(kBig)), 0); }).code,
           std::string("fat_free_cluster_in_chain"));
}

TEST(AChainShorterThanTheFileIsRefused) {
  // End the chain after its first cluster: 512 bytes of a 20000-byte file.
  CHECK_EQ(ReadByPath(kBig, [](BuiltImage& b) { Put16(b.bytes, b.layout.FatEntryOffset(b.first_cluster.at(kBig)), 0xFFFF); }).code,
           std::string("fat_chain_too_short"));
}

TEST(AFileThatStartsAtNoClusterIsRefused) {
  // A non-empty file whose first cluster is 0.
  CHECK_EQ(ReadByPath(kBig, [](BuiltImage& b) { Put16(b.bytes, b.entry_offset.at(kBig) + 26, 0); }).code,
           std::string("fat_chain_out_of_range"));
}

TEST(ALoopedDirectoryDoesNotHangTheWalk) {
  Opened o = Open(Sample(), [](BuiltImage& b) {
    const std::uint32_t d = b.first_cluster.at("_data");
    Put16(b.bytes, b.layout.FatEntryOffset(d), d);  // the directory's chain continues into itself
  });
  CHECK(o.status.ok);
  Status s;
  WalkAll(*o.volume, &s);
  CHECK(!s.ok);
  CHECK_EQ(s.code, std::string("fat_chain_loop"));
}

TEST(ADirectoryThatContainsItselfStopsAtTheDepthLimit) {
  Opened o = Open(Sample(), [](BuiltImage& b) {
    // "_data/." becomes an entry named "x" that points back at "_data": a cycle through the tree.
    const std::size_t dir_cluster_offset = b.layout.ClusterOffset(b.first_cluster.at("_data"));
    b.bytes[dir_cluster_offset] = 'X';
    for (int i = 1; i < 11; ++i) b.bytes[dir_cluster_offset + i] = ' ';
  });
  CHECK(o.status.ok);
  Status s;
  WalkAll(*o.volume, &s);
  CHECK(!s.ok);
  CHECK_EQ(s.code, std::string("fat_directory_too_deep"));
}

TEST(UnsafeNamesAreRefused) {
  for (const char* bad : {"a/b", "..", ".", "a\\b"}) {
    FatSpec spec;
    spec.root.push_back(File(bad, Pattern(4, 1)));
    Opened o = Open(spec);
    CHECK(o.status.ok);
    Status s;
    WalkAll(*o.volume, &s);
    CHECK(!s.ok);
    CHECK_EQ(s.code, std::string("fat_unsafe_name"));
  }
}

TEST(ALongNameWithAWrongChecksumFallsBackToTheShortName) {
  FatSpec spec;
  spec.root.push_back(File("a_rather_long_name.txt", Pattern(4, 1)));
  Opened o = Open(spec, [](BuiltImage& b) {
    // The checksum byte of the long-name entry that sits just before the 8.3 entry.
    b.bytes[b.entry_offset.at("a_rather_long_name.txt") - 32 + 13] ^= 0xFF;
  });
  CHECK(o.status.ok);
  Status s;
  const auto all = WalkAll(*o.volume, &s);
  CHECK(s.ok);
  CHECK_EQ(all.size(), 1u);
  CHECK(all.count("A_RATH~1.TXT") == 1);
}

TEST(DeletedLabelAndTrailingEntriesAreSkipped) {
  FatSpec spec;
  spec.root.push_back(File("one.txt", Pattern(4, 1)));
  spec.root.push_back(File("two.txt", Pattern(4, 2)));
  spec.root.push_back(File("three.txt", Pattern(4, 3)));
  Opened o = Open(spec, [](BuiltImage& b) {
    b.bytes[b.entry_offset.at("two.txt") - 32] = 0xE5;                 // its long-name entry is deleted
    b.bytes[b.entry_offset.at("two.txt")] = 0xE5;                      // and so is the 8.3 entry
    b.bytes[b.entry_offset.at("one.txt") + 11] = 0x08;                 // one.txt becomes a volume label
    // An entry after the first end marker is never read: three.txt's long-name entry becomes the marker.
    b.bytes[b.entry_offset.at("three.txt") - 32] = 0x00;
  });
  CHECK(o.status.ok);
  Status s;
  const auto all = WalkAll(*o.volume, &s);
  CHECK(s.ok);
  CHECK_EQ(all.size(), 0u);
}

int main(int argc, char** argv) { return mini_test::RunAll(argc, argv); }
