// The shared decisions of a map's script claim (core/map_script_check.h; docs/adr/0048 decisions 6, 7, 10 and 15): each refusal
// alone, the order when several hold, the size bounds, and the token each image-rule refusal reports. Pure: no file, no
// Win32, no Log().
#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <string>

#include "core/map_script_check.h"
#include "core/resource_set_registry.h"

namespace {

namespace check = nevr_map_script;

// The digest length the format fixes (a sha-256 in hex), and its neighbours for the must-fail cases.
constexpr std::size_t kDigestChars = nevr_cargo::kSha256HexChars;
constexpr std::size_t kOneByte = 1;
constexpr std::uint64_t kByteLimit = 4096;  // a stand-in for the device's share (MapDocumentBytes); the test sets it
constexpr std::uint64_t kPinned = 1000;
const std::string kDigest(kDigestChars, 'a');

TEST(MapScriptCheck, AnOwnerIsTheRegistrysMapOwnerAndAValidId) {
  EXPECT_EQ(check::ArgumentRefusal("map:arena1", 1, kDigest), nullptr);
  EXPECT_STREQ(check::ArgumentRefusal("plugin-x", 1, kDigest), "bad_owner");
  EXPECT_STREQ(check::ArgumentRefusal("map:", 1, kDigest), "bad_owner");
  EXPECT_STREQ(check::ArgumentRefusal("", 1, kDigest), "bad_owner");
  EXPECT_STREQ(check::ArgumentRefusal("Map:arena1", 1, kDigest), "bad_owner");
}

TEST(MapScriptCheck, TheIdAfterThePrefixFollowsTheMapIdRule) {
  EXPECT_STREQ(check::ArgumentRefusal("map:Arena1", 1, kDigest), "bad_owner");  // upper case
  EXPECT_STREQ(check::ArgumentRefusal("map:1arena", 1, kDigest), "bad_owner");  // a digit first
  EXPECT_STREQ(check::ArgumentRefusal("map:arena-1", 1, kDigest), "bad_owner");  // a hyphen
  EXPECT_EQ(check::ArgumentRefusal("map:arena_1", 1, kDigest), nullptr);
}

TEST(MapScriptCheck, AnOwnerIsAtMostTheRegistrysOwnerBound) {
  const std::string longest = nevr_resource_sets::kMapOwnerPrefix + std::string(nevr_resource_sets::kMaxMapOwnerBytes - 4, 'a');
  EXPECT_EQ(check::ArgumentRefusal(longest, 1, kDigest), nullptr);
  EXPECT_STREQ(check::ArgumentRefusal(longest + "a", 1, kDigest), "bad_owner");
}

TEST(MapScriptCheck, AHashOfZeroNamesNoScript) {
  EXPECT_STREQ(check::ArgumentRefusal("map:arena1", 0, kDigest), "bad_hash");
}

TEST(MapScriptCheck, ADigestIsSixtyFourLowercaseHexCharacters) {
  EXPECT_TRUE(check::IsSha256Digest(kDigest));
  EXPECT_TRUE(check::IsSha256Digest(std::string(kDigestChars, '0')));
  EXPECT_FALSE(check::IsSha256Digest(""));
  EXPECT_FALSE(check::IsSha256Digest(std::string(kDigestChars - kOneByte, 'a')));
  EXPECT_FALSE(check::IsSha256Digest(std::string(kDigestChars + kOneByte, 'a')));
  EXPECT_FALSE(check::IsSha256Digest(std::string(kDigestChars, 'A')));
  EXPECT_FALSE(check::IsSha256Digest(std::string(kDigestChars - kOneByte, 'a') + "g"));
  EXPECT_STREQ(check::ArgumentRefusal("map:arena1", 1, "ABC"), "bad_sha256");
}

TEST(MapScriptCheck, TheArgumentsAreCheckedOwnerThenHashThenDigest) {
  EXPECT_STREQ(check::ArgumentRefusal("plugin-x", 0, "ABC"), "bad_owner");
  EXPECT_STREQ(check::ArgumentRefusal("map:arena1", 0, "ABC"), "bad_hash");
}

TEST(MapScriptCheck, ASizeIsOneByteUpToTheDeviceLimit) {
  EXPECT_STREQ(check::SizeRefusal(0, 0, kByteLimit), "bad_size");
  EXPECT_EQ(check::SizeRefusal(kOneByte, 0, kByteLimit), nullptr);
  EXPECT_EQ(check::SizeRefusal(kByteLimit, 0, kByteLimit), nullptr);
  EXPECT_STREQ(check::SizeRefusal(kByteLimit + kOneByte, 0, kByteLimit), "bad_size");
  EXPECT_STREQ(check::SizeRefusal(~std::uint64_t{0}, 0, kByteLimit), "bad_size");  // a negative size from the OS
}

TEST(MapScriptCheck, ASizeNoDeviceFactAllowsIsRefused) {
  EXPECT_STREQ(check::SizeRefusal(kOneByte, 0, 0), "bad_size");
}

TEST(MapScriptCheck, APinnedSizeMustBeTheIndexMembersSize) {
  EXPECT_EQ(check::SizeRefusal(kPinned, kPinned, kByteLimit), nullptr);
  EXPECT_STREQ(check::SizeRefusal(kPinned - kOneByte, kPinned, kByteLimit), "bad_size");
  EXPECT_STREQ(check::SizeRefusal(kPinned + kOneByte, kPinned, kByteLimit), "bad_size");
}

TEST(MapScriptCheck, TheBytesMustHaveTheManifestsDigest) {
  EXPECT_EQ(check::DigestRefusal(kDigest, kDigest), nullptr);
  EXPECT_STREQ(check::DigestRefusal(kDigest, std::string(kDigestChars, 'b')), "sha_mismatch");
}

TEST(MapScriptCheck, EachClaimRefusalNamesItsToken) {
  check::ClaimFacts facts;
  EXPECT_EQ(check::ClaimRefusal(facts), nullptr);
  facts = {};
  facts.withdrawn = true;
  EXPECT_STREQ(check::ClaimRefusal(facts), "withdrawn");
  facts = {};
  facts.signed_only = true;
  EXPECT_STREQ(check::ClaimRefusal(facts), "signed_only");
  facts = {};
  facts.revoked = true;
  EXPECT_STREQ(check::ClaimRefusal(facts), "revoked");
  facts = {};
  facts.duplicate = true;
  EXPECT_STREQ(check::ClaimRefusal(facts), "duplicate");
  facts = {};
  facts.stock = true;
  EXPECT_STREQ(check::ClaimRefusal(facts), "stock_hash");
  facts = {};
  facts.pack = true;
  EXPECT_STREQ(check::ClaimRefusal(facts), "pack_hash");
}

TEST(MapScriptCheck, WhenSeveralClaimRefusalsHoldTheEarlierOneInTheOrderNamesIt) {
  // The order: withdrawn, signed_only, revoked, duplicate, stock_hash, pack_hash.
  check::ClaimFacts facts;
  facts.withdrawn = facts.signed_only = facts.revoked = facts.duplicate = facts.stock = facts.pack = true;
  EXPECT_STREQ(check::ClaimRefusal(facts), "withdrawn");
  facts.withdrawn = false;
  EXPECT_STREQ(check::ClaimRefusal(facts), "signed_only");
  facts.signed_only = false;
  EXPECT_STREQ(check::ClaimRefusal(facts), "revoked");
  facts.revoked = false;
  EXPECT_STREQ(check::ClaimRefusal(facts), "duplicate");
  facts.duplicate = false;
  EXPECT_STREQ(check::ClaimRefusal(facts), "stock_hash");
  facts.stock = false;
  EXPECT_STREQ(check::ClaimRefusal(facts), "pack_hash");
}

// Every token of the image rules' vocabulary maps to itself; anything else maps to image_rejected.
TEST(MapScriptCheck, EveryImageRuleTokenMapsToItself) {
  for (const char* token : nevr_script_image::kImageRuleTokens) EXPECT_STREQ(check::ImageRefusalToken(token), token) << token;
  EXPECT_STREQ(check::ImageRefusalToken("elf_made_up"), "image_rejected");
}

}  // namespace
