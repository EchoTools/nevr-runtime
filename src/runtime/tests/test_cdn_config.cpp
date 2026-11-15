// test_cdn_config.cpp: the CDN settings the PC and the Quest read from config.yaml (cdn/cdn_config.h, ADR 0045 decision 9).
// Absent keys are the code's defaults and say nothing; a key that is set is taken; a key that is wrong is one warning that names the
// key and never the value, and its default stays. Pure: no game, no network, no log (the PC's start logs each warning,
// runtime/lifecycle/cdn_start.cpp).

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "cdn/cdn_config.h"
#include "core/nevr_config.h"

namespace {

using nevr_cdn::ConfigRead;
using nevr_cdn::ReadSettings;

constexpr const char* kStagingBase = "https://cargo.echo.taxi/v1-staging/";
constexpr const char* kStagingManifest = "manifests/6f6d9f170128180ee2599b0775cc99ee324abdbb711cf605ecce91dc50754e6a.json.zst";
constexpr const char* kSecretLooking = "SECRET-TOKEN-8841";
constexpr std::size_t kManyLabels = 40;  // more than kMaxAssetLabels
constexpr std::size_t kOneLabel = 1;
constexpr std::size_t kOneWarning = 1;
constexpr std::size_t kThreeLabels = 3;

ConfigRead Read(const std::string& yaml) { return ReadSettings(nevr::NevrConfig::LoadFromString("version: \"1\"\n" + yaml)); }

bool Mentions(const ConfigRead& r, const std::string& text) {
  for (const std::string& w : r.warnings) {
    if (w.find(text) != std::string::npos) return true;
  }
  return false;
}

void ExpectDefaults(const ConfigRead& r) {
  EXPECT_TRUE(r.settings.cdn_url.empty());
  EXPECT_TRUE(r.settings.manifest.empty());
  EXPECT_FALSE(r.settings.required_only);
  EXPECT_TRUE(r.settings.labels.empty());
  EXPECT_FALSE(r.settings.from_config);
}

TEST(CdnConfig, AbsentKeysAreTheCodesDefaultsAndSayNothing) {
  for (const char* yaml : {"", "assets: {}\n", "assets:\n", "graphics:\n  hand_flip: true\n"}) {
    const ConfigRead r = Read(yaml);
    ExpectDefaults(r);
    EXPECT_TRUE(r.warnings.empty()) << yaml;
  }
}

TEST(CdnConfig, TheStagingLinesAreTaken) {
  const ConfigRead r = Read(std::string("assets:\n  cdn_url: \"") + kStagingBase + "\"\n  manifest: \"" + kStagingManifest +
                            "\"\n  labels:\n    combat_content: on\n");
  EXPECT_TRUE(r.warnings.empty());
  EXPECT_EQ(r.settings.cdn_url, kStagingBase);
  EXPECT_EQ(r.settings.manifest, kStagingManifest);
  ASSERT_EQ(r.settings.labels.size(), kOneLabel);
  EXPECT_EQ(r.settings.labels[0].first, "combat_content");
  EXPECT_EQ(r.settings.labels[0].second, "on");
  EXPECT_FALSE(r.settings.required_only);
  EXPECT_TRUE(r.settings.from_config);
}

TEST(CdnConfig, EveryKeyIsTakenAndABaseGetsItsTrailingSlash) {
  const ConfigRead r = Read(
      "assets:\n  cdn_url: https://cdn.example.test/base\n  manifest: v1/test/manifest.json.zst\n  required_only: true\n"
      "  labels:\n    lobby_posters: required\n    combat_content: off\n    extra_one: on\n");
  EXPECT_TRUE(r.warnings.empty());
  EXPECT_EQ(r.settings.cdn_url, "https://cdn.example.test/base/");
  EXPECT_EQ(r.settings.manifest, "v1/test/manifest.json.zst");
  EXPECT_TRUE(r.settings.required_only);
  ASSERT_EQ(r.settings.labels.size(), kThreeLabels);
  EXPECT_EQ(r.settings.labels[0].second, "required");
  EXPECT_EQ(r.settings.labels[1].second, "off");
  EXPECT_EQ(r.settings.labels[kThreeLabels - 1].first, "extra_one");
}

TEST(CdnConfig, AKeyThatIsFalseIsStillTaken) {
  const ConfigRead r = Read("assets:\n  required_only: false\n");
  EXPECT_FALSE(r.settings.required_only);
  EXPECT_TRUE(r.settings.from_config);
  EXPECT_TRUE(r.warnings.empty());
}

TEST(CdnConfig, ABaseThatIsNotAnHttpsAddressIsRefusedByKeyAndNeverByValue) {
  for (const std::string& bad : std::vector<std::string>{std::string("http://") + kSecretLooking + ".example.test/", std::string("https://"),
                                std::string("https://") + kSecretLooking + "@host.example/", std::string("https://h.example/a b/"),
                                std::string("https://h.example/?q=") + kSecretLooking, std::string(kSecretLooking)}) {
    const ConfigRead r = Read("assets:\n  cdn_url: \"" + bad + "\"\n");
    ExpectDefaults(r);
    ASSERT_EQ(r.warnings.size(), kOneWarning) << bad;
    EXPECT_TRUE(Mentions(r, "key=assets.cdn_url rejected reason=not_an_https_address (default stays)")) << bad;
    EXPECT_FALSE(Mentions(r, kSecretLooking)) << bad;
  }
}

TEST(CdnConfig, ABaseAtTheLengthBoundIsTakenAndOneByteMoreIsRefused) {
  const std::string prefix = "https://h.example/";
  const std::string atBound = prefix + std::string(nevr_cdn::kMaxCdnBaseBytes - prefix.size() - 1, 'a') + "/";
  ASSERT_EQ(atBound.size(), nevr_cdn::kMaxCdnBaseBytes);
  EXPECT_EQ(Read("assets:\n  cdn_url: \"" + atBound + "\"\n").settings.cdn_url, atBound);
  const ConfigRead over = Read("assets:\n  cdn_url: \"" + atBound + "a\"\n");
  EXPECT_TRUE(over.settings.cdn_url.empty());
  EXPECT_TRUE(Mentions(over, "key=assets.cdn_url rejected"));
}

TEST(CdnConfig, AManifestThatIsNotARelativePathIsRefusedByKey) {
  for (const std::string& bad : std::vector<std::string>{std::string("/abs/manifest.json.zst"), std::string("v1/../manifest.json.zst"), std::string("a b"),
                                std::string("v1/man?x"), std::string("")}) {
    const ConfigRead r = Read("assets:\n  manifest: \"" + bad + "\"\n");
    ExpectDefaults(r);
    EXPECT_TRUE(Mentions(r, "key=assets.manifest rejected reason=not_a_relative_path (default stays)")) << bad;
  }
  const std::string longest(nevr_cdn::kMaxManifestPathBytes, 'a');
  EXPECT_EQ(Read("assets:\n  manifest: \"" + longest + "\"\n").settings.manifest, longest);
  EXPECT_TRUE(Read("assets:\n  manifest: \"" + longest + "a\"\n").settings.manifest.empty());
}

TEST(CdnConfig, ARequiredOnlyThatIsNoBooleanIsRefusedByKey) {
  const ConfigRead r = Read(std::string("assets:\n  required_only: ") + kSecretLooking + "\n");
  ExpectDefaults(r);
  EXPECT_TRUE(Mentions(r, "key=assets.required_only rejected reason=not_a_boolean (default stays)"));
  EXPECT_FALSE(Mentions(r, kSecretLooking));
}

TEST(CdnConfig, ALabelWithABadStateOrNameIsLeftOutAndTheOthersStay) {
  const ConfigRead r = Read("assets:\n  labels:\n    ok_label: on\n    Bad-Label: on\n    wrong_state: maybe\n    nested:\n      a: b\n");
  ASSERT_EQ(r.settings.labels.size(), kOneLabel);
  EXPECT_EQ(r.settings.labels[0].first, "ok_label");
  EXPECT_TRUE(r.settings.from_config);
  EXPECT_TRUE(Mentions(r, "key=assets.labels rejected reason=bad_label_name name=Bad-Label (ignored)"));
  EXPECT_TRUE(Mentions(r, "key=assets.labels rejected reason=bad_state label=wrong_state (the label keeps its default; allowed: on, off, required)"));
  EXPECT_TRUE(Mentions(r, "reason=bad_state label=nested"));
}

TEST(CdnConfig, ALabelNameThatIsNotPlainIsDescribedNotRepeated) {
  const ConfigRead r = Read("assets:\n  labels:\n    \"" + std::string(nevr::kMaxNamedConfigWordBytes + 1, 'A') + "\": on\n");
  EXPECT_TRUE(r.settings.labels.empty());
  EXPECT_TRUE(Mentions(r, "reason=bad_label_name name=(longer than 32 bytes) (ignored)"));
}

TEST(CdnConfig, ALabelAtTheLengthBoundIsTakenAndOneByteMoreIsRefused) {
  const std::string atBound(nevr_cdn::kMaxLabelBytes, 'a');
  const ConfigRead ok = Read("assets:\n  labels:\n    " + atBound + ": on\n");
  ASSERT_EQ(ok.settings.labels.size(), kOneLabel);
  EXPECT_EQ(ok.settings.labels[0].first, atBound);
  const ConfigRead over = Read("assets:\n  labels:\n    " + atBound + "a: on\n");
  EXPECT_TRUE(over.settings.labels.empty());
  EXPECT_TRUE(Mentions(over, "reason=bad_label_name"));
}

TEST(CdnConfig, LabelsThatAreNoMappingAreRefusedAndTheTableIsBounded) {
  const ConfigRead scalar = Read("assets:\n  labels: on\n");
  ExpectDefaults(scalar);
  EXPECT_TRUE(Mentions(scalar, "key=assets.labels rejected reason=not_a_mapping (default stays)"));

  std::string many = "assets:\n  labels:\n";
  for (std::size_t i = 0; i < kManyLabels; ++i) many += "    label_" + std::to_string(i) + ": on\n";
  const ConfigRead r = Read(many);
  EXPECT_EQ(r.settings.labels.size(), nevr_cdn::kMaxAssetLabels);
  // The reader stops at the first label past the bound, so it counts that one.
  EXPECT_TRUE(Mentions(r, "reason=too_many_labels more=" + std::to_string(kOneLabel) + " (ignored)"));
}

TEST(CdnConfig, OneBadKeyDoesNotStopTheOthers) {
  const ConfigRead r = Read(std::string("assets:\n  cdn_url: http://plain.example.test/\n  manifest: \"") + kStagingManifest +
                            "\"\n  labels:\n    combat_content: on\n");
  EXPECT_TRUE(r.settings.cdn_url.empty());
  EXPECT_EQ(r.settings.manifest, kStagingManifest);
  ASSERT_EQ(r.settings.labels.size(), kOneLabel);
  EXPECT_EQ(r.warnings.size(), kOneWarning);
  EXPECT_TRUE(r.settings.from_config);
}

TEST(CdnConfig, TheQuestsOwnWholeGameKeyIsNotTheCdnReadersAndSaysNothing) {
  const ConfigRead r = Read("assets:\n  whole_game: true\n");
  ExpectDefaults(r);
  EXPECT_TRUE(r.warnings.empty());
}

}  // namespace
