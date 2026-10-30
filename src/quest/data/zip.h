/* A small zip reader and extractor for the game-data zip (docs/adr/0011-quest-game-data-first-run.md).
 *
 * Scope: the one zip the sentinel fetches, a non-encrypted, single-disk archive of stored and deflated entries with a
 * central directory. Anything else is refused by name and writes nothing: zip64, encrypted or multi-disk archives, other
 * methods, symbolic links, an entry path that could leave the destination, a file that is also a directory, and sizes
 * over the caller's limits. The central directory is the authority for sizes, offsets and CRCs. The local header of
 * every file entry is checked against it: signature, name, method and flags, and (unless the entry uses a data
 * descriptor) CRC and both sizes. The header of a directory entry is never read. Names that differ only in ASCII case
 * are refused as duplicates. Every entry must end exactly at its recorded size with the recorded CRC-32.
 *
 * ExtractZip writes only below `dest_dir`, which it creates (it must not exist). It stops at the first failure and
 * leaves what it wrote: the caller owns the directory and deletes it (staging.h).
 *
 * ReadZipEntry (docs/adr/0029-quest-bundled-content-pack.md) takes ONE file entry of an archive the caller listed with its
 * own limits (an APK has hundreds of entries and is read through ReadZipListing with raised ZipLimits) and streams its bytes
 * to a sink, under every check ExtractZip makes of an entry. The bytes are provisional until it returns kOk: the size and the
 * CRC-32 are known only at the end.
 *
 * Built without exceptions (the sentinel's data step runs after a call into the game; see callback_thunk.h).
 */
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace nevr_quest::data {

enum class ZipStatus {
  kOk = 0,
  kOpenFailed,
  kNoEndRecord,
  kZip64,
  kMultiDisk,
  kBadCentralDirectory,
  kEncrypted,
  kUnsupportedMethod,
  kSpecialFile,
  kTooManyEntries,
  kBadEntryPath,
  kConflict,
  kOffsetOutOfRange,
  kSizeLimit,
  kBadLocalHeader,
  kDestExists,
  kWriteFailed,
  kReadFailed,
  kInflateFailed,
  kSizeMismatch,
  kCrcMismatch,
};

// A stable lower-case token for the log line.
const char* ZipStatusName(ZipStatus status) noexcept;

struct ZipLimits {
  std::size_t max_entries = 64;
  std::uint64_t max_entry_bytes = 1ull << 31;   // one entry's uncompressed size
  std::uint64_t max_total_bytes = 1200000000;   // all entries' uncompressed sizes
};

struct ZipEntry {
  std::string name;  // as stored; a directory ends with '/'
  std::uint16_t method = 0;  // 0 stored, 8 deflate
  std::uint32_t crc32 = 0;
  std::uint64_t compressed_size = 0;
  std::uint64_t size = 0;  // uncompressed
  std::uint64_t local_header_offset = 0;
  bool IsDirectory() const { return !name.empty() && name.back() == '/'; }
};

// True when `name` is a relative path made of plain components: no empty, "." or ".." component, no backslash, drive
// letter, NUL or control character, at most 16 components of at most 255 bytes, at most 512 bytes in all. A trailing
// '/' (a directory) is allowed.
bool SafeEntryName(const std::string& name) noexcept;

// Reads the central directory of the archive at `path`. `detail` (when not null) gets the entry or field at fault.
ZipStatus ReadZipListing(const std::string& path, const ZipLimits& limits, std::vector<ZipEntry>* entries,
                         std::string* detail);

// The entry named exactly `name` (case-sensitive) in a listing, or null.
const ZipEntry* FindZipEntry(const std::vector<ZipEntry>& entries, const std::string& name) noexcept;

// Receives the bytes of one entry in order. False stops the read (the status is kWriteFailed).
using ZipSink = std::function<bool(const unsigned char* bytes, std::size_t length)>;

// Streams the file entry `entry` (from ReadZipListing of the same archive) to `sink`: the local header is checked against the
// central directory, a stored entry is copied and a deflated one inflated, and the size and CRC-32 the directory recorded are
// enforced. A directory entry is refused (kBadEntryPath). `detail` (when not null) gets the entry at fault.
ZipStatus ReadZipEntry(const std::string& zip_path, const ZipEntry& entry, const ZipSink& sink, std::string* detail);

// mkdir -p below `base` for the relative directory `rel` (no leading or trailing '/'). A component that exists and is
// not a real directory (a link, a file) fails the call; a link is never followed. Exposed for the tests.
bool MakeDirsBelow(const std::string& base, const std::string& rel);

// Extracts every entry, with `strip_prefix` (e.g. "_data/") removed from its name, below `dest_dir`. An entry that does not
// start with the prefix is refused. Returns the sum of the bytes written through `bytes_written` (when not null).
ZipStatus ExtractZip(const std::string& zip_path, const std::string& dest_dir, const std::string& strip_prefix,
                     const ZipLimits& limits, std::uint64_t* bytes_written, std::string* detail);

}  // namespace nevr_quest::data
