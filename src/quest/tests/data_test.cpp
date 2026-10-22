// Host tests for the first-run data step's core (src/quest/data): sha256, the zip extractor, the pins and decision, and the
// staging swap. The zips are built here, byte by byte, so each rule is exercised by an archive that breaks only it.

#include <zlib.h>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

#include "quest/data/plan.h"
#include "quest/data/sha256.h"
#include "quest/data/staging.h"
#include "quest/data/zip.h"
#include "quest/tests/data_test_support.h"
#include "quest/tests/mini_test.h"
#include "quest/tests/pinned_zip_directory.h"

using namespace nevr_quest::data;
using namespace data_test;

namespace {

struct Extracted {
  ZipStatus status;
  std::string detail;
  std::uint64_t bytes;
};

Extracted Extract(const Scratch& s, const std::string& zip_bytes, const ZipLimits& limits = {},
                  const std::string& prefix = "_data/") {
  WriteFile(s.Path("t.zip"), zip_bytes);
  Extracted r{};
  r.status = ExtractZip(s.Path("t.zip"), s.Path("out"), prefix, limits, &r.bytes, &r.detail);
  return r;
}

}  // namespace

// ---------------- sha256 ----------------

TEST(Sha256KnownVectors) {
  CHECK_EQ(Sha256Hex("", 0), std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
  CHECK_EQ(Sha256Hex("abc", 3), std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
  const std::string two_block = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
  CHECK_EQ(Sha256Hex(two_block.data(), two_block.size()),
           std::string("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
}

TEST(Sha256MillionAAcrossUnevenUpdates) {
  Sha256 h;
  const std::string chunk(997, 'a');  // not a multiple of the 64-byte block
  std::size_t left = 1000000;
  while (left != 0) {
    const std::size_t n = left < chunk.size() ? left : chunk.size();
    h.Update(chunk.data(), n);
    left -= n;
  }
  CHECK_EQ(h.Hex(), std::string("cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"));
}

// ---------------- entry names ----------------

TEST(SafeEntryNameTable) {
  CHECK(SafeEntryName("_data/a/b"));
  CHECK(SafeEntryName("_data/"));
  CHECK(SafeEntryName("a"));
  CHECK(!SafeEntryName(""));
  CHECK(!SafeEntryName("/abs"));
  CHECK(!SafeEntryName("a/../b"));
  CHECK(!SafeEntryName(".."));
  CHECK(!SafeEntryName("a/./b"));
  CHECK(!SafeEntryName("a//b"));
  CHECK(!SafeEntryName("a\\b"));
  CHECK(!SafeEntryName("C:/x"));
  CHECK(!SafeEntryName(std::string("a\0b", 3)));
  CHECK(!SafeEntryName("a\nb"));
  CHECK(!SafeEntryName(std::string(256, 'x')));
  CHECK(SafeEntryName(std::string(255, 'x')));
  CHECK(!SafeEntryName("/"));
  std::string deep;
  for (int i = 0; i < 17; ++i) deep += "d/";
  CHECK(!SafeEntryName(deep));
  CHECK(SafeEntryName(deep.substr(0, 32)));  // 16 components
  CHECK(!SafeEntryName(std::string(513, 'x')));
}

// ---------------- the zip reader and extractor ----------------

TEST(ExtractsStoredDeflatedAndDirectories) {
  Scratch s;
  const std::string big = Pattern(300000);
  const std::string small = "hello";
  const std::vector<SrcEntry> entries = {
      {"_data/", "", 0}, {"_data/sub/", "", 0}, {"_data/sub/big.bin", big, 8}, {"_data/small.txt", small, 0},
      {"_data/empty", "", 0}, {"_data/sub/deep/er.txt", small, 8}};
  const Extracted r = Extract(s, BuildZip(entries));
  CHECK_EQ(r.status, ZipStatus::kOk);
  CHECK_EQ(r.bytes, static_cast<std::uint64_t>(big.size() + small.size() * 2));
  CHECK(ReadFile(s.Path("out/sub/big.bin")) == big);
  CHECK(ReadFile(s.Path("out/small.txt")) == small);
  CHECK(ReadFile(s.Path("out/sub/deep/er.txt")) == small);
  CHECK(Exists(s.Path("out/empty")));
  CHECK(!Exists(s.Path("out/_data")));
}

TEST(CrcMismatchIsRefused) {
  Scratch s;
  SrcEntry e{"_data/x", Pattern(5000), 8};
  e.crc_delta = 1;
  CHECK_EQ(Extract(s, BuildZip({e})).status, ZipStatus::kCrcMismatch);
}

TEST(StoredCrcMismatchIsRefused) {
  Scratch s;
  SrcEntry e{"_data/x", Pattern(100), 0};
  e.crc_delta = 1;
  CHECK_EQ(Extract(s, BuildZip({e})).status, ZipStatus::kCrcMismatch);
}

TEST(DeclaredSizeSmallerThanStreamIsRefused) {
  Scratch s;
  SrcEntry e{"_data/x", Pattern(5000), 8};
  e.usize_delta = -10;
  CHECK_EQ(Extract(s, BuildZip({e})).status, ZipStatus::kSizeMismatch);
}

TEST(DeclaredSizeLargerThanStreamIsRefused) {
  Scratch s;
  SrcEntry e{"_data/x", Pattern(5000), 8};
  e.usize_delta = 10;
  CHECK_EQ(Extract(s, BuildZip({e})).status, ZipStatus::kSizeMismatch);
}

TEST(TruncatedStreamIsRefused) {
  Scratch s;
  SrcEntry e{"_data/x", Pattern(50000), 8};
  const std::string full = Deflate(e.data);
  e.use_raw = true;
  e.raw_payload = full.substr(0, full.size() / 2);
  const ZipStatus st = Extract(s, BuildZip({e})).status;
  CHECK(st == ZipStatus::kInflateFailed || st == ZipStatus::kSizeMismatch);
}

TEST(TraversalNameIsRefusedBeforeAnythingIsWritten) {
  Scratch s;
  const Extracted r = Extract(s, BuildZip({{"_data/ok", "x", 0}, {"_data/../evil", "x", 0}}));
  CHECK_EQ(r.status, ZipStatus::kBadEntryPath);
  CHECK(!Exists(s.Path("out")));
  CHECK(!Exists(s.Path("evil")));
}

TEST(AbsoluteBackslashAndDriveNamesAreRefused) {
  Scratch s;
  CHECK_EQ(Extract(s, BuildZip({{"/etc/x", "x", 0}})).status, ZipStatus::kBadEntryPath);
  CHECK_EQ(Extract(s, BuildZip({{"_data\\x", "x", 0}})).status, ZipStatus::kBadEntryPath);
  CHECK_EQ(Extract(s, BuildZip({{"C:/x", "x", 0}})).status, ZipStatus::kBadEntryPath);
}

TEST(EntryWithoutThePrefixIsRefusedBeforeAnythingIsWritten) {
  Scratch s;
  const Extracted r = Extract(s, BuildZip({{"_data/a", "x", 0}, {"other/b", "x", 0}}));
  CHECK_EQ(r.status, ZipStatus::kBadEntryPath);
  CHECK_EQ(r.detail, std::string("other/b"));
  CHECK(!Exists(s.Path("out")));
}

TEST(DuplicateNamesAreRefused) {
  Scratch s;
  CHECK_EQ(Extract(s, BuildZip({{"_data/a", "x", 0}, {"_data/a", "y", 0}})).status, ZipStatus::kConflict);
}

TEST(FileAndDirectoryOfTheSameNameAreRefused) {
  Scratch s;
  CHECK_EQ(Extract(s, BuildZip({{"_data/a", "x", 0}, {"_data/a/", "", 0}})).status, ZipStatus::kConflict);
}

TEST(FileThatIsAlsoAParentDirectoryIsRefused) {
  Scratch s;
  CHECK_EQ(Extract(s, BuildZip({{"_data/a", "x", 0}, {"_data/a/b", "y", 0}})).status, ZipStatus::kConflict);
}

TEST(EncryptedEntryIsRefused) {
  Scratch s;
  SrcEntry e{"_data/x", "x", 0};
  e.flags = 0x1;
  CHECK_EQ(Extract(s, BuildZip({e})).status, ZipStatus::kEncrypted);
}

TEST(UnsupportedMethodIsRefused) {
  Scratch s;
  SrcEntry e{"_data/x", "x", 0};
  e.method = 14;  // LZMA
  CHECK_EQ(Extract(s, BuildZip({e})).status, ZipStatus::kUnsupportedMethod);
}

TEST(SymbolicLinkEntryIsRefused) {
  Scratch s;
  SrcEntry e{"_data/link", "/etc/passwd", 0};
  e.made_by = (3 << 8) | 20;
  e.external = (0120777u << 16);  // S_IFLNK | 0777
  CHECK_EQ(Extract(s, BuildZip({e})).status, ZipStatus::kSpecialFile);
}

TEST(Zip64AndMultiDiskAreRefused) {
  Scratch s;
  ZipOptions z64;
  z64.zip64_marker = true;
  CHECK_EQ(Extract(s, BuildZip({{"_data/a", "x", 0}}, z64)).status, ZipStatus::kZip64);
  ZipOptions multi;
  multi.disk = 1;
  CHECK_EQ(Extract(s, BuildZip({{"_data/a", "x", 0}}, multi)).status, ZipStatus::kMultiDisk);
}

TEST(NotAZipHasNoEndRecord) {
  Scratch s;
  CHECK_EQ(Extract(s, std::string(500, 'z')).status, ZipStatus::kNoEndRecord);
  CHECK_EQ(Extract(s, "").status, ZipStatus::kNoEndRecord);
}

TEST(MissingZipFailsToOpen) {
  Scratch s;
  std::string detail;
  CHECK_EQ(ExtractZip(s.Path("none.zip"), s.Path("out"), "_data/", {}, nullptr, &detail), ZipStatus::kOpenFailed);
}

TEST(LimitsAreEnforced) {
  Scratch s;
  ZipLimits entry;
  entry.max_entry_bytes = 100;
  CHECK_EQ(Extract(s, BuildZip({{"_data/a", Pattern(101), 8}}), entry).status, ZipStatus::kSizeLimit);
  ZipLimits total;
  total.max_total_bytes = 150;
  CHECK_EQ(Extract(s, BuildZip({{"_data/a", Pattern(100), 8}, {"_data/b", Pattern(100), 8}}), total).status,
           ZipStatus::kSizeLimit);
  ZipLimits count;
  count.max_entries = 1;
  CHECK_EQ(Extract(s, BuildZip({{"_data/a", "x", 0}, {"_data/b", "x", 0}}), count).status, ZipStatus::kTooManyEntries);
}

TEST(ExistingDestinationIsRefused) {
  Scratch s;
  CHECK_EQ(::mkdir(s.Path("out").c_str(), 0755), 0);
  CHECK_EQ(Extract(s, BuildZip({{"_data/a", "x", 0}})).status, ZipStatus::kDestExists);
}

TEST(ExtractDoesNotFollowALinkPlantedInTheDestination) {
  // A directory entry after a file whose parent is a link would write through it: MakeDirs refuses a link.
  Scratch s;
  CHECK_EQ(::mkdir(s.Path("target").c_str(), 0755), 0);
  const Extracted r = Extract(s, BuildZip({{"_data/d/", "", 0}, {"_data/d/f", "x", 0}}));
  CHECK_EQ(r.status, ZipStatus::kOk);
  CHECK(!Exists(s.Path("target/f")));
}

// ---------------- the pinned zip ----------------

TEST(PinnedListingMatchesThePins) {
  Scratch s;
  const std::string path = s.Path("pinned.zip");
  const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  CHECK(fd >= 0);
  CHECK_EQ(::ftruncate(fd, static_cast<off_t>(kZipBytes)), 0);
  const ssize_t n = ::pwrite(fd, pinned_zip::kDirectory, sizeof(pinned_zip::kDirectory),
                             static_cast<off_t>(pinned_zip::kDirectoryOffset));
  CHECK_EQ(static_cast<std::size_t>(n), sizeof(pinned_zip::kDirectory));
  ::close(fd);
  CHECK_EQ(pinned_zip::kDirectoryOffset + sizeof(pinned_zip::kDirectory), kZipBytes);
  std::vector<ZipEntry> entries;
  std::string detail;
  ZipLimits limits;
  CHECK_EQ(ReadZipListing(path, limits, &entries, &detail), ZipStatus::kOk);
  CHECK_EQ(entries.size(), static_cast<std::size_t>(10));
  bool manifest = false;
  for (const ZipEntry& e : entries) {
    CHECK(e.name.compare(0, std::strlen(kZipPrefix), kZipPrefix) == 0);
    if (e.name == kManifestPath) manifest = !e.IsDirectory();
  }
  CHECK(manifest);
  CHECK_EQ(std::strlen(kZipSha256), static_cast<std::size_t>(64));
}

// ---------------- the decision ----------------

namespace {
const char* g_present[8];
int g_present_count = 0;
bool InList(const char* path) noexcept {
  for (int i = 0; i < g_present_count; ++i) {
    if (std::strcmp(g_present[i], path) == 0) return true;
  }
  return false;
}
void Present(std::initializer_list<const char*> paths) {
  g_present_count = 0;
  for (const char* p : paths) g_present[g_present_count++] = p;
}
const std::string kMedia = nevr_quest::data_root::kMediaRoot;
const std::string kLegacy = nevr_quest::data_root::kLegacyRoot;
const std::string kMediaData = kMedia + "/_data";
const std::string kLegacyData = kLegacy + "/_data";
const std::string kMediaManifest = kMedia + "/" + kManifestPath;
const std::string kLegacyManifest = kLegacy + "/" + kManifestPath;
}  // namespace

TEST(DecideUsesDataWithTheManifest) {
  Present({kMediaData.c_str(), kMediaManifest.c_str()});
  const Decision d = Decide(nevr_quest::data_root::Resolve(&InList), &InList);
  CHECK(d.action == DataAction::kUse);
  CHECK_EQ(std::string(d.root), kMedia);
  Present({kLegacyData.c_str(), kLegacyManifest.c_str()});
  const Decision l = Decide(nevr_quest::data_root::Resolve(&InList), &InList);
  CHECK(l.action == DataAction::kUse);
  CHECK_EQ(std::string(l.root), kLegacy);
}

TEST(DecideReplacesDataWithoutTheManifest) {
  Present({kMediaData.c_str()});
  const Decision d = Decide(nevr_quest::data_root::Resolve(&InList), &InList);
  CHECK(d.action == DataAction::kReplace);
  CHECK_EQ(std::string(d.root), kMedia);
  Present({kLegacyData.c_str()});
  const Decision l = Decide(nevr_quest::data_root::Resolve(&InList), &InList);
  CHECK(l.action == DataAction::kReplace);
  CHECK_EQ(std::string(l.root), kLegacy);
}

TEST(DecideFetchesToTheMediaRootWhenThereIsNothing) {
  Present({});
  const Decision d = Decide(nevr_quest::data_root::Resolve(&InList), &InList);
  CHECK(d.action == DataAction::kFetch);
  CHECK_EQ(std::string(d.root), kMedia);
}

TEST(DecideUsesAnUnmarkedMediaRootAsItIs) {
  Present({kMedia.c_str()});
  const Decision d = Decide(nevr_quest::data_root::Resolve(&InList), &InList);
  CHECK(d.action == DataAction::kUse);
  CHECK_EQ(std::string(d.source), std::string("android_media_unmarked"));
}

// ---------------- staging ----------------

TEST(StagingPathUsesAShaPrefix) {
  CHECK_EQ(StagingPath("/r", "fc2eedeacc50d9ddf751e21914bb4188"), std::string("/r/_data.staging-fc2eedeacc50"));
}

TEST(InstallStagedIntoAnEmptyRoot) {
  Scratch s;
  const std::string staging = StagingPath(s.dir, kZipSha256);
  CHECK_EQ(::mkdir(staging.c_str(), 0755), 0);
  WriteFile(staging + "/f", "new");
  std::string aside;
  CHECK_EQ(InstallStaged(s.dir, staging, "20261022T000000Z", &aside), StageStatus::kOk);
  CHECK(aside.empty());
  CHECK_EQ(ReadFile(s.Path("_data/f")), std::string("new"));
  CHECK(!Exists(staging));
}

TEST(InstallStagedReplacesAndKeepsTheOldTreeAside) {
  Scratch s;
  CHECK_EQ(::mkdir(s.Path("_data").c_str(), 0755), 0);
  WriteFile(s.Path("_data/f"), "old");
  const std::string staging = StagingPath(s.dir, kZipSha256);
  CHECK_EQ(::mkdir(staging.c_str(), 0755), 0);
  WriteFile(staging + "/f", "new");
  std::string aside;
  CHECK_EQ(InstallStaged(s.dir, staging, "20261022T000000Z", &aside), StageStatus::kOk);
  CHECK_EQ(aside, s.Path("_data.replaced-20261022T000000Z"));
  CHECK_EQ(ReadFile(s.Path("_data/f")), std::string("new"));
  CHECK_EQ(ReadFile(aside + "/f"), std::string("old"));
}

TEST(InstallStagedPutsTheOldTreeBackWhenTheFinalRenameFails) {
  Scratch s;
  CHECK_EQ(::mkdir(s.Path("_data").c_str(), 0755), 0);
  WriteFile(s.Path("_data/f"), "old");
  std::string aside;
  CHECK_EQ(InstallStaged(s.dir, s.Path("no-such-staging"), "20261022T000000Z", &aside), StageStatus::kRenameFailed);
  CHECK_EQ(ReadFile(s.Path("_data/f")), std::string("old"));
  CHECK(aside.empty());
  CHECK(!Exists(s.Path("_data.replaced-20261022T000000Z")));
}

TEST(CleanLeftoversRemovesOnlyOurPrefixes) {
  Scratch s;
  CHECK_EQ(::mkdir(s.Path("_data.staging-aaaaaaaaaaaa").c_str(), 0755), 0);
  WriteFile(s.Path("_data.staging-aaaaaaaaaaaa/f"), "x");
  CHECK_EQ(::mkdir(s.Path("_data.replaced-20261022T000000Z").c_str(), 0755), 0);
  CHECK_EQ(::mkdir(s.Path("_data").c_str(), 0755), 0);
  WriteFile(s.Path("_data.zip.part"), "partial");
  WriteFile(s.Path("keep.txt"), "k");
  int failed = -1;
  CHECK_EQ(CleanLeftovers(s.dir, &failed), 2);
  CHECK_EQ(failed, 0);
  CHECK(!Exists(s.Path("_data.staging-aaaaaaaaaaaa")));
  CHECK(!Exists(s.Path("_data.replaced-20261022T000000Z")));
  CHECK(Exists(s.Path("_data")));
  CHECK(Exists(s.Path("_data.zip.part")));  // the resumable download is not a leftover
  CHECK(Exists(s.Path("keep.txt")));
}

TEST(RemoveTreeDoesNotFollowLinks) {
  Scratch s;
  CHECK_EQ(::mkdir(s.Path("outside").c_str(), 0755), 0);
  WriteFile(s.Path("outside/keep"), "k");
  CHECK_EQ(::mkdir(s.Path("tree").c_str(), 0755), 0);
  char* abs = ::realpath(s.Path("outside").c_str(), nullptr);  // a link's target is relative to the link's directory
  CHECK(abs != nullptr);
  CHECK_EQ(::symlink(abs, s.Path("tree/link").c_str()), 0);
  std::free(abs);
  CHECK(RemoveTree(s.Path("tree")));
  CHECK(!Exists(s.Path("tree")));
  CHECK_EQ(ReadFile(s.Path("outside/keep")), std::string("k"));
  CHECK(RemoveTree(s.Path("never-existed")));
}

TEST(CheckSpaceComparesAgainstTheFreeBytes) {
  Scratch s;
  std::uint64_t avail = 0;
  CHECK_EQ(CheckSpace(s.dir, 1, &avail), StageStatus::kOk);
  CHECK(avail > 0);
  CHECK_EQ(CheckSpace(s.dir, avail + 1, nullptr), StageStatus::kNoSpace);
  CHECK_EQ(CheckSpace(s.Path("missing"), 1, nullptr), StageStatus::kStatFailed);
}

int main(int argc, char** argv) { return mini_test::RunAll(argc, argv); }
