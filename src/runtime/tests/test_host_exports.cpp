// The sixteen exports BugSplat64.dll keeps for nevr-runtime.dll (runtime/host/runtime_exports.cpp): before the
// runtime is up each returns its failure value and touches nothing; once the runtime published its descriptor
// each calls through the table, with the caller's arguments and the callee's result unchanged; a descriptor that
// does not cover the table (a v1.0 runtime) is treated as no table.

#include "core/runtime_abi.h"
#include "runtime/host/host_entry.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>

extern "C" {
void NEVR_ScheduleReturnToLobby();
void NEVR_GetUPnPConfig(void* out);
void NEVR_RegisterResourceOverride(std::uint64_t type_hash, std::uint64_t name_hash, const void* data,
                                   std::uint64_t size, const char* label);
void NEVR_ResetResourceOverrides();
void NEVR_DeregisterResourceOverrides(const void* data_start, const void* data_end);
int NEVR_RegisterAsset(std::uint64_t type_hash, std::uint64_t name_hash, const void* data, std::uint64_t size,
                       const char* label);
void NEVR_DeregisterAssets(const void* data_start, const void* data_end);
const char* TokenAuth_GetToken();
std::uint64_t TokenAuth_GetDiscordId();
const char* TokenAuth_GetUsername();
std::uint32_t platform_compat_ApiVersion();
int platform_compat_Init(const void* ctx);
void platform_compat_Shutdown();
std::uint32_t token_auth_ApiVersion();
int token_auth_Init(const void* ctx);
void token_auth_Shutdown();
}

