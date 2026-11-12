#include "core/map_derive.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <utility>

namespace nevr_maps {
namespace {

// sdk/include/nevr/resource_set.h: the set layout the registry parses.
constexpr char kSetMagic[8] = {'N', 'E', 'V', 'R', 'R', 'E', 'S', '1'};
constexpr std::uint64_t kSetHeaderBytes = 48, kSetRecordBytes = 0x20, kSetChunkBytes = 0x10;
constexpr std::uint32_t kDerivedAlignment = 0x1000;
constexpr std::uint64_t kU32Limit = 0xFFFFFFFFull;

// RFC 8878: the frame magic, then a descriptor with a 4-byte content size and a window descriptor (no single segment), the
// window of 128 KiB (exponent 7), and raw blocks of at most 128 KiB, so any zstd decoder reads it without a large window.
constexpr std::uint8_t kRawFrameHeader[6] = {0x28, 0xB5, 0x2F, 0xFD, 0x80, 0x38};
constexpr std::uint64_t kRawFrameHeaderBytes = 10, kRawBlockHeaderBytes = 3, kRawBlockMax = 128 * 1024;

constexpr std::size_t kStreamBuffer = 1u << 20;
constexpr std::uint32_t kSectionExecutable = 0x20000000;  // IMAGE_SCN_MEM_EXECUTE

bool Inside(std::uint64_t offset, std::uint64_t length, std::uint64_t total) {
  return offset <= total && length <= total - offset;
}

std::uint64_t LoadLe(const std::uint8_t* p, unsigned width) {
  std::uint64_t value = 0;
  for (unsigned i = 0; i < width; ++i) value |= std::uint64_t{p[i]} << (8 * i);
  return value;
}

void StoreLe(std::uint8_t* p, unsigned width, std::uint64_t value) {
  for (unsigned i = 0; i < width; ++i) p[i] = static_cast<std::uint8_t>(value >> (8 * i));
}

bool FitsWidth(std::uint64_t value, unsigned width) { return width == 8 || (value >> (8 * width)) == 0; }

std::string CheckResultSize(std::uint64_t result_size) {
  if (result_size < kStockBytesMinSize) return "recipe_tiny";
  if (result_size > kRecipeResultMax) return "recipe_cap";
  return "";
}

std::uint64_t InsertCap(std::uint64_t result_size) {
  return std::min(result_size / kRecipeInsertDivisor, kRecipeInsertMax);
}

// Every byte a field edit writes is authored, so it counts as an INSERT byte against the recipe's cap (AUDIT4 D1).
std::string CheckFields(std::size_t size, const std::vector<FieldEdit>& fields, std::uint64_t insert_cap) {
  if (fields.size() > kRecipeOpsMax) return "recipe_cap";
  std::uint64_t end = 0, written = 0;
  for (const FieldEdit& edit : fields) {
    const unsigned width = edit.width;
    if (width != 1 && width != 2 && width != 4 && width != 8) return "recipe_bounds";
    if (edit.at < end || !Inside(edit.at, width, size)) return "recipe_bounds";
    if (!FitsWidth(edit.to, width)) return "recipe_bounds";
    written += width;
    if (written > insert_cap) return "recipe_cap";
    end = edit.at + width;
  }
  return "";
}

struct Range {
  std::uint64_t begin = 0, end = 0;
};

// The file ranges of the PE's executable sections; empty when `image` is not a PE.
std::vector<Range> ExecutableSections(const std::uint8_t* image, std::size_t size) {
  std::vector<Range> ranges;
  if (size < 0x40 || image[0] != 'M' || image[1] != 'Z') return ranges;
  const std::uint64_t pe = LoadLe(image + 0x3c, 4);
  if (!Inside(pe, 24, size) || std::memcmp(image + pe, "PE\0\0", 4) != 0) return ranges;
  const std::uint64_t count = LoadLe(image + pe + 6, 2);
  const std::uint64_t table = pe + 24 + LoadLe(image + pe + 20, 2);
  if (!Inside(table, count * 40, size)) return ranges;
  for (std::uint64_t i = 0; i < count; ++i) {
    const std::uint8_t* section = image + table + i * 40;
    const std::uint64_t raw_size = LoadLe(section + 16, 4);
    const std::uint64_t raw_at = LoadLe(section + 20, 4);
    if ((LoadLe(section + 36, 4) & kSectionExecutable) == 0 || raw_at >= size) continue;
    ranges.push_back({raw_at, std::min<std::uint64_t>(raw_at + raw_size, size)});
  }
  return ranges;
}

std::string Hex16(std::uint64_t value) {
  char text[17];
  std::snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(value));
  return text;
}

std::string KeyText(std::uint64_t type, std::uint64_t name) { return "0x" + Hex16(type) + ":0x" + Hex16(name); }

std::string Join(const std::string& dir, const std::string& leaf) {
  if (!dir.empty() && (dir.back() == '/' || dir.back() == '\\')) return dir + leaf;
  return dir + "/" + leaf;
}

std::string ParentOf(const std::string& path) {
  const std::size_t cut = path.find_last_of("/\\");
  return cut == std::string::npos ? std::string() : path.substr(0, cut);
}

class Blob {
 public:
  explicit Blob(nevr_cargo::Env& env) : env_(env) {}
  Blob(const Blob&) = delete;
  Blob& operator=(const Blob&) = delete;
  ~Blob() {
    if (open_) env_.CloseBlob(handle_);
  }
  bool Open(const std::string& path) {
    open_ = env_.OpenBlob(path, &handle_, &size_);
    return open_;
  }
  std::uint64_t size() const { return size_; }
  bool Read(std::uint64_t offset, void* buffer, std::size_t length) {
    auto* out = static_cast<std::uint8_t*>(buffer);
    while (length > 0) {
      std::size_t got = 0;
      if (!env_.ReadBlob(handle_, offset, out, length, &got) || got == 0 || got > length) return false;
      out += got;
      offset += got;
      length -= got;
    }
    return true;
  }

