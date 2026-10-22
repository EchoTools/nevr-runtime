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
#include "quest/tests/mini_test.h"
#include "quest/tests/pinned_zip_directory.h"

using namespace nevr_quest::data;

namespace {

// ---- a scratch directory that removes itself ----
struct Scratch {
  std::string dir;
  Scratch() {
    char tmpl[] = "build/quest-data-host/work-XXXXXX";
    const char* made = ::mkdtemp(tmpl);
    dir = made != nullptr ? made : "";
  }
  ~Scratch() {
    if (!dir.empty()) RemoveTree(dir);
  }
  std::string Path(const std::string& rel) const { return dir + "/" + rel; }
};

bool Exists(const std::string& path) {
  struct stat st;
  return ::lstat(path.c_str(), &st) == 0;
}

std::string ReadFile(const std::string& path) {
  std::string out;
  const int fd = ::open(path.c_str(), O_RDONLY);
  if (fd < 0) return out;
  char buf[4096];
  ssize_t n;
  while ((n = ::read(fd, buf, sizeof(buf))) > 0) out.append(buf, static_cast<std::size_t>(n));
  ::close(fd);
  return out;
}

void WriteFile(const std::string& path, const std::string& data) {
  const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) return;
  if (!data.empty()) {
    const ssize_t n = ::write(fd, data.data(), data.size());
    (void)n;
  }
  ::close(fd);
}

// ---- a zip writer: one knob per rule the reader enforces ----
struct SrcEntry {
  SrcEntry(std::string n, std::string d, std::uint16_t m = 8) : name(std::move(n)), data(std::move(d)), method(m) {}
  std::string name;
  std::string data;
  std::uint16_t method = 8;
  std::uint16_t flags = 0;
  std::uint16_t made_by = 20;
  std::uint32_t external = 0;
  std::int64_t crc_delta = 0;     // added to the CRC the directory records
  std::int64_t usize_delta = 0;   // added to the uncompressed size the directory records
  std::string raw_payload;        // when not empty, the bytes stored as the entry's payload instead of the compressed data
  bool use_raw = false;
  std::int32_t local_method = -1;      // >= 0: the method the LOCAL header says, the directory keeps `method`
  std::string local_name;              // not empty: the name the LOCAL header carries (same length as the name)
  std::int64_t local_crc_delta = 0;    // added to the CRC the LOCAL header records
  std::int64_t local_usize_delta = 0;  // added to the uncompressed size the LOCAL header records
  std::uint16_t local_flags = 0;       // flags of the LOCAL header only
};

void Put16(std::string& s, std::uint16_t v) {
  s.push_back(static_cast<char>(v & 0xff));
  s.push_back(static_cast<char>(v >> 8));
}
void Put32(std::string& s, std::uint32_t v) {
  Put16(s, static_cast<std::uint16_t>(v & 0xffff));
  Put16(s, static_cast<std::uint16_t>(v >> 16));
}

std::string Deflate(const std::string& in) {
  z_stream zs;
  std::memset(&zs, 0, sizeof(zs));
  deflateInit2(&zs, 6, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY);
  std::string out(deflateBound(&zs, static_cast<uLong>(in.size())) + 16, '\0');
  zs.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(in.data()));
  zs.avail_in = static_cast<uInt>(in.size());
  zs.next_out = reinterpret_cast<Bytef*>(&out[0]);
  zs.avail_out = static_cast<uInt>(out.size());
  deflate(&zs, Z_FINISH);
  out.resize(zs.total_out);
  deflateEnd(&zs);
  return out;
}

struct ZipOptions {
  bool zip64_marker = false;     // 0xFFFF in the end record's entry counts
  std::uint16_t disk = 0;
  bool zip64_locator = false;    // a zip64 end-of-central-directory locator in front of the end record
  std::string trailing;          // bytes after the end record
  std::uint16_t comment_len = 0; // the comment length the end record claims (no comment follows)
};

std::string BuildZip(const std::vector<SrcEntry>& entries, const ZipOptions& opt = {}) {
  std::string body, central;
  for (const SrcEntry& e : entries) {
    const bool dir = !e.name.empty() && e.name.back() == '/';
    std::string payload = dir ? std::string() : (e.method == 8 ? Deflate(e.data) : e.data);
    if (e.use_raw) payload = e.raw_payload;
    const std::uint32_t crc = static_cast<std::uint32_t>(
        crc32(crc32(0L, Z_NULL, 0), reinterpret_cast<const Bytef*>(e.data.data()), static_cast<uInt>(e.data.size())));
    const std::uint32_t rec_crc = static_cast<std::uint32_t>(static_cast<std::int64_t>(crc) + e.crc_delta);
    const std::uint32_t usize = static_cast<std::uint32_t>(static_cast<std::int64_t>(e.data.size()) + e.usize_delta);
    const std::uint32_t offset = static_cast<std::uint32_t>(body.size());
    Put32(body, 0x04034b50);
    Put16(body, 20);
    Put16(body, static_cast<std::uint16_t>(e.flags | e.local_flags));
    Put16(body, static_cast<std::uint16_t>(e.local_method >= 0 ? e.local_method : e.method));
    Put32(body, 0);
    Put32(body, static_cast<std::uint32_t>(static_cast<std::int64_t>(rec_crc) + e.local_crc_delta));
    Put32(body, static_cast<std::uint32_t>(payload.size()));
    Put32(body, static_cast<std::uint32_t>(static_cast<std::int64_t>(usize) + e.local_usize_delta));
    Put16(body, static_cast<std::uint16_t>(e.name.size()));
    Put16(body, 0);
    body += e.local_name.empty() ? e.name : e.local_name;
    body += payload;
    Put32(central, 0x02014b50);
    Put16(central, e.made_by);
    Put16(central, 20);
    Put16(central, e.flags);
    Put16(central, e.method);
    Put32(central, 0);
    Put32(central, rec_crc);
    Put32(central, static_cast<std::uint32_t>(payload.size()));
    Put32(central, usize);
    Put16(central, static_cast<std::uint16_t>(e.name.size()));
    Put16(central, 0);
    Put16(central, 0);
    Put16(central, 0);
    Put16(central, 0);
    Put32(central, e.external);
    Put32(central, offset);
    central += e.name;
  }
  std::string out = body;
  const std::uint32_t cd_offset = static_cast<std::uint32_t>(out.size());
  out += central;
  if (opt.zip64_locator) {
    Put32(out, 0x07064b50);
    out.append(16, '\0');
  }
  Put32(out, 0x06054b50);
  Put16(out, opt.disk);
  Put16(out, 0);
  const std::uint16_t count = opt.zip64_marker ? 0xFFFF : static_cast<std::uint16_t>(entries.size());
  Put16(out, count);
  Put16(out, count);
  Put32(out, static_cast<std::uint32_t>(central.size()));
  Put32(out, cd_offset);
  Put16(out, opt.comment_len);
  out += opt.trailing;
  return out;
}

std::string Pattern(std::size_t n) {
  std::string s;
  for (std::size_t i = 0; i < n; ++i) s.push_back(static_cast<char>('a' + (i * 7 + i / 13) % 26));
  return s;
}

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
