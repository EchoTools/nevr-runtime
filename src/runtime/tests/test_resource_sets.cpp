// #2161: plugin resource sets. The format a plugin embeds (nevr/resource_set.h), the registry that checks and mounts
// sets and builds the shadow the resolver is pointed at (runtime/patch/resource_set_registry.h), and the mapping of
// a pointer into a mapped PE image to an offset in its file (runtime/patch/image_file_range.h). Every fixture is
// built here; the game's manifest is a fake StockView.

#include <gtest/gtest.h>
#include <zstd.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "core/resource_shadow.h"
#include "nevr/pc/plugin_resources.h"
#include "nevr/resource_set.h"
#include "runtime/patch/image_file_range.h"
#include "runtime/patch/resource_set_registry.h"
#include "runtime/tests/resource_set_builder.h"

namespace {

using nevr_resource_set::Parse;
using nevr_resource_set::Problem;
using nevr_resource_sets::kArchiveType;
using nevr_resource_sets::kLevelRootType;
using nevr_resource_sets::kScriptType;
using nevr_resource_sets::kTextureGpuType;
using nevr_resource_sets::kTextureType;
using nevr_resource_sets::Refusal;
using nevr_resource_sets::Registry;
using nevr_resource_sets::StockView;
using nevr_test_sets::Archive;
using nevr_test_sets::Bytes;
using nevr_test_sets::SetBuilder;
using nevr_test_sets::SetU32;
using nevr_test_sets::SetU64;

constexpr std::uint64_t kLevel = 0xdb69545ff59c76d0ULL;  // CSymbol64("mpl_arena_build")
constexpr std::uint64_t kMeshType = 0x1111111111111111ULL;
constexpr std::uint64_t kStockMesh = 0x2222222222222222ULL;

// A level: its root, its archive listing a new mesh and a stock mesh, and the new mesh. One chunk.
SetBuilder LevelSet(std::uint64_t level = kLevel, std::uint64_t mesh = 0x3333333333333333ULL) {
  SetBuilder b;
  const std::uint32_t c = b.AddChunk();
  b.AddRecord(kLevelRootType, level, Bytes(64, 0x11), c);
  b.AddRecord(kArchiveType, level, Archive({}, {{kMeshType, mesh}, {kMeshType, kStockMesh}, {kLevelRootType, level}}), c);
  b.AddRecord(kMeshType, mesh, Bytes(200, 0x22), c);
  return b;
}

// The game's manifest as a set of keys.
struct FakeStock {
  std::set<std::pair<std::uint64_t, std::uint64_t>> keys{{kMeshType, kStockMesh}};
  static bool Has(void* ctx, std::uint64_t type, std::uint64_t name) {
    return static_cast<FakeStock*>(ctx)->keys.count({type, name}) != 0;
  }
  StockView View() { return StockView{&FakeStock::Has, this, 69651}; }
};

Problem ParseProblem(const std::vector<unsigned char>& bytes) {
  nevr_resource_set::View view;
  return Parse(bytes.data(), bytes.size(), &view).problem;
}

// ---- the format ---------------------------------------------------------------------------------------------------

TEST(ResourceSetFormat, AWellFormedSetParsesAndReadsBack) {
  SetBuilder b = LevelSet();
  b.AddOverride(0x4444444444444444ULL, 0x5555ULL, Bytes(10, 0x33));
  const std::vector<unsigned char> bytes = b.Build();
  nevr_resource_set::View view;
  ASSERT_TRUE(Parse(bytes.data(), bytes.size(), &view).ok());
  EXPECT_EQ(view.record_count(), 3u);
  EXPECT_EQ(view.chunk_count(), 1u);
  EXPECT_EQ(view.override_count(), 1u);
  EXPECT_EQ(view.RecordAt(0).type, kLevelRootType);
  EXPECT_EQ(view.RecordAt(2).size, 200u);
  const nevr_resource_set::Override o = view.OverrideAt(0);
  EXPECT_EQ(o.size, 10u);
  EXPECT_EQ(bytes[o.offset], 0x33);
  // The chunk is a zstd frame every decoder reads: decompress it and find the mesh's bytes where its record says.
  const nevr_resource_set::Chunk c = view.ChunkAt(0);
  std::vector<unsigned char> plain(c.decompressed);
  ASSERT_EQ(ZSTD_decompress(plain.data(), plain.size(), bytes.data() + c.offset, c.compressed), plain.size());
  EXPECT_EQ(plain[view.RecordAt(2).offset_in_chunk], 0x22);
}

TEST(ResourceSetFormat, EachRuleTheReaderNeedsIsARefusal) {
  struct Case {
    const char* name;
    Problem expected;
    void (*mutate)(SetBuilder&, std::vector<unsigned char>&);
  };
  const Case cases[] = {
      {"too small", Problem::kTooSmall, [](SetBuilder&, std::vector<unsigned char>& v) { v.resize(40); }},
      {"magic", Problem::kMagic, [](SetBuilder&, std::vector<unsigned char>& v) { v[7] = '2'; }},
      {"header", Problem::kHeader, [](SetBuilder&, std::vector<unsigned char>& v) { SetU32(&v, 0x08, 64); }},
      {"no records", Problem::kNoRecords, [](SetBuilder&, std::vector<unsigned char>& v) { SetU32(&v, 0x0c, 0); }},
      {"table range", Problem::kTableRange,
       [](SetBuilder&, std::vector<unsigned char>& v) { SetU64(&v, 0x20, v.size() - 4); }},
      {"chunk range", Problem::kChunkRange,
       [](SetBuilder& b, std::vector<unsigned char>& v) { SetU32(&v, b.ChunkAt(0) + 8, static_cast<std::uint32_t>(v.size())); }},
      {"chunk not zstd", Problem::kChunkNotZstd,
       [](SetBuilder& b, std::vector<unsigned char>& v) {
         std::uint32_t at = 0;
         std::memcpy(&at, v.data() + b.ChunkAt(0) + 4, 4);
         v[at] = 0;
       }},
      {"chunk size", Problem::kChunkSize, [](SetBuilder& b, std::vector<unsigned char>& v) { SetU32(&v, b.ChunkAt(0) + 12, 0); }},
      {"record chunk", Problem::kRecordChunk, [](SetBuilder& b, std::vector<unsigned char>& v) { SetU32(&v, b.RecordAt(1) + 0x10, 7); }},
      {"record chunk equal to the count", Problem::kRecordChunk,
       [](SetBuilder& b, std::vector<unsigned char>& v) { SetU32(&v, b.RecordAt(1) + 0x10, 1); }},
      {"record range", Problem::kRecordRange,
       [](SetBuilder& b, std::vector<unsigned char>& v) { SetU32(&v, b.RecordAt(2) + 0x18, 100000); }},
      {"alignment", Problem::kRecordAlignment, [](SetBuilder& b, std::vector<unsigned char>& v) { SetU32(&v, b.RecordAt(2) + 0x1c, 12); }},
      {"alignment over 4096", Problem::kRecordAlignment,
       [](SetBuilder& b, std::vector<unsigned char>& v) { SetU32(&v, b.RecordAt(2) + 0x1c, 8192); }},
      {"chunk reserved", Problem::kChunkReserved, [](SetBuilder& b, std::vector<unsigned char>& v) { SetU32(&v, b.ChunkAt(0), 1); }},
      {"chunk under 4 bytes", Problem::kChunkRange, [](SetBuilder& b, std::vector<unsigned char>& v) { SetU32(&v, b.ChunkAt(0) + 8, 3); }},
      {"duplicate", Problem::kDuplicate,
       [](SetBuilder& b, std::vector<unsigned char>& v) {
         std::memcpy(v.data() + b.RecordAt(2), v.data() + b.RecordAt(0), 16);
       }},
  };
  for (const Case& c : cases) {
    SetBuilder b = LevelSet();
    std::vector<unsigned char> bytes = b.Build();
    ASSERT_EQ(ParseProblem(bytes), Problem::kNone) << c.name;
    c.mutate(b, bytes);
    EXPECT_EQ(ParseProblem(bytes), c.expected) << c.name << ": got " << nevr_resource_set::ProblemName(ParseProblem(bytes));
  }
}

TEST(ResourceSetFormat, TheLargestAlignmentTheGameUsesIsAccepted) {
  SetBuilder b = LevelSet();
  b.AddRecord(kMeshType, 0x7778ULL, Bytes(8, 1), 0, 4096);  // the game's manifests use 4, 16, 2048 and 4096
  EXPECT_EQ(ParseProblem(b.Build()), Problem::kNone);
}

TEST(ResourceSetFormat, ARecordOfZeroBytesIsAccepted) {
  // The game's own manifest (48037dc70b0ecab2) holds 2,515 records of 0 bytes; a level built from it carries some.
  SetBuilder b = LevelSet();
  b.AddRecord(kMeshType, 0x7777ULL, {}, 0);
  EXPECT_EQ(ParseProblem(b.Build()), Problem::kNone);
}

TEST(ResourceSetFormat, ASmallRecordInALargeChunkIsRefusedAndALargeRecordMustBeAlone) {
  // The reader serves a record of at most 0x80000 bytes from its block cache of chunks of at most that size.
  SetBuilder small;
  const std::uint32_t c = small.AddChunk();
  small.AddRecord(kMeshType, 1, Bytes(0x80000, 1), c);
  small.AddRecord(kMeshType, 2, Bytes(64, 2), c);
  EXPECT_EQ(ParseProblem(small.Build()), Problem::kRecordInLargeChunk);

  // A record larger than that is decompressed straight into its buffer: alone, at 0, the whole chunk.
  SetBuilder alone;
  const std::uint32_t big = alone.AddChunk();
  alone.AddRecord(kMeshType, 3, Bytes(0x90000, 3), big);
  EXPECT_EQ(ParseProblem(alone.Build()), Problem::kNone);

  // A large record that is not the chunk's whole size (a second record follows it in the chunk).
  SetBuilder notWhole;
  const std::uint32_t shared = notWhole.AddChunk();
  notWhole.AddRecord(kMeshType, 5, Bytes(0x90000, 5), shared, 16);
  notWhole.AddRecord(kMeshType, 4, Bytes(32, 4), shared, 16);
  EXPECT_EQ(ParseProblem(notWhole.Build()), Problem::kRecordNotAlone);

  // Two large records that both claim one chunk's whole bytes: the second would alias the first's buffer.
  SetBuilder twice;
  const std::uint32_t one = twice.AddChunk();
  twice.AddRecord(kMeshType, 6, Bytes(0x90000, 6), one);
  const std::uint32_t other = twice.AddChunk();
  twice.AddRecord(kMeshType, 7, Bytes(16, 7), other);
  std::vector<unsigned char> v = twice.Build();
  ASSERT_EQ(ParseProblem(v), Problem::kNone);
  SetU32(&v, twice.RecordAt(1) + 0x10, one);         // the second record names the large chunk
  SetU32(&v, twice.RecordAt(1) + 0x14, 0);
  SetU32(&v, twice.RecordAt(1) + 0x18, 0x90000);
  EXPECT_EQ(ParseProblem(v), Problem::kRecordNotAlone);
}

TEST(ResourceSetFormat, OverridesAreCheckedForRangeDuplicatesAndRecordKeys) {
  {
    SetBuilder b = LevelSet();
    b.AddOverride(9, 9, Bytes(4, 1));
    std::vector<unsigned char> v = b.Build();
    SetU32(&v, b.OverrideAt(0) + 0x14, static_cast<std::uint32_t>(v.size()));
    EXPECT_EQ(ParseProblem(v), Problem::kOverrideRange);
  }
  {
    SetBuilder b = LevelSet();
    b.AddOverride(9, 9, Bytes(4, 1));
    b.AddOverride(9, 9, Bytes(4, 2));
    EXPECT_EQ(ParseProblem(b.Build()), Problem::kOverrideDuplicate);
  }
  {
    SetBuilder b = LevelSet();
    b.AddOverride(9, 9, {});
    EXPECT_EQ(ParseProblem(b.Build()), Problem::kOverrideRange) << "an override of 0 bytes replaces nothing";
  }
  {
    SetBuilder b = LevelSet();
    b.AddOverride(kLevelRootType, kLevel, Bytes(4, 1));
    EXPECT_EQ(ParseProblem(b.Build()), Problem::kOverrideIsRecord);
  }
}

// ---- the registry -------------------------------------------------------------------------------------------------

TEST(ResourceSetRegistry, AMountedSetIsFoundThroughTheShadowAtItsFileOffset) {
  nevr_shadow::Shadow::ResetChunkCounterForTest();
  SetBuilder b = LevelSet();
  const std::vector<unsigned char> bytes = b.Build();
  Registry registry;
  ASSERT_TRUE(registry.Add("levels", "test levels", bytes.data(), bytes.size(), 77, 0x4000).ok());
  registry.Commit("levels");
  FakeStock stock;
  const nevr_resource_sets::MountReport report = registry.Mount(stock.View());
  EXPECT_EQ(report.sets, 1u);
  EXPECT_EQ(report.refused, 0u);
  EXPECT_EQ(report.records, 3u);
  EXPECT_EQ(report.levels, 1u);
  ASSERT_NE(registry.shadow(), nullptr);
  EXPECT_GE(report.chunk_base, nevr_shadow::kFirstChunkIndex);

  // What the game's own function reads from the shadow's manager: the plugin's file at the frame's offset in it.
  nevr_shadow::Found found;
  ASSERT_TRUE(nevr_shadow::ReadLikeTheEngine(registry.shadow()->manager(), kMeshType, 0x3333333333333333ULL, &found));
  nevr_resource_set::View view;
  ASSERT_TRUE(Parse(bytes.data(), bytes.size(), &view).ok());
  EXPECT_EQ(found.handle, 77);
  EXPECT_EQ(found.file_offset, 0x4000u + view.ChunkAt(0).offset);
  EXPECT_EQ(found.compressed, view.ChunkAt(0).compressed);
  EXPECT_EQ(found.decompressed, view.ChunkAt(0).decompressed);
  EXPECT_EQ(found.offset_in_chunk, view.RecordAt(2).offset_in_chunk);
  EXPECT_EQ(found.size, 200u);
  EXPECT_GE(found.chunk, nevr_shadow::kFirstChunkIndex);
  EXPECT_EQ(registry.MountedLevels(), std::vector<std::uint64_t>{kLevel});
}

TEST(ResourceSetRegistry, ASetThatDoesNotParseIsRefusedNamingTheRule) {
  std::vector<unsigned char> bytes = LevelSet().Build();
  bytes[7] = '2';
  Registry registry;
  const nevr_resource_sets::Outcome o = registry.Add("p", "l", bytes.data(), bytes.size(), 1, 0);
  EXPECT_EQ(o.refusal, Refusal::kFormat);
  EXPECT_EQ(o.detail, "magic at index 0");
  EXPECT_EQ(registry.held(), 0u);
}

TEST(ResourceSetRegistry, AnArchiveWhoseDependencyCountRunsPastItsEndIsRefused) {
  SetBuilder b;
  const std::uint32_t c = b.AddChunk();
  b.AddRecord(kLevelRootType, kLevel, Bytes(64, 1), c);
  std::vector<unsigned char> archive = Archive({}, {});
  archive[0] = 0xFF;  // 255 dependencies in a 16-byte archive
  b.AddRecord(kArchiveType, kLevel, archive, c);
  const std::vector<unsigned char> bytes = b.Build();
  Registry registry;
  const nevr_resource_sets::Outcome o = registry.Add("p", "l", bytes.data(), bytes.size(), 1, 0);
  EXPECT_EQ(o.refusal, Refusal::kArchive);
  EXPECT_NE(o.detail.find("does not parse"), std::string::npos) << o.detail;
}

// The SDK's helper with a host from before v9: its context ends before register_resources, so the helper does not
// call it (it would read past the context).
int g_registerCalls = 0;
int32_t CountingRegister(const char*, const NevrResourcesDesc*) {
  ++g_registerCalls;
  return NEVR_RESOURCES_OK;
}

TEST(ResourceSetSdk, AnOlderHostsContextIsNotCalledThrough) {
  static const unsigned char set[8] = {};
  NevrGameContext ctx = {};
  ctx.register_resources = &CountingRegister;
  ctx.ctx_size = static_cast<decltype(ctx.ctx_size)>(offsetof(NevrGameContext, register_resources));
  g_registerCalls = 0;
  EXPECT_EQ(NevrRegisterEmbeddedResources(&ctx, set, sizeof(set), "old host").status, -1);
  EXPECT_EQ(g_registerCalls, 0);
  ctx.ctx_size = static_cast<decltype(ctx.ctx_size)>(sizeof(ctx));
  EXPECT_EQ(NevrRegisterEmbeddedResources(&ctx, set, sizeof(set), "v9 host").status, NEVR_RESOURCES_OK);
  EXPECT_EQ(g_registerCalls, 1);
}

TEST(ResourceSetRegistry, ARecordTheGameLoadsAsADllByNameIsRefused) {
  SetBuilder b = LevelSet();
  b.AddRecord(kScriptType, 0x77ULL, Bytes(16, 1), 0);
  const std::vector<unsigned char> bytes = b.Build();
  Registry registry;
  const nevr_resource_sets::Outcome o = registry.Add("p", "l", bytes.data(), bytes.size(), 1, 0);
  EXPECT_EQ(o.refusal, Refusal::kScript);
  EXPECT_EQ(registry.held(), 0u);
}

TEST(ResourceSetRegistry, ATextureDescriptorNeedsItsPayloadRecord) {
  SetBuilder without = LevelSet();
  without.AddRecord(kTextureType, 0x88ULL, Bytes(256, 1), 0);
  std::vector<unsigned char> bytes = without.Build();
  Registry r1;
  EXPECT_EQ(r1.Add("p", "l", bytes.data(), bytes.size(), 1, 0).refusal, Refusal::kTexturePayload);

  SetBuilder with = LevelSet();
  with.AddRecord(kTextureType, 0x88ULL, Bytes(256, 1), 0);
  with.AddRecord(kTextureGpuType, 0x88ULL, Bytes(1024, 2), 0);
  bytes = with.Build();
  Registry r2;
  EXPECT_TRUE(r2.Add("p", "l", bytes.data(), bytes.size(), 1, 0).ok());
}

TEST(ResourceSetRegistry, ALevelRootWithoutItsArchiveIsRefusedNamingTheArchiveKey) {
  SetBuilder b;
  const std::uint32_t c = b.AddChunk();
  b.AddRecord(kLevelRootType, kLevel, Bytes(64, 1), c);
  const std::vector<unsigned char> bytes = b.Build();
  Registry registry;
  const nevr_resource_sets::Outcome o = registry.Add("p", "l", bytes.data(), bytes.size(), 1, 0);
  EXPECT_EQ(o.refusal, Refusal::kLevelIncomplete);
  EXPECT_EQ(o.detail, "0x2a41cf1c1d9e5d32:0xdb69545ff59c76d0");
}

TEST(ResourceSetRegistry, AnArchiveThatDoesNotParseIsRefused) {
  SetBuilder b;
  const std::uint32_t c = b.AddChunk();
  b.AddRecord(kLevelRootType, kLevel, Bytes(64, 1), c);
  std::vector<unsigned char> archive = Archive({}, {});
  archive[4] = 0xFF;  // an entry count far past the archive's end
  b.AddRecord(kArchiveType, kLevel, archive, c);
  const std::vector<unsigned char> bytes = b.Build();
  Registry registry;
  EXPECT_EQ(registry.Add("p", "l", bytes.data(), bytes.size(), 1, 0).refusal, Refusal::kArchive);
}

TEST(ResourceSetRegistry, ASecondPluginsSetWithAKeyTheFirstHoldsIsRefusedWholeAndTheFirstKeepsIt) {
  const std::vector<unsigned char> first = LevelSet().Build();
  const std::vector<unsigned char> second = LevelSet(kLevel, 0x9999ULL).Build();  // same level name
  Registry registry;
  ASSERT_TRUE(registry.Add("a", "first", first.data(), first.size(), 1, 0).ok());
  registry.Commit("a");
  const nevr_resource_sets::Outcome o = registry.Add("b", "second", second.data(), second.size(), 2, 0);
  EXPECT_EQ(o.refusal, Refusal::kCollides);
  EXPECT_NE(o.detail.find("owner=plugin:a"), std::string::npos) << o.detail;
  EXPECT_EQ(registry.held(), 1u);
  FakeStock stock;
  EXPECT_EQ(registry.Mount(stock.View()).sets, 1u);
}

TEST(ResourceSetRegistry, AFailedInitDropsItsSetsAndAnUncommittedSetIsNotMounted) {
  const std::vector<unsigned char> bytes = LevelSet().Build();
  Registry registry;
  ASSERT_TRUE(registry.Add("p", "l", bytes.data(), bytes.size(), 5, 0).ok());
  EXPECT_EQ(registry.Drop("p"), std::vector<std::int64_t>{5});
  EXPECT_EQ(registry.held(), 0u);

  ASSERT_TRUE(registry.Add("q", "l", bytes.data(), bytes.size(), 6, 0).ok());
  FakeStock stock;
  const nevr_resource_sets::MountReport report = registry.Mount(stock.View());
  EXPECT_EQ(report.sets, 0u);
  EXPECT_EQ(report.handles_to_close, std::vector<std::int64_t>{6});
  EXPECT_EQ(registry.shadow(), nullptr);
}

TEST(ResourceSetRegistry, ASetThatNamesAKeyTheGameHasIsRefusedAtTheMount) {
  const std::vector<unsigned char> clash = LevelSet(kLevel, kStockMesh).Build();  // carries the stock mesh
  const std::vector<unsigned char> fine = LevelSet(0xABCULL, 0xDEFULL).Build();
  Registry registry;
  ASSERT_TRUE(registry.Add("clash", "c", clash.data(), clash.size(), 1, 0).ok());
  ASSERT_TRUE(registry.Add("fine", "f", fine.data(), fine.size(), 2, 0).ok());
  registry.Commit("clash");
  registry.Commit("fine");
  FakeStock stock;
  const nevr_resource_sets::MountReport report = registry.Mount(stock.View());
  ASSERT_EQ(report.refusals.size(), 1u);
  EXPECT_EQ(report.refusals[0].plugin, "clash");
  EXPECT_EQ(report.refusals[0].outcome.refusal, Refusal::kStockKey);
  EXPECT_EQ(report.refusals[0].outcome.detail, nevr_resource_sets::KeyText(kMeshType, kStockMesh));
  EXPECT_EQ(report.sets, 1u);
  EXPECT_EQ(report.handles_to_close, std::vector<std::int64_t>{1});
  EXPECT_EQ(registry.MountedLevels(), std::vector<std::uint64_t>{0xABCULL});
}

TEST(ResourceSetRegistry, ALevelWhoseArchiveListsWhatNothingServesIsRefusedNamingTheEntry) {
  SetBuilder b;
  const std::uint32_t c = b.AddChunk();
  b.AddRecord(kLevelRootType, kLevel, Bytes(64, 1), c);
  b.AddRecord(kArchiveType, kLevel, Archive({}, {{kMeshType, 0x4242ULL}}), c);
  const std::vector<unsigned char> bytes = b.Build();
  Registry registry;
  ASSERT_TRUE(registry.Add("p", "l", bytes.data(), bytes.size(), 1, 0).ok());
  registry.Commit("p");
  FakeStock stock;
  const nevr_resource_sets::MountReport report = registry.Mount(stock.View());
  ASSERT_EQ(report.refusals.size(), 1u);
  EXPECT_EQ(report.refusals[0].outcome.refusal, Refusal::kUnresolved);
  EXPECT_NE(report.refusals[0].outcome.detail.find(nevr_resource_sets::KeyText(kMeshType, 0x4242ULL)), std::string::npos);
  EXPECT_EQ(registry.MountedLevels().size(), 0u);
}

TEST(ResourceSetRegistry, AnArchiveDependencyResolvesInAnEarlierSetAndOneNothingHasIsRefused) {
  // The first set carries an archive the second level depends on; the third depends on one nothing has.
  SetBuilder shared;
  const std::uint32_t c0 = shared.AddChunk();
  shared.AddRecord(kArchiveType, 0x5151ULL, Archive({}, {}), c0);
  shared.AddRecord(kMeshType, 0x5252ULL, Bytes(8, 1), c0);
  SetBuilder dependent;
  const std::uint32_t c1 = dependent.AddChunk();
  dependent.AddRecord(kLevelRootType, 0x6161ULL, Bytes(8, 1), c1);
  dependent.AddRecord(kArchiveType, 0x6161ULL, Archive({0x5151ULL}, {{kMeshType, 0x5252ULL}}), c1);
  SetBuilder orphan;
  const std::uint32_t c2 = orphan.AddChunk();
  orphan.AddRecord(kLevelRootType, 0x7171ULL, Bytes(8, 1), c2);
  orphan.AddRecord(kArchiveType, 0x7171ULL, Archive({0x9191ULL}, {}), c2);
  const std::vector<unsigned char> a = shared.Build(), b = dependent.Build(), c = orphan.Build();
  Registry registry;
  ASSERT_TRUE(registry.Add("shared", "s", a.data(), a.size(), 1, 0).ok());
  ASSERT_TRUE(registry.Add("dependent", "d", b.data(), b.size(), 2, 0).ok());
  ASSERT_TRUE(registry.Add("orphan", "o", c.data(), c.size(), 3, 0).ok());
  registry.Commit("shared");
  registry.Commit("dependent");
  registry.Commit("orphan");
  FakeStock stock;
  const nevr_resource_sets::MountReport report = registry.Mount(stock.View());
  EXPECT_EQ(report.sets, 2u);
  ASSERT_EQ(report.refusals.size(), 1u);
  EXPECT_EQ(report.refusals[0].plugin, "orphan");
  EXPECT_NE(report.refusals[0].outcome.detail.find("depends on " + nevr_resource_sets::KeyText(kArchiveType, 0x9191ULL)),
            std::string::npos)
      << report.refusals[0].outcome.detail;
}

TEST(ResourceSetRegistry, NothingIsTakenAfterTheMount) {
  const std::vector<unsigned char> bytes = LevelSet().Build();
  Registry registry;
  FakeStock stock;
  registry.Mount(stock.View());
  EXPECT_EQ(registry.Add("p", "l", bytes.data(), bytes.size(), 1, 0).refusal, Refusal::kTooLate);
}

TEST(ResourceSetRegistry, AChunkWhoseFileOffsetDoesNotFitTheGamesU32IsRefused) {
  const std::vector<unsigned char> bytes = LevelSet().Build();
  Registry registry;
  const nevr_resource_sets::Outcome o = registry.Add("p", "l", bytes.data(), bytes.size(), 1, 0xFFFFFF00ULL);
  EXPECT_EQ(o.refusal, Refusal::kFileOffset);
  EXPECT_STREQ(nevr_resource_sets::RefusalName(o.refusal), "file_offset_range");
  EXPECT_NE(o.detail.find("chunk 0 ends at byte"), std::string::npos) << o.detail;
  EXPECT_FALSE(registry.Holds(kLevelRootType, kLevel));
}

TEST(ResourceSetRegistry, AnArchiveLargerThanTheReadersBlockIsRefusedBeforeItIsRead) {
  // An archive sits in a block-cache chunk (the game's largest is 66,256 bytes); a larger one would make the registry
  // decompress a chunk of any size the set declares.
  SetBuilder b;
  const std::uint32_t root = b.AddChunk();
  b.AddRecord(kLevelRootType, kLevel, Bytes(64, 1), root);
  std::vector<unsigned char> archive = Archive({}, {});
  archive.resize(0x80001, 0);
  const std::uint32_t big = b.AddChunk();
  b.AddRecord(kArchiveType, kLevel, archive, big);
  const std::vector<unsigned char> bytes = b.Build();
  ASSERT_EQ(ParseProblem(bytes), Problem::kNone);
  Registry registry;
  const nevr_resource_sets::Outcome o = registry.Add("p", "l", bytes.data(), bytes.size(), 1, 0);
  EXPECT_EQ(o.refusal, Refusal::kArchive);
  EXPECT_NE(o.detail.find("an archive of 524289 bytes"), std::string::npos) << o.detail;
}

TEST(ResourceSetRegistry, AnArchiveWhoseChunkDoesNotDecompressToItsSizeIsRefused) {
  SetBuilder b = LevelSet();
  std::vector<unsigned char> v = b.Build();
  std::uint32_t decompressed = 0;
  std::memcpy(&decompressed, v.data() + b.ChunkAt(0) + 12, 4);
  SetU32(&v, b.ChunkAt(0) + 12, decompressed + 16);  // the frame holds 16 bytes fewer than the chunk says
  ASSERT_EQ(ParseProblem(v), Problem::kNone);
  Registry registry;
  const nevr_resource_sets::Outcome o = registry.Add("p", "l", v.data(), v.size(), 1, 0);
  EXPECT_EQ(o.refusal, Refusal::kArchive);
  EXPECT_NE(o.detail.find("does not decompress"), std::string::npos) << o.detail;
}

TEST(ResourceSetRegistry, ASetThatWouldRunTheShadowOutOfChunkIndicesIsRefusedAloneAtRegistration) {
  SetBuilder two;
  two.AddChunk();
  two.AddChunk();
  two.AddRecord(kMeshType, 1, Bytes(8, 1), 0);
  two.AddRecord(kMeshType, 2, Bytes(8, 2), 1);
  SetBuilder twoMore;
  twoMore.AddChunk();
  twoMore.AddChunk();
  twoMore.AddRecord(kMeshType, 3, Bytes(8, 3), 0);
  twoMore.AddRecord(kMeshType, 4, Bytes(8, 4), 1);
  const std::vector<unsigned char> a = two.Build(), b = twoMore.Build();
  Registry registry(3);  // three indices to give out
  ASSERT_TRUE(registry.Add("a", "a", a.data(), a.size(), 1, 0).ok());
  const nevr_resource_sets::Outcome o = registry.Add("b", "b", b.data(), b.size(), 2, 0);
  EXPECT_EQ(o.refusal, Refusal::kIndexSpace);
  EXPECT_EQ(o.detail, "2 chunks, 1 left");
  EXPECT_EQ(registry.Drop("a"), std::vector<std::int64_t>{1});
  EXPECT_TRUE(registry.Add("b", "b", b.data(), b.size(), 2, 0).ok()) << "a dropped set gives its indices back";
}

TEST(ResourceSetRegistry, ASetsOverridesAreHeldWithItPointingIntoItsBytes) {
  SetBuilder b = LevelSet();
  b.AddOverride(0x4444444444444444ULL, 0x5555ULL, Bytes(10, 0x33));
  const std::vector<unsigned char> bytes = b.Build();
  Registry registry;
  ASSERT_TRUE(registry.Add("p", "l", bytes.data(), bytes.size(), 1, 0).ok());
  ASSERT_EQ(registry.HeldSets().size(), 1u);
  const std::vector<nevr_resource_sets::OverrideRef>& overrides = registry.HeldSets()[0]->overrides;
  ASSERT_EQ(overrides.size(), 1u);
  EXPECT_EQ(overrides[0].type, 0x4444444444444444ULL);
  EXPECT_EQ(overrides[0].size, 10u);
  EXPECT_EQ(overrides[0].data, bytes.data() + bytes.size() - 10);
  EXPECT_TRUE(registry.Holds(kMeshType, 0x3333333333333333ULL));
  EXPECT_FALSE(registry.Holds(0x4444444444444444ULL, 0x5555ULL)) << "an override is not a record the set serves";
}

TEST(ResourceSetRegistry, ADependencyArchiveInASetMustListOnlyWhatResolves) {
  // The first set carries an archive (no level root) that lists a mesh nothing has; the second set's level depends on
  // it. The second is refused naming the dependency's entry; the first (no level of its own) mounts.
  SetBuilder shared;
  const std::uint32_t c0 = shared.AddChunk();
  shared.AddRecord(kArchiveType, 0x5151ULL, Archive({}, {{kMeshType, 0x4343ULL}}), c0);
  SetBuilder dependent;
  const std::uint32_t c1 = dependent.AddChunk();
  dependent.AddRecord(kLevelRootType, 0x6161ULL, Bytes(8, 1), c1);
  dependent.AddRecord(kArchiveType, 0x6161ULL, Archive({0x5151ULL}, {}), c1);
  const std::vector<unsigned char> a = shared.Build(), b = dependent.Build();
  Registry registry;
  ASSERT_TRUE(registry.Add("shared", "s", a.data(), a.size(), 1, 0).ok());
  ASSERT_TRUE(registry.Add("dependent", "d", b.data(), b.size(), 2, 0).ok());
  registry.Commit("shared");
  registry.Commit("dependent");
  FakeStock stock;
  const nevr_resource_sets::MountReport report = registry.Mount(stock.View());
  ASSERT_EQ(report.refusals.size(), 1u);
  EXPECT_EQ(report.refusals[0].plugin, "dependent");
  EXPECT_NE(report.refusals[0].outcome.detail.find("archive " + nevr_resource_sets::KeyText(kArchiveType, 0x5151ULL) +
                                                   " lists " + nevr_resource_sets::KeyText(kMeshType, 0x4343ULL)),
            std::string::npos)
      << report.refusals[0].outcome.detail;
}

TEST(ResourceSetRegistry, AStockArchiveEndsTheWalkAndACycleOfArchivesEnds) {
  // The level depends on an archive the game has (its contents are the game's) and on one of its own that depends back
  // on the level's archive: the walk visits each archive once and the set mounts.
  SetBuilder b;
  const std::uint32_t c = b.AddChunk();
  b.AddRecord(kLevelRootType, kLevel, Bytes(8, 1), c);
  b.AddRecord(kArchiveType, kLevel, Archive({0xA0A0ULL, 0xB0B0ULL}, {}), c);
  b.AddRecord(kArchiveType, 0xB0B0ULL, Archive({kLevel}, {{kMeshType, kStockMesh}}), c);
  const std::vector<unsigned char> bytes = b.Build();
  Registry registry;
  ASSERT_TRUE(registry.Add("p", "l", bytes.data(), bytes.size(), 1, 0).ok());
  registry.Commit("p");
  FakeStock stock;
  stock.keys.insert({kArchiveType, 0xA0A0ULL});
  const nevr_resource_sets::MountReport report = registry.Mount(stock.View());
  EXPECT_EQ(report.sets, 1u);
  EXPECT_TRUE(report.refusals.empty()) << report.refusals[0].outcome.detail;
}

TEST(ResourceSetRegistry, TwoMountedSetsGetTheirOwnPackageHandleAndChunkRange) {
  nevr_shadow::Shadow::ResetChunkCounterForTest();
  const std::vector<unsigned char> a = LevelSet(0xA1ULL, 0xA2ULL).Build();
  const std::vector<unsigned char> b = LevelSet(0xB1ULL, 0xB2ULL).Build();
  Registry registry;
  ASSERT_TRUE(registry.Add("a", "a", a.data(), a.size(), 10, 0x100).ok());
  ASSERT_TRUE(registry.Add("b", "b", b.data(), b.size(), 20, 0x200).ok());
  registry.Commit("a");
  registry.Commit("b");
  FakeStock stock;
  ASSERT_EQ(registry.Mount(stock.View()).sets, 2u);
  nevr_shadow::Found fa, fb;
  std::uint32_t ua = 9, ub = 9;
  ASSERT_TRUE(registry.shadow()->Find(kMeshType, 0xA2ULL, &fa, &ua));
  ASSERT_TRUE(registry.shadow()->Find(kMeshType, 0xB2ULL, &fb, &ub));
  EXPECT_EQ(fa.handle, 10);
  EXPECT_EQ(fb.handle, 20);
  EXPECT_NE(fa.chunk, fb.chunk);
  EXPECT_EQ(registry.SetForUser(ua)->plugin, "a");
  EXPECT_EQ(registry.SetForUser(ub)->plugin, "b");
}

// ---- the image mapping --------------------------------------------------------------------------------------------

// A minimal mapped PE32+ image: one section at 0x1000 (virtual size 0x3000, raw size 0x2000 at file 0x400), and a
// base relocation directory at 0x5000 with one DIR64 entry at 0x1800.
std::vector<unsigned char> Image(bool withRelocation, bool writable = false) {
  std::vector<unsigned char> img(0x6000, 0);
  img[0] = 'M';
  img[1] = 'Z';
  SetU32(&img, 0x3c, 0x80);
  SetU32(&img, 0x80, 0x00004550u);
  auto put16 = [&img](std::size_t at, std::uint16_t v) {
    img[at] = static_cast<unsigned char>(v);
    img[at + 1] = static_cast<unsigned char>(v >> 8);
  };
  put16(0x80 + 6, 1);       // one section
  put16(0x80 + 20, 0xF0);   // optional header bytes
  const std::size_t opt = 0x80 + 24;
  put16(opt, 0x20b);
  SetU32(&img, opt + 56, 0x6000);  // SizeOfImage
  SetU32(&img, opt + 108, 16);     // NumberOfRvaAndSizes
  if (withRelocation) {
    SetU32(&img, opt + 112 + 5 * 8, 0x5000);
    SetU32(&img, opt + 112 + 5 * 8 + 4, 12);
    SetU32(&img, 0x5000, 0x1000);              // page
    SetU32(&img, 0x5004, 12);                  // block size: header + two entries
    put16(0x5008, static_cast<std::uint16_t>((10u << 12) | 0x800));  // DIR64 at 0x1800
    put16(0x500a, 0);                          // ABSOLUTE padding
  }
  const std::size_t section = opt + 0xF0;
  SetU32(&img, section + 8, 0x3000);   // VirtualSize
  SetU32(&img, section + 12, 0x1000);  // VirtualAddress
  SetU32(&img, section + 16, 0x2000);  // SizeOfRawData
  SetU32(&img, section + 20, 0x400);   // PointerToRawData
  SetU32(&img, section + 36, writable ? 0xC0000040u : 0x40000040u);  // initialised data, read (and write)
  return img;
}

TEST(ImageFileRange, ARangeInASectionsRawDataMapsToItsFileOffset) {
  const std::vector<unsigned char> img = Image(false);
  const nevr_image_range::Result r = nevr_image_range::FileOffsetOf(img.data(), 0x1100, 0x200);
  ASSERT_TRUE(r.ok()) << nevr_image_range::ProblemName(r.problem);
  EXPECT_EQ(r.file_offset, 0x400u + 0x100u);
}

TEST(ImageFileRange, EachRangeTheFileDoesNotHoldAsMappedIsRefused) {
  const std::vector<unsigned char> plain = Image(false);
  const std::vector<unsigned char> relocated = Image(true);
  EXPECT_EQ(nevr_image_range::FileOffsetOf(plain.data(), 0x500, 0x10).problem, nevr_image_range::Problem::kNoSection);
  EXPECT_EQ(nevr_image_range::FileOffsetOf(plain.data(), 0x3F00, 0x200).problem,
            nevr_image_range::Problem::kCrossesSection);
  EXPECT_EQ(nevr_image_range::FileOffsetOf(plain.data(), 0x2F00, 0x200).problem, nevr_image_range::Problem::kPastRawData);
  EXPECT_EQ(nevr_image_range::FileOffsetOf(relocated.data(), 0x1700, 0x200).problem,
            nevr_image_range::Problem::kRelocated);
  EXPECT_TRUE(nevr_image_range::FileOffsetOf(relocated.data(), 0x1900, 0x200).ok());
  const std::vector<unsigned char> writable = Image(false, true);
  EXPECT_EQ(nevr_image_range::FileOffsetOf(writable.data(), 0x1100, 0x200).problem, nevr_image_range::Problem::kWritable);
  std::vector<unsigned char> broken = plain;
  broken[0] = 'X';
  EXPECT_EQ(nevr_image_range::FileOffsetOf(broken.data(), 0x1100, 0x10).problem, nevr_image_range::Problem::kHeaders);
}

}  // namespace
