// Host tests for the stock-data pins (src/quest/obb/stock_pins.cpp, docs/adr/0042): the file's syntax, and the three checks
// (the OBB, the tree) with the path, the pinned value and the measured one named in every refusal.

#include <string>

#include "quest/obb/stock_pins.h"
#include "quest/tests/mini_test.h"

using namespace nevr_quest::obb;

namespace {
const std::string kObb(64, 'a');
const std::string kA(64, '1');
const std::string kB(64, '2');

std::string Good() {
  return "# the pins\n"
         "\n"
         "obb_sha256 " + kObb + "\n"
         "obb_size 946094131\n"
         "file " + kA + " 828213 _data/x/manifests/m1   \n"
         "  file " + kB + " 0 _data/x/packages/with space_0\n";
}

StockPins Parsed() {
  StockPins pins;
  const Status s = ParseStockPins(Good(), &pins);
  CHECK(s.ok);
  return pins;
}
}  // namespace

TEST(AGoodPinFileParses) {
  StockPins pins;
  CHECK(ParseStockPins(Good(), &pins).ok);
  CHECK_EQ(pins.obb_sha256, kObb);
  CHECK_EQ(pins.obb_size, 946094131ull);
  CHECK_EQ(pins.files.size(), 2u);
  CHECK_EQ(pins.files[0].path, std::string("_data/x/manifests/m1"));
  CHECK_EQ(pins.files[0].sha256, kA);
  CHECK_EQ(pins.files[0].size, 828213ull);
  CHECK_EQ(pins.files[1].path, std::string("_data/x/packages/with space_0"));
  CHECK_EQ(pins.files[1].size, 0ull);
}

TEST(PinSyntaxFaultsNameTheirLine) {
  const std::string head = "obb_sha256 " + kObb + "\nobb_size 5\n";
  struct Case { std::string text; std::string code; };
  const Case cases[] = {
      {head + "file " + kA + " 10\n", "pin_syntax"},                              // no path
      {head + "file " + kA + " ten p\n", "pin_syntax"},                           // size is not a number
      {head + "file " + kA + " -1 p\n", "pin_syntax"},
      {head + "file " + std::string(63, '1') + " 10 p\n", "pin_syntax"},          // digest too short
      {head + "file " + std::string(64, 'G') + " 10 p\n", "pin_syntax"},          // not hex
      {head + "file " + std::string(64, 'A') + " 10 p\n", "pin_syntax"},          // upper case
      {head + "bogus 1\n", "pin_syntax"},                                         // unknown record
      {"obb_sha256 xyz\nobb_size 5\nfile " + kA + " 1 p\n", "pin_syntax"},
      {head + "obb_size 6\nfile " + kA + " 1 p\n", "pin_syntax"},                 // obb_size twice
      {"obb_sha256 " + kObb + "\nobb_sha256 " + kObb + "\nobb_size 5\nfile " + kA + " 1 p\n", "pin_syntax"},
  };
  for (const Case& c : cases) {
    StockPins pins;
    const Status s = ParseStockPins(c.text, &pins);
    CHECK(!s.ok);
    CHECK_EQ(s.code, c.code);
    CHECK(s.detail.find("line ") != std::string::npos);
  }
}

TEST(MissingPartsAreRefused) {
  StockPins pins;
  CHECK_EQ(ParseStockPins("obb_size 5\nfile " + kA + " 1 p\n", &pins).code, std::string("pin_missing_obb"));
  CHECK_EQ(ParseStockPins("obb_sha256 " + kObb + "\nfile " + kA + " 1 p\n", &pins).code, std::string("pin_missing_obb"));
  CHECK_EQ(ParseStockPins("obb_sha256 " + kObb + "\nobb_size 5\n", &pins).code, std::string("pin_no_files"));
  CHECK_EQ(ParseStockPins("", &pins).code, std::string("pin_missing_obb"));
}

TEST(DuplicateAndUnsafePathsAreRefused) {
  const std::string head = "obb_sha256 " + kObb + "\nobb_size 5\n";
  StockPins pins;
  CHECK_EQ(ParseStockPins(head + "file " + kA + " 1 p\nfile " + kB + " 2 p\n", &pins).code, std::string("pin_duplicate_file"));
  for (const char* bad : {"../x", "a/../x", "/abs", "a//b", "a/", "./a", "a/./b", "a\\b"}) {
    const Status s = ParseStockPins(head + "file " + kA + " 1 " + bad + "\n", &pins);
    CHECK(!s.ok);
    CHECK_EQ(s.code, std::string("pin_unsafe_path"));
  }
}

TEST(TheObbIsCheckedAgainstTheDigestAndTheSize) {
  const StockPins pins = Parsed();
  CHECK(CheckObbAgainstPins(pins, kObb, 946094131).ok);
  const Status wrong_digest = CheckObbAgainstPins(pins, kB, 946094131);
  CHECK_EQ(wrong_digest.code, std::string("pin_obb_mismatch"));
  CHECK(wrong_digest.detail.find(kObb) != std::string::npos);
  CHECK(wrong_digest.detail.find(kB) != std::string::npos);
  const Status wrong_size = CheckObbAgainstPins(pins, kObb, 946094132);
  CHECK_EQ(wrong_size.code, std::string("pin_obb_mismatch"));
  CHECK(wrong_size.detail.find("946094132") != std::string::npos);
}

TEST(TheTreeIsCheckedFileByFile) {
  const StockPins pins = Parsed();
  const std::vector<MeasuredFile> good = {{"_data/x/packages/with space_0", kB, 0}, {"_data/x/manifests/m1", kA, 828213}};
  CHECK(CheckTreeAgainstPins(pins, good).ok);  // order does not matter

  std::vector<MeasuredFile> extra = good;
  extra.push_back({"_data/x/other", kA, 1});
  Status s = CheckTreeAgainstPins(pins, extra);
  CHECK_EQ(s.code, std::string("pin_unlisted_file"));
  CHECK(s.detail.find("_data/x/other") != std::string::npos);

  std::vector<MeasuredFile> missing = {good[1]};
  s = CheckTreeAgainstPins(pins, missing);
  CHECK_EQ(s.code, std::string("pin_missing_file"));
  CHECK(s.detail.find("with space_0") != std::string::npos);

  std::vector<MeasuredFile> size = good;
  size[1].size = 828214;
  s = CheckTreeAgainstPins(pins, size);
  CHECK_EQ(s.code, std::string("pin_size_mismatch"));
  CHECK(s.detail.find("828213") != std::string::npos);
  CHECK(s.detail.find("828214") != std::string::npos);

  std::vector<MeasuredFile> digest = good;
  digest[1].sha256 = kB;
  s = CheckTreeAgainstPins(pins, digest);
  CHECK_EQ(s.code, std::string("pin_digest_mismatch"));
  CHECK(s.detail.find("_data/x/manifests/m1") != std::string::npos);
  CHECK(s.detail.find(kA) != std::string::npos);
  CHECK(s.detail.find(kB) != std::string::npos);

  std::vector<MeasuredFile> twice = good;
  twice.push_back(good[0]);
  CHECK_EQ(CheckTreeAgainstPins(pins, twice).code, std::string("pin_duplicate_file"));  // a path seen twice is not the pinned tree
}

int main(int argc, char** argv) { return mini_test::RunAll(argc, argv); }
