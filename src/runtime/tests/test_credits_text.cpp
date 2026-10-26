// #938: the project section of the in-game CREDITS page. Pure byte logic (runtime/patch/credits_text.h)
// plus the near-allocation helper (runtime/hook/near_alloc.h). The fake game blob below is not
// Ready At Dawn's text; the real one is never copied into a test.

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "runtime/hook/near_alloc.h"
#include "runtime/patch/credits_text.h"

namespace {

using nevr_credits_text::BlobIdentity;
using nevr_credits_text::BlockStats;
using nevr_credits_text::BlockStatus;
using nevr_credits_text::Outcome;
using nevr_credits_text::SitePlan;

constexpr uint64_t kBase = 0x140000000ULL;
constexpr uint64_t kInitVa = kBase + nevr_credits_text::kInitSiteRva;
constexpr uint64_t kDisabledVa = kBase + nevr_credits_text::kDisabledSiteRva;
constexpr uint64_t kBufferVa = 0x180000000ULL;  // where the DLL links; within 2 GiB of the sites

// A fake game: two sites holding the real expected bytes, and a blob with the two non-ASCII bytes
// the real one has (0xA9 copyright sign, 0xFC u-umlaut) and a pipe line.
const char kFakeBlob[] = "\nFake Studio\nFake Game\n\n\nFake Team\n\nA Name|A Title\nCopyright \xA9 1999 M\xFC" "ller\n";

struct FakeImage {
  uint8_t init[nevr_credits_text::kSiteSize];
  uint8_t disabled[nevr_credits_text::kSiteSize];
  FakeImage() {
    std::memcpy(init, nevr_credits_text::kInitSiteExpected, sizeof(init));
    std::memcpy(disabled, nevr_credits_text::kDisabledSiteExpected, sizeof(disabled));
  }
  SitePlan InitPlan() { return SitePlan{init, kInitVa, nevr_credits_text::kInitSiteExpected, {}}; }
  SitePlan DisabledPlan() { return SitePlan{disabled, kDisabledVa, nevr_credits_text::kDisabledSiteExpected, {}}; }
};

uint64_t TargetOf(const uint8_t* site, uint64_t va) {
  int32_t disp = 0;
  std::memcpy(&disp, site + 3, sizeof(disp));
  return va + nevr_credits_text::kSiteSize + static_cast<int64_t>(disp);
}

bool CopyWriter(void* dest, const void* src, size_t n) {
  std::memcpy(dest, src, n);
  return true;
}
bool RefusingWriter(void*, const void*, size_t) { return false; }

// Fails the Nth call (1-based), copies the others. A plain function with state, because WriteFn is
// a function pointer.
int g_calls = 0;
int g_failOn = 0;
bool FailNthWriter(void* dest, const void* src, size_t n) {
  ++g_calls;
  if (g_calls == g_failOn) return false;
  std::memcpy(dest, src, n);
  return true;
}
void ResetWriter(int failOn) {
  g_calls = 0;
  g_failOn = failOn;
}

std::string Lines(size_t count, const std::string& line) {
  std::string out;
  for (size_t i = 0; i < count; ++i) out += line + "\n";
  return out;
}

// ---- the constants are the binary's, not a guess ----

TEST(CreditsTextGroundTruth, BothExpectedLeasResolveToTheBlob) {
  // 48 8d 05 <disp32>: the target is siteVa + 7 + disp. Both must land on kBlobRva, or the patch
  // would redirect the wrong load.
  EXPECT_EQ(TargetOf(nevr_credits_text::kInitSiteExpected, kInitVa), kBase + nevr_credits_text::kBlobRva);
  EXPECT_EQ(TargetOf(nevr_credits_text::kDisabledSiteExpected, kDisabledVa), kBase + nevr_credits_text::kBlobRva);
}

TEST(CreditsTextGroundTruth, TheBlobHashIsASha256Hex) {
  EXPECT_EQ(std::strlen(nevr_credits_text::kBlobSha256), 64u);
  EXPECT_EQ(nevr_credits_text::kBlobLength, 30091u);
}

// ---- the block ----

TEST(CreditsTextBlock, AWellFormedBlockIsOkAndMeasured) {
  const std::string block = "CODE CONTRIBUTIONS\n\nXenomega\nDualgame\n\n\n\n";
  BlockStats stats;
  EXPECT_EQ(nevr_credits_text::ValidateBlock(block.data(), block.size(), &stats), BlockStatus::Ok);
  EXPECT_EQ(stats.lines, 7u);
  EXPECT_EQ(stats.longestLine, 18u);
  EXPECT_EQ(stats.overReview, 0u);
}

TEST(CreditsTextBlock, NothingIsEmpty) {
  EXPECT_EQ(nevr_credits_text::ValidateBlock(nullptr, 0), BlockStatus::Empty);
  EXPECT_EQ(nevr_credits_text::ValidateBlock("", 0), BlockStatus::Empty);
}

TEST(CreditsTextBlock, ALastLineWithoutLfIsRefused) {
  const std::string block = "HEADING\n\nName";
  EXPECT_EQ(nevr_credits_text::ValidateBlock(block.data(), block.size()), BlockStatus::NotTerminated);
}

TEST(CreditsTextBlock, BytesThatChangeTheGamesParseAreRefused) {
  const std::string pipe = "A|B\n";
  const std::string cr = "A\rB\n";
  const std::string nul("A\0B\n", 4);
  EXPECT_EQ(nevr_credits_text::ValidateBlock(pipe.data(), pipe.size()), BlockStatus::ForbiddenByte);
  EXPECT_EQ(nevr_credits_text::ValidateBlock(cr.data(), cr.size()), BlockStatus::ForbiddenByte);
  EXPECT_EQ(nevr_credits_text::ValidateBlock(nul.data(), nul.size()), BlockStatus::ForbiddenByte);
}

TEST(CreditsTextBlock, SixtyFourBytesIsTheLastLengthTheGameShows) {
  const std::string ok = std::string(64, 'W') + "\n";
  const std::string tooLong = std::string(65, 'W') + "\n";
  EXPECT_EQ(nevr_credits_text::ValidateBlock(ok.data(), ok.size()), BlockStatus::Ok);
  EXPECT_EQ(nevr_credits_text::ValidateBlock(tooLong.data(), tooLong.size()), BlockStatus::LineTooLong);
}

TEST(CreditsTextBlock, ALineOverTheReviewLimitIsCountedNotRefused) {
  const std::string block = std::string(40, 'a') + "\n" + std::string(41, 'b') + "\n";
  BlockStats stats;
  EXPECT_EQ(nevr_credits_text::ValidateBlock(block.data(), block.size(), &stats), BlockStatus::Ok);
  EXPECT_EQ(stats.overReview, 1u);
  EXPECT_EQ(stats.longestLine, 41u);
}

TEST(CreditsTextBlock, TwoHundredLinesIsTheCap) {
  const std::string atCap = Lines(200, "x");
  const std::string over = Lines(201, "x");
  EXPECT_EQ(nevr_credits_text::ValidateBlock(atCap.data(), atCap.size()), BlockStatus::Ok);
  EXPECT_EQ(nevr_credits_text::ValidateBlock(over.data(), over.size()), BlockStatus::TooManyLines);
}

TEST(CreditsTextBlock, AnEarlierProblemIsNotMaskedByALaterOne) {
  // A pipe on line 1 and a too-long line 2: the verdict names the first thing found, and stats
  // still cover the whole block.
  const std::string block = "a|b\n" + std::string(70, 'z') + "\n";
  BlockStats stats;
  EXPECT_EQ(nevr_credits_text::ValidateBlock(block.data(), block.size(), &stats), BlockStatus::ForbiddenByte);
  EXPECT_EQ(stats.lines, 2u);
}

// ---- is it the blob we know ----

TEST(CreditsTextIdentity, OnlyTheKnownLengthAndHashMatch) {
  EXPECT_EQ(nevr_credits_text::IdentifyBlob(nevr_credits_text::kBlobLength, nevr_credits_text::kBlobSha256),
            BlobIdentity::Match);
  EXPECT_EQ(nevr_credits_text::IdentifyBlob(nevr_credits_text::kBlobLength + 1, nevr_credits_text::kBlobSha256),
            BlobIdentity::LengthMismatch);
  EXPECT_EQ(nevr_credits_text::IdentifyBlob(nevr_credits_text::kBlobLength, std::string(64, '0').c_str()),
            BlobIdentity::HashMismatch);
  EXPECT_EQ(nevr_credits_text::IdentifyBlob(nevr_credits_text::kBlobLength, ""), BlobIdentity::HashUnavailable);
  EXPECT_EQ(nevr_credits_text::IdentifyBlob(nevr_credits_text::kBlobLength, nullptr), BlobIdentity::HashUnavailable);
}

// ---- the buffer: the project block above, the game's bytes after, unchanged and complete ----

TEST(CreditsTextBuffer, TheTailIsTheGamesBytesUnchangedAndCompleteWithItsNul) {
  const std::string block = "CODE CONTRIBUTIONS\n\nZo\xEB\n\n\n\n";
  const size_t blobLen = sizeof(kFakeBlob) - 1;
  const std::vector<uint8_t> buf = nevr_credits_text::BuildBuffer(
      block.data(), block.size(), reinterpret_cast<const uint8_t*>(kFakeBlob), blobLen);
  ASSERT_EQ(buf.size(), block.size() + blobLen + 1);
  EXPECT_EQ(std::memcmp(buf.data(), block.data(), block.size()), 0);
  EXPECT_EQ(std::memcmp(buf.data() + block.size(), kFakeBlob, blobLen), 0);
  EXPECT_EQ(buf.back(), 0);
}

TEST(CreditsTextBuffer, TheGamesNonAsciiBytesSurviveTheCopy) {
  const std::string block = "H\n";
  const size_t blobLen = sizeof(kFakeBlob) - 1;
  const std::vector<uint8_t> buf = nevr_credits_text::BuildBuffer(
      block.data(), block.size(), reinterpret_cast<const uint8_t*>(kFakeBlob), blobLen);
  size_t a9 = 0, fc = 0;
  for (uint8_t b : buf) {
    a9 += b == 0xA9;
    fc += b == 0xFC;
  }
  EXPECT_EQ(a9, 1u);
  EXPECT_EQ(fc, 1u);
}

// ---- the displacement ----

TEST(CreditsTextDisplacement, ReachesAnythingWithinSignedThirtyTwoBits) {
  int32_t disp = 0;
  ASSERT_TRUE(nevr_credits_text::ComputeDisplacement(kInitVa, kBufferVa, &disp));
  EXPECT_EQ(static_cast<int64_t>(kInitVa) + 7 + disp, static_cast<int64_t>(kBufferVa));
  ASSERT_TRUE(nevr_credits_text::ComputeDisplacement(kInitVa, kInitVa - 0x1000, &disp));
  EXPECT_LT(disp, 0);
}

TEST(CreditsTextDisplacement, RefusesWhatDoesNotFit) {
  int32_t disp = 0;
  EXPECT_FALSE(nevr_credits_text::ComputeDisplacement(kInitVa, kInitVa + 7 + 0x80000000ULL, &disp));
  EXPECT_TRUE(nevr_credits_text::ComputeDisplacement(kInitVa, kInitVa + 7 + 0x7FFFFFFFULL, &disp));
  EXPECT_EQ(disp, INT32_MAX);
  EXPECT_FALSE(nevr_credits_text::ComputeDisplacement(kInitVa + 0x200000000ULL, kInitVa, &disp));
}

TEST(CreditsTextDisplacement, MakePatchedIsTheLeaWithTheNewOperand) {
  uint8_t out[nevr_credits_text::kSiteSize];
  nevr_credits_text::MakePatched(0x01020304, out);
  const uint8_t want[7] = {0x48, 0x8d, 0x05, 0x04, 0x03, 0x02, 0x01};
  EXPECT_EQ(std::memcmp(out, want, sizeof(want)), 0);
  nevr_credits_text::MakePatched(-2, out);
  const uint8_t wantNeg[7] = {0x48, 0x8d, 0x05, 0xFE, 0xFF, 0xFF, 0xFF};
  EXPECT_EQ(std::memcmp(out, wantNeg, sizeof(wantNeg)), 0);
}

// ---- retargeting both sites, or neither ----

TEST(CreditsTextRetarget, BothSitesEndUpLoadingTheBuffer) {
  FakeImage image;
  SitePlan a = image.InitPlan();
  SitePlan b = image.DisabledPlan();
  EXPECT_EQ(nevr_credits_text::Retarget(a, b, kBufferVa, CopyWriter), Outcome::Applied);
  EXPECT_EQ(TargetOf(image.init, kInitVa), kBufferVa);
  EXPECT_EQ(TargetOf(image.disabled, kDisabledVa), kBufferVa);
  EXPECT_EQ(image.init[0], 0x48);
  EXPECT_EQ(image.disabled[2], 0x05);
}

TEST(CreditsTextRetarget, ASecondCallIsAVerifiedNoOp) {
  FakeImage image;
  SitePlan a = image.InitPlan();
  SitePlan b = image.DisabledPlan();
  ASSERT_EQ(nevr_credits_text::Retarget(a, b, kBufferVa, CopyWriter), Outcome::Applied);
  SitePlan a2 = image.InitPlan();
  SitePlan b2 = image.DisabledPlan();
  EXPECT_EQ(nevr_credits_text::Retarget(a2, b2, kBufferVa, RefusingWriter), Outcome::AlreadyApplied);
}

TEST(CreditsTextRetarget, ADifferentFirstSiteWritesNothingToEither) {
  FakeImage image;
  image.init[3] ^= 0xFF;
  FakeImage untouched = image;
  SitePlan a = image.InitPlan();
  SitePlan b = image.DisabledPlan();
  EXPECT_EQ(nevr_credits_text::Retarget(a, b, kBufferVa, CopyWriter), Outcome::Mismatch);
  EXPECT_EQ(std::memcmp(image.init, untouched.init, sizeof(image.init)), 0);
  EXPECT_EQ(std::memcmp(image.disabled, untouched.disabled, sizeof(image.disabled)), 0);
}

TEST(CreditsTextRetarget, ADifferentSecondSiteWritesNothingToEither) {
  // The check covers both sites before the first write; a first-site write followed by a
  // second-site check would leave the game half retargeted.
  FakeImage image;
  image.disabled[0] = 0x90;
  FakeImage untouched = image;
  SitePlan a = image.InitPlan();
  SitePlan b = image.DisabledPlan();
  EXPECT_EQ(nevr_credits_text::Retarget(a, b, kBufferVa, CopyWriter), Outcome::Mismatch);
  EXPECT_EQ(std::memcmp(image.init, untouched.init, sizeof(image.init)), 0);
}

TEST(CreditsTextRetarget, OneSitePatchedAndTheOtherNotIsRefused) {
  FakeImage image;
  SitePlan a = image.InitPlan();
  SitePlan b = image.DisabledPlan();
  ASSERT_EQ(nevr_credits_text::Retarget(a, b, kBufferVa, CopyWriter), Outcome::Applied);
  std::memcpy(image.disabled, nevr_credits_text::kDisabledSiteExpected, sizeof(image.disabled));
  SitePlan a2 = image.InitPlan();
  SitePlan b2 = image.DisabledPlan();
  EXPECT_EQ(nevr_credits_text::Retarget(a2, b2, kBufferVa, CopyWriter), Outcome::Mismatch);
}

TEST(CreditsTextRetarget, ABufferOutOfRangeWritesNothing) {
  FakeImage image;
  FakeImage untouched = image;
  SitePlan a = image.InitPlan();
  SitePlan b = image.DisabledPlan();
  EXPECT_EQ(nevr_credits_text::Retarget(a, b, kBase + 0x300000000ULL, CopyWriter), Outcome::OutOfRange);
  EXPECT_EQ(std::memcmp(image.init, untouched.init, sizeof(image.init)), 0);
  EXPECT_EQ(std::memcmp(image.disabled, untouched.disabled, sizeof(image.disabled)), 0);
}

TEST(CreditsTextRetarget, AFailedFirstWriteLeavesBothSitesAsTheyWere) {
  FakeImage image;
  FakeImage untouched = image;
  SitePlan a = image.InitPlan();
  SitePlan b = image.DisabledPlan();
  EXPECT_EQ(nevr_credits_text::Retarget(a, b, kBufferVa, RefusingWriter), Outcome::WriteFailed);
  EXPECT_EQ(std::memcmp(image.init, untouched.init, sizeof(image.init)), 0);
  EXPECT_EQ(std::memcmp(image.disabled, untouched.disabled, sizeof(image.disabled)), 0);
}

TEST(CreditsTextRetarget, AFailedSecondWritePutsTheFirstSiteBack) {
  FakeImage image;
  FakeImage untouched = image;
  SitePlan a = image.InitPlan();
  SitePlan b = image.DisabledPlan();
  ResetWriter(2);
  EXPECT_EQ(nevr_credits_text::Retarget(a, b, kBufferVa, FailNthWriter), Outcome::WriteFailed);
  EXPECT_EQ(std::memcmp(image.init, untouched.init, sizeof(image.init)), 0)
      << "the first site was left retargeted while the second was not";
  EXPECT_EQ(std::memcmp(image.disabled, untouched.disabled, sizeof(image.disabled)), 0);
}

// ---- the records ----

TEST(CreditsTextRecord, SiteRecordNamesTheSiteBytesAndOutcome) {
  char out[256];
  nevr_credits_text::FormatSiteRecord(out, sizeof(out), "init", kInitVa, nevr_credits_text::kInitSiteExpected,
                                      nevr_credits_text::kInitSiteExpected, Outcome::Applied);
  EXPECT_STREQ(out,
               "[NEVR.PATCH] credits_text site=init addr=0x140973520 expected=488d05b9103001 "
               "actual=488d05b9103001 result=applied");
}

TEST(CreditsTextRecord, SiteRecordShowsWhatWasFoundWhenItDiffers) {
  const uint8_t found[7] = {0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90};
  char out[256];
  nevr_credits_text::FormatSiteRecord(out, sizeof(out), "disabled", kDisabledVa,
                                      nevr_credits_text::kDisabledSiteExpected, found, Outcome::Mismatch);
  EXPECT_NE(std::string(out).find("actual=90909090909090 result=mismatch"), std::string::npos);
}

TEST(CreditsTextRecord, BuiltRecordCarriesTheCounts) {
  char out[256];
  nevr_credits_text::FormatBuiltRecord(out, sizeof(out), 8, 60, 18, 0, 30091, 30152, 0x180001000ULL);
  EXPECT_STREQ(out,
               "[NEVR.PATCH] credits_text built block_lines=8 block_bytes=60 longest_line=18 over_review=0 "
               "game_bytes=30091 total_bytes=30152 buffer=0x180001000");
}

// ---- memory near the code (the real function, under Wine) ----

uint8_t g_anchor = 0;

TEST(CreditsTextNearAlloc, GivesWritableMemoryWithinReachOfTheAnchor) {
  const uintptr_t anchor = reinterpret_cast<uintptr_t>(&g_anchor);
  void* p = nevr_near_alloc::AllocateNear(anchor, 40000);
  ASSERT_NE(p, nullptr);
  const uintptr_t at = reinterpret_cast<uintptr_t>(p);
  const uintptr_t distance = at > anchor ? at - anchor : anchor - at;
  EXPECT_LE(distance, nevr_near_alloc::kMaxDistance);
  std::memset(p, 0xAB, 40000);
  EXPECT_EQ(static_cast<uint8_t*>(p)[39999], 0xAB);
  int32_t disp = 0;
  EXPECT_TRUE(nevr_credits_text::ComputeDisplacement(anchor, at, &disp));
  EXPECT_TRUE(VirtualFree(p, 0, MEM_RELEASE));
}

}  // namespace
