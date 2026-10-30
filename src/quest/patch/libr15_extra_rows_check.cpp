// The host check of an extra-rows file (ADR 0021 section 11): the same parser the patcher and the variant sentinel run, on the
// public spec. The Quest build runs it at configure time (cmake/libr15_extra_rows.cmake) so that a file the patcher would refuse
// stops the sentinel's build instead of surfacing on a headset.
//
// Usage: libr15_extra_rows_check <file>
// Exit 0 and one line `rows=<n> words=<n>` on success; exit 1 and the refusal as one JSON line on stderr when the file is
// refused; exit 2 on a usage or read error.
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "quest/patch/libr15_extra_rows.h"

int main(int argc, char** argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: libr15_extra_rows_check <file>\n");
    return 2;
  }
  std::ifstream in(argv[1], std::ios::binary);
  if (!in) {
    std::fprintf(stderr, "libr15_extra_rows_check: cannot read %s\n", argv[1]);
    return 2;
  }
  const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  std::vector<nevr_quest::patch::WordPatchRow> rows;
  nevr_quest::patch::Refusal refusal;
  if (!nevr_quest::patch::ParseExtraRows(bytes, nevr_quest::patch::PublicSpec(), &rows, &refusal)) {
    std::fprintf(stderr, "%s\n", refusal.ToJson().dump().c_str());
    return 1;
  }
  std::size_t words = 0;
  for (const nevr_quest::patch::WordPatchRow& row : rows) words += row.edits.size();
  std::printf("rows=%zu words=%zu\n", rows.size(), words);
  return 0;
}
