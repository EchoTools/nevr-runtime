// core/map_derive.h (CDN_DESIGN PC10): recipes rebuilt from a fake install into memory files. Synthetic bytes only.
#include "core/map_derive.h"

#include <gtest/gtest.h>
#include <zstd.h>

#include <cstring>
#include <fstream>
#include <map>
#include <nlohmann/json.hpp>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "nevr/resource_set.h"
#include "quest/tests/cargo_fake_env.h"

namespace {

using Bytes = std::vector<std::uint8_t>;
using nevr_maps::DiffOp;
using nevr_maps::EditKind;
using nevr_maps::FieldEdit;
using nevr_maps::Recipe;

constexpr std::uint64_t kType = 0x1111222233334444ULL;
constexpr std::uint64_t kStockSmall = 0x5000, kStockLarge = 0x6000;
constexpr std::uint64_t kLevel = 0x7777000000000001ULL;
constexpr std::uint64_t kStockScript = 0x00000000000000abULL, kMapScript = 0x99990000000000cdULL;
const std::string kDir = "/cache/m1/windows-x86_64/abc";
const std::string kSetPath = kDir + "/set/windows-x86_64.nevrres";
const std::string kRecipePath = kDir + "/recipe/windows-x86_64.bin";
const std::string kMergedPath = kDir + "/set/windows-x86_64.mounted.nevrres";

std::string Sha(const Bytes& b) { return cargo_test::Sha256Of(std::string(b.begin(), b.end())); }

Bytes Pattern(std::size_t size, unsigned mul, unsigned add) {
  Bytes b(size);
  for (std::size_t i = 0; i < size; ++i) b[i] = static_cast<std::uint8_t>(i * mul + add);
  return b;
}

void PutLe(Bytes* b, std::size_t at, unsigned width, std::uint64_t v) {
  for (unsigned i = 0; i < width; ++i) (*b)[at + i] = static_cast<std::uint8_t>(v >> (8 * i));
}

Bytes FromHex(const std::string& hex) {
  Bytes b;
  for (std::size_t i = 0; i + 1 < hex.size(); i += 2) b.push_back(static_cast<std::uint8_t>(std::stoul(hex.substr(i, 2), nullptr, 16)));
  return b;
}

DiffOp Copy(std::uint64_t offset, std::uint64_t size) { return DiffOp{false, offset, size}; }
DiffOp Insert(std::uint64_t offset, std::uint64_t size) { return DiffOp{true, offset, size}; }

class NoMapFs : public nevr_maps::MapFs {
 public:
  bool StampOf(std::int64_t, nevr_maps::FileStamp*) override { return false; }
  std::string FinalPathOf(std::int64_t) override { return std::string(); }
  bool OpenHeld(const std::string&, std::int64_t*, std::uint64_t*) override { return false; }
  bool ListDir(const std::string&, std::vector<std::string>*) override { return false; }
  bool RemoveTree(const std::string&) override { return false; }
  bool RenameDir(const std::string&, const std::string&) override { return false; }
};

// Each resource is read from "manifest" plus its own package file; StampsCurrent compares with `current`.
class FakeStock : public nevr_maps::StockSource {
 public:
  std::map<std::pair<std::uint64_t, std::uint64_t>, Bytes> resources;
  std::map<std::uint64_t, Bytes> scripts;
  std::map<std::pair<std::uint64_t, std::uint64_t>, int> reads;
  std::map<std::string, nevr_maps::FileStamp> current;

