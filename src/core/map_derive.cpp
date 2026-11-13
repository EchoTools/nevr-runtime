#include "core/map_derive.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <utility>

#include "core/cargo_limits.h"
#include "core/device_memory.h"
#include "core/map_format.h"
#include "core/map_set_tables.h"
#include "core/script_image_rules.h"
#include "nevr/resource_set.h"

namespace nevr_maps {
namespace {

namespace set = nevr_resource_set;

// A derived record is aligned to the largest alignment a set may ask for (WriteMergedSet).
constexpr std::uint32_t kDerivedAlignment = set::kMaxAlignment;
// Every chunk's file offset and size is a u32 word of the set (nevr/resource_set.h), as the store's are (cargo_limits.h).
constexpr std::uint64_t kU32Limit = nevr_cargo::kFormatMaxStoreBytes;

// RFC 8878 section 3.1.1: the frame magic, then a descriptor with a 4-byte content size and a window descriptor (no single
// segment), the window of 128 KiB (exponent 7), and raw blocks of at most 128 KiB, so any zstd decoder reads it without a large
// window. The content size follows as 4 little-endian bytes.
constexpr std::uint8_t kRawFrameHeader[] = {0x28, 0xB5, 0x2F, 0xFD, 0x80, 0x38};
constexpr std::uint64_t kRawFrameContentSizeBytes = 4;
constexpr std::uint64_t kRawFrameHeaderBytes = sizeof(kRawFrameHeader) + kRawFrameContentSizeBytes;
// A block header (RFC 8878 section 3.1.1.2) is 3 bytes: bit 0 last block, bits 1-2 the type (0, raw), bits 3-23 the size.
constexpr std::uint64_t kRawBlockHeaderBytes = 3;
constexpr unsigned kRawBlockSizeShift = 3;
constexpr std::uint64_t kRawBlockLastFlag = 1;
constexpr std::uint64_t kRawBlockMax = std::uint64_t{128} * 1024;

// The copy step of a published chunk into the merged set: at most this many bytes a read, and never more than the memory
// share allows.
constexpr std::uint64_t kCopyStepBytes = std::uint64_t{1} << 20;

// A rekey site is `mov r64, imm64`: a REX.W prefix (0x48, or 0x49 with REX.B), an opcode 0xB8 + the register, then the
// immediate the edit replaces (map_format.h kRekeyOpcodeBytes, kRekeyEditWidth).
constexpr std::uint8_t kRexW = 0x48;
constexpr std::uint8_t kRexWB = 0x49;
constexpr std::uint8_t kMovImmFirst = 0xB8;
constexpr std::uint8_t kMovImmLast = 0xBF;
constexpr unsigned kBitsPerByte = 8;
constexpr unsigned kHex16Digits = 16;

bool Inside(std::uint64_t offset, std::uint64_t length, std::uint64_t total) {
  return offset <= total && length <= total - offset;
}

void StoreLe(std::uint8_t* p, std::uint64_t width, std::uint64_t value) {
  for (std::uint64_t i = 0; i < width; ++i) p[i] = static_cast<std::uint8_t>(value >> (kBitsPerByte * i));
}

bool FitsWidth(std::uint64_t value, unsigned width) {
  return width == sizeof(std::uint64_t) || (value >> (kBitsPerByte * width)) == 0;
}

bool IsFieldWidth(std::uint64_t width) {
  return std::find(std::begin(kFieldWidths), std::end(kFieldWidths), width) != std::end(kFieldWidths);
}

std::string CheckResultSize(std::uint64_t result_size) {
  if (result_size < kStockBytesMinSize) return "recipe_tiny";
  if (result_size > kRecipeResultMax) return "recipe_cap";
  return "";
}

std::uint64_t InsertCap(std::uint64_t result_size) {
  return std::min(result_size / kRecipeInsertDivisor, kRecipeInsertMax);
}

// Every byte a field edit writes is authored, so it counts as an inserted byte against the recipe's cap.
std::string CheckFields(std::size_t size, const std::vector<FieldEdit>& fields, std::uint64_t insert_cap) {
  if (fields.size() > kRecipeOpsMax) return "recipe_cap";
  std::uint64_t end = 0, written = 0;
  for (const FieldEdit& edit : fields) {
    const unsigned width = edit.width;
    if (!IsFieldWidth(width)) return "recipe_bounds";
    if (edit.at < end || !Inside(edit.at, width, size)) return "recipe_bounds";
    if (!FitsWidth(edit.to, width)) return "recipe_bounds";
    written += width;
    if (written > insert_cap) return "recipe_cap";
    end = edit.at + width;
  }
  return "";
}

std::string Hex16(std::uint64_t value) {
  char text[kHex16Digits + 1];
  std::snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(value));
  return text;
}

std::string KeyText(std::uint64_t type, std::uint64_t name) { return "0x" + Hex16(type) + ":0x" + Hex16(name); }

std::string Join(const std::string& dir, const std::string& leaf) {
  if (!dir.empty() && (dir.back() == '/' || dir.back() == '\\')) return dir + leaf;
  return dir + "/" + leaf;
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
  std::int64_t handle() const { return handle_; }
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

// The published set's tables, through the one set reader (core/map_set_tables.h). A map's set only adds resources (the registry
// refuses its overrides), so a set that carries overrides has none a merged set could keep, and is refused.
std::string ReadPublishedTables(nevr_cargo::Env& env, Blob& set, std::uint64_t table_bytes, SetTables* out) {
  const SetProblem problem = ReadSetTables(env, set.handle(), set.size(), table_bytes, out);
  if (problem.reason == "read") return "derive_io";
  if (problem.reason == "no_memory") return "no_memory";
  if (!problem.reason.empty() || out->override_count != 0) return "set_format";
  return "";
}

bool KeyLess(const set::Record& a, const set::Record& b) { return a.type != b.type ? a.type < b.type : a.name < b.name; }

}  // namespace

DeriveMemory DeriveMemoryOf(const nevr_cargo::DeviceFacts& facts) noexcept {
  DeriveMemory memory;
  if (!facts.have_memory) return memory;
  memory.object_bytes = nevr_device::Share(facts.memory_available, nevr_cargo::kMemoryShareForObject);
  memory.table_bytes = nevr_device::Share(facts.memory_available, nevr_cargo::kMemoryShareForEntries);
  return memory;
}

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
  const std::vector<nevr_script_image::FileRange> executable = nevr_script_image::ExecutableSectionFileRanges(image, size);
  std::vector<std::uint64_t> allowed(allowed_targets);
  std::sort(allowed.begin(), allowed.end());
  std::uint64_t end = 0;
  for (const FieldEdit& edit : pin.edits) {
    // The opcode bytes of one site may not lie in the immediate of the one before: editing it would change them.
    if (edit.width != kRekeyEditWidth || edit.at < end + kRekeyOpcodeBytes) return "rekey_site";
    const std::uint64_t site = edit.at - kRekeyOpcodeBytes;
    const bool in_code = std::any_of(executable.begin(), executable.end(), [&](const nevr_script_image::FileRange& r) {
      return site >= r.begin && Inside(site, kRekeyOpcodeBytes + kRekeyEditWidth, r.end);
    });
    if (!in_code) return "rekey_site";
    const std::uint8_t rex = image[site], opcode = image[site + 1];
    if ((rex != kRexW && rex != kRexWB) || opcode < kMovImmFirst || opcode > kMovImmLast) return "rekey_site";
    if (!std::binary_search(allowed.begin(), allowed.end(), edit.to)) return "rekey_site";
    end = edit.at + kRekeyEditWidth;
  }
  return "";
}