namespace {

// What the fake runtime saw, one field per entry.
struct Calls {
  int schedule = 0;
  void* upnp_out = nullptr;
  std::uint64_t override_type = 0, override_name = 0, override_size = 0;
  const void* override_data = nullptr;
  const char* override_label = nullptr;
  int reset = 0;
  const void* dereg_override_start = nullptr;
  const void* dereg_override_end = nullptr;
  std::uint64_t asset_type = 0, asset_name = 0, asset_size = 0;
  const char* asset_label = nullptr;
  const void* dereg_asset_start = nullptr;
  const void* dereg_asset_end = nullptr;
  const void* pc_init_ctx = nullptr;
  const void* ta_init_ctx = nullptr;
  int pc_shutdown = 0;
  int ta_shutdown = 0;
};
Calls g_calls;

constexpr char kToken[] = "token-from-runtime";
constexpr char kName[] = "name-from-runtime";

void FakeSchedule() { ++g_calls.schedule; }
void FakeUpnp(void* out) { g_calls.upnp_out = out; }
void FakeRegisterOverride(std::uint64_t type, std::uint64_t name, const void* data, std::uint64_t size,
                          const char* label) {
  g_calls.override_type = type;
  g_calls.override_name = name;
  g_calls.override_data = data;
  g_calls.override_size = size;
  g_calls.override_label = label;
}
void FakeReset() { ++g_calls.reset; }
void FakeDeregOverride(const void* start, const void* end) {
  g_calls.dereg_override_start = start;
  g_calls.dereg_override_end = end;
}
int FakeRegisterAsset(std::uint64_t type, std::uint64_t name, const void*, std::uint64_t size, const char* label) {
  g_calls.asset_type = type;
  g_calls.asset_name = name;
  g_calls.asset_size = size;
  g_calls.asset_label = label;
  return 3;
}
void FakeDeregAssets(const void* start, const void* end) {
  g_calls.dereg_asset_start = start;
  g_calls.dereg_asset_end = end;
}
const char* FakeToken() { return kToken; }
std::uint64_t FakeDiscord() { return 0x1122334455667788ull; }
const char* FakeName() { return kName; }
std::uint32_t FakePcVersion() { return 11; }
int FakePcInit(const void* ctx) {
  g_calls.pc_init_ctx = ctx;
  return 5;
}
void FakePcShutdown() { ++g_calls.pc_shutdown; }
std::uint32_t FakeTaVersion() { return 22; }
int FakeTaInit(const void* ctx) {
  g_calls.ta_init_ctx = ctx;
  return 6;
}
void FakeTaShutdown() { ++g_calls.ta_shutdown; }

NevrRuntimeApi FullApi(std::uint32_t struct_size = nevr_runtime_abi::kRuntimeApiV1_1Size) {
  NevrRuntimeApi api{};
  api.header.struct_size = struct_size;
  api.header.abi_major = nevr_runtime_abi::kMajor;
  api.header.abi_minor = nevr_runtime_abi::kMinor;
  api.initialize = [](const NevrHostContext*) -> std::int32_t { return 0; };
  api.game_main = [](std::int64_t command_line) -> std::int64_t { return command_line; };
  api.exports.schedule_return_to_lobby = &FakeSchedule;
  api.exports.get_upnp_config = &FakeUpnp;
  api.exports.register_resource_override = &FakeRegisterOverride;
  api.exports.reset_resource_overrides = &FakeReset;
  api.exports.deregister_resource_overrides = &FakeDeregOverride;
  api.exports.register_asset = &FakeRegisterAsset;
  api.exports.deregister_assets = &FakeDeregAssets;
  api.exports.token_auth_get_token = &FakeToken;
  api.exports.token_auth_get_discord_id = &FakeDiscord;
  api.exports.token_auth_get_username = &FakeName;
  api.exports.platform_compat_api_version = &FakePcVersion;
  api.exports.platform_compat_init = &FakePcInit;
  api.exports.platform_compat_shutdown = &FakePcShutdown;
  api.exports.token_auth_api_version = &FakeTaVersion;
  api.exports.token_auth_init = &FakeTaInit;
  api.exports.token_auth_shutdown = &FakeTaShutdown;
  return api;
}

// Publishes `api` as the loaded runtime for the length of a test, and takes it away again.
class PublishedRuntime {
 public:
  explicit PublishedRuntime(const NevrRuntimeApi* api) {
    g_calls = Calls{};
    nevr_host::Rendezvous().SetRuntimeApiForTest(api);
  }
  ~PublishedRuntime() { nevr_host::Rendezvous().SetRuntimeApiForTest(nullptr); }
};

void ExpectEveryFailureValue() {
  g_calls = Calls{};
  int untouched = 41;
  NEVR_ScheduleReturnToLobby();
  NEVR_GetUPnPConfig(&untouched);
  NEVR_RegisterResourceOverride(1, 2, &untouched, 4, "x");
  NEVR_ResetResourceOverrides();
  NEVR_DeregisterResourceOverrides(&untouched, &untouched);
  NEVR_DeregisterAssets(&untouched, &untouched);
  platform_compat_Shutdown();
  token_auth_Shutdown();
  EXPECT_EQ(untouched, 41);  // nothing wrote through the caller's pointer
  EXPECT_EQ(g_calls.schedule, 0);

  EXPECT_EQ(NEVR_RegisterAsset(1, 2, &untouched, 4, "x"), -1);  // the plugins' NEVR_ASSET_UNAVAILABLE
  EXPECT_STREQ(TokenAuth_GetToken(), "");
  EXPECT_STREQ(TokenAuth_GetUsername(), "");
  EXPECT_EQ(TokenAuth_GetDiscordId(), 0u);
  EXPECT_EQ(platform_compat_ApiVersion(), 0u);  // no loader accepts version 0
  EXPECT_EQ(token_auth_ApiVersion(), 0u);
  EXPECT_EQ(platform_compat_Init(&untouched), -1);  // a failure
  EXPECT_EQ(token_auth_Init(&untouched), -1);
}

}  // namespace

TEST(HostExports, BeforeTheRuntimeIsUpEveryExportReturnsItsFailureValueAndTouchesNothing) {
  ExpectEveryFailureValue();
}