  nevr_maps::StockStamp Stamp(const std::string& path) {
    if (current.find(path) == current.end()) current[path].id_lo = current.size() + 1;
    return {path, current[path]};
  }
  bool ReadResource(std::uint64_t type, std::uint64_t name, Bytes* out, std::vector<nevr_maps::StockStamp>* read_from) override {
    ++reads[{type, name}];
    read_from->push_back(Stamp("stock/manifest"));
    const auto it = resources.find({type, name});
    if (it == resources.end()) return false;
    read_from->push_back(Stamp("stock/package_" + std::to_string(name)));
    *out = it->second;
    return true;
  }
  bool ReadScript(std::uint64_t hash, Bytes* out, std::vector<nevr_maps::StockStamp>* read_from) override {
    const auto it = scripts.find(hash);
    if (it == scripts.end()) return false;
    read_from->push_back(Stamp("stock/scripts/" + std::to_string(hash)));
    *out = it->second;
    return true;
  }
  bool StampsCurrent(const std::vector<nevr_maps::StockStamp>& stamps) override {
    for (const auto& s : stamps) {
      const auto it = current.find(s.path);
      if (it == current.end() || it->second != s.stamp) return false;
    }
    return true;
  }
};

// A set with each record in its own zstd chunk.
std::string BuildSet(const std::vector<std::pair<std::pair<std::uint64_t, std::uint64_t>, Bytes>>& records) {
  std::vector<std::string> frames;
  for (const auto& r : records) {
    std::string frame(ZSTD_compressBound(r.second.size()), '\0');
    frame.resize(ZSTD_compress(&frame[0], frame.size(), r.second.data(), r.second.size(), 1));
    frames.push_back(frame);
  }
  const std::size_t n = records.size();
  Bytes head(48 + n * 0x20 + n * 0x10, 0);
  std::memcpy(head.data(), "NEVRRES1", 8);
  PutLe(&head, 8, 4, 48);
  PutLe(&head, 0x0c, 4, n);
  PutLe(&head, 0x10, 4, n);
  PutLe(&head, 0x18, 8, 48);
  PutLe(&head, 0x20, 8, 48 + n * 0x20);
  PutLe(&head, 0x28, 8, head.size());
  std::uint64_t at = head.size();
  for (std::size_t i = 0; i < n; ++i) {
    const std::size_t r = 48 + i * 0x20, c = 48 + n * 0x20 + i * 0x10;
    PutLe(&head, r, 8, records[i].first.first);
    PutLe(&head, r + 8, 8, records[i].first.second);
    PutLe(&head, r + 0x10, 4, i);
    PutLe(&head, r + 0x18, 4, records[i].second.size());
    PutLe(&head, r + 0x1c, 4, 16);
    PutLe(&head, c + 4, 4, at);
    PutLe(&head, c + 8, 4, frames[i].size());
    PutLe(&head, c + 12, 4, records[i].second.size());
    at += frames[i].size();
  }
  std::string set(head.begin(), head.end());
  for (const std::string& f : frames) set += f;
  return set;
}

struct ReadBack {
  bool parsed = false;
  std::map<std::pair<std::uint64_t, std::uint64_t>, Bytes> records;
  std::map<std::pair<std::uint64_t, std::uint64_t>, std::string> frames;
};

// What the registry checks before it mounts a set (Parse, and every frame header states its chunk's size), then each record.
ReadBack Read(const std::string& set) {
  ReadBack out;
  nevr_resource_set::View view;
  if (!nevr_resource_set::Parse(set.data(), set.size(), &view).ok()) return out;
  for (std::uint32_t i = 0; i < view.chunk_count(); ++i) {
    const nevr_resource_set::Chunk c = view.ChunkAt(i);
    if (ZSTD_getFrameContentSize(view.data() + c.offset, c.compressed) != c.decompressed) return out;
  }
  for (std::uint32_t i = 0; i < view.record_count(); ++i) {
    const nevr_resource_set::Record r = view.RecordAt(i);
    const nevr_resource_set::Chunk c = view.ChunkAt(r.chunk);
    Bytes chunk(c.decompressed);
    if (ZSTD_decompress(chunk.data(), chunk.size(), view.data() + c.offset, c.compressed) != c.decompressed) return out;
    out.records[{r.type, r.name}] = Bytes(chunk.begin() + r.offset_in_chunk, chunk.begin() + r.offset_in_chunk + r.size);
    out.frames[{r.type, r.name}] = set.substr(c.offset, c.compressed);
  }
  out.parsed = true;
  return out;
}

void Section(Bytes* image, std::size_t at, std::uint32_t raw_at, std::uint32_t raw_size, std::uint32_t characteristics) {
  PutLe(image, at + 16, 4, raw_size);
  PutLe(image, at + 20, 4, raw_at);
  PutLe(image, at + 36, 4, characteristics);
}

void Mov(Bytes* image, std::size_t site, std::uint8_t rex, std::uint8_t opcode, std::uint64_t imm) {
  (*image)[site] = rex;
  (*image)[site + 1] = opcode;
  PutLe(image, site + 2, 8, imm);
}

// A PE with .text at 0x200 (three movs) and .data at 0x300 (one mov-shaped run that is not code).
Bytes MakeImage() {
  Bytes image(0x400, 0x90);
  image[0] = 'M';
  image[1] = 'Z';
  PutLe(&image, 0x3c, 4, 0x40);
  std::memcpy(&image[0x40], "PE\0\0", 4);
  PutLe(&image, 0x46, 2, 2);
  PutLe(&image, 0x54, 2, 0);
  Section(&image, 0x58, 0x200, 0x100, 0x60000020);
  Section(&image, 0x80, 0x300, 0x100, 0xC0000040);
  Mov(&image, 0x210, 0x48, 0xB8, 0x1000000000000001ULL);
  Mov(&image, 0x230, 0x49, 0xBB, 0x1000000000000002ULL);
  Mov(&image, 0x250, 0x48, 0xBF, 0x1000000000000003ULL);
  Mov(&image, 0x310, 0x48, 0xB8, 0x1000000000000004ULL);
  return image;
}

nevr_maps::RekeyPin GoodPin(const Bytes& image) {
  nevr_maps::RekeyPin pin;
  pin.hash = kMapScript;
  pin.stock_source = kStockScript;
  pin.stock_sha256 = Sha(image);
  pin.edits = {{0x212, 8, kLevel}, {0x232, 8, 0x02}, {0x252, 8, 0xA1}};
  Bytes result = image;
  for (const FieldEdit& e : pin.edits) PutLe(&result, e.at, 8, e.to);
  pin.result_sha256 = Sha(result);
  pin.result_size = result.size();
  return pin;
}

Recipe MakeRecipe(std::uint64_t name, std::uint64_t stock_name, const Bytes& stock, EditKind kind) {
  Recipe r;
  r.type = kType;
  r.name = name;
  r.stock_type = kType;
  r.stock_name = stock_name;
  r.stock_size = stock.size();
  r.stock_sha256 = Sha(stock);
  r.kind = kind;
  return r;
}

// One map: two published records, three recipes over two stock sources (two share one), a recipe member and a rekeyed script.
struct Fixture {
  cargo_test::FakeEnv env;
  NoMapFs fs;
  FakeStock stock;
  nevr_maps::Package package;
  Bytes small = Pattern(4096, 31, 7), large = Pattern(600000, 13, 1), member = Pattern(2048, 5, 0xC0), image = MakeImage();
  std::map<std::uint64_t, Bytes> expected;
  std::string published;

