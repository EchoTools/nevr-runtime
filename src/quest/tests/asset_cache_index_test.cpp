// The asset cache's index rules (runtime/patch/asset_cache_index.h) on a POSIX host: which names the cache may record and remove,
// the index text (lines that are not names are dropped and counted, never trusted), the legacy adoption decision, and the stale-temp
// age. The Win32 cache (patch/asset_cache.cpp) runs these same functions; the file operations around them are checked by the Win32
// GTests in src/runtime/tests/test_asset_cache.cpp.

#include <cstdio>
#include <string>
#include <vector>

#include "quest/tests/test_check.h"
#include "runtime/patch/asset_cache_index.h"

namespace {

using namespace nevr_asset_cache_index;

// SHA-256 of "abc".
const char kAbcSha[] = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";

std::string Hex64(char c) { return std::string(64, c); }

void CaseBlobNames() {
  QCHECK(IsBlobLeaf(Hex64('a') + ".blob"));
  QCHECK(IsBlobLeaf(kAbcSha + std::string(".blob")));
  QCHECK(!IsBlobLeaf(std::string(63, 'a') + ".blob"));  // 63 characters
  QCHECK(!IsBlobLeaf(std::string(65, 'a') + ".blob"));  // 65 characters
  QCHECK(!IsBlobLeaf(Hex64('A') + ".blob"));            // upper-case hex is not the spelling the cache writes
  QCHECK(!IsBlobLeaf(Hex64('g') + ".blob"));            // not hex
  QCHECK(!IsBlobLeaf(Hex64('a') + ".BLOB"));
  QCHECK(!IsBlobLeaf(Hex64('a') + ".blob.tmp"));
  QCHECK(!IsBlobLeaf("../" + Hex64('a') + ".blob"));
  QCHECK(!IsBlobLeaf("a\\" + Hex64('a').substr(2) + ".blob"));
}

void CaseTempNames() {
  QCHECK(IsTempLeaf(Hex64('a') + ".1.2.3.tmp"));
  QCHECK(IsTempLeaf("index.4.5.6.tmp"));  // index.json's own writes
  QCHECK(IsTempLeaf(Hex64('a') + ".4294967295.18446744073709551615.7.tmp"));
  QCHECK(!IsTempLeaf("notes.1.2.3.tmp"));  // the stem is neither a key nor "index"
  QCHECK(!IsTempLeaf(Hex64('a') + ".1.2.tmp"));  // three numbers, not four
  QCHECK(!IsTempLeaf(Hex64('a') + ".1.2.3.4.tmp"));
  QCHECK(!IsTempLeaf(Hex64('a') + ".x.2.3.tmp"));
  QCHECK(!IsTempLeaf(Hex64('a') + ".1..3.tmp"));
  QCHECK(!IsTempLeaf(Hex64('a') + ".123456789012345678901.2.3.tmp"));  // 21 digits
  QCHECK(!IsTempLeaf(Hex64('a') + ".1.2.3.TMP"));
  QCHECK(!IsTempLeaf(Hex64('a') + ".1.2.3.tmp.blob"));
  QCHECK(!IsTempLeaf("owned.txt.tmp"));  // the index's own literal temp is a name the code uses, not an owned leaf
}

void CaseOwnedAndStrangers() {
  QCHECK(IsOwnedLeaf(Hex64('a') + ".blob"));
  QCHECK(IsOwnedLeaf(Hex64('a') + ".1.2.3.tmp"));
  QCHECK(!IsOwnedLeaf(kOwnedIndexName));
  QCHECK(!IsOwnedLeaf("index.json"));
  QCHECK(!IsOwnedLeaf("nevr-2026-11-14T07-30-00.123.jsonl"));
  QCHECK(!IsOwnedLeaf(""));
}

void CaseIndexText() {
  std::vector<std::string> names;
  std::size_t dropped = 99;
  std::string error;
  const std::string text = Hex64('a') + ".blob\n" + Hex64('b') + ".1.2.3.tmp\n";
  QCHECK(ParseOwnedIndex(text, &names, &dropped, &error));
  QCHECK(names.size() == 2u && dropped == 0u);
  QCHECK(FormatOwnedIndex(names) == text);  // what is written is what is read
  QCHECK(ParseOwnedIndex("", &names, &dropped, &error) && names.empty() && dropped == 0u);
}

void CaseDroppedLines() {
  std::vector<std::string> names;
  std::size_t dropped = 0;
  std::string error;
  // a garbage line is dropped and counted; the good names around it are kept
  QCHECK(ParseOwnedIndex(Hex64('a') + ".blob\njunk\n" + Hex64('b') + ".blob\n", &names, &dropped, &error));
  QCHECK(names.size() == 2u && dropped == 1u);
  // a traversal line is dropped, never trusted
  QCHECK(ParseOwnedIndex("../x.blob\n" + Hex64('c') + ".blob\n", &names, &dropped, &error));
  QCHECK(names.size() == 1u && names[0] == Hex64('c') + ".blob" && dropped == 1u);
  // a CRLF file: the CR before each LF is tolerated, so every name survives and none is dropped
  QCHECK(ParseOwnedIndex(Hex64('a') + ".blob\r\n" + Hex64('b') + ".blob\r\n", &names, &dropped, &error));
  QCHECK(names.size() == 2u && dropped == 0u);
  // a NUL inside a line makes the line a non-name: dropped, the others kept
  const std::string nul = std::string(Hex64('a') + ".blob\n") + std::string("\0", 1) + Hex64('d') + ".blob\n";
  QCHECK(ParseOwnedIndex(nul, &names, &dropped, &error));
  QCHECK(names.size() == 1u && dropped == 1u);
  // the size cap still refuses: the text is not read at all
  QCHECK(!ParseOwnedIndex(std::string(kMaxOwnedIndexBytes + 1, '\n'), &names, &dropped, &error));
  QCHECK(!error.empty());
}

void CaseTornAppendThenAppend() {
  // A torn append leaves a last line with no newline. The next write rewrites the list from the names kept, so the new name is
  // on its own line and the file parses clean.
  std::vector<std::string> names;
  std::size_t dropped = 0;
  std::string error;
  const std::string torn = Hex64('a') + ".blob\n" + Hex64('b') + ".blo";
  QCHECK(ParseOwnedIndex(torn, &names, &dropped, &error));
  QCHECK(names.size() == 1u && dropped == 1u);
  names.push_back(Hex64('c') + ".blob");
  const std::string rewritten = FormatOwnedIndex(names);
  std::vector<std::string> again;
  std::size_t lost = 99;
  QCHECK(ParseOwnedIndex(rewritten, &again, &lost, &error));
  QCHECK(again.size() == 2u && lost == 0u && again[1] == Hex64('c') + ".blob");
}

void CaseAdoption() {
  QCHECK(ShouldAdoptLegacyBlob(std::string(kAbcSha) + ".blob", kAbcSha));  // a blob this cache wrote, by its content
  QCHECK(!ShouldAdoptLegacyBlob(Hex64('a') + ".blob", kAbcSha));           // the content does not match the name
  QCHECK(!ShouldAdoptLegacyBlob(std::string(kAbcSha) + ".1.2.3.tmp", kAbcSha));  // a legacy temp is never adopted
  QCHECK(!ShouldAdoptLegacyBlob(std::string(kAbcSha) + ".blob", ""));      // a hash that failed adopts nothing
  QCHECK(!ShouldAdoptLegacyBlob("notes.blob", kAbcSha));
}

void CaseStale() {
  QCHECK(IsStaleTemp(/*now=*/1000, /*stamp=*/100, /*staleTicks=*/500));
  QCHECK(!IsStaleTemp(1000, 600, 500));  // younger than the age
  QCHECK(!IsStaleTemp(1000, 500, 500));  // exactly the age: not yet stale
  QCHECK(!IsStaleTemp(100, 1000, 500));  // a stamp in the future is never stale
}

}  // namespace

int main() {
  CaseBlobNames();
  CaseTempNames();
  CaseOwnedAndStrangers();
  CaseIndexText();
  CaseDroppedLines();
  CaseTornAppendThenAppend();
  CaseAdoption();
  CaseStale();
  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "asset_cache_index_test: %d check(s) failed\n", quest_test::Failures());
    return 1;
  }
  std::printf("asset_cache_index_test: all checks passed\n");
  return 0;
}