 private:
  nevr_cargo::Env& env_;
  std::int64_t handle_ = -1;
  std::uint64_t size_ = 0;
  bool open_ = false;
};

struct SetRecord {
  std::uint64_t type = 0, name = 0;
  std::uint32_t chunk = 0, offset = 0, size = 0, alignment = 0;
};
struct SetChunk {
  std::uint32_t offset = 0, compressed = 0, decompressed = 0;
};
struct SetTables {
  std::vector<SetRecord> records;
  std::vector<SetChunk> chunks;
};

// The published set's tables, bounds-checked against the file. Frames are not read: the mount parses the merged set.
std::string ReadSetTables(Blob& set, SetTables* out) {
  std::uint8_t head[kSetHeaderBytes];
  if (set.size() < kSetHeaderBytes) return "set_format";
  if (!set.Read(0, head, sizeof(head))) return "derive_io";
  if (std::memcmp(head, kSetMagic, sizeof(kSetMagic)) != 0 || LoadLe(head + 8, 4) != kSetHeaderBytes) return "set_format";
  const std::uint64_t records = LoadLe(head + 0x0c, 4), chunks = LoadLe(head + 0x10, 4);
  const std::uint64_t record_table = LoadLe(head + 0x18, 8), chunk_table = LoadLe(head + 0x20, 8);
  // A map's set only adds resources (the registry refuses its overrides), so a merged set has none to carry.
  if (records == 0 || LoadLe(head + 0x14, 4) != 0) return "set_format";
  if (!Inside(record_table, records * kSetRecordBytes, set.size()) ||
      !Inside(chunk_table, chunks * kSetChunkBytes, set.size())) {
    return "set_format";
  }
  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(chunks * kSetChunkBytes));
  if (!bytes.empty() && !set.Read(chunk_table, bytes.data(), bytes.size())) return "derive_io";
  out->chunks.resize(static_cast<std::size_t>(chunks));
  for (std::size_t i = 0; i < out->chunks.size(); ++i) {
    const std::uint8_t* at = bytes.data() + i * kSetChunkBytes;
    SetChunk& chunk = out->chunks[i];
    chunk.offset = static_cast<std::uint32_t>(LoadLe(at + 4, 4));
    chunk.compressed = static_cast<std::uint32_t>(LoadLe(at + 8, 4));
    chunk.decompressed = static_cast<std::uint32_t>(LoadLe(at + 12, 4));
    if (LoadLe(at, 4) != 0 || chunk.compressed < 4 || !Inside(chunk.offset, chunk.compressed, set.size())) return "set_format";
  }
  bytes.resize(static_cast<std::size_t>(records * kSetRecordBytes));
  if (!set.Read(record_table, bytes.data(), bytes.size())) return "derive_io";
  out->records.resize(static_cast<std::size_t>(records));
  for (std::size_t i = 0; i < out->records.size(); ++i) {
    const std::uint8_t* at = bytes.data() + i * kSetRecordBytes;
    SetRecord& record = out->records[i];
    record.type = LoadLe(at, 8);
    record.name = LoadLe(at + 8, 8);
    record.chunk = static_cast<std::uint32_t>(LoadLe(at + 0x10, 4));
    record.offset = static_cast<std::uint32_t>(LoadLe(at + 0x14, 4));
    record.size = static_cast<std::uint32_t>(LoadLe(at + 0x18, 4));
    record.alignment = static_cast<std::uint32_t>(LoadLe(at + 0x1c, 4));
    if (record.chunk >= chunks) return "set_format";
  }
  return "";
}

