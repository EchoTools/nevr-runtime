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

std::uint64_t FileSize(const std::string& path) {
  struct stat st;
  return ::stat(path.c_str(), &st) == 0 ? static_cast<std::uint64_t>(st.st_size) : ~std::uint64_t{0};
}

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

TEST(MakeDirsBelowNeverFollowsALink) {
  // A link planted where a directory component is expected: the call fails and nothing is made through the link.
  Scratch s;
  CHECK_EQ(::mkdir(s.Path("base").c_str(), 0755), 0);
  CHECK_EQ(::mkdir(s.Path("target").c_str(), 0755), 0);
  char* abs = ::realpath(s.Path("target").c_str(), nullptr);
  CHECK(abs != nullptr);
  CHECK_EQ(::symlink(abs, s.Path("base/d").c_str()), 0);
  std::free(abs);
  CHECK(!MakeDirsBelow(s.Path("base"), "d"));
  CHECK(!MakeDirsBelow(s.Path("base"), "d/sub"));
  CHECK(!Exists(s.Path("target/sub")));
  // A file where a directory is expected fails too, and a real directory path is made.
  WriteFile(s.Path("base/f"), "x");
  CHECK(!MakeDirsBelow(s.Path("base"), "f/sub"));
  CHECK(MakeDirsBelow(s.Path("base"), "a/b/c"));
  CHECK(Exists(s.Path("base/a/b/c")));
}

TEST(AnEntryThatExpandsPastItsSizeIsStoppedBeforeItIsWritten) {
  // 3 MiB of zeros declared as 1000 bytes: the first inflate chunk already exceeds the record, so no byte is written.
  Scratch s;
  const Extracted r = Extract(s, BuildZip({{"_data/bomb", std::string(3u << 20, '\0'), 8}}));
  CHECK_EQ(r.status, ZipStatus::kOk);  // control: an honest size extracts
  Scratch t;
  std::vector<SrcEntry> lie = {{"_data/bomb", std::string(3u << 20, '\0'), 8}};
  lie[0].usize_delta = 1000 - static_cast<std::int64_t>(3u << 20);
  const Extracted bomb = Extract(t, BuildZip(lie));
  CHECK_EQ(bomb.status, ZipStatus::kSizeMismatch);
  CHECK_EQ(FileSize(t.Path("out/bomb")), std::uint64_t{0});
}

TEST(AZip64LocatorIsRefused) {
  Scratch s;
  ZipOptions opt;
  opt.zip64_locator = true;
  CHECK_EQ(Extract(s, BuildZip({{"_data/a", "x", 0}}, opt)).status, ZipStatus::kZip64);
}

TEST(BytesAfterTheEndRecordOrALyingCommentLengthAreRefused) {
  Scratch s;
  ZipOptions trailing;
  trailing.trailing = "1234567";
  CHECK_EQ(Extract(s, BuildZip({{"_data/a", "x", 0}}, trailing)).status, ZipStatus::kNoEndRecord);
  ZipOptions lying;
  lying.comment_len = 3;  // no comment follows
  CHECK_EQ(Extract(s, BuildZip({{"_data/a", "x", 0}}, lying)).status, ZipStatus::kNoEndRecord);
  ZipOptions honest;
  honest.comment_len = 7;
  honest.trailing = "comment";
  CHECK_EQ(Extract(s, BuildZip({{"_data/a", "x", 0}}, honest)).status, ZipStatus::kOk);
}

TEST(ALocalHeaderThatDisagreesWithTheDirectoryIsRefused) {
  const auto status = [](const std::vector<SrcEntry>& entries) {
    Scratch s;  // a failed extraction leaves its directory, so each case gets its own
    return Extract(s, BuildZip(entries)).status;
  };
  std::vector<SrcEntry> method = {{"_data/a", "hello", 0}};
  method[0].local_method = 8;
  CHECK_EQ(status(method), ZipStatus::kBadLocalHeader);
  std::vector<SrcEntry> name = {{"_data/a", "hello", 0}};
  name[0].local_name = "_data/b";
  CHECK_EQ(status(name), ZipStatus::kBadLocalHeader);
  std::vector<SrcEntry> crc = {{"_data/a", "hello", 0}};
  crc[0].local_crc_delta = 1;
  CHECK_EQ(status(crc), ZipStatus::kBadLocalHeader);
  std::vector<SrcEntry> size = {{"_data/a", "hello", 0}};
  size[0].local_usize_delta = 1;
  CHECK_EQ(status(size), ZipStatus::kBadLocalHeader);
  // With a data descriptor (flag bit 3) the local CRC and sizes are not authoritative.
  std::vector<SrcEntry> descriptor = {{"_data/a", "hello", 0}};
  descriptor[0].local_flags = 0x8;
  descriptor[0].local_crc_delta = 1;
  CHECK_EQ(status(descriptor), ZipStatus::kOk);
  // An encryption flag on the local header only.
  std::vector<SrcEntry> encrypted = {{"_data/a", "hello", 0}};
  encrypted[0].local_flags = 0x1;
  CHECK_EQ(status(encrypted), ZipStatus::kBadLocalHeader);
}

TEST(NamesThatDifferOnlyInCaseAreRefused) {
  Scratch s;
  CHECK_EQ(Extract(s, BuildZip({{"_data/a", "x", 0}, {"_data/A", "y", 0}})).status, ZipStatus::kConflict);
  CHECK_EQ(Extract(s, BuildZip({{"_data/A", "x", 0}, {"_data/a/b", "y", 0}})).status, ZipStatus::kConflict);
  CHECK_EQ(Extract(s, BuildZip({{"_data/Dir/", "", 0}, {"_data/dir", "y", 0}})).status, ZipStatus::kConflict);
}

