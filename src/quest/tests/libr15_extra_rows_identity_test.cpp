// Host test for the variant build's identity (docs/adr/0021 section 11): the sentinel compiled with NEVR_LIBR15_EXTRA_ROWS and the
// source the configure step generated for the example fixture. The rows the sentinel reads back are the parser's rows for the
// file, the patched build ID it expects is the one the patcher derives for them (and not the public one), and the digest it
// carries is the file's.
//
// Run: libr15_extra_rows_identity_test <fixtures-dir> [name-filter]

#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "libr15_identity.h"
#include "mini_test.h"
#include "quest/data/sha256.h"
#include "quest/patch/libr15_extra_rows.h"
#include "quest/patch/libr15_patch_identity.h"

namespace nevr_libr15_extra_rows_data {
extern const unsigned char kBytes[];
extern const std::size_t kSize;
extern const char kSha256[];
}  // namespace nevr_libr15_extra_rows_data

namespace {

using namespace nevr_quest;

std::string g_fixtures;

std::string Fixture() {
  std::ifstream in(g_fixtures + "/libr15_extra_rows_example.json", std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::vector<patch::WordPatchRow> Parsed() {
  std::vector<patch::WordPatchRow> rows;
  patch::Refusal refusal;
  CHECK(patch::ParseExtraRows(Fixture(), patch::PublicSpec(), &rows, &refusal));
  return rows;
}

TEST(TheEmbeddedTextIsTheFilesBytes) {
  const std::string file = Fixture();
  CHECK(!file.empty());
  CHECK_EQ(nevr_libr15_extra_rows_data::kSize, file.size());
  CHECK(std::memcmp(nevr_libr15_extra_rows_data::kBytes, file.data(), file.size()) == 0);
}

TEST(TheDigestIsTheFilesSha256) {
  const std::string file = Fixture();
  CHECK_EQ(std::string(nevr_libr15_extra_rows_data::kSha256), data::Sha256Hex(file.data(), file.size()));
  CHECK_EQ(std::string(sentinel::pinned::LibR15ExtraRowsSha256()), data::Sha256Hex(file.data(), file.size()));
}

TEST(TheRowsAreTheParsersRowsForTheFile) {
  const std::vector<patch::WordPatchRow> want = Parsed();
  const std::vector<patch::WordPatchRow>& got = sentinel::pinned::LibR15ExtraRows();
  CHECK_EQ(got.size(), want.size());
  CHECK_EQ(got.size(), 1u);
  if (got.size() != 1 || want.size() != 1) return;
  CHECK_EQ(got[0].id, want[0].id);
  CHECK_EQ(got[0].id, std::string("x.example_guard"));
  CHECK_EQ(got[0].disasmVa, want[0].disasmVa);
  CHECK_EQ(got[0].edits.size(), want[0].edits.size());
  for (std::size_t i = 0; i < got[0].edits.size() && i < want[0].edits.size(); ++i) {
    CHECK_EQ(got[0].edits[i].offset, want[0].edits[i].offset);
    CHECK(got[0].edits[i].stock == want[0].edits[i].stock);
    CHECK(got[0].edits[i].word == want[0].edits[i].word);
  }
}

TEST(ThePatchedIdIsTheOneThePatcherDerivesForTheRowsAndNotThePublicOne) {
  const std::string expected = patch::PatchedBuildId(Parsed());
  const std::string publicId = patch::PatchedBuildId();
  CHECK_EQ(std::string(sentinel::pinned::LibR15PatchedBuildId()), expected);
  CHECK(expected != publicId);
  CHECK_EQ(expected.size(), 40u);
  CHECK(std::string(sentinel::pinned::LibR15PatchedBuildId()) != publicId);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: libr15_extra_rows_identity_test <fixtures-dir> [name-filter]\n");
    return 2;
  }
  g_fixtures = argv[1];
  return mini_test::RunAll(argc - 1, argv + 1);
}
