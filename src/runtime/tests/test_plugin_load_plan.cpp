// test_plugin_load_plan.cpp — the PURE core of the config-driven plugin loader
// (N134 S6). Locks the three things the loader delegates to pure code:
//
//   1. BuildLoadPlan: ordered, enabled-only selection of a parsed `plugins:` list,
//      file-name defaulting, and required/target carry-through.
//   2. ArgsToJson: the v4 args_json contract (flat JSON object, string values,
//      deterministic key order) — including nested args flattened to dotted keys
//      and ${VAR} interpolation applied before serialization.
//   3. ChoosePluginInit: prefer the v4 InitEx, fall back to the v3 Init (so a v3
//      plugin still loads), None when neither is exported.
//
// The LoadLibrary-entangled half (LoadPlugins in plugin_loader.cpp — required ->
// ServerFatal, actual GetProcAddress/init dispatch) is NOT unit-testable without a
// real DLL and is exercised by the differential server run; this pins the logic
// that IS pure. Links plugin_load_plan.cpp + nevr_core + yaml-cpp, no game stubs.

#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "core/nevr_config.h"
#include "runtime/ext/plugin_load_plan.h"
#include "runtime/ext/plugin_load_plan_build.h"

namespace {

using nevr::NevrConfig;
using nevr_plugincfg::ArgsToJson;
using nevr_plugincfg::BuildLoadPlan;

// ---------------------------------------------------------------------------
// 1. BuildLoadPlan — ordered, enabled-only selection.
// ---------------------------------------------------------------------------
TEST(PluginLoadPlan, OrderPreservedAndDisabledSkipped) {
  const NevrConfig cfg = NevrConfig::LoadFromString(R"YAML(
plugins:
  - name: first
    file: first.dll
  - name: middle
    enabled: false
  - name: last
    file: last.dll
)YAML");
  const std::vector<PluginLoadItem> plan = BuildLoadPlan(cfg);
  ASSERT_EQ(plan.size(), 2u);            // middle (enabled:false) dropped
  EXPECT_EQ(plan[0].name, "first");      // list order == load order
  EXPECT_EQ(plan[1].name, "last");
}

TEST(PluginLoadPlan, FileDefaultsToNamePlusDll) {
  const NevrConfig cfg = NevrConfig::LoadFromString(R"YAML(
plugins:
  - name: nevr_example
)YAML");
  const std::vector<PluginLoadItem> plan = BuildLoadPlan(cfg);
  ASSERT_EQ(plan.size(), 1u);
  EXPECT_EQ(plan[0].file, "nevr_example.dll");  // parser default, carried through
}

TEST(PluginLoadPlan, RequiredAndTargetCarried) {
  const NevrConfig cfg = NevrConfig::LoadFromString(R"YAML(
plugins:
  - name: gate
    file: gate.dll
    required: true
    target: server
  - name: opt
    file: opt.dll
)YAML");
  const std::vector<PluginLoadItem> plan = BuildLoadPlan(cfg);
  ASSERT_EQ(plan.size(), 2u);
  EXPECT_TRUE(plan[0].required);
  EXPECT_EQ(plan[0].target, "server");
  EXPECT_FALSE(plan[1].required);   // default
  EXPECT_EQ(plan[1].target, "");    // absent
}

TEST(PluginLoadPlan, EmptyWhenNoPluginsKey) {
  // Config authoritative + no glob fallback: no `plugins:` key -> load nothing.
  const NevrConfig cfg = NevrConfig::LoadFromString("version: \"1\"\n");
  EXPECT_TRUE(BuildLoadPlan(cfg).empty());
}

TEST(PluginLoadPlan, EmptyWhenPluginsListEmpty) {
  const NevrConfig cfg = NevrConfig::LoadFromString("plugins: []\n");
  EXPECT_TRUE(BuildLoadPlan(cfg).empty());
}

// ---------------------------------------------------------------------------
// 2. args_json — the v4 contract: flat object, string values, dotted nesting,
//    interpolation applied. Deterministic key order (std::map -> sorted).
// ---------------------------------------------------------------------------
TEST(PluginLoadPlan, ArgsSerializedFlatWithDottedNesting) {
  const NevrConfig cfg = NevrConfig::LoadFromString(R"YAML(
plugins:
  - name: p
    file: p.dll
    args:
      greeting: hi
      limits:
        max: 5
)YAML");
  const std::vector<PluginLoadItem> plan = BuildLoadPlan(cfg);
  ASSERT_EQ(plan.size(), 1u);
  // sorted keys: "greeting" < "limits.max"; every value a JSON string.
  EXPECT_EQ(plan[0].args_json, R"({"greeting":"hi","limits.max":"5"})");
}

TEST(PluginLoadPlan, NoArgsIsEmptyObject) {
  const NevrConfig cfg = NevrConfig::LoadFromString(R"YAML(
plugins:
  - name: p
    file: p.dll
)YAML");
  const std::vector<PluginLoadItem> plan = BuildLoadPlan(cfg);
  ASSERT_EQ(plan.size(), 1u);
  EXPECT_EQ(plan[0].args_json, "{}");
}

TEST(PluginLoadPlan, ArgsInterpolateThroughNevrConfig) {
  // A ${VAR:-default} arg resolves like every other config scalar (the unset
  // case here yields the default — no env needed, no fail-loud for :- form).
  const NevrConfig cfg = NevrConfig::LoadFromString(R"YAML(
plugins:
  - name: p
    file: p.dll
    args:
      token: "${NEVR_TEST_PLUGIN_ARG:-fallback}"
)YAML");
  const std::vector<PluginLoadItem> plan = BuildLoadPlan(cfg);
  ASSERT_EQ(plan.size(), 1u);
  EXPECT_EQ(plan[0].args_json, R"({"token":"fallback"})");
}

TEST(PluginLoadPlan, ArgsToJsonEmptyMapIsEmptyObject) {
  EXPECT_EQ(ArgsToJson({}), "{}");
}

TEST(PluginLoadPlan, ArgsToJsonSortedStringValues) {
  const std::map<std::string, std::string> args = {{"b", "2"}, {"a", "1"}};
  EXPECT_EQ(ArgsToJson(args), R"({"a":"1","b":"2"})");
}

// ---------------------------------------------------------------------------
// 3. ChoosePluginInit — v4 preferred, v3 fallback, None when neither. This is the
//    backward-compat guarantee in pure form: (hasInitEx=false, hasInit=true) MUST
//    map to Legacy so an existing v3 plugin still loads (called without args).
// ---------------------------------------------------------------------------
TEST(PluginLoadPlan, ChoosePluginInitPrefersExAndFallsBackToV3) {
  EXPECT_EQ(ChoosePluginInit(/*hasInitEx=*/true,  /*hasInit=*/true),  PluginInitKind::Ex);
  EXPECT_EQ(ChoosePluginInit(/*hasInitEx=*/true,  /*hasInit=*/false), PluginInitKind::Ex);
  EXPECT_EQ(ChoosePluginInit(/*hasInitEx=*/false, /*hasInit=*/true),  PluginInitKind::Legacy);
  EXPECT_EQ(ChoosePluginInit(/*hasInitEx=*/false, /*hasInit=*/false), PluginInitKind::None);
}

// ---------------------------------------------------------------------------
// profiles: — DEFERRED (N134). The schema must TOLERATE a profiles key (no
// active-profile selection yet); its presence must not break plugins parsing.
// ---------------------------------------------------------------------------
TEST(PluginLoadPlan, ProfilesKeyToleratedAndIgnored) {
  const NevrConfig cfg = NevrConfig::LoadFromString(R"YAML(
plugins:
  - name: p
    file: p.dll
profiles:
  dev:
    plugins: [p]
  comp:
    plugins: []
)YAML");
  const std::vector<PluginLoadItem> plan = BuildLoadPlan(cfg);
  ASSERT_EQ(plan.size(), 1u);           // the top-level plugins list, unaffected
  EXPECT_EQ(plan[0].name, "p");         // profiles ignored (no selection in S6)
}

}  // namespace
