// The POSIX UpdateFs (src/core/update/fs_posix.cpp) on a temporary nevr root, on the Linux host: each operation the engine asks of
// it; a link refused at each folder and at the file; the lock held by another process until it times out, and retried in the
// meantime; a move that is a rename and never an overwrite; a move across two volumes reported as EXDEV. The lock's timeout and
// step are the test's own, given to TakeLock. Plain main (quest/tests/test_check.h), no game stub, no Log().

#include <dirent.h>
#include <fcntl.h>
#include <sched.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>

#include "core/update/fs.h"
#include "core/update/names.h"
#include "quest/tests/test_check.h"

namespace {

namespace names = nevr_update_names;
using nevr_update::ReadResult;
using nevr_update::UpdateFs;

// The lock's timeout and step as this test gives them to TakeLock.
constexpr std::uint32_t kTestLockTimeoutMs = 2000;
constexpr std::uint32_t kTestLockStepMs = 10;
constexpr std::uint32_t kNoWaitMs = 0;
constexpr mode_t kFolderMode = 0700;
constexpr std::size_t kTwoEntries = 2;
constexpr std::size_t kOneMoreByte = 1;
constexpr int kPipeEnds = 2;
constexpr int kExitPassed = 0;
constexpr int kExitFailed = 1;
constexpr int kExitNotRun = 2;
constexpr std::size_t kMapLineBytes = 64;

// Whether anything, a file, a folder or a link, is at `path`. lstat: a link counts and is not followed.
bool Present(const std::string& path) {
  struct stat st{};
  return ::lstat(path.c_str(), &st) == 0;
}

// The entries of a folder, `.` and `..` excluded.
std::size_t CountEntries(const std::string& path) {
  DIR* dir = ::opendir(path.c_str());
  if (dir == nullptr) return 0;
  std::size_t count = 0;
  while (const dirent* entry = ::readdir(dir)) {
    if (std::strcmp(entry->d_name, ".") != 0 && std::strcmp(entry->d_name, "..") != 0) ++count;
  }
  ::closedir(dir);
  return count;
}

// The bytes of the file at `path`, or "<absent>" when it cannot be opened.
std::string Slurp(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return "<absent>";
  std::ostringstream text;
  text << in.rdbuf();
  return text.str();
}

std::uint64_t ElapsedMs(std::chrono::steady_clock::time_point start) {
  const std::chrono::milliseconds spent =
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
  return static_cast<std::uint64_t>(spent.count());
}

// Removes `path` by exact name, never entering a link: lstat sees a link, and a link is unlinked itself.
void RemoveAll(const std::string& path) {
  struct stat st{};
  if (::lstat(path.c_str(), &st) != 0) return;
  if (!S_ISDIR(st.st_mode)) {
    static_cast<void>(::unlink(path.c_str()));
    return;
  }
  if (DIR* dir = ::opendir(path.c_str())) {
    while (const dirent* entry = ::readdir(dir)) {
      if (std::strcmp(entry->d_name, ".") == 0 || std::strcmp(entry->d_name, "..") == 0) continue;
      RemoveAll(path + "/" + entry->d_name);
    }
    ::closedir(dir);
  }
  static_cast<void>(::rmdir(path.c_str()));
}

// A nevr root of this test's own, made by mkdtemp and removed at the end.
class Root {
 public:
  Root() {
    char templ[] = "/tmp/nevr-update-fs-posix-XXXXXX";
    const char* made = ::mkdtemp(templ);
    QCHECK(made != nullptr);
    if (made != nullptr) path_ = made;
  }
  ~Root() { RemoveAll(path_); }
  Root(const Root&) = delete;
  Root& operator=(const Root&) = delete;

  const std::string& Path() const { return path_; }
  std::string At(const std::string& rel) const { return path_ + "/" + rel; }

  void Mkdir(const std::string& rel) const { QCHECK(::mkdir(At(rel).c_str(), kFolderMode) == 0); }

  void Write(const std::string& rel, const std::string& text) const {
    std::FILE* f = std::fopen(At(rel).c_str(), "wb");
    QCHECK(f != nullptr);
    if (f == nullptr) return;
    QCHECK(std::fwrite(text.data(), 1, text.size(), f) == text.size());
    std::fclose(f);
  }

  // A symbolic link at `link`, pointing at `target` (both relative to the root).
  void Link(const std::string& target, const std::string& link) const {
    QCHECK(::symlink(At(target).c_str(), At(link).c_str()) == 0);
  }

