// The shared decisions of a map's script claim (core/map_script_check.h; docs/adr/0048 decisions 6, 7 and 10): each refusal
// alone, the order when several hold, the size bounds, and the token each image-rule refusal reports. Pure: no file, no
// Win32, no Log().
#include <gtest/gtest.h>

#include <cstdint>
#include <string>

#include "core/map_script_check.h"

namespace {

namespace check = nevr_map_script;

// The digest length the format fixes (a sha-256 in hex), and its neighbours for the must-fail cases.
constexpr std::size_t kDigestChars = nevr_cargo::kSha256HexChars;

const std::string kDigest(kDigestChars, 'a');

TEST(MapScriptCheck, AnOwnerIsMapColonAndAnId) {
  EXPECT_EQ(check::ArgumentRefusal("map:arena1", 1, kDigest), nullptr);
  EXPECT_STREQ(check::ArgumentRefusal("plugin-x", 1, kDigest), "bad_owner");
  EXPECT_STREQ(check::ArgumentRefusal("map:", 1, kDigest), "bad_owner");
  EXPECT_STREQ(check::ArgumentRefusal("", 1, kDigest), "bad_owner");
  EXPECT_STREQ(check::ArgumentRefusal("Map:arena1", 1, kDigest), "bad_owner");
}

TEST(MapScriptCheck, AHashOfZeroNamesNoScript) {
  EXPECT_STREQ(check::ArgumentRefusal("map:arena1", 0, kDigest), "bad_hash");
}

TEST(MapScriptCheck, ADigestIsSixtyFourLowercaseHexCharacters) {
  EXPECT_TRUE(check::IsSha256Digest(kDigest));
  EXPECT_TRUE(check::IsSha256Digest(std::string(kDigestChars, '0')));
  EXPECT_FALSE(check::IsSha256Digest(""));
  EXPECT_FALSE(check::IsSha256Digest(std::string(kDigestChars - 1, 'a')));
  EXPECT_FALSE(check::IsSha256Digest(std::string(kDigestChars + 1, 'a')));
  EXPECT_FALSE(check::IsSha256Digest(std::string(kDigestChars, 'A')));
  EXPECT_FALSE(check::IsSha256Digest(std::string(kDigestChars - 1, 'a') + "g"));
  EXPECT_STREQ(check::ArgumentRefusal("map:arena1", 1, "ABC"), "bad_sha256");
}

TEST(MapScriptCheck, TheArgumentsAreCheckedOwnerThenHashThenDigest) {
  EXPECT_STREQ(check::ArgumentRefusal("plugin-x", 0, "ABC"), "bad_owner");
  EXPECT_STREQ(check::ArgumentRefusal("map:arena1", 0, "ABC"), "bad_hash");
}

TEST(MapScriptCheck, ASizeIsOneByteUpToTheBound) {
  EXPECT_STREQ(check::SizeRefusal(0), "bad_size");
  EXPECT_EQ(check::SizeRefusal(1), nullptr);
  EXPECT_EQ(check::SizeRefusal(check::kMaxScriptBytes), nullptr);
  EXPECT_STREQ(check::SizeRefusal(check::kMaxScriptBytes + 1), "bad_size");
  EXPECT_STREQ(check::SizeRefusal(~std::uint64_t{0}), "bad_size");  // a negative size from the OS, read as unsigned
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

// Every token the image rules (core/script_image_rules.h) name maps to itself; a token the table does not hold maps to
// image_rejected, so a new rule without its entry here is caught by the platform tests that run every rule.
TEST(MapScriptCheck, EveryImageRuleTokenMapsToItself) {
  const char* const tokens[] = {
      "pe_truncated",   "pe_not_pe",          "pe_not_amd64",     "pe_not_pe32plus",   "pe_not_dll",
      "pe_imports",     "pe_delay_imports",   "pe_bound_imports", "pe_iat",            "pe_tls",
      "pe_clr",         "pe_entry_point",     "pe_no_exports",    "pe_export_count",   "pe_export_name",
      "pe_export_forwarded", "pe_export_not_code", "pe_section_name", "pe_section_wx",
      "elf_truncated",  "elf_not_elf",        "elf_not_64",       "elf_not_le",        "elf_not_dyn",
      "elf_not_aarch64", "elf_segment_wx",    "elf_no_dynamic",   "elf_textrel",       "elf_no_symbols",
      "elf_needed",     "elf_init",           "elf_no_export",    "elf_export_not_code"};
  for (const char* token : tokens) EXPECT_STREQ(check::ImageRefusalToken(token), token) << token;
  EXPECT_STREQ(check::ImageRefusalToken("elf_made_up"), "image_rejected");
}

}  // namespace
