// The map channel's cache index (src/core/map_index.h; docs/adr/0048-custom-maps-channel.md decisions 3 and 4): its JSON round trip
// with floors kept per channel, guild and key, and its read, bounded by the memory the device reports (ADR 0045 decision 8), over
// the fake Env the cargo tests use.
#include <gtest/gtest.h>

#include <cstdint>
#include <string>

#include <nlohmann/json.hpp>

#include "core/map_index.h"
#include "quest/tests/cargo_fake_env.h"

namespace {

using nevr_maps::CacheIndex;
using nevr_maps::CacheIndexStatus;

constexpr const char* kDir = "cache/maps";
constexpr const char* kChannel = "echo-community";
constexpr const char* kGuild = "0b6e4c1a-7f3d-4e2a-9c5b-8d1e2f3a4b5c";
constexpr const char* kOtherGuild = "c4d5e6f7-0a1b-4c2d-8e3f-a1b2c3d4e5f6";
constexpr const char* kKeyId = "A8A7A6A5A4A3A2A1";

CacheIndex SampleIndex() {
  CacheIndex index;
  nevr_maps::Floors& a = index.floors.channels[kChannel][kGuild][kKeyId];
  a.serial = 42;
  a.serial_sha256 = std::string(64, 'a');
  a.version["arena/windows-x86_64"] = "1.2.0";
  nevr_maps::Floors& b = index.floors.channels[kChannel][kOtherGuild][kKeyId];
  b.serial = 7;
  b.serial_sha256 = std::string(64, 'b');
  index.floors.bound["https://maps.example/"] = kChannel;

  nevr_maps::CachedMap m;
  m.id = "arena";
  m.platform = nevr_maps::kPlatformPc;
  m.version = "1.2.0";
  m.index_sha256 = std::string(64, 'c');
  m.index_size = 5321;
  m.level = 0x2fc38a8c4a79569aull;
  m.level_name = "arena";
  m.game_build = "echovr.exe 34.4.631547";
  m.dir = "arena/windows-x86_64/" + nevr_maps::VersionDirName(m.index_sha256);
  m.pinned_serial = 42;
  nevr_maps::CachedFile set;
  set.path = "set/windows-x86_64.nevrres";
  set.size = 3000;
  set.sha256 = std::string(64, 'd');
  set.stamp.volume = 1;
  set.stamp.id_lo = 2;
  set.stamp.size = 3000;
  set.frames_checked = true;
  nevr_maps::CachedFile script;
  script.path = "scripts/00aa11bb22cc33dd.dll";
  script.role = nevr_maps::FileRole::kScript;
  script.script_hash = 0x00aa11bb22cc33ddull;
  script.size = 90112;
  script.sha256 = std::string(64, 'e');
  m.files = {set, script};
  index.maps[nevr_maps::MapKey(m.id, m.platform)] = m;
  return index;
}

TEST(MapCacheIndex, RoundTripsWithFloorsPerGuild) {
  const CacheIndex index = SampleIndex();
  CacheIndex back;
  ASSERT_TRUE(nevr_maps::CacheIndexFromJson(nevr_maps::CacheIndexToJson(index), &back));
  EXPECT_EQ(back.floors.channels.at(kChannel).at(kGuild).at(kKeyId).serial, 42u);
  EXPECT_EQ(back.floors.channels.at(kChannel).at(kGuild).at(kKeyId).version.at("arena/windows-x86_64"), "1.2.0");
  EXPECT_EQ(back.floors.channels.at(kChannel).at(kOtherGuild).at(kKeyId).serial, 7u);
  EXPECT_EQ(back.floors.bound.at("https://maps.example/"), kChannel);
  ASSERT_EQ(back.maps.size(), 1u);
  EXPECT_TRUE(back.maps.begin()->second == index.maps.begin()->second);
  EXPECT_EQ(back.maps.begin()->second.dir, "arena/windows-x86_64/cccccccccccc");
}

// A floor not under a guild id, or not under a key id, has nothing to hold it to: it is dropped, and the rest of the index reads.
TEST(MapCacheIndex, AFloorWithNoGuildOrNoKeyIsDropped) {
  const std::string text = R"({"schema":"nevr-maps-index","schema_version":1,"maps":{},"floors":{"bound":{},"channels":{
      "echo-community":{
        "A8A7A6A5A4A3A2A1":{"serial":3,"serial_sha256":"","version":{}},
        "0b6e4c1a-7f3d-4e2a-9c5b-8d1e2f3a4b5c":{"serial":5,"serial_sha256":"","version":{}},
        "c4d5e6f7-0a1b-4c2d-8e3f-a1b2c3d4e5f6":{"A8A7A6A5A4A3A2A1":{"serial":9,"serial_sha256":"","version":{}}}}}}})";
  CacheIndex index;
  ASSERT_TRUE(nevr_maps::CacheIndexFromJson(text, &index));
  ASSERT_EQ(index.floors.channels.at(kChannel).size(), 1u);
  EXPECT_EQ(index.floors.channels.at(kChannel).at(kOtherGuild).at(kKeyId).serial, 9u);
}