std::string WriteMergedSet(nevr_cargo::Env& env, const std::string& published_set, const std::vector<Recipe>& derived,
                           const std::function<std::string(std::size_t, std::vector<std::uint8_t>*)>& produce,
                           const std::string& out_path, const DeriveMemory& memory, std::string* sha256_out) {
  sha256_out->clear();
  SetTables published;
  Blob source(env);
  if (!published_set.empty()) {
    if (!source.Open(published_set)) return "derive_io";
    const std::string reason = ReadPublishedTables(env, source, memory.table_bytes, &published);
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

  const std::uint64_t tables_end = set::kHeaderBytes + record_count * set::kRecordBytes + chunk_count * set::kChunkBytes;
  // The merged tables, in parsed form and as the bytes written, are held at once.
  if (SetTableMemoryBytes(record_count, chunk_count) + tables_end > memory.table_bytes) return "no_memory";

  std::vector<set::Record> records(published.records);
  records.reserve(static_cast<std::size_t>(record_count));
  for (std::size_t i = 0; i < derived.size(); ++i) {
    set::Record record;
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

  std::vector<set::Chunk> chunks;
  chunks.reserve(static_cast<std::size_t>(chunk_count));
  std::uint64_t at = tables_end;
  for (const set::Chunk& chunk : published.chunks) {
    set::Chunk moved;
    moved.offset = static_cast<std::uint32_t>(at);
    moved.compressed = chunk.compressed;
    moved.decompressed = chunk.decompressed;
    chunks.push_back(moved);
    at += chunk.compressed;
    if (at > kU32Limit) return "file_offset";
  }
  for (const Recipe& recipe : derived) {
    const std::uint64_t frame = RawFrameSize(recipe.result_size);
    set::Chunk built;
    built.offset = static_cast<std::uint32_t>(at);
    built.compressed = static_cast<std::uint32_t>(frame);
    built.decompressed = static_cast<std::uint32_t>(recipe.result_size);
    chunks.push_back(built);
    at += frame;
    if (at > kU32Limit) return "file_offset";
  }

  std::vector<std::uint8_t> head = SetHeaderAndTables(records, chunks);

  std::unique_ptr<nevr_cargo::AppendFile> file = env.OpenAppend(out_path);
  if (!file || !file->Truncate(0)) return "derive_io";
  std::unique_ptr<nevr_cargo::Hasher> hasher = env.NewHasher();
  const auto put = [&](const void* data, std::size_t length) {
    hasher->Update(data, length);
    return file->Append(data, length);
  };
  const auto fail = [&](std::string reason) {
    file->Truncate(0);
    return reason;
  };

  if (!put(head.data(), head.size())) return fail("derive_io");
  std::vector<std::uint8_t>().swap(head);

  std::vector<std::uint8_t> buffer(static_cast<std::size_t>(std::max<std::uint64_t>(1, std::min(kCopyStepBytes, memory.object_bytes))));
  for (const set::Chunk& chunk : published.chunks) {
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
    StoreLe(frame_header + sizeof(kRawFrameHeader), kRawFrameContentSizeBytes, bytes.size());
    if (!put(frame_header, sizeof(frame_header))) return fail("derive_io");
    std::size_t done = 0;
    do {
      const std::size_t length = static_cast<std::size_t>(std::min<std::uint64_t>(bytes.size() - done, kRawBlockMax));
      const bool last = done + length == bytes.size();
      std::uint8_t block_header[kRawBlockHeaderBytes];
      StoreLe(block_header, kRawBlockHeaderBytes, (std::uint64_t{length} << kRawBlockSizeShift) | (last ? kRawBlockLastFlag : 0));
      if (!put(block_header, sizeof(block_header)) || !put(bytes.data() + done, length)) return fail("derive_io");
      done += length;
    } while (done < bytes.size());
  }

  if (!file->Sync()) return fail("derive_io");
  *sha256_out = hasher->Hex();
  return "";
}

namespace {

// One Derive call: the per-fetch stock cache, the stamps read, and the first refusal.
class DeriveRun {
 public:
  DeriveRun(nevr_cargo::Env& env, StockSource& stock, const Package& package, const std::string& out_dir,
            const std::string& version_dir, const DeriveMemory& memory, const std::function<bool()>& keep_going)
      : env_(env), stock_(stock), package_(package), out_dir_(out_dir), version_dir_(version_dir), memory_(memory),
        keep_going_(keep_going), recipe_member_(env) {}

  // Writes the outputs under `out_dir_`; their paths in `out` are relative to it.
  std::string Run(DeriveOutput* out) {
    const bool rekeys = std::any_of(package_.rekeyed.begin(), package_.rekeyed.end(),
                                    [](const RekeyPin& pin) { return !pin.edits.empty(); });
    if (memory_.object_bytes == 0 || memory_.table_bytes == 0) return Refuse("no_memory_fact", "");

    // Inserted payloads and field-edit bytes are both authored bytes under the per-map cap.
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
        Blob set_file(env_);
        if (!set_file.Open(set_path)) return Refuse("derive_io", "");
        SetTables tables;
        const std::string reason = ReadPublishedTables(env_, set_file, memory_.table_bytes, &tables);
        if (!reason.empty()) return Refuse(reason, "");
        for (const set::Record& record : tables.records) allowed.push_back(record.name);
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
    const std::string set_name = package_.platform + kDerivedSetSuffix;
    std::string sha256;
    const std::string reason = WriteMergedSet(
        env_, set_path, ordered,
        [&](std::size_t i, std::vector<std::uint8_t>* bytes) { return Produce(ordered[i], bytes); },
        Join(out_dir_, set_name), memory_, &sha256);
    std::vector<std::uint8_t>().swap(stock_bytes_);
    if (!reason.empty()) return Refuse(reason, "");
    out->set_path = set_name;
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
                 {nevr_cargo::Field("id", package_.id), nevr_cargo::Field("reason", reason), nevr_cargo::Field("key", key),
                  nevr_cargo::Field("consequence", "the map is not stored; it is tried again at the next fetch")});
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
    const std::string scripts_dir = Join(out_dir_, kDerivedScriptsDir);
    for (const RekeyPin& pin : package_.rekeyed) {
      if (pin.edits.empty()) continue;
      if (Stopped()) return "cancelled";
      const std::string key = "script 0x" + Hex16(pin.hash);
      // The stock image and its edited copy are the same size (checked below), so one image is what this holds.
      if (pin.result_size > memory_.object_bytes) return Refuse("no_memory", key);
      std::vector<std::uint8_t> image;
      std::vector<StockStamp> read_from;
      const bool found = stock_.ReadScript(pin.stock_source, &image, &read_from);
      Note(read_from);
      if (!found) return Refuse("derived_stock_missing", key);
      if (Hash(image) != pin.stock_sha256) return Refuse("derived_stock_mismatch", key);
      if (pin.result_size != image.size()) return Refuse("recipe_bounds", key);
      const std::string reason = CheckRekeyEdits(image.data(), image.size(), pin, allowed);
      if (!reason.empty()) return Refuse(reason, key);
      for (const FieldEdit& edit : pin.edits) StoreLe(image.data() + edit.at, kRekeyEditWidth, edit.to);
      if (Hash(image) != pin.result_sha256) return Refuse("recipe_result", key);

      if (!env_.MakeDirs(scripts_dir)) return Refuse("derive_io", key);
      const std::string name = std::string(kDerivedScriptsDir) + "/" + Hex16(pin.hash) + kDerivedScriptSuffix;
      if (!env_.WriteFileAtomically(Join(out_dir_, name), std::string(image.begin(), image.end()))) {
        return Refuse("derive_io", key);
      }
      out->scripts.emplace_back(pin.hash, name);
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
  // only these ranges keeps the member out of memory.
  std::string ReadInserts(const Recipe& recipe, Recipe* rebased, std::vector<std::uint8_t>* inserts) {
    *rebased = recipe;
    std::uint64_t total = 0;
    for (const DiffOp& op : rebased->diff) {
      if (!op.insert) continue;
      if (!Inside(op.offset, op.size, recipe_size_)) return "recipe_bounds";
      if (op.size > InsertCap(recipe.result_size) - total) return "recipe_cap";
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

  std::uint64_t InsertedBytes(const Recipe& recipe) const {
    std::uint64_t total = 0;
    for (const DiffOp& op : recipe.diff) total += op.insert ? op.size : 0;
    return total;
  }

  std::string Produce(const Recipe& recipe, std::vector<std::uint8_t>* bytes) {
    if (Stopped()) return "cancelled";
    const std::string key = KeyText(recipe.type, recipe.name);
    std::string reason = CheckResultSize(recipe.result_size);
    if (!reason.empty()) return Refuse(reason, key);
    // The stock bytes, the authored bytes and the result are held at once; the recipe's sizes say how many before anything is
    // read, and the device's share bounds them together.
    const std::uint64_t inserted = std::min(InsertedBytes(recipe), InsertCap(recipe.result_size));
    if (recipe.stock_size > memory_.object_bytes || recipe.result_size > memory_.object_bytes - recipe.stock_size ||
        inserted > memory_.object_bytes - recipe.stock_size - recipe.result_size) {
      return Refuse("no_memory", key);
    }
    reason = LoadStock(recipe);
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
  StockSource& stock_;
  const Package& package_;
  const std::string& out_dir_;
  const std::string& version_dir_;
  const DeriveMemory memory_;
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
  const bool rekeys = std::any_of(package.rekeyed.begin(), package.rekeyed.end(),
                                  [](const RekeyPin& pin) { return !pin.edits.empty(); });
  if (package.derived.empty() && !rekeys) return out;  // nothing to rebuild: nothing is written
  const std::string final_dir = Join(version_dir, kDerivedDir);
  const std::string staging = final_dir + kStagingSuffix;
  // A staging directory a stopped rebuild left behind is not used: the rebuild starts again from the stock files.
  if (!fs_.RemoveTree(staging) || !env_.MakeDirs(staging)) {
    out.reason = "derive_io";
    return out;
  }
  nevr_cargo::DeviceFacts facts;
  env_.QueryDevice(version_dir, &facts);
  DeriveRun run(env_, stock_, package, staging, version_dir, DeriveMemoryOf(facts), keep_going);
  out.reason = run.Run(&out);
  out.stock_stamps = run.TakeStamps();
  // Every output verified: the staging directory takes the outputs' name, replacing a rebuild of an earlier stock.
  if (out.reason.empty() && (!fs_.RemoveTree(final_dir) || !fs_.RenameDir(staging, final_dir))) out.reason = "derive_io";
  if (!out.reason.empty()) {
    fs_.RemoveTree(staging);
    out.set_path.clear();
    out.set_sha256.clear();
    out.scripts.clear();
    return out;
  }
  if (!out.set_path.empty()) out.set_path = Join(final_dir, out.set_path);
  for (auto& script : out.scripts) script.second = Join(final_dir, script.second);
  return out;
}

bool ChannelDeriver::StockCurrent(const std::vector<StockStamp>& stamps) { return stock_.StampsCurrent(stamps); }

}  // namespace nevr_maps