TEST(ControlBytesInAnEntryNameDoNotReachTheDetail) {
  Scratch s;
  const Extracted r = Extract(s, BuildZip({{"_data/a\x1b[31m\nforged", "x", 0}}));
  CHECK_EQ(r.status, ZipStatus::kBadEntryPath);
  for (const char c : r.detail) CHECK(static_cast<unsigned char>(c) >= 0x20 && static_cast<unsigned char>(c) != 0x7f);
  CHECK_EQ(r.detail, std::string("_data/a?[31m?forged"));
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

TEST(DecideFetchesIntoAnUnmarkedMediaRoot) {
  // The media directory exists and holds nothing: an interrupted first fetch must be retried, not used.
  const auto fetches = [](std::initializer_list<const char*> present) {
    Present(present);
    const Decision d = Decide(nevr_quest::data_root::Resolve(&InList), &InList);
    return d.action == DataAction::kFetch && std::string(d.root) == kMedia &&
           std::string(d.source) == "android_media_unmarked";
  };
  const std::string part = kMedia + "/_data.zip.part";
  const std::string staging = kMedia + "/_data.staging-fc2eedeacc50";
  const std::string replaced = kMedia + "/_data.replaced-20261022T000000Z";
  CHECK(fetches({kMedia.c_str()}));
  CHECK(fetches({kMedia.c_str(), part.c_str()}));
  CHECK(fetches({kMedia.c_str(), staging.c_str()}));
  CHECK(fetches({kMedia.c_str(), replaced.c_str()}));
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

TEST(CleanLeftoversRemovesOnlyAMarkedStagingDirectory) {
  Scratch s;
  const std::string staging = s.Path("_data.staging-aaaaaaaaaaaa");
  CHECK(MarkOwned(staging));
  CHECK(RecordOwned(staging, {"f"}));  // the manifest names the one file the item holds
  CHECK_EQ(::mkdir(staging.c_str(), 0755), 0);
  WriteFile(staging + "/f", "x");
  CHECK_EQ(::mkdir(s.Path("_data").c_str(), 0755), 0);
  WriteFile(s.Path("_data/f"), "player");
  WriteFile(s.Path("_data.zip.part"), "partial");
  WriteFile(s.Path("keep.txt"), "k");
  // Names that only look like ours: a player's own.
  CHECK_EQ(::mkdir(s.Path("_data.staging-notes").c_str(), 0755), 0);
  WriteFile(s.Path("_data.staging-notes/n"), "notes");
  CHECK_EQ(::mkdir(s.Path("_data.replaced-my-backup").c_str(), 0755), 0);
  WriteFile(s.Path("_data.replaced-my-backup/b"), "backup");
  int failed = -1;
  CHECK_EQ(CleanLeftovers(s.dir, &failed), 1);
  CHECK_EQ(failed, 0);
  CHECK(!Exists(staging));
  CHECK(!Exists(OwnerMarkerPath(staging)));
  CHECK_EQ(ReadFile(s.Path("_data/f")), std::string("player"));
  CHECK(Exists(s.Path("_data.zip.part")));  // the resumable download is not a leftover
  CHECK(Exists(s.Path("keep.txt")));
  CHECK_EQ(ReadFile(s.Path("_data.staging-notes/n")), std::string("notes"));
  CHECK_EQ(ReadFile(s.Path("_data.replaced-my-backup/b")), std::string("backup"));
}

TEST(CleanLeftoversNeverDeletesAMarkedReplacedDirectory) {
  Scratch s;
  const std::string replaced = s.Path("_data.replaced-20261022T000000Z");
  CHECK(MarkOwned(replaced));
  CHECK_EQ(::mkdir(replaced.c_str(), 0755), 0);
  WriteFile(replaced + "/old", "old");
  CHECK_EQ(::mkdir(s.Path("_data").c_str(), 0755), 0);
  CHECK_EQ(CleanLeftovers(s.dir, nullptr), 0);
  CHECK_EQ(ReadFile(replaced + "/old"), std::string("old"));
  CHECK(IsOwned(replaced));
}

TEST(CleanLeftoversRemovesAnOrphanMarkerOnly) {
  Scratch s;
  const std::string staging = s.Path("_data.staging-bbbbbbbbbbbb");
  CHECK(MarkOwned(staging));  // the crash came before the directory was made
  CHECK_EQ(CleanLeftovers(s.dir, nullptr), 0);
  CHECK(!Exists(OwnerMarkerPath(staging)));
}

TEST(InstallStagedMarksTheKeptAsideTreeAndDropsTheStagingMarker) {
  Scratch s;
  CHECK_EQ(::mkdir(s.Path("_data").c_str(), 0755), 0);
  WriteFile(s.Path("_data/f"), "old");
  const std::string staging = StagingPath(s.dir, kZipSha256);
  CHECK(MarkOwned(staging));
  CHECK_EQ(::mkdir(staging.c_str(), 0755), 0);
  WriteFile(staging + "/f", "new");
  std::string aside;
  CHECK_EQ(InstallStaged(s.dir, staging, "20261022T000000Z", &aside), StageStatus::kOk);
  CHECK(IsOwned(aside));
  CHECK(!Exists(OwnerMarkerPath(staging)));
  CHECK_EQ(CleanLeftovers(s.dir, nullptr), 0);
  CHECK_EQ(ReadFile(aside + "/f"), std::string("old"));
}

TEST(AFailedSwapLeavesNoMarkerBehind) {
  Scratch s;
  CHECK_EQ(::mkdir(s.Path("_data").c_str(), 0755), 0);
  std::string aside;
  CHECK_EQ(InstallStaged(s.dir, s.Path("no-such-staging"), "20261022T000000Z", &aside), StageStatus::kRenameFailed);
  CHECK(!Exists(s.Path("_data.replaced-20261022T000000Z.nevr-owned")));
}

TEST(ACrashBetweenTheTwoRenamesIsRecoveredOnTheNextLaunch) {
  // The old tree is aside and marked, the new one is in staging, `_data` does not exist.
  Scratch s;
  const std::string replaced = s.Path("_data.replaced-20261022T000000Z");
  CHECK(MarkOwned(replaced));
  CHECK_EQ(::mkdir(replaced.c_str(), 0755), 0);
  WriteFile(replaced + "/old", "old");
  const std::string staging = StagingPath(s.dir, kZipSha256);
  CHECK(MarkOwned(staging));
  CHECK(RecordOwned(staging, {"new"}));
  CHECK_EQ(::mkdir(staging.c_str(), 0755), 0);
  WriteFile(staging + "/new", "new");
  std::string from;
  CHECK(RecoverReplaced(s.dir, &from));
  CHECK_EQ(from, replaced);
  CHECK_EQ(ReadFile(s.Path("_data/old")), std::string("old"));
  CHECK(!Exists(replaced));
  CHECK(!Exists(OwnerMarkerPath(replaced)));
  CHECK_EQ(CleanLeftovers(s.dir, nullptr), 1);
  CHECK(!Exists(staging));
  CHECK_EQ(ReadFile(s.Path("_data/old")), std::string("old"));
}

TEST(RecoverReplacedTakesTheNewestMarkedOneAndOnlyWhenDataIsMissing) {
  Scratch s;
  for (const char* stamp : {"20261020T000000Z", "20261022T000000Z"}) {
    const std::string dir = s.Path(std::string("_data.replaced-") + stamp);
    CHECK(MarkOwned(dir));
    CHECK_EQ(::mkdir(dir.c_str(), 0755), 0);
    WriteFile(dir + "/id", stamp);
  }
  CHECK(RecoverReplaced(s.dir, nullptr));
  CHECK_EQ(ReadFile(s.Path("_data/id")), std::string("20261022T000000Z"));
  CHECK(Exists(s.Path("_data.replaced-20261020T000000Z")));
  // `_data` is there now: nothing more is moved.
  CHECK(!RecoverReplaced(s.dir, nullptr));
  CHECK_EQ(ReadFile(s.Path("_data/id")), std::string("20261022T000000Z"));
}

TEST(RecoverReplacedLeavesAPlayersOwnBackupAlone) {
  Scratch s;
  CHECK_EQ(::mkdir(s.Path("_data.replaced-my-backup").c_str(), 0755), 0);
  WriteFile(s.Path("_data.replaced-my-backup/b"), "backup");
  CHECK(!RecoverReplaced(s.dir, nullptr));
  CHECK(!Exists(s.Path("_data")));
  CHECK_EQ(ReadFile(s.Path("_data.replaced-my-backup/b")), std::string("backup"));
}

namespace {

std::string Absolute(const std::string& path) {
  char* abs = ::realpath(path.c_str(), nullptr);
  const std::string out = abs != nullptr ? abs : "";
  std::free(abs);
  return out;
}

std::string Snapshot(const std::string& root, const std::string& aside, const std::string& staging) {
  std::string out;
  out += Exists(root + "/_data") ? "data " : "nodata ";
  out += Exists(aside) ? "aside " : "noaside ";
  out += IsOwned(aside) ? "asidemarked " : "asideunmarked ";
  out += Exists(staging) ? "staging" : "nostaging";
  return out;
}

}  // namespace

// The two states a crash during InstallStaged leaves. The marker is written before anything moves, so a crash after
// the first rename leaves the old tree recognisable as ours, and RecoverReplaced puts it back.
TEST(InstallStagedWritesTheMarkerBeforeItMovesAnythingAndCanBeRecoveredBetweenTheRenames) {
  Scratch s;
  const std::string aside = s.Path("_data.replaced-20261022T000000Z");
  const std::string staging = s.Path("_data.staging-aaaaaaaaaaaa");
  CHECK_EQ(::mkdir(s.Path("_data").c_str(), 0755), 0);
  WriteFile(s.Path("_data/old"), "old");
  CHECK(MarkOwned(staging));
  CHECK_EQ(::mkdir(staging.c_str(), 0755), 0);
  WriteFile(staging + "/new", "new");

  struct Ctx {
    const std::string* root;
    const std::string* aside;
    const std::string* staging;
    std::string first;
    std::string between;
  } ctx{&s.dir, &aside, &staging, {}, {}};
  InstallProbe probe;
  probe.context = &ctx;
  probe.at = [](void* raw, const char* point) {
    auto* c = static_cast<Ctx*>(raw);
    const std::string state = Snapshot(*c->root, *c->aside, *c->staging);
    if (std::strcmp(point, "before_first_rename") == 0) c->first = state;
    if (std::strcmp(point, "between_renames") == 0) c->between = state;
  };
  std::string replaced;
  CHECK_EQ(InstallStaged(s.dir, staging, "20261022T000000Z", &replaced, &probe), StageStatus::kOk);
  // Before anything moved: the old tree is still `_data`, and the aside marker already exists.
  CHECK_EQ(ctx.first, std::string("data noaside asidemarked staging"));
  // Between the renames: no `_data`, the old tree aside and marked, the new one still staged.
  CHECK_EQ(ctx.between, std::string("nodata aside asidemarked staging"));
  CHECK_EQ(ReadFile(s.Path("_data/new")), std::string("new"));
}

TEST(ARecoveryFromTheStateBetweenTheRenamesPutsTheOldTreeBack) {
  Scratch s;
  const std::string aside = s.Path("_data.replaced-20261022T000000Z");
  CHECK(MarkOwned(aside));
  CHECK_EQ(::mkdir(aside.c_str(), 0755), 0);
  WriteFile(aside + "/old", "old");
  CHECK(RecoverReplaced(s.dir, nullptr));
  CHECK_EQ(ReadFile(s.Path("_data/old")), std::string("old"));
  CHECK(!IsOwned(aside));
}

TEST(AFailedFirstRenameLeavesNoNewMarkerAndKeepsAnEarlierOne) {
  Scratch s;
  const std::string aside = s.Path("_data.replaced-20261022T000000Z");
  CHECK_EQ(::mkdir(s.Path("_data").c_str(), 0755), 0);
  WriteFile(s.Path("_data/old"), "old");
  CHECK_EQ(::mkdir(aside.c_str(), 0755), 0);  // a non-empty directory of that name: rename onto it fails
  WriteFile(aside + "/x", "x");
  const std::string staging = s.Path("_data.staging-aaaaaaaaaaaa");
  CHECK_EQ(::mkdir(staging.c_str(), 0755), 0);
  std::string replaced;
  CHECK_EQ(InstallStaged(s.dir, staging, "20261022T000000Z", &replaced), StageStatus::kRenameFailed);
  CHECK(!IsOwned(aside));  // the marker this call made is gone
  CHECK_EQ(ReadFile(s.Path("_data/old")), std::string("old"));
  CHECK_EQ(ReadFile(aside + "/x"), std::string("x"));

  CHECK(MarkOwned(aside));  // an earlier marker of ours stays
  CHECK_EQ(InstallStaged(s.dir, staging, "20261022T000000Z", &replaced), StageStatus::kRenameFailed);
  CHECK(IsOwned(aside));
}

TEST(MarkOwnedNeverTruncatesOrFollowsWhatIsAlreadyAtTheMarkerName) {
  Scratch s;
  const std::string item = s.Path("_data.replaced-20261022T000000Z");
  const std::string marker = OwnerMarkerPath(item);
  WriteFile(marker, "the player's notes");  // a file with content at the exact marker name
  CHECK(!MarkOwned(item));
  CHECK_EQ(ReadFile(marker), std::string("the player's notes"));
  CHECK(!IsOwned(item));
  CHECK_EQ(::unlink(marker.c_str()), 0);

  WriteFile(s.Path("target"), "user file");  // a link there is refused and its target is untouched
  CHECK_EQ(::symlink(Absolute(s.Path("target")).c_str(), marker.c_str()), 0);
  CHECK(!MarkOwned(item));
  CHECK_EQ(ReadFile(s.Path("target")), std::string("user file"));
  CHECK_EQ(::unlink(marker.c_str()), 0);

  CHECK(MarkOwned(item));  // an empty file of ours is accepted again, as a resume does
  CHECK(MarkOwned(item));
  CHECK(IsOwned(item));
}

TEST(IsOwnedNeedsAnEmptyRegularFile) {
  Scratch s;
  const std::string item = s.Path("_data.staging-aaaaaaaaaaaa");
  const std::string marker = OwnerMarkerPath(item);
  CHECK_EQ(::mkdir(marker.c_str(), 0755), 0);  // a directory
  CHECK(!IsOwned(item));
  CHECK_EQ(::rmdir(marker.c_str()), 0);
  WriteFile(s.Path("empty"), "");
  CHECK_EQ(::symlink(Absolute(s.Path("empty")).c_str(), marker.c_str()), 0);  // a link to an empty file
  CHECK(!IsOwned(item));
  CHECK_EQ(::unlink(marker.c_str()), 0);
  WriteFile(marker, "content");  // a file with content
  CHECK(!IsOwned(item));
  CHECK_EQ(::unlink(marker.c_str()), 0);
  WriteFile(marker, "");
  CHECK(IsOwned(item));
}

TEST(CleanLeftoversKeepsAFileWithContentThatOnlyLooksLikeAnOrphanMarker) {
  Scratch s;
  const std::string file = s.Path("_data.staging-notes.nevr-owned");
  WriteFile(file, "mine");
  int failed = 0;
  CHECK_EQ(CleanLeftovers(s.dir, &failed), 0);
  CHECK_EQ(ReadFile(file), std::string("mine"));
}

// Case (a): a leftover that holds a link to a canary folder outside the root. The item never recorded that link, so cleanup
// must refuse the item: the link, the item and its marker stay, and the canary keeps its file.
TEST(CleanLeftoversNeverWalksAnItemIntoALinkedCanary) {
  Scratch s;
  CHECK_EQ(::mkdir(s.Path("canary").c_str(), 0755), 0);
  WriteFile(s.Path("canary/keep"), "k");
  const std::string item = s.Path("_data.staging-cccccccccccc");
  CHECK(MarkOwned(item));
  CHECK_EQ(::mkdir(item.c_str(), 0755), 0);
  char* abs = ::realpath(s.Path("canary").c_str(), nullptr);
  CHECK(abs != nullptr);
  CHECK_EQ(::symlink(abs, (item + "/sub").c_str()), 0);
  std::free(abs);
  int failed = -1;
  CHECK_EQ(CleanLeftovers(s.dir, &failed), 0);
  CHECK(Exists(item + "/sub"));
  CHECK(Exists(item));
  CHECK(Exists(OwnerMarkerPath(item)));
  CHECK_EQ(ReadFile(s.Path("canary/keep")), std::string("k"));
}

// Case (d): an item on disk that the ledger does not name is not touched, even with a marker that holds nothing. The
// marker here is written by hand, so no ledger line of ours names the item.
TEST(CleanLeftoversNeverTouchesAnItemTheLedgerDoesNotName) {
  Scratch s;
  const std::string item = s.Path("_data.staging-dddddddddddd");
  WriteFile(OwnerMarkerPath(item), "");
  CHECK_EQ(::mkdir(item.c_str(), 0755), 0);
  WriteFile(item + "/f", "x");
  CHECK_EQ(CleanLeftovers(s.dir, nullptr), 0);
  CHECK_EQ(ReadFile(item + "/f"), std::string("x"));
  CHECK(IsOwned(item));
}

TEST(RecoverReplacedNeverTakesALinkEvenWithAMarker) {
  Scratch s;
  CHECK_EQ(::mkdir(s.Path("userdir").c_str(), 0755), 0);
  WriteFile(s.Path("userdir/keep"), "k");
  const std::string link = s.Path("_data.replaced-20261022T000000Z");
  CHECK(MarkOwned(link));
  CHECK_EQ(::symlink(Absolute(s.Path("userdir")).c_str(), link.c_str()), 0);
  CHECK(!RecoverReplaced(s.dir, nullptr));
  CHECK(!Exists(s.Path("_data")));
  CHECK_EQ(ReadFile(s.Path("userdir/keep")), std::string("k"));
}

// Appends raw bytes to a file, for the cases a test makes by hand (a manifest line the grammar refuses).
void AppendRaw(const std::string& path, const std::string& bytes) {
  const int fd = ::open(path.c_str(), O_WRONLY | O_APPEND);
  CHECK(fd >= 0);
  CHECK_EQ(::write(fd, bytes.data(), bytes.size()), static_cast<ssize_t>(bytes.size()));
  ::close(fd);
}

bool IsLink(const std::string& path) {
  struct stat st;
  return ::lstat(path.c_str(), &st) == 0 && S_ISLNK(st.st_mode);
}

// The canary: a folder outside the item, with a file that must survive every case below.
void MakeCanary(const Scratch& s) {
  CHECK_EQ(::mkdir(s.Path("canary").c_str(), 0755), 0);
  WriteFile(s.Path("canary/keep"), "k");
}

std::string LinkToCanary(const Scratch& s, const std::string& at) {
  char* abs = ::realpath(s.Path("canary").c_str(), nullptr);
  CHECK(abs != nullptr);
  CHECK_EQ(::symlink(abs, at.c_str()), 0);
  std::free(abs);
  return at;
}

// Case (b): a folder holding a file its manifest does not name keeps its folder and that file.
TEST(RemoveItemKeepsAnItemThatHoldsAFileItsManifestDoesNotName) {
  Scratch s;
  const std::string item = s.Path("_data.staging-eeeeeeeeeeee");
  CHECK(MarkOwned(item));
  CHECK_EQ(::mkdir(item.c_str(), 0755), 0);
  CHECK(RecordOwned(item, {"listed.txt"}));
  WriteFile(item + "/listed.txt", "a");
  WriteFile(item + "/unlisted.txt", "b");
  const ItemRemoval r = RemoveItem(item);
  CHECK(!r.removed);
  CHECK(r.had_item);
  CHECK_EQ(r.outcome, std::string("not_empty"));
  CHECK_EQ(r.entry, std::string("_data.staging-eeeeeeeeeeee"));
  CHECK(!Exists(item + "/listed.txt"));
  CHECK_EQ(ReadFile(item + "/unlisted.txt"), std::string("b"));
  CHECK(Exists(item));
  CHECK(Exists(OwnerMarkerPath(item)));
}

// Case (a), with the member recorded: the member's parent is a link to the canary. The guard refuses it; the link, the item and
// the marker stay, and the canary keeps its file.
TEST(RemoveItemRefusesAMemberWhoseParentIsALinkToACanary) {
  Scratch s;
  MakeCanary(s);
  const std::string item = s.Path("_data.staging-ffffffffffff");
  CHECK(MarkOwned(item));
  CHECK_EQ(::mkdir(item.c_str(), 0755), 0);
  CHECK(RecordOwned(item, {"sub/keep"}));
  LinkToCanary(s, item + "/sub");
  const ItemRemoval r = RemoveItem(item);
  CHECK(!r.removed);
  CHECK_EQ(r.outcome, std::string("refused_link"));
  CHECK_EQ(r.entry, std::string("sub/keep"));
  CHECK(IsLink(item + "/sub"));
  CHECK(Exists(item));
  CHECK(Exists(OwnerMarkerPath(item)));
  CHECK_EQ(ReadFile(s.Path("canary/keep")), std::string("k"));
}

// Case (c): the ledger names an item that is a link to the canary. The link is refused and left; the canary keeps its file.
TEST(CleanLeftoversRefusesALedgeredItemThatIsALink) {
  Scratch s;
  MakeCanary(s);
  const std::string item = s.Path("_data.staging-gggggggggggg");
  CHECK(MarkOwned(item));
  LinkToCanary(s, item);
  int failed = 0;
  std::vector<std::string> left;
  CHECK_EQ(CleanLeftovers(s.dir, &failed, &left), 0);
  CHECK_EQ(failed, 1);
  CHECK_EQ(left.size(), 1u);
  CHECK_EQ(left.at(0), std::string("refused_link _data.staging-gggggggggggg"));
  CHECK(IsLink(item));
  CHECK(Exists(OwnerMarkerPath(item)));
  CHECK_EQ(ReadFile(s.Path("canary/keep")), std::string("k"));
}

// Case (e): a marker with a line the grammar refuses (a parent step, a NUL byte, an absolute path) makes the item someone else's.
TEST(ABadManifestLineMakesTheWholeItemSomeoneElses) {
  Scratch s;
  const std::vector<std::pair<std::string, std::string>> cases = {
      {"bad-parent", "../x\n"}, {"bad-nul", std::string("a\0b\n", 4)}, {"bad-absolute", "/abs\n"}};
  for (const auto& c : cases) {
    const std::string item = s.Path("_data.staging-" + c.first);
    CHECK(MarkOwned(item));
    CHECK_EQ(::mkdir(item.c_str(), 0755), 0);
    WriteFile(item + "/ok", "x");
    AppendRaw(OwnerMarkerPath(item), c.second);
    CHECK(!IsOwned(item));
    const ItemRemoval r = RemoveItem(item);
    CHECK(!r.removed);
    CHECK_EQ(r.outcome, std::string("not_owned"));
    CHECK_EQ(ReadFile(item + "/ok"), std::string("x"));
  }
  int failed = -1;
  CHECK_EQ(CleanLeftovers(s.dir, &failed), 0);
  CHECK_EQ(failed, 3);
  for (const auto& c : cases) CHECK(Exists(s.Path("_data.staging-" + c.first)));
}

// A manifest over kMaxOwnerManifestBytes is someone else's, and no member is recorded past the cap.
TEST(AManifestOverTheCapIsSomeoneElses) {
  Scratch s;
  const std::string item = s.Path("_data.staging-oversize");
  CHECK(MarkOwned(item));
  std::string big;
  while (big.size() <= kMaxOwnerManifestBytes) big += "m\n";
  WriteFile(OwnerMarkerPath(item), big);
  CHECK(!IsOwned(item));
  CHECK(!RecordOwned(item, {"one"}));
}

// Case (f): a crash midway through an item's members. Cleanup removes every member the item recorded, then the folders, the
// ledger's name and the marker, and nothing beside the item.
TEST(ACrashedStagingItemIsRemovedWhollyAndNothingElse) {
  Scratch s;
  WriteFile(s.Path("stranger.txt"), "keep");
  const std::string item = s.Path("_data.staging-iiiiiiiiiiii");
  CHECK(MarkOwned(item));
  CHECK(RecordOwned(item, {"a.txt", "sub", "sub/b.txt", "sub/c.txt"}));  // the whole set, before the first member
  CHECK_EQ(::mkdir(item.c_str(), 0755), 0);
  WriteFile(item + "/a.txt", "a");
  CHECK_EQ(::mkdir((item + "/sub").c_str(), 0755), 0);
  WriteFile(item + "/sub/b.txt", "b");
  // The crash comes here: sub/c.txt was never made.
  int failed = -1;
  CHECK_EQ(CleanLeftovers(s.dir, &failed), 1);
  CHECK_EQ(failed, 0);
  CHECK(!Exists(item));
  CHECK(!Exists(OwnerMarkerPath(item)));
  CHECK_EQ(ReadFile(s.Path("stranger.txt")), std::string("keep"));
  CHECK(ReadFile(s.Path(kLedgerName)).find("_data.staging-iiiiiiiiiiii") == std::string::npos);
}

// A file item (a download, a pack temporary) is removed by its name and its marker; a link at the name is refused.
TEST(AFileItemIsRemovedWithItsMarkerOnly) {
  Scratch s;
  const std::string item = s.Path("_data.zip.part");
  CHECK(MarkOwned(item));
  WriteFile(item, "bytes");
  const ItemRemoval r = RemoveItem(item);
  CHECK(r.removed);
  CHECK(r.had_item);
  CHECK(!Exists(item));
  CHECK(!Exists(OwnerMarkerPath(item)));
}

// A link the manifest names as a member is the link the item made: the guard unlinks it by name and never follows it, so the
// item goes and the canary keeps its file.
TEST(RemoveItemUnlinksARecordedLinkByNameAndNeverFollowsIt) {
  Scratch s;
  MakeCanary(s);
  const std::string item = s.Path("_data.staging-jjjjjjjjjjjj");
  CHECK(MarkOwned(item));
  CHECK_EQ(::mkdir(item.c_str(), 0755), 0);
  CHECK(RecordOwned(item, {"link"}));
  LinkToCanary(s, item + "/link");
  const ItemRemoval r = RemoveItem(item);
  CHECK(r.removed);
  CHECK(!Exists(item));
  CHECK(!Exists(OwnerMarkerPath(item)));
  CHECK_EQ(ReadFile(s.Path("canary/keep")), std::string("k"));
}

// The build before manifests wrote an empty marker for its staging directory. Its zip is the record of what it made: a legacy staging
// is cleared by exactly those members, and the folder only when it is then empty.
TEST(ALegacyStagingWithExactlyItsZipMembersIsCleared) {
  Scratch s;
  const std::string part = s.Path("_data.zip.part");
  WriteFile(part, BuildZip({SrcEntry("_data/a.bin", "AAAA"), SrcEntry("_data/sub/b.bin", "BBBB")}));
  const std::string staging = s.Path("_data.staging-kkkkkkkkkkkk");
  CHECK(MarkOwned(staging));
  CHECK_EQ(::mkdir(staging.c_str(), 0755), 0);
  CHECK_EQ(::mkdir((staging + "/sub").c_str(), 0755), 0);
  WriteFile(staging + "/a.bin", "AAAA");
  WriteFile(staging + "/sub/b.bin", "BBBB");
  std::string detail;
  const ItemRemoval r = ClearLegacyStaging(staging, part, ZipLimits(), &detail);
  CHECK(r.removed);
  CHECK(r.had_item);
  CHECK(!Exists(staging));
  CHECK(!Exists(OwnerMarkerPath(staging)));
  CHECK(Exists(part));
}

// With a stranger in one of its folders, a legacy staging keeps that folder and the stranger; what the zip names is removed.
TEST(ALegacyStagingWithAStrangerIsRefusedAndTheStrangerSurvives) {
  Scratch s;
  const std::string part = s.Path("_data.zip.part");
  WriteFile(part, BuildZip({SrcEntry("_data/a.bin", "AAAA"), SrcEntry("_data/sub/b.bin", "BBBB")}));
  const std::string staging = s.Path("_data.staging-llllllllllll");
  CHECK(MarkOwned(staging));
  CHECK_EQ(::mkdir(staging.c_str(), 0755), 0);
  CHECK_EQ(::mkdir((staging + "/sub").c_str(), 0755), 0);
  WriteFile(staging + "/a.bin", "AAAA");
  WriteFile(staging + "/sub/b.bin", "BBBB");
  WriteFile(staging + "/sub/stranger.txt", "mine");
  std::string detail;
  const ItemRemoval r = ClearLegacyStaging(staging, part, ZipLimits(), &detail);
  CHECK(!r.removed);
  CHECK_EQ(r.outcome, std::string("not_empty"));
  CHECK_EQ(r.entry, std::string("sub"));
  CHECK_EQ(ReadFile(staging + "/sub/stranger.txt"), std::string("mine"));
  CHECK(!Exists(staging + "/a.bin"));
  CHECK(Exists(staging));
  CHECK(Exists(OwnerMarkerPath(staging)));
}

// A legacy staging whose zip is no longer there names nothing: it is left, and the reason says so.
TEST(ALegacyStagingWhoseZipIsGoneIsLeftAndNamed) {
  Scratch s;
  const std::string staging = s.Path("_data.staging-mmmmmmmmmmmm");
  CHECK(MarkOwned(staging));
  CHECK_EQ(::mkdir(staging.c_str(), 0755), 0);
  WriteFile(staging + "/a.bin", "AAAA");
  std::string detail;
  const ItemRemoval r = ClearLegacyStaging(staging, s.Path("_data.zip.part"), ZipLimits(), &detail);
  CHECK(!r.removed);
  CHECK_EQ(r.outcome, std::string("legacy_no_listing"));
  CHECK(Exists(staging + "/a.bin"));
}

// A torn tail of the ledger (the last line cut short) must not stop the cleanup of the lines before it.
TEST(ATornLedgerTailStillCleansTheRestOfTheLedger) {
  Scratch s;
  const std::string a = s.Path("_data.staging-aaaaaaaaaaaa");
  CHECK(MarkOwned(a));
  CHECK_EQ(::mkdir(a.c_str(), 0755), 0);
  const std::string b = s.Path("_data.staging-bbbbbbbbbbbb");
  CHECK(MarkOwned(b));
  const std::string ledger = s.Path(kLedgerName);
  const std::string text = ReadFile(ledger);
  WriteFile(ledger, text.substr(0, text.size() - 2));
  int failed = -1;
  CHECK_EQ(CleanLeftovers(s.dir, &failed), 1);
  CHECK_EQ(failed, 0);
  CHECK(!Exists(a));
}

// RecoverReplaced chooses from the ledger: a marked replaced directory that no ledger names is not renamed back, however it looks.
TEST(ARecoverReplacedDoesNotChooseAMarkedDirThatTheLedgerDoesNotName) {
  Scratch s;
  const std::string replaced = s.Path("_data.replaced-20261022T000000Z");
  WriteFile(OwnerMarkerPath(replaced), "");  // a marker written by hand: no ledger line names this directory
  CHECK_EQ(::mkdir(replaced.c_str(), 0755), 0);
  WriteFile(replaced + "/old", "old");
  CHECK(!RecoverReplaced(s.dir, nullptr));
  CHECK(!Exists(s.Path("_data")));
  CHECK_EQ(ReadFile(replaced + "/old"), std::string("old"));
}

// A marker at a name that is NOT pinned is only ours when its text is a torn prefix of a member the zip lists. Anything else is a stranger's:
// it is left, its bytes are kept, and nothing is removed.
TEST(ALegacyStagingWithAStrangerMarkerIsLeftAsNotOwned) {
  Scratch s;
  const std::string part = s.Path("_data.zip.part");
  WriteFile(part, BuildZip({SrcEntry("_data/a.bin", "AAAA")}));
  const std::string staging = s.Path("_data.staging-nnnnnnnnnnnn");
  WriteFile(OwnerMarkerPath(staging), "the player's notes");
  CHECK_EQ(::mkdir(staging.c_str(), 0755), 0);
  WriteFile(staging + "/a.bin", "AAAA");
  std::string detail;
  const ItemRemoval r = ClearLegacyStaging(staging, part, ZipLimits(), &detail);
  CHECK(!r.removed);
  CHECK_EQ(r.outcome, std::string("not_owned"));
  CHECK_EQ(ReadFile(OwnerMarkerPath(staging)), std::string("the player's notes"));
  CHECK(!Exists(OwnerMarkerPath(staging) + ".bad"));
  CHECK_EQ(ReadFile(staging + "/a.bin"), std::string("AAAA"));
}

// The pinned name: a garbage marker is moved aside and the staging is cleared by its zip's names (the pinned flag).
TEST(ALegacyStagingWithAGarbageMarkerOnThePinnedNameIsMovedAsideAndCleared) {
  Scratch s;
  const std::string part = s.Path("_data.zip.part");
  WriteFile(part, BuildZip({SrcEntry("_data/a.bin", "AAAA")}));
  const std::string staging = s.Path("_data.staging-pppppppppppp");
  const std::string garbage = std::string("../escape\n\x01garbage\n", 19);
  WriteFile(OwnerMarkerPath(staging), garbage);
  CHECK_EQ(::mkdir(staging.c_str(), 0755), 0);
  WriteFile(staging + "/a.bin", "AAAA");
  std::string detail;
  const ItemRemoval r = ClearLegacyStaging(staging, part, ZipLimits(), &detail, /*pinned=*/true);
  CHECK(r.removed);
  CHECK_EQ(ReadFile(OwnerMarkerPath(staging) + ".bad"), garbage);
  CHECK(!Exists(staging));
}

// A torn first line of our record for a legacy staging is moved aside, and the staging is then cleared by its zip's listing.
TEST(ALegacyStagingWithATornFirstLineIsMovedAsideAndClearedByItsZip) {
  Scratch s;
  const std::string part = s.Path("_data.zip.part");
  WriteFile(part, BuildZip({SrcEntry("_data/a.bin", "AAAA")}));
  const std::string staging = s.Path("_data.staging-oooooooooooo");
  WriteFile(OwnerMarkerPath(staging), "a.b");
  CHECK_EQ(::mkdir(staging.c_str(), 0755), 0);
  WriteFile(staging + "/a.bin", "AAAA");
  std::string detail;
  const ItemRemoval r = ClearLegacyStaging(staging, part, ZipLimits(), &detail);
  CHECK(r.removed);
  CHECK_EQ(ReadFile(OwnerMarkerPath(staging) + ".bad"), std::string("a.b"));
  CHECK(!Exists(staging));
}

TEST(CheckSpaceComparesAgainstTheFreeBytes) {
  Scratch s;
  std::uint64_t avail = 0;
  CHECK_EQ(CheckSpace(s.dir, 1, &avail), StageStatus::kOk);
  CHECK(avail > 0);
  CHECK_EQ(CheckSpace(s.dir, avail + 1, nullptr), StageStatus::kNoSpace);
  CHECK_EQ(CheckSpace(s.Path("missing"), 1, nullptr), StageStatus::kStatFailed);
}

// ---------------- one entry of a listed archive (ReadZipEntry, the bundled content pack) ----------------

namespace {

struct EntryRead {
  ZipStatus status;
  std::string detail;
  std::string bytes;
};

// Lists the zip `zip_bytes` (with `limits`) and reads the entry `name` into memory.
EntryRead ReadEntry(const Scratch& s, const std::string& zip_bytes, const std::string& name, const ZipLimits& limits = {}) {
  WriteFile(s.Path("e.zip"), zip_bytes);
  EntryRead r{};
  std::vector<ZipEntry> entries;
  r.status = ReadZipListing(s.Path("e.zip"), limits, &entries, &r.detail);
  if (r.status != ZipStatus::kOk) return r;
  const ZipEntry* e = FindZipEntry(entries, name);
  if (e == nullptr) {
    r.status = ZipStatus::kBadEntryPath;
    return r;
  }
  r.status = ReadZipEntry(s.Path("e.zip"), *e,
                          [&r](const unsigned char* bytes, std::size_t length) {
                            r.bytes.append(reinterpret_cast<const char*>(bytes), length);
                            return true;
                          },
                          &r.detail);
  return r;
}

}  // namespace

TEST(ReadZipEntryGivesTheBytesOfAStoredAndOfADeflatedEntry) {
  Scratch s;
  const std::string zip = BuildZip({{"a/stored.bin", Pattern(300000), 0}, {"a/deflated.bin", Pattern(300000), 8},
                                    {"other", "o", 8}});
  CHECK(ReadEntry(s, zip, "a/stored.bin").status == ZipStatus::kOk);
  CHECK(ReadEntry(s, zip, "a/stored.bin").bytes == Pattern(300000));
  CHECK(ReadEntry(s, zip, "a/deflated.bin").status == ZipStatus::kOk);
  CHECK(ReadEntry(s, zip, "a/deflated.bin").bytes == Pattern(300000));
}

TEST(ReadZipEntryEnforcesTheCrcAndTheSizeTheDirectoryRecorded) {
  Scratch s;
  SrcEntry bad_crc("f", Pattern(1000), 0);
  bad_crc.crc_delta = 1;
  CHECK(ReadEntry(s, BuildZip({bad_crc}), "f").status == ZipStatus::kCrcMismatch);
  SrcEntry bad_crc_deflated("f", Pattern(1000), 8);
  bad_crc_deflated.crc_delta = 1;
  CHECK(ReadEntry(s, BuildZip({bad_crc_deflated}), "f").status == ZipStatus::kCrcMismatch);
  SrcEntry short_size("f", Pattern(1000), 8);
  short_size.usize_delta = 5;  // the directory says more bytes than the stream holds
  CHECK(ReadEntry(s, BuildZip({short_size}), "f").status == ZipStatus::kSizeMismatch);
  SrcEntry long_size("f", Pattern(1000), 8);
  long_size.usize_delta = -5;  // the stream expands past the recorded size
  CHECK(ReadEntry(s, BuildZip({long_size}), "f").status == ZipStatus::kSizeMismatch);
}

TEST(ReadZipEntryChecksTheLocalHeaderAgainstTheDirectory) {
  Scratch s;
  SrcEntry other_name("file-a", "x", 0);
  other_name.local_name = "file-b";
  CHECK(ReadEntry(s, BuildZip({other_name}), "file-a").status == ZipStatus::kBadLocalHeader);
  SrcEntry other_method("f", Pattern(100), 0);
  other_method.local_method = 8;
  CHECK(ReadEntry(s, BuildZip({other_method}), "f").status == ZipStatus::kBadLocalHeader);
  SrcEntry encrypted_local("f", "x", 0);
  encrypted_local.local_flags = 0x1;
  CHECK(ReadEntry(s, BuildZip({encrypted_local}), "f").status == ZipStatus::kBadLocalHeader);
}

TEST(ReadZipEntryRefusesADirectoryAnUnopenableArchiveAndAStoppedSink) {
  Scratch s;
  WriteFile(s.Path("e.zip"), BuildZip({{"d/", "", 0}, {"d/f", "abc", 0}}));
  std::vector<ZipEntry> entries;
  CHECK(ReadZipListing(s.Path("e.zip"), ZipLimits{}, &entries, nullptr) == ZipStatus::kOk);
  const ZipEntry* dir = FindZipEntry(entries, "d/");
  const ZipEntry* file = FindZipEntry(entries, "d/f");
  CHECK(dir != nullptr && file != nullptr);
  const ZipSink keep = [](const unsigned char*, std::size_t) { return true; };
  CHECK(ReadZipEntry(s.Path("e.zip"), *dir, keep, nullptr) == ZipStatus::kBadEntryPath);
  CHECK(ReadZipEntry(s.Path("missing.zip"), *file, keep, nullptr) == ZipStatus::kOpenFailed);
  CHECK(ReadZipEntry(s.Path("e.zip"), *file, [](const unsigned char*, std::size_t) { return false; }, nullptr) ==
        ZipStatus::kWriteFailed);
}

TEST(FindZipEntryMatchesTheWholeNameInTheCaseItWasStored) {
  Scratch s;
  WriteFile(s.Path("e.zip"), BuildZip({{"assets/Pack.json", "{}", 8}}));
  std::vector<ZipEntry> entries;
  CHECK(ReadZipListing(s.Path("e.zip"), ZipLimits{}, &entries, nullptr) == ZipStatus::kOk);
  CHECK(FindZipEntry(entries, "assets/Pack.json") != nullptr);
  CHECK(FindZipEntry(entries, "assets/pack.json") == nullptr);
  CHECK(FindZipEntry(entries, "assets/Pack.jso") == nullptr);
  CHECK(FindZipEntry(entries, "assets/Pack.json/") == nullptr);
}

TEST(AListingWithRaisedLimitsReadsAnArchiveTheDataZipLimitsRefuseAndTheExtractorStillRefusesItsPrefixMiss) {
  Scratch s;
  std::vector<SrcEntry> many;
  for (int i = 0; i < 70; ++i) many.emplace_back("_data/f" + std::to_string(i), "x", 0);
  const std::string zip = BuildZip(many);
  WriteFile(s.Path("m.zip"), zip);
  std::vector<ZipEntry> entries;
  CHECK(ReadZipListing(s.Path("m.zip"), ZipLimits{}, &entries, nullptr) == ZipStatus::kTooManyEntries);
  ZipLimits raised;
  raised.max_entries = 200;
  CHECK(ReadZipListing(s.Path("m.zip"), raised, &entries, nullptr) == ZipStatus::kOk);
  CHECK_EQ(entries.size(), std::size_t{70});
  // The extractor keeps its rule: an entry outside the prefix is refused, and nothing is written.
  std::vector<SrcEntry> mixed = many;
  mixed.emplace_back("outside/f", "x", 0);
  const Extracted r = Extract(s, BuildZip(mixed), raised);
  CHECK(r.status == ZipStatus::kBadEntryPath);
  CHECK(!Exists(s.Path("out")));
}

TEST(ExtractZipStillWritesTheEntriesOfAZipAndFsyncsThem) {
  Scratch s;
  const Extracted r = Extract(s, BuildZip({{"_data/a/", "", 0}, {"_data/a/f", Pattern(5000), 8}, {"_data/g", "gg", 0}}));
  CHECK(r.status == ZipStatus::kOk);
  CHECK_EQ(r.bytes, std::uint64_t{5002});
  CHECK(ReadFile(s.Path("out/a/f")) == Pattern(5000));
  CHECK(ReadFile(s.Path("out/g")) == "gg");
}

int main(int argc, char** argv) { return mini_test::RunAll(argc, argv); }
