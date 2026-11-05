// Host tests for the OBB footer parse (src/quest/obb/obb_footer.cpp, docs/adr/0042): a good footer, a wrong tag, a file too short
// to hold one, a footer size that does not fit, an unsalted OBB, a footer version other than 1, a name length that does not
// add up, and an image that is not a whole number of sectors.

#include "quest/obb/byte_source.h"
#include "quest/obb/obb_footer.h"
#include "quest/tests/mini_test.h"
#include "quest/tests/obb_test_support.h"

using namespace nevr_quest::obb;
using namespace obb_test;

namespace {
std::vector<std::uint8_t> SmallImage(std::size_t sectors = 4) { return Pattern(sectors * 512, 11); }
Status Parse(std::vector<std::uint8_t> bytes, ObbFooter* footer) {
  MemorySource source(std::move(bytes));
  return ParseObbFooter(source, footer);
}
}  // namespace

TEST(GoodFooterIsRead) {
  ObbFooter footer;
  const Status s = Parse(BuildObb(SmallImage()), &footer);
  CHECK(s.ok);
  CHECK_EQ(footer.footer_version, 1u);
  CHECK_EQ(footer.package_version, 1u);
  CHECK_EQ(footer.flags, 2u);
  const std::uint8_t salt[8] = {0xdf, 0xc9, 0x0a, 0x5f, 0x1b, 0xbe, 0x68, 0x35};
  CHECK(std::equal(footer.salt.begin(), footer.salt.end(), salt));
  CHECK_EQ(footer.package_name, std::string("com.readyatdawn.r15"));
  CHECK_EQ(footer.footer_bytes, 43u);  // 24 + len("com.readyatdawn.r15"), the store OBB's own footer size
  CHECK_EQ(footer.image_bytes, 4u * 512u);
}

TEST(WrongTagIsRefused) {
  ObbSpec spec;
  spec.tag = 0x01059984;
  ObbFooter footer;
  const Status s = Parse(BuildObb(SmallImage(), spec), &footer);
  CHECK(!s.ok);
  CHECK_EQ(s.code, std::string("obb_bad_tag"));
}

TEST(ShortFilesAreRefused) {
  ObbFooter footer;
  CHECK_EQ(Parse({}, &footer).code, std::string("obb_too_short"));
  CHECK_EQ(Parse({1, 2, 3, 4, 5, 6, 7}, &footer).code, std::string("obb_too_short"));
  // Eight bytes that are only a tag and a zero size: a footer smaller than the least one.
  std::vector<std::uint8_t> bare;
  AppendLe32(bare, 0);
  AppendLe32(bare, 0x01059983);
  CHECK_EQ(Parse(bare, &footer).code, std::string("obb_bad_footer_size"));
}

TEST(FooterSizeBeyondTheFileIsRefused) {
  std::vector<std::uint8_t> small = BuildObb(SmallImage());  // about 2 KiB: the size field beyond the file is the only fault
  Put32(small, small.size() - 8, static_cast<std::uint32_t>(small.size()));
  ObbFooter footer;
  CHECK_EQ(Parse(small, &footer).code, std::string("obb_bad_footer_size"));
  Put32(small, small.size() - 8, static_cast<std::uint32_t>(small.size() - 7));
  CHECK_EQ(Parse(small, &footer).code, std::string("obb_bad_footer_size"));
}

TEST(FooterSizeAboveWhatAndroidReadsIsRefused) {
  std::vector<std::uint8_t> obb = BuildObb(SmallImage(80));  // 40 KiB, so 32769 still fits in the file
  Put32(obb, obb.size() - 8, 32769);
  ObbFooter footer;
  CHECK_EQ(Parse(obb, &footer).code, std::string("obb_bad_footer_size"));
}

TEST(UnsaltedObbIsRefused) {
  ObbSpec spec;
  spec.flags = 0;
  ObbFooter footer;
  const Status s = Parse(BuildObb(SmallImage(), spec), &footer);
  CHECK(!s.ok);
  CHECK_EQ(s.code, std::string("obb_unsalted"));
}

TEST(FooterVersionOtherThanOneIsRefused) {
  ObbSpec spec;
  spec.footer_version = 2;
  ObbFooter footer;
  CHECK_EQ(Parse(BuildObb(SmallImage(), spec), &footer).code, std::string("obb_bad_footer_version"));
}

TEST(NameLengthThatDoesNotAddUpIsRefused) {
  std::vector<std::uint8_t> obb = BuildObb(SmallImage());
  const std::size_t footer_start = obb.size() - 8 - 43;
  Put32(obb, footer_start + 20, 18);  // one byte short of the name that is there
  ObbFooter footer;
  CHECK_EQ(Parse(obb, &footer).code, std::string("obb_bad_name_length"));
  Put32(obb, footer_start + 20, 20);  // one byte too long
  CHECK_EQ(Parse(obb, &footer).code, std::string("obb_bad_name_length"));
}

TEST(ImageThatIsNotWholeSectorsIsRefused) {
  ObbFooter footer;
  std::vector<std::uint8_t> odd = Pattern(513, 3);
  CHECK_EQ(Parse(BuildObb(odd), &footer).code, std::string("obb_image_not_sector_aligned"));
  CHECK_EQ(Parse(BuildObb(Pattern(768, 3)), &footer).code, std::string("obb_image_not_sector_aligned"));  // a multiple of 256, not of 512
  CHECK_EQ(Parse(BuildObb({}), &footer).code, std::string("obb_image_not_sector_aligned"));
}

TEST(ASaltedFlagOnAFooterTooSmallForASaltIsRefused) {
  // An unsalted footer with an empty name is 16 bytes; flip the flag to salted and there is no room for the salt.
  ObbSpec spec;
  spec.flags = 0;
  spec.package_name = "";
  std::vector<std::uint8_t> obb = BuildObb(SmallImage(), spec);
  const std::size_t footer_start = obb.size() - 8 - 16;
  Put32(obb, footer_start + 8, 2);
  ObbFooter footer;
  CHECK_EQ(Parse(obb, &footer).code, std::string("obb_bad_footer_size"));
}

int main(int argc, char** argv) { return mini_test::RunAll(argc, argv); }
