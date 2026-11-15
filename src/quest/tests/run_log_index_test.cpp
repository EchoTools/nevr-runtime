// The run-log index rules (runtime/log/run_log_index.h) on a POSIX host: the run-log grammar, the index text (a line that is not a
// name is dropped and counted), the JSON-lines adoption check, the adoption filter and the retention selection
// (runtime/log/log_retention.h, which the filter calls). The Win32 file operations are in builtin_filter.cpp.

#include <cstdio>
#include <string>
#include <vector>

#include "quest/tests/test_check.h"
#include "runtime/log/log_retention.h"
#include "runtime/log/run_log_index.h"

namespace {

using namespace nevr_run_log_index;

void CaseGrammar() {
  // The names core/run_log_path.h makes: a creation-time stamp, or pid<hex> when the time cannot be read.
  QCHECK(IsRunLogLeaf("nevr-2026-11-14T07-30-00.123.jsonl"));
  QCHECK(IsRunLogLeaf("nevr-pid1a2b.jsonl"));
  QCHECK(IsRunLogLeaf("nevr-pidffffffff.jsonl"));
  // A rotated archive: <name>.<n>.jsonl (run_log_writer.h Rotate).
  QCHECK(IsRunLogLeaf("nevr-2026-11-14T07-30-00.123.1.jsonl"));
  QCHECK(IsRunLogLeaf("nevr-pid1a2b.12.jsonl"));
  // Strangers: never adopted, never deleted.
  QCHECK(!IsRunLogLeaf("nevr-notes.jsonl"));
  QCHECK(!IsRunLogLeaf("nevr-2026-11-14T07-30-00.jsonl"));  // no milliseconds
  QCHECK(!IsRunLogLeaf("nevr-2026-11-14T07-30-00.123.jsonl.tmp"));
  QCHECK(!IsRunLogLeaf("nevr-2026-11-14 07-30-00.123.jsonl"));
  QCHECK(!IsRunLogLeaf("nevr-pidZZ.jsonl"));
  QCHECK(!IsRunLogLeaf("nevr-pid.jsonl"));
  QCHECK(!IsRunLogLeaf("nevr-pid123456789.jsonl"));  // more than 8 hex digits
  QCHECK(!IsRunLogLeaf("nevr-2026-11-14T07-30-00.123.0000.jsonl"));  // archive number of more than 3 digits
  QCHECK(!IsRunLogLeaf("NEVR-2026-11-14T07-30-00.123.jsonl"));
  QCHECK(!IsRunLogLeaf("nevr-crashes"));
  QCHECK(!IsRunLogLeaf("../nevr-pid1a2b.jsonl"));
  QCHECK(!IsRunLogLeaf("nevr-pid1a2b.jsonl\\x"));
  QCHECK(!IsRunLogLeaf("runlog-index.txt"));
  QCHECK(!IsRunLogLeaf(""));
}

void CaseIndexText() {
  std::vector<std::string> names;
  std::size_t dropped = 99;
  std::string error;
  const std::string text = "nevr-pid1a2b.jsonl\nnevr-2026-11-14T07-30-00.123.1.jsonl\n";
  QCHECK(ParseRunLogIndex(text, &names, &dropped, &error));
  QCHECK(names.size() == 2u && dropped == 0u);
  QCHECK(FormatRunLogIndex(names) == text);
  QCHECK(ParseRunLogIndex("", &names, &dropped, &error) && names.empty() && dropped == 0u);
  QCHECK(!ParseRunLogIndex(std::string(kMaxRunLogIndexBytes + 1, '\n'), &names, &dropped, &error));  // over the cap: not read
}

void CaseDroppedLines() {
  std::vector<std::string> names;
  std::size_t dropped = 0;
  std::string error;
  // a stranger line is dropped and counted; the good names around it are kept
  QCHECK(ParseRunLogIndex("nevr-pid1a2b.jsonl\nnevr-notes.jsonl\nnevr-pid2b3c.jsonl\n", &names, &dropped, &error));
  QCHECK(names.size() == 2u && dropped == 1u);
  // a traversal line is dropped, never trusted
  QCHECK(ParseRunLogIndex("..\\nevr-pid1a2b.jsonl\nnevr-pid2b3c.jsonl\n", &names, &dropped, &error));
  QCHECK(names.size() == 1u && names[0] == "nevr-pid2b3c.jsonl" && dropped == 1u);
  // a CRLF file: every name survives, none is dropped
  QCHECK(ParseRunLogIndex("nevr-pid1a2b.jsonl\r\nnevr-pid2b3c.jsonl\r\n", &names, &dropped, &error));
  QCHECK(names.size() == 2u && dropped == 0u);
  // a NUL inside a line: that line is dropped
  const std::string nul = std::string("nevr-pid1a2b.jsonl\n") + std::string("\0", 1) + "nevr-pid2b3c.jsonl\n";
  QCHECK(ParseRunLogIndex(nul, &names, &dropped, &error));
  QCHECK(names.size() == 1u && dropped == 1u);
  // a torn last line (no newline) is dropped
  QCHECK(ParseRunLogIndex("nevr-pid1a2b.jsonl\nnevr-pid2b3c.jso", &names, &dropped, &error));
  QCHECK(names.size() == 1u && dropped == 1u);
}

void CaseTornAppendThenAppend() {
  // After a torn append, the next write rewrites the list from the names kept, with the new name on its own line.
  std::vector<std::string> names;
  std::size_t dropped = 0;
  std::string error;
  QCHECK(ParseRunLogIndex("nevr-pid1a2b.jsonl\nnevr-pid2b3c", &names, &dropped, &error));
  names.push_back("nevr-pid3c4d.jsonl");
  std::vector<std::string> again;
  std::size_t lost = 99;
  QCHECK(ParseRunLogIndex(FormatRunLogIndex(names), &again, &lost, &error));
  QCHECK(again.size() == 2u && lost == 0u && again[1] == "nevr-pid3c4d.jsonl");
}

void CaseJsonStart() {
  // The legacy adoption takes a run log only when its first byte is '{' (a JSON-lines record).
  QCHECK(IsJsonLogStart('{'));
  QCHECK(!IsJsonLogStart('x'));
  QCHECK(!IsJsonLogStart(0));  // empty or unreadable: not adopted
}

void CaseAdoption() {
  // The one-time legacy adoption takes exactly the names of the grammar from a folder listing, and nothing else.
  const std::vector<std::string> listing = {"nevr-notes.jsonl", "nevr-pid1a2b.jsonl", "readme.txt",
                                            "nevr-2026-11-14T07-30-00.123.1.jsonl", "nevr-crashes", "runlog-index.txt"};
  const std::vector<std::string> adopted = AdoptableRunLogs(listing);
  QCHECK(adopted.size() == 2u);
  QCHECK(adopted.size() == 2u && adopted[0] == "nevr-pid1a2b.jsonl");
  QCHECK(adopted.size() == 2u && adopted[1] == "nevr-2026-11-14T07-30-00.123.1.jsonl");
}

void CaseSelection() {
  // The filter passes the indexed names with their sizes to the same selection the unit test pins (log_retention.h).
  std::vector<nevr_log_retention::RunLogFile> files = {
      {"logs\\nevr-pid00000001.jsonl", 100},
      {"logs\\nevr-pid00000002.jsonl", 100},
      {"logs\\nevr-pid00000003.jsonl", 100},
  };
  const std::vector<std::string> doomed = nevr_log_retention::SelectLogsToDelete(files, 150, "logs\\nevr-pid00000003.jsonl");
  QCHECK(doomed.size() == 2u);
  QCHECK(doomed.size() == 2u && doomed[0] == "logs\\nevr-pid00000001.jsonl");
  QCHECK(doomed.size() == 2u && doomed[1] == "logs\\nevr-pid00000002.jsonl");
  QCHECK(nevr_log_retention::SelectLogsToDelete(files, 0, "logs\\nevr-pid00000003.jsonl").empty());  // no cap, no deletion
}

}  // namespace

int main() {
  CaseGrammar();
  CaseIndexText();
  CaseDroppedLines();
  CaseTornAppendThenAppend();
  CaseJsonStart();
  CaseAdoption();
  CaseSelection();
  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "run_log_index_test: %d check(s) failed\n", quest_test::Failures());
    return 1;
  }
  std::printf("run_log_index_test: all checks passed\n");
  return 0;
}