  Fixture() {
    stock.resources[{kType, kStockSmall}] = small;
    stock.resources[{kType, kStockLarge}] = large;
    stock.scripts[kStockScript] = image;
    published = BuildSet({{{kType, 0x01}, Pattern(100, 3, 1)}, {{kType, 0x02}, Pattern(200, 7, 2)}});
    env.fs_->files[kSetPath] = published;
    env.fs_->files[kRecipePath] = std::string(member.begin(), member.end());

    Recipe fields = MakeRecipe(0xA1, kStockSmall, small, EditKind::kFields);
    fields.result_size = small.size();
    fields.fields = {{100, 8, 0x1234}, {200, 2, 0xBEEF}};
    Bytes a = small;
    PutLe(&a, 100, 8, 0x1234);
    PutLe(&a, 200, 2, 0xBEEF);
    fields.result_sha256 = Sha(a);
    expected[0xA1] = a;

    Recipe diff = MakeRecipe(0xB1, kStockSmall, small, EditKind::kDiff);
    diff.diff = {Copy(0, 2048), Insert(0, 512), Copy(2048, 2048)};
    Bytes b(small.begin(), small.begin() + 2048);
    b.insert(b.end(), member.begin(), member.begin() + 512);
    b.insert(b.end(), small.begin() + 2048, small.end());
    diff.result_size = b.size();
    diff.result_sha256 = Sha(b);
    expected[0xB1] = b;

    Recipe big = MakeRecipe(0xC1, kStockLarge, large, EditKind::kDiff);
    big.diff = {Copy(0, large.size()), Insert(512, 1000)};
    Bytes c = large;
    c.insert(c.end(), member.begin() + 512, member.begin() + 1512);
    big.result_size = c.size();
    big.result_sha256 = Sha(c);
    expected[0xC1] = c;

    package.id = "m1";
    package.platform = nevr_maps::kPlatformPc;
    package.level = kLevel;
    package.members = {{"set/windows-x86_64.nevrres", nevr_maps::Role::kSet, published.size(), "", 0, {}},
                       {"recipe/windows-x86_64.bin", nevr_maps::Role::kRecipe, member.size(), "", 0, {}}};
    package.derived = {big, fields, diff};  // out of stock order on purpose
    package.rekeyed = {GoodPin(image)};
  }