bool KeyLess(const SetRecord& a, const SetRecord& b) { return a.type != b.type ? a.type < b.type : a.name < b.name; }

}  // namespace

std::uint64_t RawFrameSize(std::uint64_t content_size) {
  const std::uint64_t blocks = content_size == 0 ? 1 : (content_size + kRawBlockMax - 1) / kRawBlockMax;
  return kRawFrameHeaderBytes + blocks * kRawBlockHeaderBytes + content_size;
}

std::string ApplyRecipe(const std::uint8_t* stock, std::size_t stock_size, const Recipe& recipe, const std::uint8_t* inserts,
                        std::size_t inserts_size, std::vector<std::uint8_t>* out) {
  out->clear();
  std::string reason = CheckResultSize(recipe.result_size);
  if (!reason.empty()) return reason;
  if (stock_size != recipe.stock_size) return "derived_stock_mismatch";

  if (recipe.kind == EditKind::kFields) {
    if (!recipe.diff.empty() || recipe.result_size != stock_size) return "recipe_bounds";
    reason = CheckFields(stock_size, recipe.fields, InsertCap(recipe.result_size));
    if (!reason.empty()) return reason;
    out->assign(stock, stock + stock_size);
    for (const FieldEdit& edit : recipe.fields) StoreLe(out->data() + edit.at, edit.width, edit.to);
    return "";
  }

  if (!recipe.fields.empty()) return "recipe_bounds";
  if (recipe.diff.size() > kRecipeOpsMax) return "recipe_cap";
  std::uint64_t total = 0, inserted = 0;
  for (const DiffOp& op : recipe.diff) {
    if (!Inside(op.offset, op.size, op.insert ? inserts_size : stock_size)) return "recipe_bounds";
    if (op.size > recipe.result_size - total) return "recipe_bounds";
    total += op.size;
    if (op.insert) inserted += op.size;
  }
  if (total != recipe.result_size) return "recipe_bounds";
  if (inserted > InsertCap(recipe.result_size)) return "recipe_cap";
  out->reserve(static_cast<std::size_t>(total));
  for (const DiffOp& op : recipe.diff) {
    const std::uint8_t* from = (op.insert ? inserts : stock) + op.offset;
    out->insert(out->end(), from, from + op.size);
  }
  return "";
}

std::string CheckRekeyEdits(const std::uint8_t* image, std::size_t size, const RekeyPin& pin,
                            const std::vector<std::uint64_t>& allowed_targets) {
  if (pin.edits.empty()) return "";
  if (pin.edits.size() > kRecipeOpsMax) return "recipe_cap";
  const std::vector<Range> executable = ExecutableSections(image, size);
  std::vector<std::uint64_t> allowed(allowed_targets);
  std::sort(allowed.begin(), allowed.end());
  std::uint64_t end = 0;
  for (const FieldEdit& edit : pin.edits) {
    // The opcode bytes of one site may not lie in the immediate of the one before: editing it would change them.
    if (edit.width != 8 || edit.at < end + 2) return "rekey_site";
    const std::uint64_t site = edit.at - 2;
    const bool in_code = std::any_of(executable.begin(), executable.end(), [&](const Range& r) {
      return site >= r.begin && Inside(site, 10, r.end);
    });
    if (!in_code) return "rekey_site";
    const std::uint8_t rex = image[site], opcode = image[site + 1];
    if ((rex != 0x48 && rex != 0x49) || opcode < 0xB8 || opcode > 0xBF) return "rekey_site";
    if (!std::binary_search(allowed.begin(), allowed.end(), edit.to)) return "rekey_site";
    end = edit.at + 8;
  }
  return "";
}

