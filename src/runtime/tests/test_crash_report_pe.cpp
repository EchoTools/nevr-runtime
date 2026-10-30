// The PE reader of the crash report (#1605): the identity and the nearest export, over a synthetic image whose
// layout is written out here byte by byte.

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>

#include "runtime/lifecycle/crash_report_pe.h"

namespace {

constexpr std::size_t kImageBytes = 0x2000;
constexpr std::uint32_t kStamp = 0x5f3e1a2b;

void Put16(std::uint8_t* image, std::size_t at, std::uint16_t value) { std::memcpy(image + at, &value, 2); }
void Put32(std::uint8_t* image, std::size_t at, std::uint32_t value) { std::memcpy(image + at, &value, 4); }
void PutText(std::uint8_t* image, std::size_t at, const char* text) {
  std::memcpy(image + at, text, std::strlen(text) + 1);
}

// A PE32+ image: PE signature at 0x80, optional header at 0x98, export directory at RVA 0x1000 holding three
// functions (Alpha at 0x300, Beta at 0x500, and Fwd, a forwarder whose "address" is a string inside the directory).
class PeImage {
 public:
  PeImage() {
    std::memset(bytes_, 0, sizeof(bytes_));
    Put16(bytes_, 0, 0x5a4d);
    Put32(bytes_, 0x3c, 0x80);
    Put32(bytes_, 0x80, 0x00004550);
    Put32(bytes_, 0x88, kStamp);
    Put16(bytes_, 0x98, 0x20b);
    Put32(bytes_, 0x98 + 56, kImageBytes);
    Put32(bytes_, 0x98 + 112, 0x1000);
    Put32(bytes_, 0x98 + 116, 0x100);
    Put32(bytes_, 0x1000 + 20, 3);       // NumberOfFunctions
    Put32(bytes_, 0x1000 + 24, 3);       // NumberOfNames
    Put32(bytes_, 0x1000 + 28, 0x1040);  // AddressOfFunctions
    Put32(bytes_, 0x1000 + 32, 0x1050);  // AddressOfNames
    Put32(bytes_, 0x1000 + 36, 0x1060);  // AddressOfNameOrdinals
    Put32(bytes_, 0x1040, 0x300);
    Put32(bytes_, 0x1044, 0x500);
    Put32(bytes_, 0x1048, 0x1010);
    Put32(bytes_, 0x1050, 0x1080);
    Put32(bytes_, 0x1054, 0x1090);
    Put32(bytes_, 0x1058, 0x10a0);
    Put16(bytes_, 0x1060, 0);
    Put16(bytes_, 0x1062, 1);
    Put16(bytes_, 0x1064, 2);
    PutText(bytes_, 0x1080, "Alpha");
    PutText(bytes_, 0x1090, "Beta");
    PutText(bytes_, 0x10a0, "Fwd");
  }
  const std::uint8_t* data() const { return bytes_; }
  std::uint8_t* data() { return bytes_; }

 private:
  alignas(8) std::uint8_t bytes_[kImageBytes];
};

}  // namespace

TEST(CrashReportPe, IdentityIsTheStampAndTheSizeOfImage) {
  PeImage image;
  nevr_crash_pe::ImageIdentity identity{};
  ASSERT_TRUE(nevr_crash_pe::ReadIdentity(image.data(), nullptr, &identity));
  EXPECT_EQ(identity.time_date_stamp, kStamp);
  EXPECT_EQ(identity.size_of_image, kImageBytes);
  char text[40];
  nevr_crash_pe::FormatIdentity(identity, text, sizeof(text));
  EXPECT_STREQ(text, "pe-5f3e1a2b-2000");
}

TEST(CrashReportPe, IdentityRefusesAnImageThatIsNotAPe) {
  PeImage image;
  Put16(image.data(), 0, 0);
  nevr_crash_pe::ImageIdentity identity{};
  EXPECT_FALSE(nevr_crash_pe::ReadIdentity(image.data(), nullptr, &identity));
}

TEST(CrashReportPe, NearestExportIsTheHighestAtOrBelowTheAddress) {
  PeImage image;
  char name[64];
  std::uint64_t offset = 0;
  ASSERT_TRUE(nevr_crash_pe::NearestExport(image.data(), nullptr, 0x350, name, sizeof(name), &offset));
  EXPECT_STREQ(name, "Alpha");
  EXPECT_EQ(offset, 0x50u);
  ASSERT_TRUE(nevr_crash_pe::NearestExport(image.data(), nullptr, 0x500, name, sizeof(name), &offset));
  EXPECT_STREQ(name, "Beta");
  EXPECT_EQ(offset, 0u);
}

TEST(CrashReportPe, NoExportAtOrBelowTheAddressIsNoSymbol) {
  PeImage image;
  char name[64];
  std::uint64_t offset = 0;
  EXPECT_FALSE(nevr_crash_pe::NearestExport(image.data(), nullptr, 0x2ff, name, sizeof(name), &offset));
}

TEST(CrashReportPe, AForwardedExportIsNotCode) {
  PeImage image;
  char name[64];
  std::uint64_t offset = 0;
  // Fwd's "address" is 0x1010, inside the export directory: an address past it must still name Beta.
  ASSERT_TRUE(nevr_crash_pe::NearestExport(image.data(), nullptr, 0x1800, name, sizeof(name), &offset));
  EXPECT_STREQ(name, "Beta");
  EXPECT_EQ(offset, 0x1300u);
}

TEST(CrashReportPe, AnImageWithNoExportsHasNoSymbols) {
  PeImage image;
  Put32(image.data(), 0x98 + 112, 0);
  char name[64];
  std::uint64_t offset = 0;
  EXPECT_FALSE(nevr_crash_pe::NearestExport(image.data(), nullptr, 0x350, name, sizeof(name), &offset));
}

TEST(CrashReportPe, AReadTheProbeRefusesEndsTheLookup) {
  PeImage image;
  char name[64];
  std::uint64_t offset = 0;
  const auto refuse = [](const void*, std::size_t) { return false; };
  EXPECT_FALSE(nevr_crash_pe::NearestExport(image.data(), refuse, 0x350, name, sizeof(name), &offset));
  nevr_crash_pe::ImageIdentity identity{};
  EXPECT_FALSE(nevr_crash_pe::ReadIdentity(image.data(), refuse, &identity));
}

TEST(CrashReportPe, ANameLongerThanTheBufferIsCutAndTerminated) {
  PeImage image;
  char name[4];
  std::uint64_t offset = 0;
  ASSERT_TRUE(nevr_crash_pe::NearestExport(image.data(), nullptr, 0x350, name, sizeof(name), &offset));
  EXPECT_STREQ(name, "Alp");
}
