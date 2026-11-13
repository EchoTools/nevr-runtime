#include "core/cargo_store.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <set>
#include <utility>

namespace nevr_cargo {

namespace {

constexpr std::size_t kReadChunk = 64 * 1024;  // Verify's read buffer: a size for the stack of one call, not a limit on any object

void PutU32(unsigned char* at, std::uint32_t v) noexcept {
  for (std::size_t i = 0; i < kU32Bytes; ++i) at[i] = static_cast<unsigned char>((v >> (kBitsPerByte * i)) & kByteMask);
}
std::uint32_t GetU32(const unsigned char* at) noexcept {
  std::uint32_t v = 0;
  for (std::size_t i = 0; i < kU32Bytes; ++i) v |= static_cast<std::uint32_t>(at[i]) << (kBitsPerByte * i);
  return v;
}

std::string HexOfBytes(const unsigned char* bytes, std::size_t n) {
  std::string out(n * kHexCharsPerByte, '0');
  for (std::size_t i = 0; i < n; ++i) {
    out[kHexCharsPerByte * i] = kHexDigits[bytes[i] >> kHexDigitBits];
    out[kHexCharsPerByte * i + 1] = kHexDigits[bytes[i] & kHexDigitMask];
  }
  return out;
}

bool BytesOfHex(const std::string& hex, unsigned char* out) noexcept {
  if (hex.size() != kSha256HexChars) return false;
  for (std::size_t i = 0; i < kSha256DigestBytes; ++i) {
    unsigned high = 0, low = 0;
    if (!HexDigitValue(hex[kHexCharsPerByte * i], &high) || !HexDigitValue(hex[kHexCharsPerByte * i + 1], &low)) return false;
    out[i] = static_cast<unsigned char>((high << kHexDigitBits) | low);
  }
  return true;
}

void IndexHeader(unsigned char* out) noexcept {
  std::memcpy(out, kIndexMagic, kIndexMagicBytes);
  PutU32(out + kIndexVersionAt, kIndexVersion);
  PutU32(out + kIndexRecordSizeAt, static_cast<std::uint32_t>(kIndexRecordBytes));
}

}  // namespace

std::string FormatHex64(std::uint64_t v) {
  std::string out = "0x";
  bool started = false;
  for (unsigned shift = static_cast<unsigned>(kSymbolBits - kHexDigitBits);; shift -= kHexDigitBits) {
    const unsigned d = static_cast<unsigned>((v >> shift) & kHexDigitMask);
    if (d != 0 || started || shift == 0) {
      out.push_back(kHexDigits[d]);
      started = true;
    }
    if (shift == 0) break;
  }
  return out;
}

std::string StoreFilePath(const std::string& dir, std::uint32_t file) { return dir + "/store-" + std::to_string(file) + ".pack"; }
std::string IndexPath(const std::string& dir) { return dir + "/blobs.idx"; }
std::string ManifestCopyPath(const std::string& dir) { return dir + "/manifest.json.zst"; }

std::uint32_t Crc32(const void* data, std::size_t length) noexcept {
  static const struct Table {
    std::uint32_t v[kCrc32TableEntries];
    Table() {
      for (std::uint32_t i = 0; i < kCrc32TableEntries; ++i) {
        std::uint32_t c = i;
        for (unsigned k = 0; k < kBitsPerByte; ++k) c = (c & 1) != 0 ? kCrc32Polynomial ^ (c >> 1) : c >> 1;
        v[i] = c;
      }
    }
  } table;
  std::uint32_t crc = kCrc32Init;
  const unsigned char* p = static_cast<const unsigned char*>(data);
  for (std::size_t i = 0; i < length; ++i) crc = table.v[(crc ^ p[i]) & kByteMask] ^ (crc >> kBitsPerByte);
  return crc ^ kCrc32Init;
}

// ── the store ────────────────────────────────────────────────────────────────────────────────────────────────────────────

std::unique_ptr<Store> Store::Open(Env& env, const std::string& dir, const Limits& limits, StoreOpenInfo* info) {
  *info = StoreOpenInfo();
  if (!limits.ok) return nullptr;
  std::unique_ptr<Store> store(new Store(env, dir, limits));

  struct Record {
    std::string sha;
    BlobLoc loc;
  };
  std::vector<Record> records;
  std::string bytes;
  std::uint64_t goodLength = 0;  // the bytes of the index worth keeping: the header and the whole records that passed their crc
  bool cut = false;
  if (env.ReadSmallFile(IndexPath(dir), &bytes, static_cast<std::size_t>(limits.index_bytes))) {
    const unsigned char* b = reinterpret_cast<const unsigned char*>(bytes.data());
    const bool headerOk = bytes.size() >= kIndexHeaderBytes && std::memcmp(b, kIndexMagic, kIndexMagicBytes) == 0 &&
                          GetU32(b + kIndexVersionAt) == kIndexVersion && GetU32(b + kIndexRecordSizeAt) == kIndexRecordBytes;
    if (!headerOk) {
      info->index = "invalid";
      cut = true;
    } else {
      info->index = "ok";
      std::size_t off = kIndexHeaderBytes;
      while (off + kIndexRecordBytes <= bytes.size()) {
        const unsigned char* r = b + off;
        if (Crc32(r, kIndexCrcAt) != GetU32(r + kIndexCrcAt)) break;
        Record rec;
        rec.sha = HexOfBytes(r + kIndexShaAt, kSha256DigestBytes);
        rec.loc.file = GetU32(r + kIndexFileAt);
        rec.loc.offset = GetU32(r + kIndexOffsetAt);
        rec.loc.size = GetU32(r + kIndexSizeAt);
        rec.loc.bytes = GetU32(r + kIndexBytesAt);
        records.push_back(std::move(rec));
        off += kIndexRecordBytes;
      }
      goodLength = off;
      if (off != bytes.size()) {
        info->index = "tail_cut";
        cut = true;
      }
    }
  }
  if (cut) {
    // Whatever follows the last record that can be trusted is cut off, so the next record is appended where the good ones end.
    std::unique_ptr<AppendFile> idx = env.OpenAppend(IndexPath(dir));
    if (idx != nullptr && idx->Truncate(goodLength)) store->index_ = std::move(idx);
  }

  // Measure the store files the records name, once each. Nothing is read from them.
  std::map<std::uint32_t, std::uint64_t> lengths;  // file -> length, absent when it cannot be opened
  std::set<std::uint32_t> tried;
  for (const Record& r : records) {
    if (r.loc.file >= limits.max_store_files || !tried.insert(r.loc.file).second) continue;
    std::int64_t handle = 0;
    std::uint64_t size = 0;
    if (env.OpenBlob(StoreFilePath(dir, r.loc.file), &handle, &size)) {
      lengths[r.loc.file] = size;
      env.CloseBlob(handle);
    }
  }
  for (const Record& r : records) {
    const auto it = lengths.find(r.loc.file);
    const std::uint64_t end = static_cast<std::uint64_t>(r.loc.offset) + r.loc.size;
    if (it == lengths.end() || end > it->second || r.loc.size == 0) {
      ++info->dropped;
      continue;
    }
    if (!store->blobs_.emplace(r.sha, r.loc).second) continue;  // a name the index holds twice is held once
    if (store->files_.size() <= r.loc.file) store->files_.resize(r.loc.file + 1);
    store->files_[r.loc.file].end = std::max(store->files_[r.loc.file].end, end);
    store->held_bytes_ += r.loc.size;
  }
  for (const auto& kv : lengths) {
    if (store->files_.size() <= kv.first) store->files_.resize(kv.first + 1);
    store->files_[kv.first].length = kv.second;
  }
  info->blobs = store->blobs_.size();
  info->files = store->files_.size();
  info->bytes = store->held_bytes_;
  return store;
}

class StoreWriter : public Store::Writer {
 public:
  StoreWriter(Store* store, std::uint32_t file, std::uint64_t start, std::uint64_t size)
      : store_(store), file_(file), start_(start), size_(size) {}
  ~StoreWriter() override {
    if (!done_) Abort();
  }