  nevr_maps::DeriveOutput Derive(const std::function<bool()>& keep_going = nullptr) {
    nevr_maps::ChannelDeriver deriver(env, fs, stock);
    return deriver.Derive(package, kDir, keep_going);
  }
  bool Logged(const std::string& reason) const {
    for (const std::string& e : env.events) {
      if (e.find("map_derive_refused") != std::string::npos && e.find(reason) != std::string::npos) return true;
    }
    return false;
  }
};

std::string Apply(const Bytes& stock, const Recipe& r, const Bytes& inserts, Bytes* out) {
  return nevr_maps::ApplyRecipe(stock.data(), stock.size(), r, inserts.data(), inserts.size(), out);
}

TEST(MapDeriveVectors, SharedRecipeVectorsGiveTheirResultsOrReasons) {
  std::ifstream file(std::string(NEVR_RECIPE_VECTORS) + "/recipes.json");
  ASSERT_TRUE(file.good());
  const nlohmann::json doc = nlohmann::json::parse(file);
  ASSERT_EQ(doc.at("schema").get<std::string>(), "nevr-maps-recipes");
  int checked = 0;
  for (const nlohmann::json& v : doc.at("vectors")) {
    SCOPED_TRACE(v.at("name").get<std::string>());
    const Bytes stock = FromHex(v.at("stock_hex").get<std::string>());
    const Bytes inserts = FromHex(v.at("inserts_hex").get<std::string>());
    const nlohmann::json& j = v.at("recipe");
    Recipe r;
    r.stock_size = j.at("stock_size").get<std::uint64_t>();
    r.result_size = j.at("result_size").get<std::uint64_t>();
    r.kind = j.at("kind").get<std::string>() == "fields" ? EditKind::kFields : EditKind::kDiff;
    for (const nlohmann::json& f : j.value("fields", nlohmann::json::array())) {
      ASSERT_FALSE(f.contains("from"));
      r.fields.push_back({f.at("at").get<std::uint64_t>(), static_cast<std::uint8_t>(f.at("width").get<int>()),
                          std::stoull(f.at("to").get<std::string>(), nullptr, 16)});
    }
    for (const nlohmann::json& d : j.value("diff", nlohmann::json::array())) {
      r.diff.push_back({d.at("op").get<std::string>() == "insert", d.at("offset").get<std::uint64_t>(),
                        d.at("size").get<std::uint64_t>()});
    }
    Bytes out;
    const std::string reason = Apply(stock, r, inserts, &out);
    if (v.contains("reason")) {
      EXPECT_EQ(reason, v.at("reason").get<std::string>());
    } else {
      EXPECT_EQ(reason, "");
      EXPECT_EQ(out, FromHex(v.at("result_hex").get<std::string>()));
    }
    ++checked;
  }
  EXPECT_EQ(checked, 8);  // FIX4 dropped refuse_from_mismatch: field edits carry no stock `from` bytes
}

TEST(MapDeriveApply, RefusesMalformedRecipes) {
  const Bytes stock = Pattern(256, 1, 0);
  Bytes out;
  Recipe r;
  r.stock_size = 256;
  r.result_size = 256;
  r.fields = {{10, 3, 1}};
  EXPECT_EQ(Apply(stock, r, {}, &out), "recipe_bounds");  // width 3
  r.fields = {{10, 4, 1}, {12, 2, 1}};
  EXPECT_EQ(Apply(stock, r, {}, &out), "recipe_bounds");  // overlapping
  r.fields = {{254, 4, 1}};
  EXPECT_EQ(Apply(stock, r, {}, &out), "recipe_bounds");  // past the end
  r.fields = {{10, 1, 0x100}};
  EXPECT_EQ(Apply(stock, r, {}, &out), "recipe_bounds");  // `to` wider than its field
  r.fields = {{10, 1, 1}};
  r.result_size = 260;
  EXPECT_EQ(Apply(stock, r, {}, &out), "recipe_bounds");  // fields change the size
  r.result_size = 256;
  r.stock_size = 255;
  EXPECT_EQ(Apply(stock, r, {}, &out), "derived_stock_mismatch");
  r.stock_size = 256;
  r.result_size = nevr_maps::kRecipeResultMax + 1;
  EXPECT_EQ(Apply(stock, r, {}, &out), "recipe_cap");

  Recipe d;
  d.kind = EditKind::kDiff;
  d.stock_size = 256;
  d.result_size = 256;
  d.diff = {Copy(200, 100)};
  EXPECT_EQ(Apply(stock, d, {}, &out), "recipe_bounds");  // a copy past the stock
  d.diff = {Copy(0, 200), Insert(0, 56)};
  EXPECT_EQ(Apply(stock, d, Bytes(40), &out), "recipe_bounds");  // an insert past the member
  d.diff.assign(nevr_maps::kRecipeOpsMax + 1, Copy(0, 0));
  EXPECT_EQ(Apply(stock, d, {}, &out), "recipe_cap");
  d.diff = {Copy(0, 256)};
  d.fields = {{0, 1, 1}};
  EXPECT_EQ(Apply(stock, d, {}, &out), "recipe_bounds");  // a diff with field edits
  d.fields.clear();
  EXPECT_EQ(Apply(stock, d, {}, &out), "");
  EXPECT_EQ(out, stock);
}

TEST(MapDeriveRekey, AcceptsImmediatesOfMovsInCodeThatNameTheMap) {
  const Bytes image = MakeImage();
  const std::vector<std::uint64_t> allowed{kLevel, 0x02, 0xA1};
  EXPECT_EQ(nevr_maps::CheckRekeyEdits(image.data(), image.size(), GoodPin(image), allowed), "");
}

TEST(MapDeriveRekey, RefusesEveryOtherSite) {
  const Bytes image = MakeImage();
  const std::vector<std::uint64_t> allowed{kLevel, 0x02, 0xA1};
  const auto check = [&](const FieldEdit& edit) {
    nevr_maps::RekeyPin pin;
    pin.edits = {edit};
    return nevr_maps::CheckRekeyEdits(image.data(), image.size(), pin, allowed);
  };
  EXPECT_EQ(check({0x282, 8, kLevel}), "rekey_site");  // not a mov
  EXPECT_EQ(check({0x312, 8, kLevel}), "rekey_site");  // a mov in .data
  EXPECT_EQ(check({0x212, 8, 0x5555}), "rekey_site");  // `to` names nothing of the map
  EXPECT_EQ(check({0x212, 4, kLevel}), "rekey_site");  // width 4
  EXPECT_EQ(check({0x2fa, 8, kLevel}), "rekey_site");  // the immediate runs out of .text
  nevr_maps::RekeyPin pin = GoodPin(image);
  std::swap(pin.edits[0], pin.edits[1]);
  EXPECT_EQ(nevr_maps::CheckRekeyEdits(image.data(), image.size(), pin, allowed), "rekey_site");  // unsorted
  Bytes not_pe = image;
  not_pe[0] = 'X';
  EXPECT_EQ(nevr_maps::CheckRekeyEdits(not_pe.data(), not_pe.size(), GoodPin(image), allowed), "rekey_site");
}

TEST(MapDerive, RebuildsTheMapIntoAMergedSetTheRegistryReads) {
  Fixture f;
  const nevr_maps::DeriveOutput out = f.Derive();
  ASSERT_EQ(out.reason, "");
  EXPECT_EQ(out.set_path, kMergedPath);
  const std::string& merged = f.env.fs_->files[kMergedPath];
  EXPECT_EQ(out.set_sha256, cargo_test::Sha256Of(merged));

  const ReadBack back = Read(merged);
  ASSERT_TRUE(back.parsed);
  EXPECT_EQ(back.records.size(), 5u);
  for (const auto& e : f.expected) EXPECT_EQ(back.records.at({kType, e.first}), e.second) << e.first;
  EXPECT_EQ(back.records.at({kType, 0x01}), Pattern(100, 3, 1));
  EXPECT_EQ(back.records.at({kType, 0x02}), Pattern(200, 7, 2));
  const ReadBack original = Read(f.published);
  ASSERT_TRUE(original.parsed);
  EXPECT_EQ(back.frames.at({kType, 0x01}), original.frames.at({kType, 0x01}));  // published frames copied byte for byte
  EXPECT_EQ(back.frames.at({kType, 0xC1}).size(), nevr_maps::RawFrameSize(f.expected[0xC1].size()));

  EXPECT_EQ((f.stock.reads[{kType, kStockSmall}]), 1);  // two recipes, one read
  EXPECT_EQ((f.stock.reads[{kType, kStockLarge}]), 1);

  ASSERT_EQ(out.scripts.size(), 1u);
  EXPECT_EQ(out.scripts[0].first, kMapScript);
  EXPECT_EQ(out.scripts[0].second, kDir + "/scripts/99990000000000cd.dll");
  const std::string& dll = f.env.fs_->files[out.scripts[0].second];
  EXPECT_EQ(cargo_test::Sha256Of(dll), f.package.rekeyed[0].result_sha256);

  // One stamp per file: the manifest both resources came through, two packages, the script.
  std::set<std::string> paths;
  for (const auto& s : out.stock_stamps) EXPECT_TRUE(paths.insert(s.path).second) << s.path;
  EXPECT_EQ(paths.size(), 4u);
  nevr_maps::ChannelDeriver deriver(f.env, f.fs, f.stock);
  EXPECT_TRUE(deriver.StockCurrent(out.stock_stamps));
  f.stock.current["stock/package_" + std::to_string(kStockLarge)].last_write += 1;
  EXPECT_FALSE(deriver.StockCurrent(out.stock_stamps));
}

TEST(MapDerive, APackageWithoutRecipesWritesNothing) {
  Fixture f;
  f.package.derived.clear();
  f.package.rekeyed.clear();
  const nevr_maps::DeriveOutput out = f.Derive();
  EXPECT_EQ(out.reason, "");
  EXPECT_TRUE(out.set_path.empty());
  EXPECT_TRUE(out.stock_stamps.empty());
  EXPECT_EQ(f.env.fs_->files.count(kMergedPath), 0u);
}

void ExpectRefused(Fixture& f, const std::string& reason) {
  const nevr_maps::DeriveOutput out = f.Derive();
  EXPECT_EQ(out.reason, reason);
  EXPECT_TRUE(out.set_path.empty());
  EXPECT_TRUE(out.scripts.empty());
  EXPECT_EQ(f.env.fs_->files.count(kMergedPath), 0u);
  for (const auto& file : f.env.fs_->files) EXPECT_EQ(file.first.find(".tmp"), std::string::npos) << file.first;
  if (reason != "cancelled") {
    EXPECT_TRUE(f.Logged(reason));
  }
}

TEST(MapDeriveRefusal, StockMissing) {
  Fixture f;
  f.stock.resources.erase({kType, kStockLarge});
  ExpectRefused(f, "derived_stock_missing");
}

TEST(MapDeriveRefusal, StockMismatch) {
  Fixture f;
  f.stock.resources[{kType, kStockSmall}][5] ^= 1;
  ExpectRefused(f, "derived_stock_mismatch");
}

// Edits carry no `from`, so the stock sha256 alone stops a recipe applying to stock bytes that differ under its edits.
TEST(MapDeriveRefusal, StockMismatchUnderAFieldEdit) {
  Fixture f;
  f.stock.resources[{kType, kStockSmall}][100] ^= 1;
  ExpectRefused(f, "derived_stock_mismatch");
}

TEST(MapDeriveRefusal, StockScriptMismatchUnderARekeyEdit) {
  Fixture f;
  f.stock.scripts[kStockScript][0x212] ^= 1;
  ExpectRefused(f, "derived_stock_mismatch");
}

TEST(MapDeriveRefusal, ResultMismatch) {
  Fixture f;
  f.package.derived[1].result_sha256 = std::string(64, '0');
  ExpectRefused(f, "recipe_result");
}

TEST(MapDeriveRefusal, MapInsertCapCountsInsertsAndFieldBytes) {
  Fixture f;
  Recipe huge = f.package.derived[0];
  huge.name = 0xD1;
  huge.diff = {Insert(0, nevr_maps::kRecipeMapInsertMax - 1000)};
  f.package.derived.push_back(huge);
  ExpectRefused(f, "recipe_cap");  // the fixture's own 1,512 insert bytes and 10 field bytes tip it over
  EXPECT_EQ(f.stock.reads.size(), 0u);
}

TEST(MapDeriveRefusal, DerivedKeyThePublishedSetHolds) {
  Fixture f;
  // derived[2] (0xB1) is no rekey target, so the rekeyed script still rebuilds and the merge meets the duplicate.
  f.package.derived[2].name = 0x02;
  ExpectRefused(f, "derived_dup");
}

TEST(MapDeriveRefusal, RekeyToOutsideTheMap) {
  Fixture f;
  f.package.rekeyed[0].edits[1].to = 0x5555;
  ExpectRefused(f, "rekey_site");
}

TEST(MapDeriveRefusal, StockScriptMismatch) {
  Fixture f;
  f.stock.scripts[kStockScript][0x100] ^= 1;
  ExpectRefused(f, "derived_stock_mismatch");
}

TEST(MapDeriveRefusal, CancelledMidRebuildLeavesNoFile) {
  Fixture f;
  int calls = 0;
  const nevr_maps::DeriveOutput out = f.Derive([&] { return ++calls < 3; });
  EXPECT_EQ(out.reason, "cancelled");
  EXPECT_EQ(f.env.fs_->files.count(kMergedPath), 0u);
  for (const auto& file : f.env.fs_->files) EXPECT_EQ(file.first.find(".tmp"), std::string::npos) << file.first;
}

TEST(MapDeriveMergedSet, OverFourGiBIsRefusedBeforeAnyByteIsProduced) {
  cargo_test::FakeEnv env;
  NoMapFs fs;
  std::vector<Recipe> derived(33);
  for (std::size_t i = 0; i < derived.size(); ++i) {
    derived[i].type = kType;
    derived[i].name = i + 1;
    derived[i].result_size = nevr_maps::kRecipeResultMax;
  }
  int produced = 0;
  std::string sha;
  const std::string reason = nevr_maps::WriteMergedSet(
      env, fs, "", derived, [&](std::size_t, Bytes*) { ++produced; return std::string(); }, kMergedPath, &sha);
  EXPECT_EQ(reason, "file_offset");
  EXPECT_EQ(produced, 0);
}

TEST(MapDeriveMergedSet, RawFramesDecodeAtEveryBlockBoundary) {
  for (const std::size_t size : {std::size_t{64}, std::size_t{131072}, std::size_t{131073}, std::size_t{262144 + 5}}) {
    cargo_test::FakeEnv env;
    NoMapFs fs;
    Recipe r;
    r.type = kType;
    r.name = 1;
    r.result_size = size;
    const Bytes bytes = Pattern(size, 17, 3);
    std::string sha;
    ASSERT_EQ(nevr_maps::WriteMergedSet(env, fs, "", {r}, [&](std::size_t, Bytes* out) { *out = bytes; return std::string(); },
                                        kMergedPath, &sha),
              "");
    const ReadBack back = Read(env.fs_->files[kMergedPath]);
    ASSERT_TRUE(back.parsed) << size;
    EXPECT_EQ(back.records.at({kType, 1}), bytes) << size;
  }
}

}  // namespace