std::string WriteMergedSet(nevr_cargo::Env& env, MapFs&, const std::string& published_set,
                           const std::vector<Recipe>& derived,
                           const std::function<std::string(std::size_t, std::vector<std::uint8_t>*)>& produce,
                           const std::string& out_path, std::string* sha256_out) {
  sha256_out->clear();
  SetTables published;
  Blob source(env);
  if (!published_set.empty()) {
    if (!source.Open(published_set)) return "derive_io";
    const std::string reason = ReadSetTables(source, &published);
    if (!reason.empty()) return reason;
  }
  for (const Recipe& recipe : derived) {
    const std::string reason = CheckResultSize(recipe.result_size);
    if (!reason.empty()) return reason;
  }
  const std::uint64_t record_count = published.records.size() + derived.size();
  const std::uint64_t chunk_count = published.chunks.size() + derived.size();
  if (record_count == 0) return "set_format";
  if (record_count > kU32Limit || chunk_count > kU32Limit) return "file_offset";

  std::vector<SetRecord> records(published.records);
  records.reserve(static_cast<std::size_t>(record_count));
  for (std::size_t i = 0; i < derived.size(); ++i) {
    SetRecord record;
    record.type = derived[i].type;
    record.name = derived[i].name;
    record.chunk = static_cast<std::uint32_t>(published.chunks.size() + i);
    record.size = static_cast<std::uint32_t>(derived[i].result_size);
    record.alignment = kDerivedAlignment;
    records.push_back(record);
  }
  std::sort(records.begin(), records.end(), KeyLess);
  for (std::size_t i = 1; i < records.size(); ++i) {
    if (records[i - 1].type == records[i].type && records[i - 1].name == records[i].name) return "derived_dup";
  }

  const std::uint64_t record_table = kSetHeaderBytes;
  const std::uint64_t chunk_table = record_table + record_count * kSetRecordBytes;
  const std::uint64_t tables_end = chunk_table + chunk_count * kSetChunkBytes;
  std::vector<SetChunk> chunks;
  chunks.reserve(static_cast<std::size_t>(chunk_count));
  std::uint64_t at = tables_end;
  for (const SetChunk& chunk : published.chunks) {
    chunks.push_back({static_cast<std::uint32_t>(at), chunk.compressed, chunk.decompressed});
    at += chunk.compressed;
    if (at > kU32Limit) return "file_offset";
  }
  for (const Recipe& recipe : derived) {
    const std::uint64_t frame = RawFrameSize(recipe.result_size);
    chunks.push_back({static_cast<std::uint32_t>(at), static_cast<std::uint32_t>(frame),
                      static_cast<std::uint32_t>(recipe.result_size)});
    at += frame;
    if (at > kU32Limit) return "file_offset";
  }

  std::vector<std::uint8_t> head(static_cast<std::size_t>(tables_end), 0);
  std::memcpy(head.data(), kSetMagic, sizeof(kSetMagic));
  StoreLe(head.data() + 0x08, 4, kSetHeaderBytes);
  StoreLe(head.data() + 0x0c, 4, record_count);
  StoreLe(head.data() + 0x10, 4, chunk_count);
  StoreLe(head.data() + 0x18, 8, record_table);
  StoreLe(head.data() + 0x20, 8, chunk_table);
  StoreLe(head.data() + 0x28, 8, tables_end);
  for (std::size_t i = 0; i < records.size(); ++i) {
    std::uint8_t* p = head.data() + record_table + i * kSetRecordBytes;
    StoreLe(p, 8, records[i].type);
    StoreLe(p + 8, 8, records[i].name);
    StoreLe(p + 0x10, 4, records[i].chunk);
    StoreLe(p + 0x14, 4, records[i].offset);
    StoreLe(p + 0x18, 4, records[i].size);
    StoreLe(p + 0x1c, 4, records[i].alignment);
  }
  for (std::size_t i = 0; i < chunks.size(); ++i) {
    std::uint8_t* p = head.data() + chunk_table + i * kSetChunkBytes;
    StoreLe(p + 4, 4, chunks[i].offset);
    StoreLe(p + 8, 4, chunks[i].compressed);
    StoreLe(p + 12, 4, chunks[i].decompressed);
  }

  std::unique_ptr<nevr_cargo::TempFile> temp = env.CreateTemp(ParentOf(out_path));
  if (!temp) return "derive_io";
  std::unique_ptr<nevr_cargo::Hasher> hasher = env.NewHasher();
  const auto put = [&](const void* data, std::size_t length) {
    hasher->Update(data, length);
    return temp->Write(data, length);
  };
  const auto fail = [&](std::string reason) {
    temp->Abort();
    return reason;
  };

  if (!put(head.data(), head.size())) return fail("derive_io");
  std::vector<std::uint8_t>().swap(head);

  std::vector<std::uint8_t> buffer(kStreamBuffer);
  for (const SetChunk& chunk : published.chunks) {
    std::uint64_t offset = chunk.offset, left = chunk.compressed;
    while (left > 0) {
      const std::size_t n = static_cast<std::size_t>(std::min<std::uint64_t>(left, buffer.size()));
      if (!source.Read(offset, buffer.data(), n) || !put(buffer.data(), n)) return fail("derive_io");
      offset += n;
      left -= n;
    }
  }

  for (std::size_t i = 0; i < derived.size(); ++i) {
    std::vector<std::uint8_t> bytes;
    const std::string reason = produce(i, &bytes);
    if (!reason.empty()) return fail(reason);
    if (bytes.size() != derived[i].result_size) return fail("recipe_result");
    std::uint8_t frame_header[kRawFrameHeaderBytes];
    std::memcpy(frame_header, kRawFrameHeader, sizeof(kRawFrameHeader));
    StoreLe(frame_header + sizeof(kRawFrameHeader), 4, bytes.size());
    if (!put(frame_header, sizeof(frame_header))) return fail("derive_io");
    std::size_t done = 0;
    do {
      const std::size_t length = static_cast<std::size_t>(std::min<std::uint64_t>(bytes.size() - done, kRawBlockMax));
      const bool last = done + length == bytes.size();
      std::uint8_t block_header[kRawBlockHeaderBytes];
      StoreLe(block_header, 3, (std::uint64_t{length} << 3) | (last ? 1u : 0u));  // block type 0: raw
      if (!put(block_header, sizeof(block_header)) || !put(bytes.data() + done, length)) return fail("derive_io");
      done += length;
    } while (done < bytes.size());
  }

  if (!temp->Commit(out_path)) return "derive_io";
  *sha256_out = hasher->Hex();
  return "";
}

