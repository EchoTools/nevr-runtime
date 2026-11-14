// The removal order of a test's recorded entries (runtime/tests/run_root_order.h), on the host, and the POSIX guard run over
// that order (core/owned_remove.h). Each case makes its folder under NEVR_TEST_RUN_ROOT when it is set, else /tmp (the same
// handling as owned_remove_test.cpp), and removes what it made by name through the guard.
//
// Cases: (a) a name recorded twice is listed once; (b) deepest first, files before folders at one depth, latest first;
// (c) a folder recorded before its children is still listed after them; (d) an end-to-end tree removed in that order with
// every entry kRemoved; (e) an unlisted file under a recorded folder keeps the folder (kNotEmpty) and the file.

#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "core/owned_remove.h"
#include "quest/tests/test_check.h"
#include "runtime/tests/run_root_order.h"

namespace {

using nevr_test::OwnedRecord;
using nevr_test::RemovalOrder;
using nevr_owned::Outcome;

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

// A mkdtemp folder named `prefix` + six characters under `base`; `name` is its last component.
bool MakeFolder(const std::string& base, const char* prefix, std::string* name) {
  std::string templ = base + "/" + prefix + "XXXXXX";
  std::vector<char> buf(templ.begin(), templ.end());
  buf.push_back('\0');
  if (::mkdtemp(buf.data()) == nullptr) return false;
  *name = std::string(buf.data()).substr(base.size() + 1);
  return true;
}

OwnedRecord File(const char* entry) { return OwnedRecord{entry, false}; }
OwnedRecord Folder(const char* entry) { return OwnedRecord{entry, true}; }

void OrderCases() {
  // (a) a repeated name is listed once, at its first record.
  const std::vector<std::string> dup = RemovalOrder({File("a"), Folder("b"), File("a")});
  QCHECK(dup.size() == 2);
  QCHECK(dup.size() == 2 && dup[0] == "a" && dup[1] == "b");  // files before folders at one depth

  // (b) deepest first; at one depth, files before folders; then the latest record first.
  const std::vector<std::string> deep = RemovalOrder({Folder("t"), File("t/f"), Folder("t/s"), File("t/s/g"), File("u"),
                                                      File("v"), Folder("w")});
  const std::vector<std::string> want_deep = {"t/s/g", "t/f", "t/s", "v", "u", "w", "t"};
  QCHECK(deep == want_deep);

  // (c) a parent recorded before its child is still listed after it.
  const std::vector<std::string> parent_first = RemovalOrder({Folder("p"), File("p/x")});
  QCHECK(parent_first.size() == 2 && parent_first[0] == "p/x" && parent_first[1] == "p");
}

// (d, e) the guard, run over the order, on a real tree.
void TreeCases(const std::string& base) {
  std::string run_name;
  QCHECK(MakeFolder(base, "nevr-order-run-", &run_name));
  const std::string run = base + "/" + run_name;
  QCHECK(::mkdir((run + "/t").c_str(), 0700) == 0);
  QCHECK(::mkdir((run + "/t/s").c_str(), 0700) == 0);
  WriteText(run + "/t/s/g", "g");
  WriteText(run + "/t/f", "f");

  // Recorded in creation order: the folder t first, then its children, one of them twice.
  const std::vector<OwnedRecord> recorded = {Folder("t"), Folder("t/s"), File("t/s/g"), File("t/f"), File("t/s/g")};
  const std::vector<std::string> order = RemovalOrder(recorded);
  const std::vector<nevr_owned::Result> results = nevr_owned::RemoveOwned(run, order);
  QCHECK(results.size() == order.size() && results.size() == 4);
  for (const nevr_owned::Result& r : results) QCHECK(r.outcome == Outcome::kRemoved);
  QCHECK(!Exists(run + "/t"));

  // (e) a file nobody recorded keeps its folder.
  QCHECK(::mkdir((run + "/k").c_str(), 0700) == 0);
  WriteText(run + "/k/unlisted", "u");
  const std::vector<nevr_owned::Result> kept = nevr_owned::RemoveOwned(run, RemovalOrder({Folder("k")}));
  QCHECK(kept.size() == 1 && kept[0].outcome == Outcome::kNotEmpty);
  QCHECK(Exists(run + "/k/unlisted"));
  QCHECK(::unlink((run + "/k/unlisted").c_str()) == 0);  // the test's own file, by name
  QCHECK(nevr_owned::RemoveEntry(run, "k").outcome == Outcome::kRemoved);

  QCHECK(nevr_owned::RemoveEntry(base, run_name).outcome == Outcome::kRemoved);
  QCHECK(!Exists(run));
}

}  // namespace

int main() {
  OrderCases();
  const char* env = std::getenv("NEVR_TEST_RUN_ROOT");
  const std::string base = (env != nullptr && env[0] != '\0') ? env : "/tmp";
  TreeCases(base);
  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "run_root_order_test: %d check(s) failed\n", quest_test::Failures());
    return 1;
  }
  std::printf("run_root_order_test: all checks passed\n");
  return 0;
}
