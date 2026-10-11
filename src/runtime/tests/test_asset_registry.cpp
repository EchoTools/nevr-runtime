#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "extension/plugin_interface.h"
#include "runtime/patch/asset_registry.h"

using nevr::assets::Asset;
using nevr::assets::kMaxAssetBytes;
using nevr::assets::Registry;
using nevr::assets::RegisterStatus;
using nevr::assets::ToString;

namespace {

constexpr uint64_t kType = 0xa71a14664bd4e047ULL;
constexpr uint64_t kNameA = 0x1111222233334444ULL;
constexpr uint64_t kNameB = 0x5555666677778888ULL;

// Stands in for the engine's loaded-resource tree: only kNameB has been loaded.
bool OnlyNameBIsLoaded(uint64_t, uint64_t name) { return name == kNameB; }

const unsigned char kBlob[64] = {1, 2, 3};
const unsigned char kOtherBlob[64] = {4, 5, 6};

}  // namespace

TEST(AssetRegistry, RegistersAndFinds) {
    Registry r;
    ASSERT_EQ(r.Register(kType, kNameA, kBlob, sizeof(kBlob), "level_a"), RegisterStatus::Ok);
    Asset a;
    ASSERT_TRUE(r.Find(kType, kNameA, &a));
    EXPECT_EQ(a.data, static_cast<const void*>(kBlob));
    EXPECT_EQ(a.size, sizeof(kBlob));
    EXPECT_EQ(a.label, "level_a");
    EXPECT_EQ(r.Count(), 1u);
    EXPECT_FALSE(r.Find(kType, kNameB, &a));
}

TEST(AssetRegistry, TheKeyIsTypeAndNameTogether) {
    Registry r;
    ASSERT_EQ(r.Register(kType, kNameA, kBlob, sizeof(kBlob), "a"), RegisterStatus::Ok);
    ASSERT_EQ(r.Register(kType + 1, kNameA, kOtherBlob, sizeof(kOtherBlob), "same name, other type"),
              RegisterStatus::Ok);
    Asset a;
    ASSERT_TRUE(r.Find(kType + 1, kNameA, &a));
    EXPECT_EQ(a.data, static_cast<const void*>(kOtherBlob));
}

TEST(AssetRegistry, RefusesADuplicateAndKeepsTheFirst) {
    Registry r;
    ASSERT_EQ(r.Register(kType, kNameA, kBlob, sizeof(kBlob), "first"), RegisterStatus::Ok);
    EXPECT_EQ(r.Register(kType, kNameA, kOtherBlob, sizeof(kOtherBlob), "second"), RegisterStatus::Duplicate);
    Asset a;
    ASSERT_TRUE(r.Find(kType, kNameA, &a));
    EXPECT_EQ(a.label, "first");
    EXPECT_EQ(r.Count(), 1u);
}

TEST(AssetRegistry, RefusesWhatTheEngineHasAlreadyLoaded) {
    Registry r(&OnlyNameBIsLoaded);
    EXPECT_EQ(r.Register(kType, kNameB, kBlob, sizeof(kBlob), "late"), RegisterStatus::AlreadyLoaded);
    EXPECT_EQ(r.Count(), 0u);
    EXPECT_EQ(r.Register(kType, kNameA, kBlob, sizeof(kBlob), "in time"), RegisterStatus::Ok);
}

TEST(AssetRegistry, RejectsArgumentsTheEngineCannotUse) {
    Registry r;
    EXPECT_EQ(r.Register(0, kNameA, kBlob, sizeof(kBlob), "no type"), RegisterStatus::BadArgument);
    EXPECT_EQ(r.Register(kType, 0, kBlob, sizeof(kBlob), "no name"), RegisterStatus::BadArgument);
    EXPECT_EQ(r.Register(kType, kNameA, nullptr, 8, "no data"), RegisterStatus::BadArgument);
    EXPECT_EQ(r.Register(kType, kNameA, kBlob, 0, "no bytes"), RegisterStatus::BadArgument);
    EXPECT_EQ(r.Register(kType, kNameA, kBlob, kMaxAssetBytes + 1, "too big"), RegisterStatus::BadArgument);
    EXPECT_EQ(r.Count(), 0u);
}

TEST(AssetRegistry, ANullLabelIsAnEmptyLabel) {
    Registry r;
    ASSERT_EQ(r.Register(kType, kNameA, kBlob, sizeof(kBlob), nullptr), RegisterStatus::Ok);
    Asset a;
    ASSERT_TRUE(r.Find(kType, kNameA, &a));
    EXPECT_TRUE(a.label.empty());
}