namespace {

// One Derive call: the per-fetch stock cache, the stamps read, and the first refusal.
class DeriveRun {
 public:
  DeriveRun(nevr_cargo::Env& env, MapFs& fs, StockSource& stock, const Package& package, const std::string& version_dir,
            const std::function<bool()>& keep_going)
      : env_(env), fs_(fs), stock_(stock), package_(package), version_dir_(version_dir), keep_going_(keep_going),
        recipe_member_(env) {}

  std::string Run(DeriveOutput* out) {
    const bool rekeys = std::any_of(package_.rekeyed.begin(), package_.rekeyed.end(),
                                    [](const RekeyPin& pin) { return !pin.edits.empty(); });
    if (package_.derived.empty() && !rekeys) return "";

    // INSERT payloads and field-edit bytes are both authored bytes under the per-map cap (AUDIT4 D1).
    std::uint64_t inserted = 0;
    for (const Recipe& recipe : package_.derived) {
      const auto add = [&](std::uint64_t bytes) {
        if (bytes > kRecipeMapInsertMax - inserted) return false;
        inserted += bytes;
        return true;
      };
      bool within = true;
      for (const DiffOp& op : recipe.diff) within = within && (!op.insert || add(op.size));
      for (const FieldEdit& edit : recipe.fields) within = within && add(edit.width);
      if (!within) return Refuse("recipe_cap", KeyText(recipe.type, recipe.name));
    }

    std::string set_path, recipe_path;
    for (const Member& member : package_.members) {
      if (member.role == Role::kSet && set_path.empty()) set_path = Join(version_dir_, member.path);
      if (member.role == Role::kRecipe && recipe_path.empty()) recipe_path = Join(version_dir_, member.path);
    }

    if (rekeys) {
      std::vector<std::uint64_t> allowed{package_.level};
      for (const Recipe& recipe : package_.derived) allowed.push_back(recipe.name);
      if (!set_path.empty()) {
        Blob set(env_);
        if (!set.Open(set_path)) return Refuse("derive_io", "");
        SetTables tables;
        const std::string reason = ReadSetTables(set, &tables);
        if (!reason.empty()) return Refuse(reason, "");
        for (const SetRecord& record : tables.records) allowed.push_back(record.name);
      }
      const std::string reason = RebuildScripts(allowed, out);
      if (!reason.empty()) return reason;
    }

    if (package_.derived.empty()) return "";
    if (!recipe_path.empty()) {
      if (!recipe_member_.Open(recipe_path)) return Refuse("derive_io", "");
      recipe_size_ = recipe_member_.size();
    }
    // Recipes that share a stock source sit together, so each source is read once.
    std::vector<Recipe> ordered(package_.derived);
    std::stable_sort(ordered.begin(), ordered.end(), [](const Recipe& a, const Recipe& b) {
      return a.stock_type != b.stock_type ? a.stock_type < b.stock_type : a.stock_name < b.stock_name;
    });
    const std::string set_dir = Join(version_dir_, "set");
    if (!env_.MakeDirs(set_dir)) return Refuse("derive_io", "");
    const std::string out_path = Join(set_dir, package_.platform + ".mounted.nevrres");
    std::string sha256;
    const std::string reason = WriteMergedSet(
        env_, fs_, set_path, ordered,
        [&](std::size_t i, std::vector<std::uint8_t>* bytes) { return Produce(ordered[i], bytes); }, out_path, &sha256);
    std::vector<std::uint8_t>().swap(stock_bytes_);
    if (!reason.empty()) return Refuse(reason, "");
    out->set_path = out_path;
    out->set_sha256 = sha256;
    return "";
  }

