#include "quest/data/staging.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#include "core/owned_remove.h"
#include "core/owned_remove_rules.h"
#include "quest/data/plan.h"
#include "quest/data/zip.h"

namespace nevr_quest::data {

namespace {

// Closes its descriptor when it leaves scope.
class Fd {
 public:
  explicit Fd(int fd) : fd_(fd) {}
  ~Fd() {
    if (fd_ >= 0) ::close(fd_);
  }
  Fd(const Fd&) = delete;
  Fd& operator=(const Fd&) = delete;
  int get() const { return fd_; }

 private:
  int fd_;
};

enum class ReadStatus { kOk, kAbsent, kBad };

bool StartsWith(const std::string& name, const char* prefix) { return name.compare(0, std::strlen(prefix), prefix) == 0; }

bool EndsWith(const std::string& name, const char* suffix) {
  const std::size_t n = std::strlen(suffix);
  return name.size() >= n && name.compare(name.size() - n, n, suffix) == 0;
}

bool Lexists(const std::string& path) {
  struct stat st;
  return ::lstat(path.c_str(), &st) == 0;
}

std::string DirOf(const std::string& path) {
  const std::size_t slash = path.rfind('/');
  return slash == std::string::npos ? std::string(".") : path.substr(0, slash == 0 ? 1 : slash);
}

std::string BaseOf(const std::string& path) {
  const std::size_t slash = path.rfind('/');
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

bool IsRealDirectory(const std::string& path) {
  struct stat st;
  return ::lstat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

// True when every component of `rel` below `root` is a real directory (no link anywhere on the way).
bool IsRealDirectoryChain(const std::string& root, const std::string& rel) {
  std::string path = root;
  for (const std::string& part : nevr_owned::SplitEntry(rel)) {
    path += "/" + part;
    if (!IsRealDirectory(path)) return false;
  }
  return true;
}

bool WriteAll(int fd, const std::string& bytes) {
  const char* p = bytes.data();
  std::size_t left = bytes.size();
  while (left > 0) {
    const ssize_t w = ::write(fd, p, left);
    if (w < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    p += w;
    left -= static_cast<std::size_t>(w);
  }
  return true;
}

// Reads a regular file of at most `cap` bytes, never through a link.
ReadStatus ReadWhole(const std::string& path, std::size_t cap, std::string* out) {
  out->clear();
  const int fd = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);  // a FIFO is refused by the fstat below, never waited on
  if (fd < 0) return errno == ENOENT ? ReadStatus::kAbsent : ReadStatus::kBad;
  const Fd guard(fd);
  struct stat st;
  if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || static_cast<std::uint64_t>(st.st_size) > cap) return ReadStatus::kBad;
  char buffer[4096];
  for (;;) {
    const ssize_t n = ::read(fd, buffer, sizeof(buffer));
    if (n < 0) {
      if (errno == EINTR) continue;
      return ReadStatus::kBad;
    }
    if (n == 0) break;
    out->append(buffer, static_cast<std::size_t>(n));
    if (out->size() > cap) return ReadStatus::kBad;
  }
  return ReadStatus::kOk;
}

// The entries of a manifest or a ledger, each line checked by the entry grammar. A write that a kill or a full disk cut short leaves a
// torn tail: trailing NUL bytes (the size grew before the data landed) and a final line with no newline. Both are dropped. The record was
// fsynced before anything it names was made, so a torn tail names nothing that exists. Any other line the grammar refuses makes the whole
// file unreadable.
bool ParseEntries(const std::string& text, std::vector<std::string>* entries) {
  entries->clear();
  std::size_t end = text.size();
  while (end > 0 && text[end - 1] == '\0') --end;
  if (end == 0 && !text.empty()) return false;  // NUL bytes only: not a record (the reset path moves it aside)
  std::string body = text.substr(0, end);
  if (!body.empty() && body.back() != '\n') {
    // A torn tail only behind at least one complete line. Text with no complete line is some other file's content, not a torn record.
    const std::size_t last = body.rfind('\n');
    if (last == std::string::npos) return false;
    body = body.substr(0, last + 1);
  }
  std::size_t start = 0;
  while (start < body.size()) {
    const std::size_t nl = body.find('\n', start);
    std::string line = body.substr(start, nl - start);
    if (!nevr_owned::ValidEntry(line)) return false;
    entries->push_back(std::move(line));
    start = nl + 1;
  }
  return true;
}

// Appends `bytes` to the regular file `path`, creating it when absent (and then syncing its directory). A link, a directory or a
// file that would grow past `cap` is refused. One fsync for the call.
bool AppendFile(const std::string& path, const std::string& bytes, std::size_t cap) {
  const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC, 0644);
  if (fd < 0) return false;
  const Fd guard(fd);
  struct stat st;
  if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) return false;
  if (static_cast<std::uint64_t>(st.st_size) + bytes.size() > cap) return false;
  const bool created = st.st_size == 0;
  // A write that fails part way (a full disk, an interrupted fsync) is cut back to the size it had: the next reader then sees the
  // record as it was before this call, not as a torn tail.
  const off_t prior = st.st_size;
  if (!WriteAll(fd, bytes) || ::fsync(fd) != 0) {
    static_cast<void>(::ftruncate(fd, prior));
    return false;
  }
  return !created || SyncDir(DirOf(path));
}

// The names a directory's ledger lists. A missing ledger lists none (and `present` is false); a link, a non-file, an oversize
// or an invalid ledger fails.
bool ReadLedger(const std::string& dir, std::vector<std::string>* names, bool* present) {
  std::string text;
  const ReadStatus status = ReadWhole(dir + "/" + kLedgerName, kMaxLedgerBytes, &text);
  *present = status != ReadStatus::kAbsent;
  if (status == ReadStatus::kAbsent) {
    names->clear();
    return true;
  }
  if (status != ReadStatus::kOk) return false;
  // A ledger that does not read names nothing: cleanup deletes nothing for it, and its rewrite replaces it.
  if (!ParseEntries(text, names)) names->clear();
  return true;
}

// Replaces a directory's ledger with `names`: a temporary at its literal name, fsync, rename.
bool WriteLedger(const std::string& dir, const std::vector<std::string>& names) {
  std::string text;
  for (const std::string& name : names) text += name + "\n";
  if (text.size() > kMaxLedgerBytes) return false;
  const std::string tmp = dir + "/" + kLedgerTmpName;
  const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0644);
  if (fd < 0) return false;
  {
    const Fd guard(fd);
    if (!WriteAll(fd, text) || ::fsync(fd) != 0) return false;
  }
  if (::rename(tmp.c_str(), (dir + "/" + kLedgerName).c_str()) != 0) return false;
  return SyncDir(dir);
}

