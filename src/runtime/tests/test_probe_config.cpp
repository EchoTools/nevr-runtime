// maps_probe.yaml for the owner probes (src/runtime/maps/probe/probe_logic.cpp).
#include <gtest/gtest.h>

#include "runtime/maps/probe/probe_logic.h"

using nevr_maps_probe::ParseProbeConfig;
using nevr_maps_probe::ProbeConfig;
using nevr_maps_probe::SkewExpect;

TEST(ProbeConfig, EmptyFileTurnsNothingOn) {
  const ProbeConfig config = ParseProbeConfig("");
  EXPECT_FALSE(config.rules);
  EXPECT_FALSE(config.skew);
  EXPECT_FALSE(config.sublevel);
  EXPECT_EQ(config.skew_expect, SkewExpect::kNone);
  EXPECT_EQ(config.ignored_lines, 0u);
}

TEST(ProbeConfig, ReadsTheShippedTemplateWithOneProbeOn) {
  const ProbeConfig config = ParseProbeConfig(
      "\xEF\xBB\xBF# maps_probe.yaml\r\n"
      "rules: off\r\n"
      "skew: off\r\n"
      "skew_expect: none\r\n"
      "sublevel: on   # P-SUBLEVEL\r\n");
  EXPECT_FALSE(config.rules);
  EXPECT_FALSE(config.skew);
  EXPECT_TRUE(config.sublevel);
  EXPECT_EQ(config.ignored_lines, 0u);
}

TEST(ProbeConfig, SwitchSpellingsAndCase) {
  const ProbeConfig config = ParseProbeConfig("Rules: ON\nskew: \"yes\"\nsublevel: 1\n");
  EXPECT_TRUE(config.rules);
  EXPECT_TRUE(config.skew);
  EXPECT_TRUE(config.sublevel);
}

TEST(ProbeConfig, SkewExpectForms) {
  EXPECT_EQ(ParseProbeConfig("skew_expect: missing").skew_expect, SkewExpect::kMissing);
  const ProbeConfig v1 = ParseProbeConfig("skew_expect: probe@1");
  EXPECT_EQ(v1.skew_expect, SkewExpect::kVersion);
  EXPECT_EQ(v1.skew_version, 1u);
  const ProbeConfig v42 = ParseProbeConfig("skew_expect: 'probe@42'");
  EXPECT_EQ(v42.skew_expect, SkewExpect::kVersion);
  EXPECT_EQ(v42.skew_version, 42u);
}

TEST(ProbeConfig, BadLinesAreCountedAndChangeNothing) {
  const ProbeConfig config = ParseProbeConfig(
      "rules: maybe\n"
      "skew_expect: probe@\n"
      "skew_expect: probe@123\n"
      "skew_expect: probe@x\n"
      "boot: on\n"
      "no colon here\n"
      "sublevel: on\n");
  EXPECT_FALSE(config.rules);
  EXPECT_EQ(config.skew_expect, SkewExpect::kNone);
  EXPECT_TRUE(config.sublevel);
  EXPECT_EQ(config.ignored_lines, 6u);
}

TEST(ProbeConfig, LastLineWithoutNewlineIsRead) {
  EXPECT_TRUE(ParseProbeConfig("rules: off\nrules: on").rules);
}