  std::vector<StockStamp> TakeStamps() { return std::move(stamps_); }

 private:
  bool Stopped() const { return keep_going_ && !keep_going_(); }

  std::string Hash(const std::vector<std::uint8_t>& bytes) {
    std::unique_ptr<nevr_cargo::Hasher> hasher = env_.NewHasher();
    hasher->Update(bytes.data(), bytes.size());
    return hasher->Hex();
  }

  // Logs the first refusal with what it names; a later return of the same reason passes through.
  std::string Refuse(const std::string& reason, const std::string& key) {
    if (!refused_ && reason != "cancelled") {
      refused_ = true;
      env_.Event(nevr_cargo::EventLevel::kWarn, "map_derive_refused",
                 {nevr_cargo::Field("id", package_.id), nevr_cargo::Field("reason", reason),
                  nevr_cargo::Field("key", key)});
    }
    return reason;
  }

  void Note(const std::vector<StockStamp>& read_from) {
    for (const StockStamp& stamp : read_from) {
      const bool known = std::any_of(stamps_.begin(), stamps_.end(),
                                     [&](const StockStamp& s) { return s.path == stamp.path; });
      if (!known) stamps_.push_back(stamp);
    }
  }

  std::string RebuildScripts(const std::vector<std::uint64_t>& allowed, DeriveOutput* out) {
    const std::string scripts_dir = Join(version_dir_, "scripts");
    for (const RekeyPin& pin : package_.rekeyed) {
      if (pin.edits.empty()) continue;
      if (Stopped()) return "cancelled";
      const std::string key = "script 0x" + Hex16(pin.hash);
      std::vector<std::uint8_t> image;
      std::vector<StockStamp> read_from;
      const bool found = stock_.ReadScript(pin.stock_source, &image, &read_from);
      Note(read_from);
      if (!found) return Refuse("derived_stock_missing", key);
      if (Hash(image) != pin.stock_sha256) return Refuse("derived_stock_mismatch", key);
      if (pin.result_size != image.size()) return Refuse("recipe_bounds", key);
      const std::string reason = CheckRekeyEdits(image.data(), image.size(), pin, allowed);
      if (!reason.empty()) return Refuse(reason, key);
      for (const FieldEdit& edit : pin.edits) StoreLe(image.data() + edit.at, 8, edit.to);
      if (Hash(image) != pin.result_sha256) return Refuse("recipe_result", key);

      if (!env_.MakeDirs(scripts_dir)) return Refuse("derive_io", key);
      const std::string path = Join(scripts_dir, Hex16(pin.hash) + ".dll");
      std::unique_ptr<nevr_cargo::TempFile> temp = env_.CreateTemp(scripts_dir);
      if (!temp || !temp->Write(image.data(), image.size()) || !temp->Commit(path)) {
        if (temp) temp->Abort();
        return Refuse("derive_io", key);
      }
      out->scripts.emplace_back(pin.hash, path);
    }
    return "";
  }