// A pack directory as the root's ledger records it: `_data/<version>/rad15/android/{manifests,packages}`.
bool IsPackDirectoryRecord(const std::string& record) {
  const std::vector<std::string> parts = nevr_owned::SplitEntry(record);
  return parts.size() == 5 && parts[0] == data_root::kMarker && parts[2] == "rad15" && parts[3] == "android" &&
         (parts[4] == kManifestsDir || parts[4] == kPackagesDir);
}

// Whether a ledger line's item, or its marker, is still on disk: only then is the line still needed.
bool StillThere(const std::string& path) { return Lexists(path) || Lexists(OwnerMarkerPath(path)); }

void AddLeft(std::vector<std::string>* left, const std::string& outcome, const std::string& entry) {
  if (left != nullptr) left->push_back(outcome + " " + entry);
}

struct CleanCounts {
  int removed = 0;
  int failed = 0;
};

// One pack directory: its `.nevr-pack.tmp` temporaries that its ledger names, and orphaned `.stock` markers. A finished `.stock`
// backup is never removed.
void CleanPackDirectory(const std::string& dir, const std::string& rel, CleanCounts* counts, std::vector<std::string>* left) {
  std::vector<std::string> names;
  bool present = false;
  if (!ReadLedger(dir, &names, &present)) {
    ++counts->failed;
    AddLeft(left, "ledger_unreadable", rel + "/" + kLedgerName);
    return;
  }
  if (!present) return;
  std::set<std::string> seen;
  std::vector<std::string> kept;
  for (const std::string& name : names) {
    if (!seen.insert(name).second || name.find('/') != std::string::npos) continue;
    const std::string path = dir + "/" + name;
    if (EndsWith(name, kStockBackupSuffix)) {
      if (Lexists(path)) {
        kept.push_back(name);
        continue;
      }
      if (!RemoveItem(path).removed) {
        ++counts->failed;
        AddLeft(left, "stock_marker_refused", rel + "/" + name);
        kept.push_back(name);
      }
      continue;
    }
    if (EndsWith(name, kPackTmpSuffix)) {
      const ItemRemoval r = RemoveItem(path);
      if (r.removed) {
        if (r.had_item) ++counts->removed;
        continue;
      }
      ++counts->failed;
      AddLeft(left, r.outcome, rel + "/" + name);
      kept.push_back(name);
      continue;
    }
    if (StillThere(path)) kept.push_back(name);
  }
  if (!WriteLedger(dir, kept)) {
    ++counts->failed;
    AddLeft(left, "ledger_write_failed", rel + "/" + kLedgerName);
  }
}

