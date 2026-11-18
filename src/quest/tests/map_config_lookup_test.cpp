// The Quest's answers to the core's reads of the maps section (quest/cdn/map_config_lookup.h; core/map_channel.h ReadSettings): the flat
// keys the core asks for, and what StartFromConfig decides from them (not_configured, a refusal, a run) before it builds the
// production objects. Framework-free: the Quest host group runs it. No libcurl, no sentinel hooks.
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "core/map_channel.h"
#include "quest/cdn/map_config_lookup.h"
#include "quest/tests/test_check.h"

namespace nevr_config_alias {
// The Quest sentinel's warning line (quest/sentinel/config_alias_levels.cpp), which this host test does not link.
void PlatformWarn(const char*) noexcept {}
}  // namespace nevr_config_alias

namespace {

namespace mc = nevr_map_channel;
using nevr_quest::MapsConfig;
using nevr_quest::maps::ListsOf;
using nevr_quest::maps::ScalarsOf;

// A public minisign key (tests/vectors/maps/minisign/test_key.pub): a valid key that signs nothing a test relies on.
constexpr const char* kTestPub =
    "untrusted comment: minisign public key 12CF26D38A5D6FFB\nRWT7b12K0ybPErgOPJNseA689+mTi7fFLcTuzrBCVDwzdi7wbqXFzF5l\n";
constexpr const char* kBase = "https://maps.test";
constexpr const char* kChannel = "stable";
constexpr std::uint32_t kBudgetMs = 4500;

MapsConfig Configured() {
  MapsConfig config;
  config.baseUrl = kBase;
  config.channel = kChannel;
  config.bootBudgetMs = kBudgetMs;
  return config;
}

void TheScalarKeysAreTheConfigsAndAnyOtherKeyIsEmpty() {
  const MapsConfig config = Configured();
  const mc::ScalarLookup scalars = ScalarsOf(config);
  QCHECK(scalars("maps_base_url") == kBase);
  QCHECK(scalars("maps_channel") == kChannel);
  QCHECK(scalars("maps_boot_budget_ms") == "4500");
  QCHECK(scalars("maps_trusted_keys").empty());
  QCHECK(scalars("").empty());
}

void ABootBudgetOfZeroReadsAsNoLimit() {
  MapsConfig config = Configured();
  config.bootBudgetMs = 0;
  QCHECK(ScalarsOf(config)("maps_boot_budget_ms").empty());
}

void TheListKeysAreTheConfigsKeysAndAnyOtherKeyIsEmpty() {
  MapsConfig config;
  config.officialKeys = {"official-1", "official-2"};
  config.trustedKeys = {"author-1"};
  const mc::ListLookup lists = ListsOf(config);
  QCHECK(lists("maps_official_keys") == config.officialKeys);
  QCHECK(lists("maps_trusted_keys") == config.trustedKeys);
  QCHECK(lists("maps_base_url").empty());
}

void ALookupKeepsTheValuesItWasMadeFromWhenTheConfigChanges() {
  MapsConfig config = Configured();
  const mc::ScalarLookup scalars = ScalarsOf(config);
  config.baseUrl = "https://changed.test";
  QCHECK(scalars("maps_base_url") == kBase);
}

mc::Settings Read(const MapsConfig& config) {
  return mc::ReadSettings(ScalarsOf(config), ListsOf(config), mc::CompiledOfficialKeys());
}

void AnEmptyConfigIsNotConfiguredAndStartsNothing() {
  const mc::Settings settings = Read(MapsConfig{});
  QCHECK(settings.reason == "not_configured");
  QCHECK(mc::DecideStart(settings) == mc::Start::kOff);
}

void AConfiguredBaseUrlRunsWithItsChannelBudgetAndTrailingSlash() {
  const mc::Settings settings = Read(Configured());
  QCHECK(settings.reason.empty());
  QCHECK(mc::DecideStart(settings) == mc::Start::kRun);
  QCHECK(settings.base_url == std::string(kBase) + "/");
  QCHECK(settings.channel == kChannel);
  QCHECK(settings.boot_budget_ms == kBudgetMs);
}

void ABadKeyRefusesTheStartAndNamesTheConfigKeyAndIndex() {
  MapsConfig config = Configured();
  config.trustedKeys = {kTestPub, "not a key"};
  const mc::Settings settings = Read(config);
  QCHECK(settings.reason == "bad_key");
  QCHECK(settings.detail == "maps.trusted_keys[1]");
  QCHECK(mc::DecideStart(settings) == mc::Start::kRefused);
}

void AGoodAuthorKeyIsTrustedAndNotOfficial() {
  MapsConfig config = Configured();
  config.trustedKeys = {kTestPub};
  const mc::Settings settings = Read(config);
  QCHECK(mc::DecideStart(settings) == mc::Start::kRun);
  QCHECK(settings.keys.size() == 1);
  QCHECK(!settings.keys[0].official);
}

void OfficialAndAuthorKeysTogetherRefuseTheStart() {
  MapsConfig config = Configured();
  config.officialKeys = {kTestPub};
  config.trustedKeys = {kTestPub};
  const mc::Settings settings = Read(config);
  QCHECK(settings.reason == "mixed_keys");
  QCHECK(mc::DecideStart(settings) == mc::Start::kRefused);
}

}  // namespace

int main() {
  TheScalarKeysAreTheConfigsAndAnyOtherKeyIsEmpty();
  ABootBudgetOfZeroReadsAsNoLimit();
  TheListKeysAreTheConfigsKeysAndAnyOtherKeyIsEmpty();
  ALookupKeepsTheValuesItWasMadeFromWhenTheConfigChanges();
  AnEmptyConfigIsNotConfiguredAndStartsNothing();
  AConfiguredBaseUrlRunsWithItsChannelBudgetAndTrailingSlash();
  ABadKeyRefusesTheStartAndNamesTheConfigKeyAndIndex();
  AGoodAuthorKeyIsTrustedAndNotOfficial();
  OfficialAndAuthorKeysTogetherRefuseTheStart();
  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "map_config_lookup_test: %d check(s) failed\n", quest_test::Failures());
    return 1;
  }
  std::printf("map_config_lookup_test: all checks passed\n");
  return 0;
}