  std::string LoadStock(const Recipe& recipe) {
    if (!stock_loaded_ || stock_type_ != recipe.stock_type || stock_name_ != recipe.stock_name) {
      stock_loaded_ = false;
      std::vector<std::uint8_t>().swap(stock_bytes_);
      std::vector<StockStamp> read_from;
      const bool found = stock_.ReadResource(recipe.stock_type, recipe.stock_name, &stock_bytes_, &read_from);
      Note(read_from);
      if (!found) return "derived_stock_missing";
      stock_sha256_ = Hash(stock_bytes_);
      stock_type_ = recipe.stock_type;
      stock_name_ = recipe.stock_name;
      stock_loaded_ = true;
    }
    if (stock_bytes_.size() != recipe.stock_size || stock_sha256_ != recipe.stock_sha256) return "derived_stock_mismatch";
    return "";
  }

  // The recipe's insert ranges read from the recipe member into one buffer, and a copy of the recipe that indexes it. Reading
  // only these ranges keeps the member (up to 64 MiB) out of memory.
  std::string ReadInserts(const Recipe& recipe, Recipe* rebased, std::vector<std::uint8_t>* inserts) {
    *rebased = recipe;
    const std::uint64_t cap = InsertCap(recipe.result_size);
    std::uint64_t total = 0;
    for (const DiffOp& op : rebased->diff) {
      if (!op.insert) continue;
      if (!Inside(op.offset, op.size, recipe_size_)) return "recipe_bounds";
      if (op.size > cap - total) return "recipe_cap";
      total += op.size;
    }
    inserts->resize(static_cast<std::size_t>(total));
    std::uint64_t at = 0;
    for (DiffOp& op : rebased->diff) {
      if (!op.insert) continue;
      if (op.size > 0 && !recipe_member_.Read(op.offset, inserts->data() + at, static_cast<std::size_t>(op.size))) {
        return "derive_io";
      }
      op.offset = at;
      at += op.size;
    }
    return "";
  }

  std::string Produce(const Recipe& recipe, std::vector<std::uint8_t>* bytes) {
    if (Stopped()) return "cancelled";
    const std::string key = KeyText(recipe.type, recipe.name);
    std::string reason = CheckResultSize(recipe.result_size);
    if (reason.empty()) reason = LoadStock(recipe);
    if (!reason.empty()) return Refuse(reason, key);
    Recipe rebased;
    std::vector<std::uint8_t> inserts;
    if (recipe.kind == EditKind::kDiff) {
      reason = ReadInserts(recipe, &rebased, &inserts);
      if (!reason.empty()) return Refuse(reason, key);
    }
    const Recipe& apply = recipe.kind == EditKind::kDiff ? rebased : recipe;
    reason = ApplyRecipe(stock_bytes_.data(), stock_bytes_.size(), apply, inserts.data(), inserts.size(), bytes);
    if (!reason.empty()) return Refuse(reason, key);
    if (Hash(*bytes) != recipe.result_sha256) return Refuse("recipe_result", key);
    return "";
  }

  nevr_cargo::Env& env_;
  MapFs& fs_;
  StockSource& stock_;
  const Package& package_;
  const std::string& version_dir_;
  const std::function<bool()>& keep_going_;
  Blob recipe_member_;
  std::uint64_t recipe_size_ = 0;
  std::vector<StockStamp> stamps_;
  bool refused_ = false;
  bool stock_loaded_ = false;
  std::uint64_t stock_type_ = 0, stock_name_ = 0;
  std::vector<std::uint8_t> stock_bytes_;
  std::string stock_sha256_;
};

}  // namespace

ChannelDeriver::ChannelDeriver(nevr_cargo::Env& env, MapFs& fs, StockSource& stock) : env_(env), fs_(fs), stock_(stock) {}

DeriveOutput ChannelDeriver::Derive(const Package& package, const std::string& version_dir,
                                    const std::function<bool()>& keep_going) {
  DeriveOutput out;
  DeriveRun run(env_, fs_, stock_, package, version_dir, keep_going);
  out.reason = run.Run(&out);
  out.stock_stamps = run.TakeStamps();
  if (!out.reason.empty()) {
    out.set_path.clear();
    out.set_sha256.clear();
    out.scripts.clear();
  }
  return out;
}

bool ChannelDeriver::StockCurrent(const std::vector<StockStamp>& stamps) { return stock_.StampsCurrent(stamps); }

}  // namespace nevr_maps
