// The owned-removal guard on a POSIX host (core/owned_remove.h, owned_remove_posix.cpp), and its platform-free rules
// (core/owned_remove_rules.h). Each case makes its own folders under NEVR_TEST_RUN_ROOT when it is set, else /tmp (the
// pattern of crash_send_test.cpp), and removes everything it made with the guard itself, then the folders by exact name.
//
// Cases: (a) a parent that is a link to a canary folder is refused; (b) invalid entry names are
// refused; (c) a listed folder holding an unlisted file is kept; (d) a link as the last component is unlinked, its target
// kept; (e) a file, an empty folder, and a missing entry; (f) RemoveOwned order; (g) a root that is a link is resolved;
// (h) a parent on another device is refused (skipped with a note when the machine has no second device).

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "core/owned_remove.h"
#include "core/owned_remove_rules.h"
#include "quest/tests/test_check.h"

namespace {

using nevr_owned::Outcome;
using nevr_owned::Result;
using nevr_owned::RemoveEntry;
using nevr_owned::RemoveOwned;

bool Exists(const std::string& path) {
  struct stat st = {};
  return ::lstat(path.c_str(), &st) == 0;
}

void WriteText(const std::string& path, const char* text) {
  std::FILE* f = std::fopen(path.c_str(), "wb");
  QCHECK(f != nullptr);
  if (f == nullptr) return;
  std::fwrite(text, 1, std::strlen(text), f);
  std::fclose(f);
}

std::string ReadText(const std::string& path) {
  std::FILE* f = std::fopen(path.c_str(), "rb");
  if (f == nullptr) return std::string();
  std::string out;
  char buf[256];
  std::size_t n = 0;
  while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
  std::fclose(f);
  return out;
}

// Makes a mkdtemp folder named `prefix` + six characters under `base`; `name` is its last component.
bool MakeFolder(const std::string& base, const char* prefix, std::string* name) {
  std::string templ = base + "/" + prefix + "XXXXXX";
  std::vector<char> buf(templ.begin(), templ.end());
  buf.push_back('\0');
  if (::mkdtemp(buf.data()) == nullptr) return false;
  *name = std::string(buf.data()).substr(base.size() + 1);
  return true;
}

struct Sandbox {
  std::string base;         // NEVR_TEST_RUN_ROOT, or /tmp
  std::string run_name;     // the root of every entry under test
  std::string canary_name;  // a folder outside the root, holding `keep`
  std::string run;
  std::string canary;
  bool ok = false;
};

Sandbox MakeSandbox() {
  Sandbox sb;
  const char* env = std::getenv("NEVR_TEST_RUN_ROOT");
  sb.base = (env != nullptr && env[0] != '\0') ? env : "/tmp";
  if (!MakeFolder(sb.base, "nevr-owned-run-", &sb.run_name)) return sb;
  if (!MakeFolder(sb.base, "nevr-owned-canary-", &sb.canary_name)) return sb;
  sb.run = sb.base + "/" + sb.run_name;
  sb.canary = sb.base + "/" + sb.canary_name;
  WriteText(sb.canary + "/keep", "keep");
  sb.ok = true;
  return sb;
}

// Removes what the test made, by exact name, through the guard: the canary's `keep`, the canary, then the root.
void Cleanup(const Sandbox& sb) {
  const std::vector<Result> kept = RemoveOwned(sb.canary, {"keep"});
  QCHECK(kept.size() == 1 && kept[0].outcome == Outcome::kRemoved);
  QCHECK(RemoveEntry(sb.base, sb.canary_name).outcome == Outcome::kRemoved);
  QCHECK(RemoveEntry(sb.base, sb.run_name).outcome == Outcome::kRemoved);  // fails (kNotEmpty) if a case left anything
}

void CaseLinkedParentRefused(const Sandbox& sb) {
  const std::string link = sb.run + "/lnk";
  QCHECK(::symlink(sb.canary.c_str(), link.c_str()) == 0);
  const Result r = RemoveEntry(sb.run, "lnk/keep");
  QCHECK(r.outcome == Outcome::kRefusedLink);
  QCHECK(ReadText(sb.canary + "/keep") == "keep");
  QCHECK(RemoveEntry(sb.run, "lnk").outcome == Outcome::kRemoved);  // the link itself, never its target
  QCHECK(ReadText(sb.canary + "/keep") == "keep");
}

void CaseInvalidNamesRefused(const Sandbox& sb) {
  const char* const names[] = {"..", "/abs", "a\\b", "C:x", "", "a//b", "a/./b", "a/"};
  for (const char* name : names) {
    QCHECK(!nevr_owned::ValidEntry(name));
    QCHECK(RemoveEntry(sb.run, name).outcome == Outcome::kRefusedName);
  }
  QCHECK(ReadText(sb.canary + "/keep") == "keep");
}

void CaseListedFolderWithUnlistedFile(const Sandbox& sb) {
  QCHECK(::mkdir((sb.run + "/d").c_str(), 0700) == 0);
  WriteText(sb.run + "/d/extra", "unlisted");
  const Result kept = RemoveEntry(sb.run, "d");
  QCHECK(kept.outcome == Outcome::kNotEmpty);
  QCHECK(Exists(sb.run + "/d/extra"));
  QCHECK(Exists(sb.run + "/d"));
  // The test's own file, removed by its exact name; then the guard removes the now-empty folder.
  QCHECK(::unlink((sb.run + "/d/extra").c_str()) == 0);
  QCHECK(RemoveEntry(sb.run, "d").outcome == Outcome::kRemoved);
}

void CaseLinkAsLastComponent(const Sandbox& sb) {
  QCHECK(::symlink(sb.canary.c_str(), (sb.run + "/lk2").c_str()) == 0);
  QCHECK(RemoveEntry(sb.run, "lk2").outcome == Outcome::kRemoved);
  QCHECK(!Exists(sb.run + "/lk2"));
  QCHECK(ReadText(sb.canary + "/keep") == "keep");
}

void CaseFileEmptyFolderMissing(const Sandbox& sb) {
  WriteText(sb.run + "/f", "x");
  QCHECK(::mkdir((sb.run + "/e").c_str(), 0700) == 0);
  QCHECK(RemoveEntry(sb.run, "f").outcome == Outcome::kRemoved);
  QCHECK(!Exists(sb.run + "/f"));
  QCHECK(RemoveEntry(sb.run, "e").outcome == Outcome::kRemoved);
  QCHECK(!Exists(sb.run + "/e"));
  QCHECK(RemoveEntry(sb.run, "missing").outcome == Outcome::kAbsent);
}

void CaseOwnedOrder(const Sandbox& sb) {
  QCHECK(::mkdir((sb.run + "/t").c_str(), 0700) == 0);
  QCHECK(::mkdir((sb.run + "/t/s").c_str(), 0700) == 0);
  WriteText(sb.run + "/t/s/g", "g");
  WriteText(sb.run + "/t/f", "f");
  const std::vector<Result> results = RemoveOwned(sb.run, {"t/f", "t/s/g", "t/s", "t"});
  QCHECK(results.size() == 4);
  for (const Result& r : results) QCHECK(r.outcome == Outcome::kRemoved);
  QCHECK(!Exists(sb.run + "/t"));
}

// The root is a link to a real folder: the guard opens the root once, following that link, and works under the real folder.
void CaseRootIsLink(const Sandbox& sb) {
  std::string real_name;
  QCHECK(MakeFolder(sb.base, "nevr-owned-real-", &real_name));
  const std::string real_path = sb.base + "/" + real_name;
  const std::string root_link = sb.base + "/" + sb.run_name + "-rootlink";
  QCHECK(::symlink(real_path.c_str(), root_link.c_str()) == 0);
  WriteText(real_path + "/g", "g");
  QCHECK(RemoveEntry(root_link, "g").outcome == Outcome::kRemoved);
  QCHECK(!Exists(real_path + "/g"));
  QCHECK(RemoveEntry(sb.base, sb.run_name + "-rootlink").outcome == Outcome::kRemoved);
  QCHECK(RemoveEntry(sb.base, real_name).outcome == Outcome::kRemoved);
}

// (b, again) escape attempts: a dot-dot component that reaches the canary, and an embedded NUL that hides a dot-dot
// from ValidEntry while c_str() cuts the name to ".." (the NUL bytes are in the std::string, not the C string).
void CaseEscapeAttempts(const Sandbox& sb) {
  QCHECK(RemoveEntry(sb.run, "../" + sb.canary_name + "/keep").outcome == Outcome::kRefusedName);
  QCHECK(ReadText(sb.canary + "/keep") == "keep");

  std::string nul_dots("..", 2);
  nul_dots.push_back('\0');
  nul_dots += "/" + sb.canary_name + "/keep";
  QCHECK(RemoveEntry(sb.run, nul_dots).outcome == Outcome::kRefusedName);
  QCHECK(ReadText(sb.canary + "/keep") == "keep");

  std::string nul_mid("x", 1);
  nul_mid.push_back('\0');
  nul_mid += "y";
  QCHECK(RemoveEntry(sb.run, nul_mid).outcome == Outcome::kRefusedName);

  std::string nul_root = sb.run;
  nul_root.push_back('\0');
  nul_root += "junk";
  QCHECK(RemoveEntry(nul_root, "missing").outcome == Outcome::kRefusedName);
}

// The shapes of a parent and of a missing root: a missing parent, a plain file as a parent, a link to a file as a parent,
// a link at the second parent level, a 300-byte name, a missing root, and a root that is a regular file.
void CaseShapes(const Sandbox& sb) {
  QCHECK(RemoveEntry(sb.run, "nodir/x").outcome == Outcome::kAbsent);

  WriteText(sb.run + "/pf", "p");
  QCHECK(RemoveEntry(sb.run, "pf/x").outcome == Outcome::kAbsent);
  QCHECK(Exists(sb.run + "/pf"));
  QCHECK(::symlink((sb.run + "/pf").c_str(), (sb.run + "/lf").c_str()) == 0);
  QCHECK(RemoveEntry(sb.run, "lf/x").outcome == Outcome::kRefusedLink);
  QCHECK(RemoveEntry(sb.run, "lf").outcome == Outcome::kRemoved);
  QCHECK(RemoveEntry(sb.run, "pf").outcome == Outcome::kRemoved);

  QCHECK(::mkdir((sb.run + "/a").c_str(), 0700) == 0);
  QCHECK(::symlink(sb.canary.c_str(), (sb.run + "/a/ln").c_str()) == 0);
  QCHECK(RemoveEntry(sb.run, "a/ln/keep").outcome == Outcome::kRefusedLink);
  QCHECK(ReadText(sb.canary + "/keep") == "keep");
  QCHECK(RemoveEntry(sb.run, "a/ln").outcome == Outcome::kRemoved);
  QCHECK(RemoveEntry(sb.run, "a").outcome == Outcome::kRemoved);

  const Result long_name = RemoveEntry(sb.run, std::string(300, 'n'));
  QCHECK(long_name.outcome == Outcome::kFailed);
  QCHECK(long_name.os_error == ENAMETOOLONG);

  QCHECK(RemoveEntry(sb.run + "/no-such-root", "x").outcome == Outcome::kAbsent);
  WriteText(sb.run + "/rootfile", "r");
  QCHECK(RemoveEntry(sb.run + "/rootfile", "x").outcome == Outcome::kFailed);
  QCHECK(RemoveEntry(sb.run, "rootfile").outcome == Outcome::kRemoved);
}

// A parent on another device: /proc is its own mount, so "proc/<name>" under "/" must be refused before anything is
// looked up. The name does not exist, so nothing can be removed in any case.
void CaseParentOnOtherDevice() {
  struct stat root_st = {};
  struct stat proc_st = {};
  if (::stat("/", &root_st) != 0 || ::stat("/proc", &proc_st) != 0) {
    std::printf("owned_remove_test: (h) skipped, no /proc to compare devices with\n");
    return;
  }
  if (root_st.st_dev == proc_st.st_dev) {
    std::printf("owned_remove_test: (h) skipped, /proc is on the same device as /\n");
    return;
  }
  const std::string name = "nevr-owned-no-such-" + std::to_string(static_cast<long>(::getpid()));
  QCHECK(RemoveEntry("/", "proc/" + name).outcome == Outcome::kRefusedOutsideRoot);
}

void RulesTests() {
  // ValidEntry: the grammar.
  QCHECK(nevr_owned::ValidEntry("a"));
  QCHECK(nevr_owned::ValidEntry("a/b.c-d_e/f+g"));
  QCHECK(!nevr_owned::ValidEntry(""));
  QCHECK(!nevr_owned::ValidEntry("/a"));
  QCHECK(!nevr_owned::ValidEntry("a\\b"));
  QCHECK(!nevr_owned::ValidEntry("C:x"));
  QCHECK(!nevr_owned::ValidEntry("a:b"));
  QCHECK(!nevr_owned::ValidEntry("a//b"));
  QCHECK(!nevr_owned::ValidEntry("a/./b"));
  QCHECK(!nevr_owned::ValidEntry("a/../b"));
  QCHECK(!nevr_owned::ValidEntry("a/"));
  QCHECK(!nevr_owned::ValidEntry(".."));
  QCHECK(!nevr_owned::ValidEntry("."));

  // SplitEntry: components in order; empty entry, no components.
  const std::vector<std::string> three = nevr_owned::SplitEntry("a/b/c");
  QCHECK(three.size() == 3 && three[0] == "a" && three[1] == "b" && three[2] == "c");
  QCHECK(nevr_owned::SplitEntry("").empty());
  QCHECK(nevr_owned::SplitEntry("solo").size() == 1);

  // FinalPathMatches: case-insensitive, the \\?\ prefix on both or neither.
  const std::vector<std::wstring> xy = {L"x", L"y"};
  QCHECK(nevr_owned::FinalPathMatches(L"\\\\?\\C:\\a\\x\\y", L"\\\\?\\C:\\a", xy));
  QCHECK(nevr_owned::FinalPathMatches(L"\\\\?\\c:\\A\\X\\Y", L"\\\\?\\C:\\a", xy));
  QCHECK(nevr_owned::FinalPathMatches(L"C:\\a\\x\\y", L"C:\\a", xy));
  QCHECK(!nevr_owned::FinalPathMatches(L"C:\\a\\x\\y", L"\\\\?\\C:\\a", xy));
  QCHECK(!nevr_owned::FinalPathMatches(L"\\\\?\\C:\\a\\x\\y", L"C:\\a", xy));
  QCHECK(!nevr_owned::FinalPathMatches(L"\\\\?\\C:\\a\\link\\y", L"\\\\?\\C:\\a", xy));
  QCHECK(nevr_owned::FinalPathMatches(L"\\\\?\\C:\\a", L"\\\\?\\C:\\a", std::vector<std::wstring>()));
  QCHECK(nevr_owned::FinalPathMatches(L"C:\\x", L"C:\\", std::vector<std::wstring>{L"x"}));
  QCHECK(!nevr_owned::FinalPathMatches(L"C:\\a\\x", L"C:\\a", std::vector<std::wstring>{L"x", L".."}));

  // FinalPathMatches, again: an empty part, a "." part, a longer final path, and an empty expectation.
  QCHECK(!nevr_owned::FinalPathMatches(L"C:\\a\\", L"C:\\a", std::vector<std::wstring>{L""}));
  QCHECK(!nevr_owned::FinalPathMatches(L"C:\\a\\.", L"C:\\a", std::vector<std::wstring>{L"."}));
  QCHECK(!nevr_owned::FinalPathMatches(L"\\\\?\\C:\\a\\x\\y\\z", L"\\\\?\\C:\\a", xy));
  QCHECK(!nevr_owned::FinalPathMatches(L"", L"", std::vector<std::wstring>()));
  QCHECK(!nevr_owned::FinalPathMatches(L"C:\\x", L"", std::vector<std::wstring>{L"x"}));

  // ValidEntry: control bytes and an embedded NUL.
  QCHECK(!nevr_owned::ValidEntry(std::string("..\0/x", 6)));
  QCHECK(!nevr_owned::ValidEntry(std::string("x\0y", 3)));
  QCHECK(!nevr_owned::ValidEntry("a\x01" "b"));
  QCHECK(!nevr_owned::ValidEntry("a\x7f" "b"));
  QCHECK(nevr_owned::ValidEntry("a\xc3\xa9" "b"));

  QCHECK(std::strcmp(nevr_owned::OutcomeName(Outcome::kRemoved), "removed") == 0);
  QCHECK(std::strcmp(nevr_owned::OutcomeName(Outcome::kRefusedLink), "refused_link") == 0);
}

}  // namespace

int main() {
  RulesTests();
  Sandbox sb = MakeSandbox();
  if (!sb.ok) {
    std::fprintf(stderr, "owned_remove_test: could not make the test folders under %s\n", sb.base.c_str());
    return 1;
  }
  CaseLinkedParentRefused(sb);
  CaseInvalidNamesRefused(sb);
  CaseListedFolderWithUnlistedFile(sb);
  CaseLinkAsLastComponent(sb);
  CaseFileEmptyFolderMissing(sb);
  CaseOwnedOrder(sb);
  CaseRootIsLink(sb);
  CaseEscapeAttempts(sb);
  CaseShapes(sb);
  CaseParentOnOtherDevice();
  Cleanup(sb);
  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "owned_remove_test: %d check(s) failed\n", quest_test::Failures());
    return 1;
  }
  std::printf("owned_remove_test: all checks passed\n");
  return 0;
}