// One root: its ledger's staging items (`staging`) and its pack directories (`packs`). Every other line is kept while its item or
// marker is on disk.
void CleanRoot(const std::string& root, bool staging, bool packs, CleanCounts* counts, std::vector<std::string>* left) {
  std::vector<std::string> names;
  bool present = false;
  if (!ReadLedger(root, &names, &present)) {
    ++counts->failed;
    AddLeft(left, "ledger_unreadable", kLedgerName);
    return;
  }
  if (!present) return;
  std::set<std::string> seen;
  std::vector<std::string> kept;
  for (const std::string& name : names) {
    if (!seen.insert(name).second) continue;
    if (name.find('/') != std::string::npos) {  // a pack directory record
      if (!IsPackDirectoryRecord(name)) continue;
      if (!packs) {
        kept.push_back(name);
        continue;
      }
      const std::string dir = root + "/" + name;
      if (!Lexists(dir)) continue;  // the directory is gone: so is its record
      if (!IsRealDirectoryChain(root, name)) {
        ++counts->failed;
        AddLeft(left, "not_a_directory", name);
        kept.push_back(name);
        continue;
      }
      CleanPackDirectory(dir, name, counts, left);
      kept.push_back(name);
      continue;
    }
    const std::string path = root + "/" + name;
    if (!staging || !StartsWith(name, kStagingPrefix)) {
      if (!staging || StillThere(path)) kept.push_back(name);
      continue;
    }
    const ItemRemoval r = RemoveItem(path);
    if (r.removed) {
      if (r.had_item) ++counts->removed;
      continue;
    }
    ++counts->failed;
    AddLeft(left, r.outcome, name);
    kept.push_back(name);
  }
  if (!WriteLedger(root, kept)) {
    ++counts->failed;
    AddLeft(left, "ledger_write_failed", kLedgerName);
  }
}

// The number of '/' in an entry: its depth below the item. Sorting by it, deepest first, puts children before parents.
std::size_t Depth(const std::string& entry) { return static_cast<std::size_t>(std::count(entry.begin(), entry.end(), '/')); }

bool Settled(const nevr_owned::Result& r) {
  return r.outcome == nevr_owned::Outcome::kRemoved || r.outcome == nevr_owned::Outcome::kAbsent;
}

// 1 when `path` is there (lstat), 0 when it is absent, -1 when that is unknown.
int LstatState(const std::string& path, struct stat* st) {
  if (::lstat(path.c_str(), st) == 0) return 1;
  return errno == ENOENT ? 0 : -1;
}

ItemRemoval Stopped(bool had_item, const char* outcome, const std::string& entry) {
  ItemRemoval r;
  r.removed = false;
  r.had_item = had_item;
  r.outcome = outcome;
  r.entry = entry;
  return r;
}

}  // namespace

const char* StageStatusName(StageStatus status) noexcept {
  switch (status) {
    case StageStatus::kOk: return "ok";
    case StageStatus::kRenameFailed: return "rename_failed";
    case StageStatus::kStatFailed: return "stat_failed";
    case StageStatus::kNoSpace: return "no_space";
    case StageStatus::kRemoveFailed: return "remove_failed";
    case StageStatus::kBadRoot: return "bad_root";
  }
  return "unknown";
}

std::string StagingPath(const std::string& root, const std::string& sha256_hex) {
  return root + "/" + kStagingPrefix + sha256_hex.substr(0, 12);
}

std::string OwnerMarkerPath(const std::string& item) { return item + kOwnerSuffix; }

bool SyncDir(const std::string& dir) noexcept {
  const int fd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (fd < 0) return false;
  const bool ok = ::fsync(fd) == 0;
  ::close(fd);
  return ok;
}