TEST(HostExports, ARuntimeWhoseDescriptorEndsBeforeTheTableIsTreatedAsHavingNoTable) {
  // A v1.0 runtime: struct_size ends at game_main. The host must not read the entries that are not there.
  NevrRuntimeApi api = FullApi(nevr_runtime_abi::kRuntimeApiV1Size);
  PublishedRuntime published(&api);
  ExpectEveryFailureValue();
}

TEST(HostExports, ATableWithAnEntryMissingFallsBackToThatEntrysFailureValueOnly) {
  NevrRuntimeApi api = FullApi();
  api.exports.register_asset = nullptr;
  api.exports.token_auth_get_token = nullptr;
  PublishedRuntime published(&api);
  EXPECT_EQ(NEVR_RegisterAsset(1, 2, nullptr, 0, "x"), -1);
  EXPECT_STREQ(TokenAuth_GetToken(), "");
  EXPECT_STREQ(TokenAuth_GetUsername(), kName);  // the entries that are there still work
}

TEST(HostExports, OnceTheRuntimeIsUpEveryExportCallsThroughWithItsArgumentsAndGivesBackTheResult) {
  NevrRuntimeApi api = FullApi();
  PublishedRuntime published(&api);
  int a = 0;
  int b = 0;

  NEVR_ScheduleReturnToLobby();
  EXPECT_EQ(g_calls.schedule, 1);

  NEVR_GetUPnPConfig(&a);
  EXPECT_EQ(g_calls.upnp_out, &a);

  NEVR_RegisterResourceOverride(0xAAAAAAAAAAAAAAAAull, 0xBBBBBBBBBBBBBBBBull, &a, 0x1111222233334444ull, "label");
  EXPECT_EQ(g_calls.override_type, 0xAAAAAAAAAAAAAAAAull);
  EXPECT_EQ(g_calls.override_name, 0xBBBBBBBBBBBBBBBBull);
  EXPECT_EQ(g_calls.override_data, &a);
  EXPECT_EQ(g_calls.override_size, 0x1111222233334444ull);
  EXPECT_STREQ(g_calls.override_label, "label");

  NEVR_ResetResourceOverrides();
  EXPECT_EQ(g_calls.reset, 1);

  NEVR_DeregisterResourceOverrides(&a, &b);
  EXPECT_EQ(g_calls.dereg_override_start, &a);
  EXPECT_EQ(g_calls.dereg_override_end, &b);

  EXPECT_EQ(NEVR_RegisterAsset(7, 8, &a, 9, "asset"), 3);
  EXPECT_EQ(g_calls.asset_type, 7u);
  EXPECT_EQ(g_calls.asset_name, 8u);
  EXPECT_EQ(g_calls.asset_size, 9u);
  EXPECT_STREQ(g_calls.asset_label, "asset");

  NEVR_DeregisterAssets(&b, &a);
  EXPECT_EQ(g_calls.dereg_asset_start, &b);
  EXPECT_EQ(g_calls.dereg_asset_end, &a);

  EXPECT_STREQ(TokenAuth_GetToken(), kToken);
  EXPECT_EQ(TokenAuth_GetDiscordId(), 0x1122334455667788ull);
  EXPECT_STREQ(TokenAuth_GetUsername(), kName);

  EXPECT_EQ(platform_compat_ApiVersion(), 11u);
  EXPECT_EQ(platform_compat_Init(&a), 5);
  EXPECT_EQ(g_calls.pc_init_ctx, &a);
  platform_compat_Shutdown();
  EXPECT_EQ(g_calls.pc_shutdown, 1);

  EXPECT_EQ(token_auth_ApiVersion(), 22u);
  EXPECT_EQ(token_auth_Init(&b), 6);
  EXPECT_EQ(g_calls.ta_init_ctx, &b);
  token_auth_Shutdown();
  EXPECT_EQ(g_calls.ta_shutdown, 1);
}