  bool Write(const void* data, std::size_t length) override {
    if (done_ || written_ + length > size_) return false;
    if (!store_->current_->Append(data, length)) return false;
    written_ += length;
    return true;
  }

  bool Commit(const std::string& sha256, std::uint64_t bytes) override {
    if (done_) return false;
    unsigned char record[kIndexRecordBytes];
    if (written_ != size_ || bytes == 0 || bytes > kFormatMaxObjectBytes || !BytesOfHex(sha256, record)) {
      Abort();
      return false;
    }
    if (store_->Has(sha256)) {  // the store already holds this object: its bytes are given back
      Abort();
      return true;
    }
    PutU32(record + kIndexFileAt, file_);
    PutU32(record + kIndexOffsetAt, static_cast<std::uint32_t>(start_));
    PutU32(record + kIndexSizeAt, static_cast<std::uint32_t>(size_));
    PutU32(record + kIndexBytesAt, static_cast<std::uint32_t>(bytes));
    PutU32(record + kIndexCrcAt, Crc32(record, kIndexCrcAt));
    // The object's bytes are durable before the record that names them is written.
    if (!store_->current_->Sync()) {
      Abort();
      return false;
    }
    if (store_->index_ == nullptr) store_->index_ = store_->env_.OpenAppend(IndexPath(store_->dir_));
    AppendFile* idx = store_->index_.get();
    if (idx == nullptr) {
      Abort();
      return false;
    }
    const std::uint64_t before = idx->Size();
    bool ok = true;
    if (before == 0) {
      unsigned char header[kIndexHeaderBytes];
      IndexHeader(header);
      ok = idx->Append(header, sizeof(header));
    }
    ok = ok && idx->Append(record, sizeof(record)) && idx->Sync();
    if (!ok) {
      idx->Truncate(before);  // a record cut in the middle would end the index there at the next boot: give it back now
      Abort();
      return false;
    }
    BlobLoc loc;
    loc.file = file_;
    loc.offset = static_cast<std::uint32_t>(start_);
    loc.size = static_cast<std::uint32_t>(size_);
    loc.bytes = static_cast<std::uint32_t>(bytes);
    store_->blobs_.emplace(sha256, loc);
    store_->files_[file_].end = start_ + size_;
    store_->files_[file_].length = start_ + size_;
    store_->held_bytes_ += size_;
    done_ = true;
    store_->writer_live_ = false;
    return true;
  }

