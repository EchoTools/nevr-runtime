// test_plugin_load_plan.cpp — the PURE core of the config-driven plugin loader
// (N134 S6). Locks the three things the loader delegates to pure code:
//
//   1. BuildLoadPlan: every entry of a parsed `plugins:` list in order with its
//      enabled flag, file-name defaulting, and required/target carry-through.
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
//
// Also pins BuildPluginManifest (plugin_manifest.cpp, #60): the `nevr_plugins`
// array the client login sends, parsed back with nlohmann::json.

#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "core/nevr_config.h"
#include "runtime/ext/plugin_load_plan.h"
#include "runtime/ext/plugin_load_plan_build.h"
#include "runtime/ext/plugin_manifest.h"

namespace {

using nevr::NevrConfig;
using nevr_plugincfg::ArgsToJson;
using nevr_plugincfg::BuildLoadPlan;

// ---------------------------------------------------------------------------
// 1. BuildLoadPlan — every configured entry, in list order, enabled flag carried.
//    (#60: the login reports disabled entries too, so the plan keeps them; the
//    loader skips any entry whose `enabled` is false.)
// ---------------------------------------------------------------------------
TEST(PluginLoadPlan, OrderPreservedAndDisabledCarriedAsNotEnabled) {
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
  ASSERT_EQ(plan.size(), 3u);            // middle (enabled:false) carried, not dropped
  EXPECT_EQ(plan[0].name, "first");      // list order == load order
  EXPECT_EQ(plan[1].name, "middle");
  EXPECT_EQ(plan[2].name, "last");
  EXPECT_TRUE(plan[0].enabled);          // absent enabled defaults to true
  EXPECT_FALSE(plan[1].enabled);
  EXPECT_TRUE(plan[2].enabled);
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

// A value that is not UTF-8 (an ANSI-code-page ${VAR}, e.g. a path with "\xe9")
// must not throw out of the boot path: the args still serialize with the bad
// byte replaced by U+FFFD (EF BF BD) and the clean entry byte-exact.
TEST(PluginLoadPlan, ArgsToJsonInvalidUtf8ReplacedWithFffd) {
  const std::map<std::string, std::string> args = {
      {"path", std::string("C:\\Users\\Ren") + "\xe9" + "\\x.txt"}, {"ok", "1"}};
  std::string out;
  ASSERT_NO_THROW(out = ArgsToJson(args));
  EXPECT_EQ(out, std::string("{\"ok\":\"1\",\"path\":\"C:\\\\Users\\\\Ren") +
                     "\xef\xbf\xbd" + "\\\\x.txt\"}");
}

// The substitution is reported by key (never value) so the loader can log it; a
// clean map reports nothing.
TEST(PluginLoadPlan, ArgsToJsonReportsReplacedKeys) {
  const std::map<std::string, std::string> bad = {
      {"path", "Ren\xe9"}, {"ok", "1"}, {"zbad", "\xff"}};
  std::vector<std::string> keys;
  (void)ArgsToJson(bad, &keys);
  const std::vector<std::string> expected = {"path", "zbad"};
  EXPECT_EQ(keys, expected);

  std::vector<std::string> none;
  (void)ArgsToJson({{"a", "1"}}, &none);
  EXPECT_TRUE(none.empty());
}

// An invalid byte in the KEY (a clean value) is the same failure: the key is
// reported by name, the key reaches the plugin with U+FFFD, the value is untouched.
TEST(PluginLoadPlan, ArgsToJsonInvalidUtf8KeyReportedAndReplaced) {
  const std::map<std::string, std::string> args = {
      {std::string("k") + "\xe9", "v"}, {"ok", "1"}};
  std::vector<std::string> keys;
  std::string out;
  ASSERT_NO_THROW(out = ArgsToJson(args, &keys));
  const std::vector<std::string> expected = {std::string("k") + "\xe9"};
  EXPECT_EQ(keys, expected);
  EXPECT_EQ(out, std::string("{\"k") + "\xef\xbf\xbd" + "\":\"v\",\"ok\":\"1\"}");
}

// The flag reaches the plan item BuildLoadPlan hands the loader.
TEST(PluginLoadPlan, BuildLoadPlanCarriesReplacedKeys) {
  const NevrConfig cfg = NevrConfig::LoadFromString(
      std::string("plugins:\n  - name: p\n    args:\n      path: \"Ren") + "\xe9" +
      "\"\n      ok: \"1\"\n");
  const std::vector<PluginLoadItem> plan = BuildLoadPlan(cfg);
  ASSERT_EQ(plan.size(), 1u);
  const std::vector<std::string> expected = {"path"};
  EXPECT_EQ(plan[0].args_replaced_keys, expected);
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

// ---------------------------------------------------------------------------
// 4. BuildPluginManifest — the `nevr_plugins` array the login carries (#60).
//    Every case parses the output back with nlohmann::json: the shape is asserted
//    on the parsed value, never on string matching.
// ---------------------------------------------------------------------------
TEST(PluginManifest, NoEntriesIsEmptyArray) {
  const nlohmann::json parsed = nlohmann::json::parse(BuildPluginManifest({}));
  ASSERT_TRUE(parsed.is_array());
  EXPECT_TRUE(parsed.empty());
}

TEST(PluginManifest, LoadedFailedAndDisabledEntriesHaveTheirOwnShape) {
  PluginManifestEntry loaded;
  loaded.name = "ex";
  loaded.file = "nevr_example.dll";
  loaded.loaded = true;
  loaded.version_major = 1;
  loaded.version_minor = 2;
  loaded.version_patch = 3;
  loaded.api_version = 5;
  loaded.capabilities = 0x11;

  PluginManifestEntry failed;
  failed.name = "gate";
  failed.file = "gate.dll";
  failed.required = true;
  failed.error = "LoadLibrary failed: error 126";

  PluginManifestEntry disabled;
  disabled.name = "off";
  disabled.file = "off.dll";
  disabled.enabled = false;

  const nlohmann::json parsed =
      nlohmann::json::parse(BuildPluginManifest({loaded, failed, disabled}));
  ASSERT_TRUE(parsed.is_array());
  ASSERT_EQ(parsed.size(), 3u);  // one per configured entry, in the given order

  const nlohmann::json& l = parsed[0];
  EXPECT_EQ(l.at("name"), "ex");
  EXPECT_EQ(l.at("file"), "nevr_example.dll");
  EXPECT_EQ(l.at("enabled"), true);
  EXPECT_EQ(l.at("required"), false);
  EXPECT_EQ(l.at("loaded"), true);
  EXPECT_EQ(l.at("ver"), "1.2.3");
  EXPECT_EQ(l.at("api"), 5);
  EXPECT_EQ(l.at("caps"), 0x11);
  EXPECT_FALSE(l.contains("error"));
  EXPECT_EQ(l.size(), 8u) << l.dump();

  const nlohmann::json& f = parsed[1];
  EXPECT_EQ(f.at("name"), "gate");
  EXPECT_EQ(f.at("enabled"), true);
  EXPECT_EQ(f.at("required"), true);
  EXPECT_EQ(f.at("loaded"), false);
  EXPECT_EQ(f.at("error"), "LoadLibrary failed: error 126");
  EXPECT_FALSE(f.contains("ver"));
  EXPECT_FALSE(f.contains("api"));
  EXPECT_FALSE(f.contains("caps"));
  EXPECT_EQ(f.size(), 6u) << f.dump();

  const nlohmann::json& d = parsed[2];
  EXPECT_EQ(d.at("name"), "off");
  EXPECT_EQ(d.at("enabled"), false);
  EXPECT_EQ(d.at("loaded"), false);
  EXPECT_FALSE(d.contains("error")) << "disabled is a choice, not a failure";
  EXPECT_FALSE(d.contains("ver"));
  EXPECT_EQ(d.size(), 5u) << d.dump();
}

// The case a snprintf builder got wrong: names come from config.yaml and may hold
// quotes, backslashes, or bytes that are not UTF-8. The output must still parse.
TEST(PluginManifest, HostileNamesStillProduceValidJson) {
  PluginManifestEntry e;
  e.name = "a \"quoted\" \\ name";
  e.file = std::string("bad\xff\xfe") + ".dll";
  e.error = "NvrPluginGetInfo returned NULL name";
  const std::string out = BuildPluginManifest({e});
  const nlohmann::json parsed = nlohmann::json::parse(out);
  ASSERT_EQ(parsed.size(), 1u);
  EXPECT_EQ(parsed[0].at("name"), "a \"quoted\" \\ name");
  EXPECT_TRUE(parsed[0].at("file").is_string());
}

}  // namespace
