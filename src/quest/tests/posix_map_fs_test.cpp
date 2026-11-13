// The Quest's MapFs (quest/cargo/posix_map_fs.h) on real POSIX files under NEVR_TEST_TMP: the stamp, a held file, the sorted
// listing, a removal that never follows a link, and a rename that never replaces. The contract is the PC's (runtime/cargo/win_env.h,
// tested by runtime/tests/win_env_test.cpp), so a case here mirrors one there.
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

#include "quest/cargo/posix_map_fs.h"
#include "quest/tests/test_check.h"

namespace {

using nevr_quest::maps::PosixMapFs;

constexpr mode_t kDirMode = 0700;
constexpr mode_t kFileMode = 0600;

std::string Root() {
  const char* dir = std::getenv("NEVR_TEST_TMP");
  return std::string(dir != nullptr ? dir : "/tmp") + "/posix_map_fs_" + std::to_string(::getpid());
}

void Write(const std::string& path, const std::string& bytes) {
  const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, kFileMode);
  QCHECK(fd >= 0);
  QCHECK(::write(fd, bytes.data(), bytes.size()) == static_cast<ssize_t>(bytes.size()));
  ::close(fd);
}

bool Exists(const std::string& path) {
  struct stat st {};
  return ::lstat(path.c_str(), &st) == 0;
}

nevr_maps::FileStamp StampNow(PosixMapFs& fs, const std::string& path) {
  std::int64_t handle = -1;
  std::uint64_t size = 0;
  nevr_maps::FileStamp stamp;
  QCHECK(fs.OpenHeld(path, &handle, &size));
  QCHECK(fs.StampOf(handle, &stamp));
  ::close(static_cast<int>(handle));
  return stamp;
}

void TheStampMovesWithANewFileAndHoldsOnAReopen(PosixMapFs& fs, const std::string& root) {
  const std::string path = root + "/map.blob";
  Write(path, "version one");
  const nevr_maps::FileStamp first = StampNow(fs, path);
  QCHECK(first.size == 11);
  QCHECK(StampNow(fs, path) == first);
  // A new file renamed over the old one: another inode, so another stamp, whatever its bytes.
  Write(root + "/map.blob.new", "version one");
  QCHECK(::rename((root + "/map.blob.new").c_str(), path.c_str()) == 0);
  const nevr_maps::FileStamp replaced = StampNow(fs, path);
  QCHECK(replaced != first);
  QCHECK(replaced.id_lo != first.id_lo);
  nevr_maps::FileStamp none;
  QCHECK(!fs.StampOf(-1, &none));
}

void AHeldFileIsAPlainFileNeverALink(PosixMapFs& fs, const std::string& root) {
  const std::string path = root + "/held.blob";
  Write(path, "mounted bytes");
  std::int64_t handle = -1;
  std::uint64_t size = 0;
  QCHECK(fs.OpenHeld(path, &handle, &size));
  QCHECK(size == 13);
  QCHECK(fs.FinalPathOf(handle).size() >= path.size());
  QCHECK(fs.FinalPathOf(handle).compare(fs.FinalPathOf(handle).size() - std::string("/held.blob").size(), std::string::npos,
                                        "/held.blob") == 0);
  ::close(static_cast<int>(handle));
  QCHECK(::symlink(path.c_str(), (root + "/link.blob").c_str()) == 0);
  QCHECK(!fs.OpenHeld(root + "/link.blob", &handle, &size));
  QCHECK(!fs.OpenHeld(root, &handle, &size));
  QCHECK(!fs.OpenHeld(root + "/absent.blob", &handle, &size));
  QCHECK(fs.FinalPathOf(-1).empty());
}

void ListDirIsSortedAndRenameDirNeverReplaces(PosixMapFs& fs, const std::string& root) {
  QCHECK(::mkdir((root + "/a.tmp").c_str(), kDirMode) == 0);
  QCHECK(::mkdir((root + "/a.tmp/scripts").c_str(), kDirMode) == 0);
  Write(root + "/a.tmp/zeta.json", "{}");
  Write(root + "/a.tmp/package.json", "{}");
  std::vector<std::string> names;
  QCHECK(fs.ListDir(root + "/a.tmp", &names));
  QCHECK(names == (std::vector<std::string>{"package.json", "scripts", "zeta.json"}));
  QCHECK(!fs.ListDir(root + "/absent", &names));

  QCHECK(fs.RenameDir(root + "/a.tmp", root + "/a"));
  QCHECK(Exists(root + "/a/package.json"));
  QCHECK(::mkdir((root + "/b").c_str(), kDirMode) == 0);
  QCHECK(!fs.RenameDir(root + "/b", root + "/a"));  // never over an existing folder, empty or not
  QCHECK(::mkdir((root + "/empty").c_str(), kDirMode) == 0);
  QCHECK(!fs.RenameDir(root + "/b", root + "/empty"));
  QCHECK(!fs.RenameDir(root + "/absent", root + "/c"));
}

void RemoveTreeRemovesALinkNotItsTarget(PosixMapFs& fs, const std::string& root) {
  QCHECK(::mkdir((root + "/outside").c_str(), kDirMode) == 0);
  Write(root + "/outside/keep.txt", "keep");
  QCHECK(::mkdir((root + "/tree").c_str(), kDirMode) == 0);
  QCHECK(::mkdir((root + "/tree/set").c_str(), kDirMode) == 0);
  Write(root + "/tree/set/a.nevrres", "set");
  QCHECK(::symlink((root + "/outside").c_str(), (root + "/tree/link").c_str()) == 0);
  QCHECK(fs.RemoveTree(root + "/tree"));
  QCHECK(!Exists(root + "/tree"));
  QCHECK(Exists(root + "/outside/keep.txt"));
  QCHECK(fs.RemoveTree(root + "/tree"));  // already gone
  // A link to a folder, given as the tree: the link goes and the folder stays.
  QCHECK(::symlink((root + "/outside").c_str(), (root + "/outside_link").c_str()) == 0);
  QCHECK(fs.RemoveTree(root + "/outside_link"));
  QCHECK(!Exists(root + "/outside_link"));
  QCHECK(Exists(root + "/outside/keep.txt"));
  QCHECK(!fs.RemoveTree(root + "/outside/keep.txt"));  // a file is not a tree
  QCHECK(Exists(root + "/outside/keep.txt"));
}

}  // namespace

int main() {
  const std::string root = Root();
  PosixMapFs fs;
  QCHECK(fs.RemoveTree(root));
  QCHECK(::mkdir(root.c_str(), kDirMode) == 0);
  TheStampMovesWithANewFileAndHoldsOnAReopen(fs, root);
  AHeldFileIsAPlainFileNeverALink(fs, root);
  ListDirIsSortedAndRenameDirNeverReplaces(fs, root);
  RemoveTreeRemovesALinkNotItsTarget(fs, root);
  QCHECK(fs.RemoveTree(root));
  return quest_test::Failures() == 0 ? 0 : 1;
}