  void Abort() override {
    if (done_) return;
    done_ = true;
    store_->writer_live_ = false;
    if (store_->current_ != nullptr && store_->current_->Size() > start_) store_->current_->Truncate(start_);
  }

 private:
  Store* store_;
  std::uint32_t file_;
  std::uint64_t start_;
  std::uint64_t size_;
  std::uint64_t written_ = 0;
  bool done_ = false;
};

std::unique_ptr<Store::Writer> Store::Begin(std::uint64_t size, const char** reason) {
  if (writer_live_) {
    *reason = "writer_live";
    return nullptr;
  }
  if (size == 0 || size > limits_.store_file_bytes || size > kFormatMaxStoreBytes) {
    *reason = "too_large";
    return nullptr;
  }
  std::uint32_t file = files_.empty() ? 0 : static_cast<std::uint32_t>(files_.size() - 1);
  if (!files_.empty() && files_[file].end + size > limits_.store_file_bytes) ++file;
  if (file >= limits_.max_store_files) {
    *reason = "no_store_file";
    return nullptr;
  }
  if (current_ == nullptr || current_file_ != file) {
    std::unique_ptr<AppendFile> f = env_.OpenAppend(StoreFilePath(dir_, file));
    if (f == nullptr) {
      *reason = "open_failed";
      return nullptr;
    }
    current_ = std::move(f);
    current_file_ = file;
  }
  if (files_.size() <= file) files_.resize(file + 1);
  // Bytes no index record names (an object whose record was never written, or a transfer that was cut) are given back.
  if (current_->Size() > files_[file].end && !current_->Truncate(files_[file].end)) {
    *reason = "open_failed";
    return nullptr;
  }
  if (current_->Size() < files_[file].end) {  // the file is shorter than what the index says it holds
    *reason = "open_failed";
    return nullptr;
  }
  writer_live_ = true;
  return std::unique_ptr<Writer>(new StoreWriter(this, file, files_[file].end, size));
}

std::size_t Store::Verify(std::size_t* checked) {
  *checked = 0;
  std::size_t bad = 0;
  std::map<std::uint32_t, std::int64_t> handles;
  std::string buffer(kReadChunk, '\0');
  for (const auto& kv : blobs_) {
    const BlobLoc& loc = kv.second;
    auto h = handles.find(loc.file);
    if (h == handles.end()) {
      std::int64_t handle = -1;
      std::uint64_t size = 0;
      if (!env_.OpenBlob(StoreFilePath(dir_, loc.file), &handle, &size)) handle = -1;
      h = handles.emplace(loc.file, handle).first;
    }
    ++*checked;
    if (h->second < 0) {
      ++bad;
      continue;
    }
    std::unique_ptr<Hasher> hasher = env_.NewHasher();
    std::uint64_t at = 0;
    bool ok = hasher != nullptr;
    while (ok && at < loc.size) {
      const std::size_t want = static_cast<std::size_t>(std::min<std::uint64_t>(buffer.size(), loc.size - at));
      std::size_t got = 0;
      if (!env_.ReadBlob(h->second, loc.offset + at, &buffer[0], want, &got) || got != want) {
        ok = false;
        break;
      }
      hasher->Update(buffer.data(), got);
      at += got;
    }
    if (!ok || hasher->Hex() != kv.first) ++bad;
  }
  for (const auto& kv : handles) {
    if (kv.second >= 0) env_.CloseBlob(kv.second);
  }
  return bad;
}

// ── the mount ────────────────────────────────────────────────────────────────────────────────────────────────────────────

Mounted Mount(Env& env, const Store& store, const Manifest& manifest, const Platform& platform, const Policy& policy) {
  Mounted result;
  // Group by (label, name): a name's on-disk records mount together or not at all.
  std::map<std::pair<std::string, std::uint64_t>, std::vector<const Entry*>> groups;
  for (const Entry& e : manifest.entries) {
    if (e.platform != platform.name || StateOf(e.label, policy) == LabelState::kOff) continue;
    groups[std::make_pair(e.label, e.name)].push_back(&e);
  }
  nevr_shadow::Input input;
  std::map<std::string, std::uint32_t> labelIndex;
  std::map<std::uint32_t, std::uint32_t> packageOfFile;  // store file -> its index in input.handles
  std::set<std::uint32_t> failedFiles;
  std::map<std::string, std::uint32_t> chunkOfBlob;
  for (const auto& group : groups) {
    // The group mounts when every record's blob is held, its sizes are the ones the store holds and its file opens.
    bool missing = false, mismatch = false, unopened = false;
    for (const Entry* e : group.second) {
      const BlobLoc* loc = store.Find(e->sha256);
      if (loc == nullptr) {
        missing = true;
        break;
      }
      if (loc->size != e->size || loc->bytes != e->bytes) {
        mismatch = true;
        break;
      }
      if (failedFiles.count(loc->file) != 0) unopened = true;
    }
    if (!missing && !mismatch && !unopened) {
      for (const Entry* e : group.second) {
        const BlobLoc* loc = store.Find(e->sha256);
        if (packageOfFile.count(loc->file) != 0) continue;
        std::int64_t handle = 0;
        std::uint64_t size = 0;
        if (!env.OpenBlob(StoreFilePath(store.dir(), loc->file), &handle, &size)) {
          failedFiles.insert(loc->file);
          unopened = true;
          env.Event(EventLevel::kWarn, "cargo_mount",
                    {{"result", "refused"}, {"file", loc->file}, {"reason", "open_failed"},
                     {"consequence", "the names in this store file are served from the game's own data"}});
          break;
        }
        packageOfFile[loc->file] = static_cast<std::uint32_t>(input.handles.size());
        input.handles.push_back(handle);
      }
    }
    if (missing) {
      ++result.missing;
      continue;
    }
    if (mismatch || unopened) {
      ++result.refused;
      continue;
    }
    const auto it = labelIndex.emplace(group.first.first, static_cast<std::uint32_t>(result.labels.size()));
    if (it.second) result.labels.push_back(group.first.first);
    for (const Entry* e : group.second) {
      const BlobLoc* loc = store.Find(e->sha256);
      auto chunk = chunkOfBlob.find(e->sha256);
      if (chunk == chunkOfBlob.end()) {
        nevr_shadow::Chunk c;
        c.package = packageOfFile[loc->file];
        c.file_offset = loc->offset;
        c.compressed = loc->size;
        c.decompressed = loc->bytes;
        chunk = chunkOfBlob.emplace(e->sha256, static_cast<std::uint32_t>(input.chunks.size())).first;
        input.chunks.push_back(c);
      }
      nevr_shadow::Record rec;
      rec.type = e->type;
      rec.name = e->name;
      rec.chunk = chunk->second;
      rec.offset_in_chunk = 0;
      rec.size = static_cast<std::uint32_t>(e->bytes);
      rec.alignment = e->align;
      rec.user = it.first->second;
      input.records.push_back(rec);
      ++result.records;
    }
    ++result.names;
  }
  // The records of 0 bytes name the shadow's first chunk (see the header): a chunk that exists, which the engine's reader reads as it
  // reads any record's chunk before it copies out the 0 bytes.
  if (!manifest.empties.empty()) {
    if (input.chunks.empty()) {
      result.refused += manifest.empties.size();
      env.Event(EventLevel::kWarn, "cargo_mount",
                {{"result", "refused"}, {"reason", "no_chunk"}, {"empty_records", manifest.empties.size()},
                 {"consequence", "no record of 0 bytes is served: there is no object in this set to name a chunk through"}});
    } else {
      for (const EmptyRecord& e : manifest.empties) {
        nevr_shadow::Record rec;
        rec.type = e.type;
        rec.name = e.name;
        rec.chunk = 0;
        rec.offset_in_chunk = 0;
        rec.size = 0;
        rec.alignment = e.align;
        rec.user = 0;
        input.records.push_back(rec);
        ++result.records;
        ++result.empty_records;
      }
    }
  }
  if (input.records.empty()) {
    for (const std::int64_t h : input.handles) env.CloseBlob(h);
    return result;
  }
  const nevr_shadow::Status status = nevr_shadow::Shadow::Build(input, &result.shadow);
  if (status != nevr_shadow::Status::kOk) {
    for (const std::int64_t h : input.handles) env.CloseBlob(h);
    env.Event(EventLevel::kWarn, "cargo_mount",
              {{"result", "refused"}, {"reason", nevr_shadow::StatusName(status)},
               {"consequence", "nothing is served from this set; the game's own data shows"}});
    result.refused += result.names + result.empty_records;
    result.names = 0;
    result.records = 0;
    result.empty_records = 0;
    result.labels.clear();
    return result;
  }
  result.files = input.handles.size();
  return result;
}

}  // namespace nevr_cargo
