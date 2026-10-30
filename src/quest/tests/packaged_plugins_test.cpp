// Host test for the packaged-plugin manifest (src/quest/plugin/packaged_plugins.h): the parser that reads the file the repack
// writes beside the plugins, and the merge with config.yaml (docs/adr/0019-quest-plugin-loader.md, section 2, rule D3).
// The loader's refusal of a file whose bytes differ from the manifest is plugin_loader_test.cpp's.
//
// Run: packaged_plugins_test

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "quest/plugin/packaged_plugins.h"
#include "quest/plugin/plugin_plan.h"
#include "quest/tests/mini_test.h"

using namespace nevr_quest::plugin;

namespace {

const std::string kHexA(64, 'a');
const std::string kHexB(64, 'b');

// One packaged entry as the repack writes it.
nlohmann::json Entry(const std::string& name, const std::string& sha = kHexA, bool enabled = true, bool early = false) {
  return {{"name", name}, {"file", "lib" + name + ".so"}, {"sha256", sha}, {"enabled", enabled}, {"early", early}};
}

std::string Doc(const nlohmann::json& plugins, int schema = 1) {
  return nlohmann::json({{"schema", schema}, {"plugins", plugins}}).dump(-1, ' ', false) + "\n";
}

bool Parse(const std::string& text, std::vector<PackagedPlugin>* out, std::string* why) {
  out->clear();
  why->clear();
  return ParseManifest(text, out, why);
}

PluginLoadItem Config(const std::string& name, const std::string& file, bool enabled = true, bool early = false) {
  PluginLoadItem item;
  item.name = name;
  item.file = file;
  item.enabled = enabled;
  item.early = early;
  item.args_json = "{}";
  return item;
}

PackagedPlugin Packaged(const std::string& name, bool enabled = true, bool early = false) {
  PackagedPlugin p;
  p.name = name;
  p.file = "lib" + name + ".so";
  p.sha256 = kHexA;
  p.enabled = enabled;
  p.early = early;
  return p;
}

// ---- the parser ----------------------------------------------------------------------------------------------------------

TEST(ManifestParseAcceptsTheRepacksBytes) {
  std::vector<PackagedPlugin> out;
  std::string why;
  const std::string text = Doc(nlohmann::json::array({Entry("fx", kHexB, true, true)}));
  CHECK(Parse(text, &out, &why));
  CHECK_EQ(why, std::string(""));
  CHECK_EQ(out.size(), std::size_t{1});
  if (out.size() != 1) return;
  CHECK_EQ(out[0].name, std::string("fx"));
  CHECK_EQ(out[0].file, std::string("libfx.so"));
  CHECK_EQ(out[0].sha256, kHexB);
  CHECK(out[0].enabled);
  CHECK(out[0].early);
}

TEST(ManifestParseAcceptsAnEmptyList) {
  std::vector<PackagedPlugin> out;
  std::string why;
  CHECK(Parse(Doc(nlohmann::json::array()), &out, &why));
  CHECK(out.empty());
}

TEST(ManifestParseRefusesEachBadShapeAsAWholeManifest) {
  struct Case {
    const char* what;
    std::string text;
    const char* token;
  };
  nlohmann::json two = nlohmann::json::array({Entry("fx"), Entry("gy")});
  nlohmann::json bad_file = nlohmann::json::array({Entry("fx")});
  bad_file[0]["file"] = "libother.so";
  nlohmann::json slash = nlohmann::json::array({Entry("a/b")});
  slash[0]["file"] = "liba/b.so";
  nlohmann::json upper_sha = nlohmann::json::array({Entry("fx", std::string(64, 'A'))});
  nlohmann::json short_sha = nlohmann::json::array({Entry("fx", std::string(63, 'a'))});
  nlohmann::json no_bool = nlohmann::json::array({Entry("fx")});
  no_bool[0].erase("enabled");
  nlohmann::json dup = nlohmann::json::array({Entry("fx"), Entry("FX")});
  dup[1]["file"] = "libFX.so";
  nlohmann::json many = nlohmann::json::array();
  for (int i = 0; i < 65; ++i) many.push_back(Entry("p" + std::to_string(i)));
  const Case cases[] = {
      {"not json", "{", "not_json"},
      {"schema 2", Doc(nlohmann::json::array({Entry("fx")}), 2), "bad_schema"},
      {"file not lib<name>.so", Doc(bad_file), "bad_file"},
      {"file with a separator", Doc(slash), "bad_file"},
      {"upper-case sha256", Doc(upper_sha), "bad_sha256"},
      {"short sha256", Doc(short_sha), "bad_sha256"},
      {"missing bool", Doc(no_bool), "bad_entry"},
      {"duplicate file, case-insensitive", Doc(dup), "duplicate_file"},
      {"65 entries", Doc(many), "too_many"},
      {"more than 64 KiB", std::string(64 * 1024 + 1, ' '), "too_large"},
  };
  for (const Case& c : cases) {
    std::vector<PackagedPlugin> out;
    std::string why;
    CHECK(!Parse(c.text, &out, &why));
    CHECK_EQ(why, std::string(c.token));
    CHECK(out.empty());  // one bad entry refuses the whole manifest, not only that entry
  }
  // Two valid entries parse, so the refusals above come from the bad entries and not from the document shape.
  std::vector<PackagedPlugin> out;
  std::string why;
  CHECK(Parse(Doc(two), &out, &why));
  CHECK_EQ(out.size(), std::size_t{2});
}

TEST(ManifestParseIgnoresUnknownKeys) {
  std::vector<PackagedPlugin> out;
  std::string why;
  nlohmann::json e = Entry("fx");
  e["future"] = 7;
  CHECK(Parse(Doc(nlohmann::json::array({e})), &out, &why));
  CHECK_EQ(out.size(), std::size_t{1});
}

// ---- the merge (D3) ------------------------------------------------------------------------------------------------------

TEST(MergeKeepsManifestOrderThenConfigOnlyEntries) {
  // Config "a" names libzz.so, which is not packaged, so it matches nothing and is config-only (as is "c").
  const std::vector<PlannedItem> merged = MergePackaged({Packaged("a"), Packaged("b")},
                                                        {Config("c", "libc.so"), Config("a", "libzz.so")});
  CHECK_EQ(merged.size(), std::size_t{4});
  if (merged.size() != 4) return;
  CHECK_EQ(merged[0].item.name, std::string("a"));
  CHECK_EQ(merged[1].item.name, std::string("b"));
  CHECK_EQ(merged[2].item.name, std::string("c"));
  CHECK_EQ(merged[3].item.name, std::string("a"));
  CHECK(merged[2].source == Source::kConfig);
  CHECK(merged[3].source == Source::kConfig);
}

TEST(MergeMatchesTheFileCaseInsensitively) {
  const std::vector<PlannedItem> merged = MergePackaged({Packaged("fx")}, {Config("fx", "LIBFX.SO")});
  CHECK_EQ(merged.size(), std::size_t{1});
  if (merged.size() != 1) return;
  CHECK(merged[0].overridden);
  CHECK(merged[0].source == Source::kPackaged);
  CHECK_EQ(merged[0].expectedSha256, kHexA);
}

TEST(MergeAppliesTheQuestDefaultFileBeforeMatching) {
  const std::vector<PlannedItem> merged = MergePackaged({Packaged("fx")}, {Config("fx", "fx.dll")});
  CHECK_EQ(merged.size(), std::size_t{1});
  if (merged.size() != 1) return;
  CHECK(merged[0].overridden);
  CHECK_EQ(merged[0].item.file, std::string("libfx.so"));
}

TEST(MergeConfigEnabledFalseDisablesAndNamingAnOffPluginEnablesIt) {
  const std::vector<PlannedItem> off = MergePackaged({Packaged("fx")}, {Config("fx", "libfx.so", false)});
  CHECK_EQ(off.size(), std::size_t{1});
  if (off.size() == 1) CHECK(!off[0].item.enabled);
  const std::vector<PlannedItem> on = MergePackaged({Packaged("fx", false)}, {Config("fx", "libfx.so", true)});
  CHECK_EQ(on.size(), std::size_t{1});
  if (on.size() == 1) CHECK(on[0].item.enabled);
}

TEST(MergeEarlyIsManifestOrConfig) {
  const std::vector<PlannedItem> kept = MergePackaged({Packaged("fx", true, true)}, {Config("fx", "libfx.so")});
  CHECK_EQ(kept.size(), std::size_t{1});
  if (kept.size() == 1) CHECK(kept[0].item.early);  // config omits early: the manifest's early stays
  const std::vector<PlannedItem> made = MergePackaged({Packaged("fx")}, {Config("fx", "libfx.so", true, true)});
  CHECK_EQ(made.size(), std::size_t{1});
  if (made.size() == 1) CHECK(made[0].item.early);  // config early on a normal packaged entry makes it early
}

TEST(MergeSecondConfigEntryForTheSameFileIsADuplicate) {
  const std::vector<PlannedItem> merged =
      MergePackaged({Packaged("fx")}, {Config("fx", "libfx.so"), Config("fx", "libfx.so")});
  CHECK_EQ(merged.size(), std::size_t{2});
  if (merged.size() != 2) return;
  const std::vector<PlanEntry> plan = BuildPlan(merged, true);
  CHECK_EQ(plan.size(), std::size_t{2});
  if (plan.size() != 2) return;
  CHECK(plan[1].outcome == Outcome::kRefused);
  CHECK_EQ(plan[1].reasonToken, std::string(reason::kDuplicateFile));
}

TEST(MergeOverridesNameArgsRequiredAndTarget) {
  PluginLoadItem config = Config("fx", "libfx.so");
  config.required = true;
  config.target = "pc";
  config.args_json = "{\"k\":\"v\"}";
  config.args_replaced_keys = {"k"};
  const std::vector<PlannedItem> merged = MergePackaged({Packaged("fx")}, {config});
  CHECK_EQ(merged.size(), std::size_t{1});
  if (merged.size() != 1) return;
  CHECK(merged[0].item.required);
  CHECK_EQ(merged[0].item.target, std::string("pc"));
  CHECK_EQ(merged[0].item.args_json, std::string("{\"k\":\"v\"}"));
  CHECK_EQ(merged[0].item.args_replaced_keys.size(), std::size_t{1});
  CHECK_EQ(merged[0].expectedSha256, kHexA);  // the manifest's hash stays
}

}  // namespace

int main(int argc, char** argv) { return mini_test::RunAll(argc, argv); }