TEST(AssetRegistry, DeregisterTakesTheAssetsInsideTheRangeOnly) {
    Registry r;
    ASSERT_EQ(r.Register(kType, kNameA, kBlob, sizeof(kBlob), "mine"), RegisterStatus::Ok);
    ASSERT_EQ(r.Register(kType, kNameB, kOtherBlob, sizeof(kOtherBlob), "theirs"), RegisterStatus::Ok);
    std::vector<Asset> removed;
    EXPECT_EQ(r.Deregister(kBlob, kBlob + sizeof(kBlob), &removed), 1u);
    ASSERT_EQ(removed.size(), 1u);
    EXPECT_EQ(removed[0].label, "mine");
    Asset a;
    EXPECT_FALSE(r.Find(kType, kNameA, &a));
    EXPECT_TRUE(r.Find(kType, kNameB, &a));
}

TEST(AssetRegistry, ADeregisteredKeyCanBeRegisteredAgain) {
    Registry r;
    ASSERT_EQ(r.Register(kType, kNameA, kBlob, sizeof(kBlob), "one"), RegisterStatus::Ok);
    ASSERT_EQ(r.Deregister(kBlob, kBlob + sizeof(kBlob)), 1u);
    EXPECT_EQ(r.Register(kType, kNameA, kOtherBlob, sizeof(kOtherBlob), "two"), RegisterStatus::Ok);
}

TEST(AssetRegistry, EmptyTracksRegistrationAndDeregistration) {
    Registry r;
    EXPECT_TRUE(r.Empty());
    ASSERT_EQ(r.Register(kType, kNameA, kBlob, sizeof(kBlob), "a"), RegisterStatus::Ok);
    EXPECT_FALSE(r.Empty());
    ASSERT_EQ(r.Deregister(kBlob, kBlob + sizeof(kBlob)), 1u);
    EXPECT_TRUE(r.Empty());
}

TEST(AssetRegistry, TheProbeCanBeInstalledAfterConstruction) {
    Registry r;
    EXPECT_EQ(r.Register(kType, kNameB, kBlob, sizeof(kBlob), "before"), RegisterStatus::Ok);
    ASSERT_EQ(r.Deregister(kBlob, kBlob + sizeof(kBlob)), 1u);
    r.SetLoadedProbe(&OnlyNameBIsLoaded);
    EXPECT_EQ(r.Register(kType, kNameB, kBlob, sizeof(kBlob), "after"), RegisterStatus::AlreadyLoaded);
}

TEST(AssetRegistry, ReportIsAnArrayInRegistrationOrder) {
    Registry r;
    EXPECT_EQ(r.ReportJson(), "[]");
    ASSERT_EQ(r.Register(kType, kNameA, kBlob, sizeof(kBlob), "level_a"), RegisterStatus::Ok);
    ASSERT_EQ(r.Register(kType, kNameB, kOtherBlob, 10, "level_b"), RegisterStatus::Ok);
    const nlohmann::json j = nlohmann::json::parse(r.ReportJson());
    ASSERT_TRUE(j.is_array());
    ASSERT_EQ(j.size(), 2u);
    EXPECT_EQ(j[0]["type"], "0xa71a14664bd4e047");
    EXPECT_EQ(j[0]["name"], "0x1111222233334444");
    EXPECT_EQ(j[0]["label"], "level_a");
    EXPECT_EQ(j[0]["bytes"], 64u);
    EXPECT_EQ(j[1]["label"], "level_b");
    EXPECT_EQ(j[1]["bytes"], 10u);
}

TEST(AssetRegistry, ReportSurvivesALabelThatIsNotUtf8) {
    Registry r;
    ASSERT_EQ(r.Register(kType, kNameA, kBlob, sizeof(kBlob), "bad\xff\xfe label"), RegisterStatus::Ok);
    nlohmann::json j;
    ASSERT_NO_THROW(j = nlohmann::json::parse(r.ReportJson()));
    EXPECT_EQ(j.size(), 1u);
}

TEST(AssetRegistry, StatusNamesAreStableForLogsAndPlugins) {
    EXPECT_STREQ(ToString(RegisterStatus::Ok), "ok");
    EXPECT_STREQ(ToString(RegisterStatus::BadArgument), "bad_argument");
    EXPECT_STREQ(ToString(RegisterStatus::Duplicate), "duplicate");
    EXPECT_STREQ(ToString(RegisterStatus::AlreadyLoaded), "already_loaded");
}

// The status codes are public ABI (extension/plugin_interface.h, NevrAssetStatus): a plugin compiled against v6
// compares the return of NEVR_RegisterAsset with those values, so the host's enum may never drift from them.
static_assert(static_cast<int>(RegisterStatus::Ok) == NEVR_ASSET_OK);
static_assert(static_cast<int>(RegisterStatus::BadArgument) == NEVR_ASSET_BAD_ARGUMENT);
static_assert(static_cast<int>(RegisterStatus::Duplicate) == NEVR_ASSET_DUPLICATE);
static_assert(static_cast<int>(RegisterStatus::AlreadyLoaded) == NEVR_ASSET_ALREADY_LOADED);
static_assert(NEVR_PLUGIN_API_VERSION >= 6, "plugin assets are API v6");
