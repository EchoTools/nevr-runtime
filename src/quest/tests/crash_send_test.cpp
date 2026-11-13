// The crash report sender's file functions on a POSIX host (src/core/crash_send.h, PosixFiles): the Quest sentinel uses them where the
// PC host uses its own wide Win32 ones (src/runtime/host/crash_send_files.cpp), so this is the test of the Quest's file work. It
// runs the behaviours the sender had when it used std::filesystem: age order, only regular report files, a byte limit on a read, the
// .sent mark, the install id made once, and a whole pass through SendPending with no Files given.

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utime.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "core/crash_report.h"
#include "core/crash_send.h"
#include "quest/tests/test_check.h"

namespace {

constexpr std::size_t kLongFileBytes = 100;
constexpr std::size_t kSmallLimit = 10;
constexpr std::size_t kListLimit = 2;
constexpr std::int64_t kOlder = 1000000000;
constexpr std::int64_t kNewer = 1000000100;
constexpr long kHttpCreated = 201;
constexpr std::size_t kRandomBytes = 16;
constexpr std::size_t kInstallIdChars = 32;
constexpr int kSigsegv = 11;
constexpr std::uint64_t kFakePc = 0x7000001234ull;

bool NoModule(std::uint64_t, nevr_crash::Location*) { return false; }

// A folder of this test's own, made by mkdtemp, emptied by exact name at the end.
struct Folder {
  std::string path;
  std::vector<std::string> made;
  Folder() {
    char templ[] = "/tmp/nevr-crash-send-files-XXXXXX";
    const char* dir = ::mkdtemp(templ);
    if (dir != nullptr) path = dir;
  }
  std::string In(const std::string& name) const { return path + "/" + name; }
  void Write(const std::string& name, const std::string& bytes) {
    std::FILE* f = std::fopen(In(name).c_str(), "wb");
    QCHECK(f != nullptr);
    if (f == nullptr) return;
    std::fwrite(bytes.data(), 1, bytes.size(), f);
    std::fclose(f);
    made.push_back(name);
  }
  void Age(const std::string& name, std::int64_t seconds) const {
    struct utimbuf times;
    times.actime = static_cast<time_t>(seconds);
    times.modtime = static_cast<time_t>(seconds);
    QCHECK(::utime(In(name).c_str(), &times) == 0);
  }
  void Directory(const std::string& name) {
    QCHECK(::mkdir(In(name).c_str(), 0700) == 0);
    made.push_back(name);
  }
  bool Exists(const std::string& name) const {
    struct stat st = {};
    return ::stat(In(name).c_str(), &st) == 0;
  }
  ~Folder() {
    // Every name the folder could hold, by exact name: what this test made, and what the sender renamed it to.
    for (const std::string& name : made) {
      ::unlink(In(name).c_str());
      ::rmdir(In(name).c_str());
      ::unlink(In(name + nevr_crash_send::kSentSuffix).c_str());
      ::unlink(In(name + nevr_crash_send::kRejectedSuffix).c_str());
    }
    ::unlink(In(nevr_crash_send::kInstallIdName).c_str());
    ::unlink(In(std::string(nevr_crash_send::kInstallIdName) + ".tmp").c_str());
    ::rmdir(path.c_str());
  }
};

const std::string kReportText = "x";

bool FakeRandom(std::uint8_t* bytes, std::size_t count) {
  for (std::size_t i = 0; i < count; ++i) bytes[i] = static_cast<std::uint8_t>(i);
  return true;
}

int g_randomCalls = 0;
bool CountingRandom(std::uint8_t* bytes, std::size_t count) {
  ++g_randomCalls;
  return FakeRandom(bytes, count);
}

bool RenameFails(const std::string&, const std::string&) { return false; }

int g_posts = 0;
nevr_crash_send::PostResult FakePost(void*, const std::string&, const std::string&, const nevr_crash_send::Timeouts&) {
  ++g_posts;
  return nevr_crash_send::PostResult{true, kHttpCreated, 0};
}

void OldestReportGoesFirstAndOnlyRegularReportFilesAreListed() {
  Folder folder;
  QCHECK(!folder.path.empty());
  folder.Write("zeta.report.txt", kReportText);
  folder.Write("alpha.report.txt", kReportText);
  folder.Age("zeta.report.txt", kOlder);
  folder.Age("alpha.report.txt", kNewer);
  folder.Directory("dir.report.txt");
  folder.Write("note.txt", kReportText);
  folder.Write("alpha.report.txt.sent", kReportText);
  const std::vector<nevr_crash_send::Pending> pending = nevr_crash_send::FindPending(*nevr_crash_send::PosixFiles(), folder.path);
  QCHECK(pending.size() == 2);  // the directory, the other name and the already-sent file are not reports
  if (pending.size() == 2) {
    QCHECK(pending[0].stem == "zeta");  // older first although its name sorts last
    QCHECK(pending[1].stem == "alpha");
  }
  // Equal ages order by name.
  folder.Age("alpha.report.txt", kOlder);
  const std::vector<nevr_crash_send::Pending> tied = nevr_crash_send::FindPending(*nevr_crash_send::PosixFiles(), folder.path);
  QCHECK(tied.size() == 2);
  if (tied.size() == 2) QCHECK(tied[0].stem == "alpha");
  // A folder that is not there has none.
  QCHECK(nevr_crash_send::FindPending(*nevr_crash_send::PosixFiles(), folder.In("absent")).empty());
}

void ListStopsAtItsLimit() {
  Folder folder;
  folder.Write("a", kReportText);
  folder.Write("b", kReportText);
  folder.Write("c", kReportText);
  std::vector<nevr_crash_send::DirectoryEntry> entries;
  QCHECK(nevr_crash_send::PosixFiles()->list(folder.path, kListLimit, &entries));
  QCHECK(entries.size() == kListLimit);
}

void AReadIsBoundedAndAMissingFileIsFalse() {
  Folder folder;
  folder.Write("long", std::string(kLongFileBytes, 'q'));
  std::string text;
  QCHECK(nevr_crash_send::ReadFileBounded(*nevr_crash_send::PosixFiles(), folder.In("long"), kSmallLimit, &text));
  QCHECK(text == std::string(kSmallLimit, 'q'));
  QCHECK(nevr_crash_send::ReadFileBounded(*nevr_crash_send::PosixFiles(), folder.In("long"), kLongFileBytes * 2, &text));
  QCHECK(text.size() == kLongFileBytes);
  QCHECK(!nevr_crash_send::ReadFileBounded(*nevr_crash_send::PosixFiles(), folder.In("absent"), kSmallLimit, &text));
}

void MarkRenamesTheReportToItsSuffixedName() {
  Folder folder;
  folder.Write("a.report.txt", kReportText);
  nevr_crash_send::Pending pending;
  pending.report = folder.In("a.report.txt");
  pending.stem = "a";
  QCHECK(nevr_crash_send::Mark(*nevr_crash_send::PosixFiles(), pending, nevr_crash_send::kSentSuffix));
  QCHECK(!folder.Exists("a.report.txt"));
  QCHECK(folder.Exists(std::string("a.report.txt") + nevr_crash_send::kSentSuffix));
  QCHECK(!nevr_crash_send::Mark(*nevr_crash_send::PosixFiles(), pending, nevr_crash_send::kSentSuffix));  // it is gone now
}

void TheInstallIdIsMadeOnceAndKept() {
  Folder folder;
  g_randomCalls = 0;
  const std::string first = nevr_crash_send::LoadOrCreateInstallId(*nevr_crash_send::PosixFiles(), folder.path, &CountingRandom);
  QCHECK(first.size() == kInstallIdChars);
  QCHECK(nevr_crash_send::IsInstallId(first));
  const std::string second = nevr_crash_send::LoadOrCreateInstallId(*nevr_crash_send::PosixFiles(), folder.path, &CountingRandom);
  QCHECK(second == first);
  QCHECK(g_randomCalls == 1);
  QCHECK(!folder.Exists(std::string(nevr_crash_send::kInstallIdName) + ".tmp"));  // the temporary file is renamed, not left
  // No random source, nothing made.
  Folder empty;
  QCHECK(nevr_crash_send::LoadOrCreateInstallId(*nevr_crash_send::PosixFiles(), empty.path, nullptr).empty());
  QCHECK(!empty.Exists(nevr_crash_send::kInstallIdName));
  // A folder that cannot take the file makes no id.
  QCHECK(nevr_crash_send::LoadOrCreateInstallId(*nevr_crash_send::PosixFiles(), empty.In("absent"), &FakeRandom).empty());
}

// A report the real writer's format gives, with no module known for any address (the parser takes it as `unknown`).
std::string RealReportText() {
  nevr_crash::ResetForTest();
  nevr_crash::Report report;
  std::memset(&report, 0, sizeof(report));
  report.signal_name = "SIGSEGV";
  report.signal_number = kSigsegv;
  report.fault_code = 1;
  report.pc = kFakePc;
  report.frames[0] = report.pc;
  report.frame_count = 1;
  static char buffer[nevr_crash::kReportCapacity];
  nevr_crash::FormatReport(report, &NoModule, "crashes/nevr-crash-1.report.txt", buffer, sizeof(buffer));
  return std::string(buffer);
}

void AFailedRenameLeavesNoTemporaryInstallIdFile() {
  Folder folder;
  nevr_crash_send::Files files = *nevr_crash_send::PosixFiles();
  files.rename = &RenameFails;
  QCHECK(nevr_crash_send::LoadOrCreateInstallId(files, folder.path, &FakeRandom).empty());
  QCHECK(!folder.Exists(std::string(nevr_crash_send::kInstallIdName) + ".tmp"));
  QCHECK(!folder.Exists(nevr_crash_send::kInstallIdName));
}

void APassWithNoFilesGivenUsesThePosixOnesAndMarksTheReportSent() {
  Folder folder;
  folder.Write("a.report.txt", RealReportText());
  g_posts = 0;
  nevr_crash_send::Config config;
  config.dir = folder.path;  // files stays null: the header's POSIX functions do the work
  config.url = "https://service.invalid/api/v3/crash-reports";
  config.build = "5.7.1";
  config.platform = "quest";
  config.post = &FakePost;
  config.random = &FakeRandom;
  const nevr_crash_send::Outcome outcome = nevr_crash_send::SendPending(config);
  QCHECK(outcome.sent == 1);
  QCHECK(g_posts == 1);
  QCHECK(!folder.Exists("a.report.txt"));
  QCHECK(folder.Exists(std::string("a.report.txt") + nevr_crash_send::kSentSuffix));
  QCHECK(folder.Exists(nevr_crash_send::kInstallIdName));
}

void JoinPathUsesTheFoldersOwnSeparator() {
  QCHECK(nevr_crash_send::JoinPath("/data/crashes", "a") == "/data/crashes/a");
  QCHECK(nevr_crash_send::JoinPath("/data/crashes/", "a") == "/data/crashes/a");
  QCHECK(nevr_crash_send::JoinPath("C:\\crashes", "a") == "C:\\crashes\\a");
  QCHECK(nevr_crash_send::JoinPath("", "a") == "a");
}

}  // namespace

int main() {
  OldestReportGoesFirstAndOnlyRegularReportFilesAreListed();
  ListStopsAtItsLimit();
  AReadIsBoundedAndAMissingFileIsFalse();
  MarkRenamesTheReportToItsSuffixedName();
  TheInstallIdIsMadeOnceAndKept();
  AFailedRenameLeavesNoTemporaryInstallIdFile();
  APassWithNoFilesGivenUsesThePosixOnesAndMarksTheReportSent();
  JoinPathUsesTheFoldersOwnSeparator();
  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "crash_send_test: %d failure(s)\n", quest_test::Failures());
    return 1;
  }
  std::puts("crash_send_test: all checks ran");
  return 0;
}