// The lobby groups the cache keeps, the latest first: they round trip in order, an index written before groups were kept has
// none, and a group that is not a path-safe id, or one listed twice, makes the index invalid.
TEST(MapCacheIndex, KeepsItsGroupsInOrder) {
  CacheIndex index = SampleIndex();
  nevr_maps::CachedGroup latest;
  latest.id = "123456789012345678";
  latest.bytes = 4000;
  latest.maps = {"arena/windows-x86_64"};
  nevr_maps::CachedGroup older;
  older.id = kGuild;
  index.groups = {latest, older};
  CacheIndex back;
  ASSERT_TRUE(nevr_maps::CacheIndexFromJson(nevr_maps::CacheIndexToJson(index), &back));
  EXPECT_EQ(back.groups, index.groups);

  nlohmann::json doc = nlohmann::json::parse(nevr_maps::CacheIndexToJson(index));
  doc.erase("groups");
  ASSERT_TRUE(nevr_maps::CacheIndexFromJson(doc.dump(), &back));
  EXPECT_TRUE(back.groups.empty());
  EXPECT_EQ(back.maps.size(), 1u);

  doc = nlohmann::json::parse(nevr_maps::CacheIndexToJson(index));
  doc["groups"][0]["id"] = "../escape";
  EXPECT_FALSE(nevr_maps::CacheIndexFromJson(doc.dump(), &back));
  doc = nlohmann::json::parse(nevr_maps::CacheIndexToJson(index));
  doc["groups"][1]["id"] = doc["groups"][0]["id"];
  EXPECT_FALSE(nevr_maps::CacheIndexFromJson(doc.dump(), &back));
}

struct MapCacheIndexRead : ::testing::Test {
  cargo_test::FakeEnv env;
  std::string path = std::string(kDir) + "/" + nevr_maps::kCacheIndexFile;
};

TEST_F(MapCacheIndexRead, ReadsWithinTheDevicesMemoryShare) {
  const std::string text = nevr_maps::CacheIndexToJson(SampleIndex());
  env.fs_->files[path] = text;
  CacheIndex index;
  ASSERT_EQ(nevr_maps::ReadCacheIndex(env, kDir, &index), CacheIndexStatus::kOk);
  EXPECT_EQ(index.maps.size(), 1u);
  EXPECT_EQ(env.device_queries, 1);

  // Memory so small that the share is one byte short of the file: too large, and nothing is read.
  nevr_cargo::DeviceFacts small;
  small.have_memory = true;
  small.memory_available = 1;
  while (nevr_maps::MapDocumentBytes(small) < text.size() - 1) ++small.memory_available;
  ASSERT_EQ(nevr_maps::MapDocumentBytes(small), text.size() - 1);
  env.device = small;
  EXPECT_EQ(nevr_maps::ReadCacheIndex(env, kDir, &index), CacheIndexStatus::kTooLarge);
  EXPECT_TRUE(index.maps.empty());

  env.device.have_memory = false;
  EXPECT_EQ(nevr_maps::ReadCacheIndex(env, kDir, &index), CacheIndexStatus::kNoMemoryFact);
  EXPECT_STREQ(nevr_maps::CacheIndexStatusName(CacheIndexStatus::kNoMemoryFact), "no_memory_fact");
}

TEST_F(MapCacheIndexRead, AbsentAndInvalid) {
  CacheIndex index;
  EXPECT_EQ(nevr_maps::ReadCacheIndex(env, kDir, &index), CacheIndexStatus::kAbsent);
  env.fs_->files[path] = "{not json";
  EXPECT_EQ(nevr_maps::ReadCacheIndex(env, kDir, &index), CacheIndexStatus::kInvalid);
  EXPECT_EQ(env.fs_->files.at(path + ".bad"), "{not json");

  // Written whole, read back, and read again: the same index both times.
  ASSERT_TRUE(nevr_maps::WriteCacheIndex(env, kDir, SampleIndex()));
  CacheIndex again;
  ASSERT_EQ(nevr_maps::ReadCacheIndex(env, kDir, &index), CacheIndexStatus::kOk);
  ASSERT_EQ(nevr_maps::ReadCacheIndex(env, kDir, &again), CacheIndexStatus::kOk);
  EXPECT_EQ(nevr_maps::CacheIndexToJson(index), nevr_maps::CacheIndexToJson(again));
}

}  // namespace