bool MarkOwned(const std::string& item) noexcept {
  const std::string parent = DirOf(item);
  const std::string name = BaseOf(item);
  if (!nevr_owned::ValidEntry(name)) return false;
  const std::string marker = OwnerMarkerPath(item);
  if (Lexists(marker)) {
    // An earlier marker of ours (a resume) is accepted as it is, with its members; anything else at that name is not.
    return IsOwned(item);
  }
  // The ledger first: the name is on record before its marker, and so before the item, exists.
  if (!AppendFile(parent + "/" + kLedgerName, name + "\n", kMaxLedgerBytes)) return false;
  // O_EXCL and no O_TRUNC: a file already there is never reopened; O_NOFOLLOW: a link there is never followed.
  const int fd = ::open(marker.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
  if (fd < 0) return false;
  const Fd guard(fd);
  return ::fsync(fd) == 0 && SyncDir(parent);
}

bool RecordOwned(const std::string& item, const std::vector<std::string>& members) noexcept {
  std::string text;
  for (const std::string& member : members) {
    if (!nevr_owned::ValidEntry(member)) return false;
    text += member;
    text += '\n';
  }
  if (text.empty()) return true;
  return AppendFile(OwnerMarkerPath(item), text, kMaxOwnerManifestBytes);
}

bool ReadOwnerManifest(const std::string& item, std::vector<std::string>* members) noexcept {
  std::string text;
  if (ReadWhole(OwnerMarkerPath(item), kMaxOwnerManifestBytes, &text) != ReadStatus::kOk) {
    members->clear();
    return false;
  }
  return ParseEntries(text, members);
}

bool IsOwned(const std::string& item) noexcept {
  std::vector<std::string> members;
  return ReadOwnerManifest(item, &members);
}

ItemRemoval RemoveRecorded(const std::string& item, const std::vector<std::string>& members) noexcept {
  const std::string parent = DirOf(item);
  const std::string name = BaseOf(item);
  if (!nevr_owned::ValidEntry(name)) return Stopped(false, "refused_name", name);
  struct stat item_st = {};
  struct stat marker_st = {};
  const int item_state = LstatState(item, &item_st);
  const int marker_state = LstatState(OwnerMarkerPath(item), &marker_st);
  if (item_state < 0 || marker_state < 0) return Stopped(false, "failed", name);
  if (marker_state == 1) {
    std::vector<std::string> ignored;
    if (!ReadOwnerManifest(item, &ignored)) return Stopped(item_state == 1, "not_owned", name);
  }
  if (item_state == 1) {
    if (S_ISLNK(item_st.st_mode)) return Stopped(true, nevr_owned::OutcomeName(nevr_owned::Outcome::kRefusedLink), name);
    if (S_ISDIR(item_st.st_mode)) {
      // Children before parents: the guard takes the entries in the order given.
      std::vector<std::string> ordered = members;
      std::stable_sort(ordered.begin(), ordered.end(),
                       [](const std::string& a, const std::string& b) { return Depth(a) > Depth(b); });
      const std::vector<nevr_owned::Result> results = nevr_owned::RemoveOwned(item, ordered);
      if (results.size() != ordered.size()) return Stopped(true, "failed", name);
      for (const nevr_owned::Result& r : results) {
        if (!Settled(r)) return Stopped(true, nevr_owned::OutcomeName(r.outcome), r.entry);
      }
    } else if (!members.empty()) {
      return Stopped(true, "failed", name);  // a file has no members
    }
    const nevr_owned::Result item_result = nevr_owned::RemoveEntry(parent, name);
    if (!Settled(item_result)) return Stopped(true, nevr_owned::OutcomeName(item_result.outcome), item_result.entry);
  }
  if (marker_state == 1) {
    const nevr_owned::Result marker_result = nevr_owned::RemoveEntry(parent, name + kOwnerSuffix);
    if (!Settled(marker_result)) {
      return Stopped(item_state == 1, nevr_owned::OutcomeName(marker_result.outcome), marker_result.entry);
    }
  }
  ItemRemoval done;
  done.removed = true;
  done.had_item = item_state == 1;
  done.outcome = item_state == 0 && marker_state == 0 ? "absent" : "removed";
  done.entry = name;
  return done;
}

ItemRemoval RemoveItem(const std::string& item, const std::vector<std::string>* legacy_members) noexcept {
  const std::string name = BaseOf(item);
  if (!nevr_owned::ValidEntry(name)) return Stopped(false, "refused_name", name);
  struct stat item_st = {};
  struct stat marker_st = {};
  const int item_state = LstatState(item, &item_st);
  const int marker_state = LstatState(OwnerMarkerPath(item), &marker_st);
  if (item_state < 0 || marker_state < 0) return Stopped(false, "failed", name);
  if (marker_state == 0) {
    // No marker: an item of ours has one, so an item that is there belongs to someone else. Nothing there is nothing to do.
    if (item_state == 1) return Stopped(true, "not_owned", name);
    return RemoveRecorded(item, {});
  }
  std::vector<std::string> members;
  if (!ReadOwnerManifest(item, &members)) return Stopped(item_state == 1, "not_owned", name);
  if (members.empty() && legacy_members != nullptr && item_state == 1 && S_ISDIR(item_st.st_mode)) {
    members = *legacy_members;
  }
  return RemoveRecorded(item, members);
}

bool PlanZipMembers(const std::vector<ZipEntry>& entries, std::vector<std::string>* members, std::string* detail) noexcept {
  const std::size_t prefix_len = std::strlen(kZipPrefix);
  std::set<std::string> planned;
  for (const ZipEntry& entry : entries) {
    if (entry.name.compare(0, prefix_len, kZipPrefix) != 0) continue;  // ExtractZip refuses the archive for such an entry
    std::string rel = entry.name.substr(prefix_len);
    if (!rel.empty() && rel.back() == '/') rel.pop_back();
    if (rel.empty()) continue;
    if (!nevr_owned::ValidEntry(rel)) {
      *detail = "entry '" + entry.name + "'";
      return false;
    }
    for (std::size_t slash = rel.find('/'); slash != std::string::npos; slash = rel.find('/', slash + 1)) {
      planned.insert(rel.substr(0, slash));
    }
    planned.insert(rel);
  }
  members->assign(planned.begin(), planned.end());
  return true;
}

// The bytes a kill or a full disk leave of our own record: nothing at all, NUL bytes only, or one unterminated line that is a prefix of
// a member this build would record. Anything else is not ours to judge.
bool IsTornRecordOfOurs(const std::string& text, const std::vector<std::string>& expected) {
  if (text.find_first_not_of('\0') == std::string::npos) return true;
  if (text.find('\n') != std::string::npos) return false;  // a complete line: a record whose grammar failed is someone else's
  for (const std::string& member : expected) {
    if (member.size() >= text.size() && member.compare(0, text.size(), text) == 0) return true;
  }
  return false;
}

// Clears a name a crash may have left: a regular file or a link is unlinked by name; a directory (or anything else) is not ours and is
// refused. True when the name is absent after the call.
bool RemoveLeftover(const std::string& path) noexcept {
  struct stat st = {};
  if (::lstat(path.c_str(), &st) != 0) return errno == ENOENT;
  if (S_ISDIR(st.st_mode)) return false;
  return ::unlink(path.c_str()) == 0;
}

// The first `cap` bytes of a regular file, never through a link. `truncated` is set when the file is longer than `cap`.
bool ReadHead(const std::string& path, std::size_t cap, std::string* out, bool* truncated) noexcept {
  out->clear();
  *truncated = false;
  const int fd = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
  if (fd < 0) return false;
  const Fd guard(fd);
  struct stat st;
  if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) return false;
  char buffer[4096];
  while (out->size() < cap) {
    const std::size_t want = std::min(sizeof(buffer), cap - out->size());
    const ssize_t n = ::read(fd, buffer, want);
    if (n < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    if (n == 0) return true;
    out->append(buffer, static_cast<std::size_t>(n));
  }
  char probe;
  *truncated = ::read(fd, &probe, 1) == 1;
  return true;
}

// Creates `path` exclusively (never through a link) holding `bytes`, and syncs it. A failure leaves the name for the caller to clear.
bool WriteFreshFile(const std::string& path, const std::string& bytes) noexcept {
  const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
  if (fd < 0) return false;
  const Fd guard(fd);
  return WriteAll(fd, bytes) && ::fsync(fd) == 0;
}

bool ResetUnreadableMarker(const std::string& item, const std::vector<std::string>& expected, bool pinned,
                           bool* truncated) noexcept {
  if (truncated != nullptr) *truncated = false;
  const std::string marker = OwnerMarkerPath(item);
  struct stat st = {};
  if (::lstat(marker.c_str(), &st) != 0) return errno == ENOENT;
  if (!S_ISREG(st.st_mode)) return false;
  std::vector<std::string> ignored;
  if (ReadOwnerManifest(item, &ignored)) return true;
  std::string head;
  bool cut = false;
  if (pinned) {
    if (!ReadHead(marker, kMaxOwnerManifestBytes, &head, &cut)) return false;
  } else {
    std::string text;
    if (ReadWhole(marker, kMaxOwnerManifestBytes, &text) != ReadStatus::kOk) return false;
    if (!IsTornRecordOfOurs(text, expected)) return false;  // a stranger's: left as it is
    head = std::move(text);
  }
  if (truncated != nullptr) *truncated = cut;
  // The old bytes are COPIED to `<marker>.bad` (never linked: shared storage may refuse links), and the fresh empty marker replaces the
  // marker in one rename. Each temporary has a literal name and is cleared by name on any failure before the last rename. An earlier
  // `.bad` is replaced only by the rename of a finished copy. The marker name is never absent.
  const std::string aside = marker + ".bad";
  const std::string fresh = marker + ".new";
  const std::string copy = aside + ".new";
  if (!RemoveLeftover(fresh) || !RemoveLeftover(copy)) return false;
  bool ok = WriteFreshFile(fresh, "") && WriteFreshFile(copy, head);
  if (ok) ok = ::rename(copy.c_str(), aside.c_str()) == 0;
  if (ok) ok = ::rename(fresh.c_str(), marker.c_str()) == 0;
  if (!ok) {
    static_cast<void>(RemoveLeftover(fresh));
    static_cast<void>(RemoveLeftover(copy));
    return false;
  }
  // The rename is done; a failed directory sync only means it might not survive a power loss, and the next run repeats the reset.
  static_cast<void>(SyncDir(DirOf(marker)));
  return true;
}

ItemRemoval RemoveUnionRecord(const std::string& item, const std::vector<std::string>& extra) noexcept {
  const std::string name = BaseOf(item);
  // The names here are the pinned ones (the fetch's staging, the stock cache and partial): their markers are moved aside, not judged.
  if (!ResetUnreadableMarker(item, extra, /*pinned=*/true)) return Stopped(false, "not_owned", name);
  std::vector<std::string> names = extra;
  std::vector<std::string> recorded;
  if (ReadOwnerManifest(item, &recorded)) names.insert(names.end(), recorded.begin(), recorded.end());
  std::sort(names.begin(), names.end());
  names.erase(std::unique(names.begin(), names.end()), names.end());
  return RemoveRecorded(item, names);
}

ItemRemoval ClearLegacyStaging(const std::string& staging, const std::string& part, const ZipLimits& limits,
                               std::string* detail, bool pinned) noexcept {
  const std::string name = BaseOf(staging);
  std::vector<ZipEntry> entries;
  std::vector<std::string> planned;
  const bool listed = ReadZipListing(part, limits, &entries, detail) == ZipStatus::kOk && PlanZipMembers(entries, &planned, detail);
  // A torn record of ours is moved aside first; anything else that does not read is not ours: left, and reported.
  if (!ResetUnreadableMarker(staging, listed ? planned : std::vector<std::string>(), pinned)) return Stopped(true, "not_owned", name);
  struct stat st = {};
  if (LstatState(staging, &st) != 1 || !S_ISDIR(st.st_mode)) return RemoveItem(staging);
  if (!IsOwned(staging)) return Stopped(true, "not_owned", name);
  std::vector<std::string> members;
  ReadOwnerManifest(staging, &members);
  if (!members.empty()) return RemoveItem(staging);
  if (!listed) return Stopped(true, "legacy_no_listing", name);
  return RemoveItem(staging, &planned);
}

bool RecordPackDirectories(const std::string& root, const std::string& android_rel) noexcept {
  std::vector<std::string> names;
  bool present = false;
  if (!ReadLedger(root, &names, &present)) return false;
  for (const char* sub : {kManifestsDir, kPackagesDir}) {
    const std::string record = android_rel + "/" + sub;
    if (!IsPackDirectoryRecord(record)) return false;
    if (std::find(names.begin(), names.end(), record) != names.end()) continue;
    if (!AppendFile(root + "/" + kLedgerName, record + "\n", kMaxLedgerBytes)) return false;
    names.push_back(record);
  }
  return true;
}

int CleanLeftovers(const std::string& root, int* failed, std::vector<std::string>* left) noexcept {
  CleanCounts counts;
  CleanRoot(root, /*staging=*/true, /*packs=*/true, &counts, left);
  if (failed != nullptr) *failed = counts.failed;
  return counts.removed;
}

int CleanPackLeftovers(const std::string& root, int* failed, std::vector<std::string>* left) noexcept {
  CleanCounts counts;
  CleanRoot(root, /*staging=*/false, /*packs=*/true, &counts, left);
  if (failed != nullptr) *failed = counts.failed;
  return counts.removed;
}

bool RecoverReplaced(const std::string& root, std::string* restored_from) noexcept {
  if (restored_from != nullptr) restored_from->clear();
  const std::string target = root + "/_data";
  if (Lexists(target)) return false;
  std::string newest;
  // The replaced names are the ledger's: MarkOwned records each one before its marker is made (InstallStaged makes every replaced
  // directory that way). A listing is never used to choose what is renamed.
  std::vector<std::string> ledger;
  bool present = false;
  if (!ReadLedger(root, &ledger, &present)) return false;
  for (const std::string& name : ledger) {
    if (name.find('/') != std::string::npos || !StartsWith(name, kReplacedPrefix) || EndsWith(name, kOwnerSuffix)) continue;
    const std::string candidate = root + "/" + name;
    if (!IsOwned(candidate)) continue;
    // A real directory only: a link here would make `_data` a way into somewhere else.
    struct stat st;
    if (::lstat(candidate.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) continue;
    if (name > newest) newest = name;  // the stamp is UTC, so names sort by time
  }
  if (newest.empty()) return false;
  const std::string from = root + "/" + newest;
  if (::rename(from.c_str(), target.c_str()) != 0) return false;
  SyncDir(root);
  ::unlink(OwnerMarkerPath(from).c_str());
  SyncDir(root);
  if (restored_from != nullptr) *restored_from = from;
  return true;
}

StageStatus InstallStaged(const std::string& root, const std::string& staging, const std::string& utc_stamp,
                          std::string* replaced_to, const InstallProbe* probe) {
  const auto at = [probe](const char* point) {
    if (probe != nullptr && probe->at != nullptr) probe->at(probe->context, point);
  };
  if (replaced_to != nullptr) replaced_to->clear();
  const std::string target = root + "/_data";
  const bool had_old = Lexists(target);
  std::string aside;
  bool marked_before = false;
  if (had_old) {
    aside = root + "/" + kReplacedPrefix + utc_stamp;
    // The marker first: a crash after the rename must leave the old tree recognisable as ours.
    marked_before = IsOwned(aside);  // the same stamp twice: leave the earlier marker as it was
    if (!MarkOwned(aside)) return StageStatus::kRenameFailed;
    at("before_first_rename");
    if (::rename(target.c_str(), aside.c_str()) != 0) {
      if (!marked_before) ::unlink(OwnerMarkerPath(aside).c_str());
      return StageStatus::kRenameFailed;
    }
    SyncDir(root);
    if (replaced_to != nullptr) *replaced_to = aside;
  }
  at("between_renames");
  if (::rename(staging.c_str(), target.c_str()) != 0) {
    if (had_old && ::rename(aside.c_str(), target.c_str()) == 0) {
      if (!marked_before) ::unlink(OwnerMarkerPath(aside).c_str());
      SyncDir(root);
      if (replaced_to != nullptr) replaced_to->clear();
    }
    return StageStatus::kRenameFailed;
  }
  SyncDir(root);
  ::unlink(OwnerMarkerPath(staging).c_str());  // the staging directory is `_data` now
  SyncDir(root);
  return StageStatus::kOk;
}

StageStatus CheckSpace(const std::string& root, std::uint64_t needed, std::uint64_t* available) {
  struct statvfs vfs;
  if (::statvfs(root.c_str(), &vfs) != 0) return StageStatus::kStatFailed;
  const std::uint64_t free_bytes = static_cast<std::uint64_t>(vfs.f_bavail) * vfs.f_frsize;
  if (available != nullptr) *available = free_bytes;
  return free_bytes >= needed ? StageStatus::kOk : StageStatus::kNoSpace;
}

}  // namespace nevr_quest::data