 private:
  std::string path_;
};

std::unique_ptr<UpdateFs> MakeFs(const Root& root) {
  std::unique_ptr<UpdateFs> fs = nevr_update::MakePosixUpdateFs(root.Path());
  QCHECK(fs != nullptr);
  return fs;
}

void ReadSmallFileTellsPresentAbsentAndTooBig() {
  Root root;
  root.Mkdir(names::kUpdateDir);
  root.Write("update/a.json", "{}");
  auto fs = MakeFs(root);
  std::string text;
  QCHECK(fs->ReadSmallFile(root.At("update/a.json"), &text) == ReadResult::kOk);
  QCHECK(text == "{}");
  QCHECK(fs->ReadSmallFile(root.At("update/none.json"), &text) == ReadResult::kAbsent);
  QCHECK(fs->ReadSmallFile(root.At("no-folder/none.json"), &text) == ReadResult::kAbsent);
  root.Write("update/big.json", std::string(names::kMaxJsonBytes + kOneMoreByte, 'x'));
  QCHECK(fs->ReadSmallFile(root.At("update/big.json"), &text) == ReadResult::kTooBig);
  root.Write("update/edge.json", std::string(names::kMaxJsonBytes, 'x'));
  QCHECK(fs->ReadSmallFile(root.At("update/edge.json"), &text) == ReadResult::kOk);
  QCHECK(text.size() == names::kMaxJsonBytes);
}

// sysfs reports a page as the size of an attribute whatever its text, so a read of a short attribute ends before its size: the file
// is damaged (kTooBig, as the PC host reads it), not unreadable. Not run where no such attribute is shorter than its size.
void ReadSmallFileTellsAFileThatEndsEarlyAsDamaged() {
  const std::string attribute = "/sys/devices/system/cpu/online";
  struct stat st{};
  if (::stat(attribute.c_str(), &st) != 0 || static_cast<std::uintmax_t>(st.st_size) <= Slurp(attribute).size()) {
    std::fprintf(stderr, "fs_posix_test: short-read case not run: no sysfs attribute shorter than its reported size\n");
    return;
  }
  std::unique_ptr<UpdateFs> fs = nevr_update::MakePosixUpdateFs("/sys/devices/system/cpu");
  QCHECK(fs != nullptr);
  if (fs == nullptr) return;
  std::string text;
  QCHECK(fs->ReadSmallFile(attribute, &text) == ReadResult::kTooBig);
}

// The control reads through no link. Then a link at the first folder, one at the second, and one at the file: each refused.
void ReadRefusesALinkAtEachFolderAndAtTheFile() {
  Root root;
  root.Mkdir("real");
  root.Write("real/x.json", "{}");
  root.Mkdir("d1");
  root.Mkdir("d1/d2");
  root.Write("d1/d2/x.json", "{}");
  root.Link("real", "link1");
  root.Mkdir("d3");
  root.Link("real", "d3/link2");
  root.Link("real/x.json", "d1/d2/leaf.json");
  auto fs = MakeFs(root);
  std::string text;
  QCHECK(fs->ReadSmallFile(root.At("d1/d2/x.json"), &text) == ReadResult::kOk);
  QCHECK(fs->ReadSmallFile(root.At("link1/x.json"), &text) == ReadResult::kUnreadable);
  QCHECK(fs->ReadSmallFile(root.At("d3/link2/x.json"), &text) == ReadResult::kUnreadable);
  QCHECK(fs->ReadSmallFile(root.At("d1/d2/leaf.json"), &text) == ReadResult::kUnreadable);
}

void WriteByRenameReplacesTheFileAndLeavesNoTemporary() {
  Root root;
  root.Mkdir("update");
  root.Write("update/state.json", "old");
  auto fs = MakeFs(root);
  QCHECK(fs->WriteByRename(root.At("update/state.json"), "new"));
  QCHECK(Slurp(root.At("update/state.json")) == "new");
  QCHECK(fs->WriteByRename(root.At("update/fresh.json"), ""));
  QCHECK(Slurp(root.At("update/fresh.json")) == "");
  QCHECK(CountEntries(root.At("update")) == kTwoEntries);  // a temporary left behind would be a third entry
}

// A link at the folder: refused. A link at the file: the write replaces the link itself, and its target is not written.
void WriteByRenameWritesNothingThroughALink() {
  Root root;
  root.Mkdir("real");
  root.Write("real/state.json", "keep");
  root.Mkdir("update");
  root.Link("real", "linkdir");
  root.Link("real/state.json", "update/state.json");
  auto fs = MakeFs(root);
  QCHECK(!fs->WriteByRename(root.At("linkdir/state.json"), "x"));
  QCHECK(Slurp(root.At("real/state.json")) == "keep");
  fs->WriteByRename(root.At("update/state.json"), "x");
  QCHECK(Slurp(root.At("real/state.json")) == "keep");
}

void RemoveFileRemovesAFileAndNeverFollowsALink() {
  Root root;
  root.Mkdir("update");
  root.Write("update/pending.json", "{}");
  root.Mkdir("real");
  root.Write("real/keep.json", "keep");
  root.Link("real", "linkdir");
  root.Link("real/keep.json", "update/alias.json");
  auto fs = MakeFs(root);
  QCHECK(!fs->RemoveFile(root.At("linkdir/keep.json")));  // a link at the folder: refused
  QCHECK(Slurp(root.At("real/keep.json")) == "keep");
  QCHECK(fs->RemoveFile(root.At("update/alias.json")));   // a link at the file goes; its target stays
  QCHECK(!Present(root.At("update/alias.json")));
  QCHECK(Slurp(root.At("real/keep.json")) == "keep");
  QCHECK(fs->RemoveFile(root.At("update/pending.json")));
  QCHECK(!fs->RemoveFile(root.At("update/pending.json")));  // absent
}

void RemoveTreeRemovesAFolderAndNeverEntersALink() {
  Root root;
  root.Mkdir("outside");
  root.Write("outside/keep.txt", "keep");
  root.Mkdir("staging");
  root.Mkdir("staging/1.0.0");
  root.Write("staging/1.0.0/nevr-runtime.so", "x");
  root.Link("outside", "staging/1.0.0/out");  // a link inside the tree
  root.Mkdir("real2");
  root.Mkdir("real2/tree");
  root.Link("real2", "lnk");                  // a link on the way to the tree
  root.Link("outside", "linked");             // a link at the folder itself
  auto fs = MakeFs(root);
  fs->RemoveTree(root.At("lnk/tree"));
  QCHECK(Present(root.At("real2/tree")));  // refused: a link on the way
  fs->RemoveTree(root.At("linked"));
  QCHECK(!Present(root.At("linked")));    // the link itself is removed
  QCHECK(Slurp(root.At("outside/keep.txt")) == "keep");
  fs->RemoveTree(root.At("staging"));
  QCHECK(!Present(root.At("staging")));
  QCHECK(Slurp(root.At("outside/keep.txt")) == "keep");  // the link inside the tree was not entered
}

void IsLinkAndExistsTellFilesFoldersLinksAndNothing() {
  Root root;
  root.Write("f", "x");
  root.Mkdir("d");
  root.Link("d", "l");
  auto fs = MakeFs(root);
  QCHECK(fs->Exists(root.At("f")));
  QCHECK(fs->Exists(root.At("d")));
  QCHECK(fs->Exists(root.At("l")));
  QCHECK(!fs->Exists(root.At("none")));
  QCHECK(!fs->IsLink(root.At("f")));
  QCHECK(!fs->IsLink(root.At("d")));
  QCHECK(fs->IsLink(root.At("l")));
  QCHECK(!fs->IsLink(root.At("none")));
  QCHECK(fs->Exists(root.At("l/none")));  // a link on the way: something is there, so the engine waits
  QCHECK(fs->IsLink(root.At("l/none")));  // and writes through nothing
}

// The update folder is a link: the lock is refused at once with ELOOP, not waited for until the timeout.
void ALockInALinkedUpdateFolderIsRefusedAtOnce() {
  Root root;
  root.Mkdir("real");
  root.Link("real", "update");
  auto fs = MakeFs(root);
  int error = 0;
  const std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
  auto lock = fs->TakeLock(root.At("update"), kTestLockTimeoutMs, kTestLockStepMs, &error);
  QCHECK(lock == nullptr);
  QCHECK(error == ELOOP);
  QCHECK(ElapsedMs(started) < kTestLockTimeoutMs);
}

void TheLockIsExclusiveAndLetGoByItsOwner() {
  Root root;
  root.Mkdir("update");
  auto fs = MakeFs(root);
  int error = 0;
  auto first = fs->TakeLock(root.At("update"), kNoWaitMs, kTestLockStepMs, &error);
  QCHECK(first != nullptr);
  int other = 0;
  QCHECK(fs->TakeLock(root.At("update"), kNoWaitMs, kTestLockStepMs, &other) == nullptr);
  QCHECK(other == EWOULDBLOCK);
  if (first != nullptr) first->Release();
  auto second = fs->TakeLock(root.At("update"), kNoWaitMs, kTestLockStepMs, &error);
  QCHECK(second != nullptr);
}

// A second process holds update/lock. This process waits the engine's whole timeout, in steps, and is refused with EWOULDBLOCK;
// then the holder lets go and the lock is taken.
void ALockHeldByAnotherProcessTimesOutAndIsRetried() {
  Root root;
  root.Mkdir("update");
  int ready[kPipeEnds];
  int go[kPipeEnds];
  QCHECK(::pipe(ready) == 0);
  QCHECK(::pipe(go) == 0);
  const pid_t child = ::fork();
  QCHECK(child >= 0);
  if (child < 0) return;
  if (child == 0) {
    // The holder: takes update/lock through the same implementation, reports whether it did, and holds it until `go` is closed.
    ::close(ready[0]);
    ::close(go[1]);
    auto fs = nevr_update::MakePosixUpdateFs(root.Path());
    int error = 0;
    auto lock = fs->TakeLock(root.At("update"), kTestLockTimeoutMs, kTestLockStepMs, &error);
    const char byte = lock != nullptr ? 'y' : 'n';
    static_cast<void>(::write(ready[1], &byte, 1));
    char end = 0;
    static_cast<void>(::read(go[0], &end, 1));
    ::_exit(lock != nullptr ? kExitPassed : kExitFailed);
  }
  ::close(ready[1]);
  ::close(go[0]);
  char holder = 0;
  QCHECK(::read(ready[0], &holder, 1) == 1 && holder == 'y');
  ::close(ready[0]);
  auto fs = MakeFs(root);
  int error = 0;
  const std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
  auto lock = fs->TakeLock(root.At("update"), kTestLockTimeoutMs, kTestLockStepMs, &error);
  const std::uint64_t waited = ElapsedMs(started);
  QCHECK(lock == nullptr);
  QCHECK(error == EWOULDBLOCK);
  QCHECK(waited >= kTestLockTimeoutMs);  // it was retried until the timeout, not refused at once
  ::close(go[1]);                     // the holder lets go and exits
  int status = 0;
  QCHECK(::waitpid(child, &status, 0) == child);
  QCHECK(WIFEXITED(status) && WEXITSTATUS(status) == kExitPassed);
  auto after = fs->TakeLock(root.At("update"), kNoWaitMs, kTestLockStepMs, &error);
  QCHECK(after != nullptr);
}

void MoveMovesWithoutACopyAndNeverOverwrites() {
  Root root;
  root.Mkdir("update");
  root.Write("update/staged.json", "new");
  root.Write("update/state.json", "old");
  auto fs = MakeFs(root);
  int error = 0;
  QCHECK(fs->Move(root.At("update/staged.json"), root.At("update/moved.json"), &error));
  QCHECK(!Present(root.At("update/staged.json")));  // moved, not copied
  QCHECK(Slurp(root.At("update/moved.json")) == "new");
  root.Write("update/again.json", "again");
  error = 0;
  QCHECK(!fs->Move(root.At("update/again.json"), root.At("update/state.json"), &error));  // a file is where it goes
  QCHECK(error == EEXIST);
  QCHECK(Slurp(root.At("update/state.json")) == "old");
  QCHECK(Slurp(root.At("update/again.json")) == "again");
  QCHECK(!fs->Move(root.At("update/none.json"), root.At("update/x.json"), &error));  // absent
  QCHECK(error == ENOENT);
}

void MoveRefusesALinkAtEachFolderAndMovesNothingThroughIt() {
  Root root;
  root.Mkdir("real");
  root.Write("real/a.json", "a");
  root.Mkdir("update");
  root.Write("update/b.json", "b");
  root.Link("real", "linkdir");
  auto fs = MakeFs(root);
  int error = 0;
  QCHECK(!fs->Move(root.At("linkdir/a.json"), root.At("update/a.json"), &error));  // from through a link
  QCHECK(error == ELOOP);
  QCHECK(Slurp(root.At("real/a.json")) == "a");
  QCHECK(!fs->Move(root.At("update/b.json"), root.At("linkdir/b.json"), &error));  // to through a link
  QCHECK(error == ELOOP);
  QCHECK(Slurp(root.At("update/b.json")) == "b");
  QCHECK(!Present(root.At("real/b.json")));
}

bool WriteProcFile(const char* path, const char* text) {
  std::FILE* f = std::fopen(path, "w");
  if (f == nullptr) return false;
  const bool wrote = std::fputs(text, f) >= 0;
  const bool closed = std::fclose(f) == 0;
  return wrote && closed;
}

// The child's half of the EXDEV case: a user and a mount namespace of its own, a tmpfs mounted under the root, then one move from
// the host's file system into the tmpfs. Returns kExitNotRun when this machine does not allow that.
int MoveAcrossAMount(const std::string& root, uid_t uid, gid_t gid) {
  if (::unshare(CLONE_NEWUSER | CLONE_NEWNS) != 0) return kExitNotRun;
  char map[kMapLineBytes];
  if (!WriteProcFile("/proc/self/setgroups", "deny\n")) return kExitNotRun;
  std::snprintf(map, sizeof map, "0 %u 1\n", static_cast<unsigned>(gid));
  if (!WriteProcFile("/proc/self/gid_map", map)) return kExitNotRun;
  std::snprintf(map, sizeof map, "0 %u 1\n", static_cast<unsigned>(uid));
  if (!WriteProcFile("/proc/self/uid_map", map)) return kExitNotRun;
  if (::mount("tmpfs", (root + "/mounted").c_str(), "tmpfs", 0, nullptr) != 0) return kExitNotRun;
  std::unique_ptr<UpdateFs> fs = nevr_update::MakePosixUpdateFs(root);
  if (fs == nullptr) return kExitFailed;
  int error = 0;
  const bool moved = fs->Move(root + "/update/a.json", root + "/mounted/a.json", &error);
  if (moved || error != EXDEV || Slurp(root + "/update/a.json") != "a") return kExitFailed;
  return kExitPassed;
}

void MoveAcrossVolumesIsAFailedMoveWithExdev() {
  Root root;
  root.Mkdir("update");
  root.Write("update/a.json", "a");
  root.Mkdir("mounted");
  const uid_t uid = ::getuid();
  const gid_t gid = ::getgid();
  const pid_t child = ::fork();
  QCHECK(child >= 0);
  if (child < 0) return;
  if (child == 0) ::_exit(MoveAcrossAMount(root.Path(), uid, gid));
  int status = 0;
  QCHECK(::waitpid(child, &status, 0) == child);
  const int code = WIFEXITED(status) ? WEXITSTATUS(status) : kExitFailed;
  if (code == kExitNotRun) {
    std::fprintf(stderr, "fs_posix_test: EXDEV case not run: this machine gives no user and mount namespace of its own\n");
    return;
  }
  QCHECK(code == kExitPassed);
}

// A path outside the root, or one with a `..` name, names nothing: every operation refuses it.
void EveryOperationRefusesAPathOutsideTheRoot() {
  Root root;
  root.Mkdir("update");
  root.Write("update/a.json", "a");
  auto fs = MakeFs(root);
  const std::string outside = root.Path() + "x/a.json";
  const std::string dotdot = root.At("update/../update/a.json");
  std::string text;
  QCHECK(fs->ReadSmallFile(outside, &text) != ReadResult::kOk);
  QCHECK(fs->ReadSmallFile(dotdot, &text) != ReadResult::kOk);
  QCHECK(!fs->WriteByRename(outside, "x"));
  QCHECK(!fs->WriteByRename(dotdot, "x"));
  QCHECK(!fs->RemoveFile(dotdot));
  QCHECK(!fs->Exists(outside));
  QCHECK(!fs->IsLink(outside));
  int error = 0;
  QCHECK(!fs->Move(root.At("update/a.json"), outside, &error));
  QCHECK(Slurp(root.At("update/a.json")) == "a");
  QCHECK(fs->TakeLock(root.Path() + "x", kNoWaitMs, kTestLockStepMs, &error) == nullptr);
}

}  // namespace

int main() {
  ReadSmallFileTellsPresentAbsentAndTooBig();
  ReadSmallFileTellsAFileThatEndsEarlyAsDamaged();
  ReadRefusesALinkAtEachFolderAndAtTheFile();
  WriteByRenameReplacesTheFileAndLeavesNoTemporary();
  WriteByRenameWritesNothingThroughALink();
  RemoveFileRemovesAFileAndNeverFollowsALink();
  RemoveTreeRemovesAFolderAndNeverEntersALink();
  IsLinkAndExistsTellFilesFoldersLinksAndNothing();
  ALockInALinkedUpdateFolderIsRefusedAtOnce();
  TheLockIsExclusiveAndLetGoByItsOwner();
  ALockHeldByAnotherProcessTimesOutAndIsRetried();
  MoveMovesWithoutACopyAndNeverOverwrites();
  MoveRefusesALinkAtEachFolderAndMovesNothingThroughIt();
  MoveAcrossVolumesIsAFailedMoveWithExdev();
  EveryOperationRefusesAPathOutsideTheRoot();
  if (quest_test::Failures() == 0) std::printf("fs_posix_test: all checks passed\n");
  return quest_test::Failures() == 0 ? kExitPassed : kExitFailed;
}
