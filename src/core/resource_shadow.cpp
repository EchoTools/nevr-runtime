#include "core/resource_shadow.h"

#include <algorithm>
#include <atomic>
#include <cstring>

namespace nevr_shadow {

namespace {

template <typename T>
T LoadAt(const unsigned char* base, std::size_t at) noexcept {
  T v{};
  std::memcpy(&v, base + at, sizeof(v));
  return v;
}

template <typename T>
void StoreAt(unsigned char* base, std::size_t at, T value) noexcept {
  std::memcpy(base + at, &value, sizeof(value));
}

// The engine's order: both keys as signed 64-bit integers (FindEntry 0x140f9eee0 uses CMOVL; libr15 0x2543a30-0x2543a90).
bool SignedLess(std::uint64_t typeA, std::uint64_t nameA, std::uint64_t typeB, std::uint64_t nameB) noexcept {
  const std::int64_t ta = static_cast<std::int64_t>(typeA), tb = static_cast<std::int64_t>(typeB);
  if (ta != tb) return ta < tb;
  return static_cast<std::int64_t>(nameA) < static_cast<std::int64_t>(nameB);
}

// The record index for (type, name) in a manifest, by the engine's binary search, or -1.
std::int64_t Search(const unsigned char* manifest, std::uint64_t type, std::uint64_t name) noexcept {
  const unsigned char* const records = reinterpret_cast<const unsigned char*>(LoadAt<std::uintptr_t>(manifest, kManifestRecords));
  if (records == nullptr) return -1;
  const std::int64_t wantType = static_cast<std::int64_t>(type);
  const std::int64_t wantName = static_cast<std::int64_t>(name);
  std::uint64_t lo = 0;
  std::uint64_t hi = LoadAt<std::uint64_t>(manifest, kManifestCount);
  while (lo < hi) {
    const std::uint64_t mid = lo + ((hi - lo) >> 1);
    const unsigned char* const rec = records + mid * kRecordBytes;
    const std::int64_t t = LoadAt<std::int64_t>(rec, 0);
    if (t == wantType) {
      const std::int64_t n = LoadAt<std::int64_t>(rec, 8);
      if (n == wantName) return static_cast<std::int64_t>(mid);
      if (n < wantName) {
        lo = mid + 1;
      } else {
        hi = mid;
      }
    } else if (t < wantType) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  return -1;
}

}  // namespace

const char* StatusName(Status status) noexcept {
  switch (status) {
    case Status::kOk: return "ok";
    case Status::kEmpty: return "empty";
    case Status::kIndexSpace: return "index_space";
    case Status::kChunkIndex: return "chunk_index";
    case Status::kPackageIndex: return "package_index";
    case Status::kDuplicate: return "duplicate";
  }
  return "unknown";
}

bool ReadLikeTheEngine(const void* manager, std::uint64_t type, std::uint64_t name, Found* out) noexcept {
  if (manager == nullptr) return false;
  const unsigned char* const mgr = static_cast<const unsigned char*>(manager);
  const unsigned char* const manifest = reinterpret_cast<const unsigned char*>(LoadAt<std::uintptr_t>(mgr, kManagerManifest));
  if (manifest == nullptr) return false;
  const std::int64_t at = Search(manifest, type, name);
  if (at < 0) return false;
  const unsigned char* const records = reinterpret_cast<const unsigned char*>(LoadAt<std::uintptr_t>(manifest, kManifestRecords));
  const unsigned char* const rec = records + static_cast<std::uint64_t>(at) * kRecordBytes;
  Found f;
  f.chunk = LoadAt<std::uint32_t>(rec, 0x10);
  f.offset_in_chunk = LoadAt<std::uint32_t>(rec, 0x14);
  f.size = LoadAt<std::uint32_t>(rec, 0x18);
  f.alignment = LoadAt<std::uint32_t>(rec, 0x1c);
  // The game's `table + chunk * 0x10` in 64 bits.
  const std::uintptr_t table = LoadAt<std::uintptr_t>(manifest, kManifestChunks);
  const unsigned char* const chunk = reinterpret_cast<const unsigned char*>(table + static_cast<std::uintptr_t>(f.chunk) * kChunkBytes);
  f.package = LoadAt<std::uint32_t>(chunk, 0);
  f.file_offset = LoadAt<std::uint32_t>(chunk, 4);
  f.compressed = LoadAt<std::uint32_t>(chunk, 8);
  f.decompressed = LoadAt<std::uint32_t>(chunk, 12);
  const unsigned char* const handles = reinterpret_cast<const unsigned char*>(LoadAt<std::uintptr_t>(manifest, kManifestHandles));
  f.handle = LoadAt<std::int64_t>(handles, static_cast<std::size_t>(f.package) * sizeof(std::int64_t));
  if (out != nullptr) *out = f;
  return true;
}

namespace {

// The one counter of the process: the next chunk index no shadow has been given. Never decreases outside a test, never gives a value
// twice, so no two shadows (of any kind, built or rebuilt) share a key in the engine's block cache.
static_assert(kFirstChunkIndex > kStockChunkCount, "a shadow index must never equal a stock chunk index");
std::atomic<std::uint32_t> g_nextChunkIndex{kFirstChunkIndex};

// Takes `n` consecutive indices and returns the first, or false when fewer than n are left below kChunkIndexLimit (none are taken).
bool TakeChunkIndices(std::size_t n, std::uint32_t* first) noexcept {
  std::uint32_t at = g_nextChunkIndex.load(std::memory_order_relaxed);
  for (;;) {
    if (static_cast<std::uint64_t>(at) + n > kChunkIndexLimit) return false;
    if (g_nextChunkIndex.compare_exchange_weak(at, static_cast<std::uint32_t>(at + n), std::memory_order_relaxed)) {
      *first = at;
      return true;
    }
  }
}

}  // namespace

std::uint32_t Shadow::NextChunkIndex() noexcept { return g_nextChunkIndex.load(std::memory_order_relaxed); }

void Shadow::ResetChunkCounterForTest(std::uint32_t next) noexcept { g_nextChunkIndex.store(next, std::memory_order_relaxed); }

Status Shadow::Build(const Input& input, std::unique_ptr<Shadow>* out) {
  out->reset();
  if (input.records.empty()) return Status::kEmpty;
  for (const Record& r : input.records) {
    if (r.chunk >= input.chunks.size()) return Status::kChunkIndex;
  }
  for (const Chunk& c : input.chunks) {
    if (c.package >= input.handles.size()) return Status::kPackageIndex;
  }
  std::vector<std::size_t> order(input.records.size());
  for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
  std::sort(order.begin(), order.end(), [&input](std::size_t a, std::size_t b) {
    const Record& ra = input.records[a];
    const Record& rb = input.records[b];
    return SignedLess(ra.type, ra.name, rb.type, rb.name);
  });
  for (std::size_t i = 1; i < order.size(); ++i) {
    const Record& a = input.records[order[i - 1]];
    const Record& b = input.records[order[i]];
    if (a.type == b.type && a.name == b.name) return Status::kDuplicate;
  }

  // Last, so a refused input takes no index: this shadow's range, above every index given before.
  std::uint32_t base = 0;
  if (!TakeChunkIndices(input.chunks.size(), &base)) return Status::kIndexSpace;

  std::unique_ptr<Shadow> shadow(new Shadow());
  shadow->chunk_base_ = base;
  shadow->records_.assign(input.records.size() * kRecordBytes, 0);
  shadow->users_.resize(input.records.size());
  for (std::size_t i = 0; i < order.size(); ++i) {
    const Record& r = input.records[order[i]];
    unsigned char* const rec = shadow->records_.data() + i * kRecordBytes;
    StoreAt<std::uint64_t>(rec, 0, r.type);
    StoreAt<std::uint64_t>(rec, 8, r.name);
    StoreAt<std::uint32_t>(rec, 0x10, base + r.chunk);
    StoreAt<std::uint32_t>(rec, 0x14, r.offset_in_chunk);
    StoreAt<std::uint32_t>(rec, 0x18, r.size);
    StoreAt<std::uint32_t>(rec, 0x1c, r.alignment);
    shadow->users_[i] = r.user;
  }
  shadow->chunks_.assign(input.chunks.size() * kChunkBytes, 0);
  for (std::size_t i = 0; i < input.chunks.size(); ++i) {
    unsigned char* const c = shadow->chunks_.data() + i * kChunkBytes;
    StoreAt<std::uint32_t>(c, 0, input.chunks[i].package);
    StoreAt<std::uint32_t>(c, 4, input.chunks[i].file_offset);
    StoreAt<std::uint32_t>(c, 8, input.chunks[i].compressed);
    StoreAt<std::uint32_t>(c, 12, input.chunks[i].decompressed);
  }
  shadow->handles_ = input.handles;
  shadow->manifest_.assign(kManifestBytes, 0);
  unsigned char* const m = shadow->manifest_.data();
  StoreAt<std::uintptr_t>(m, kManifestRecords, reinterpret_cast<std::uintptr_t>(shadow->records_.data()));
  StoreAt<std::uint64_t>(m, kManifestCount, static_cast<std::uint64_t>(input.records.size()));
  // The table address minus base * 0x10, as an integer that wraps: the game adds chunk * 0x10 back.
  StoreAt<std::uintptr_t>(m, kManifestChunks,
                          reinterpret_cast<std::uintptr_t>(shadow->chunks_.data()) -
                              static_cast<std::uintptr_t>(base) * kChunkBytes);
  StoreAt<std::uintptr_t>(m, kManifestHandles, reinterpret_cast<std::uintptr_t>(shadow->handles_.data()));
  StoreAt<std::uintptr_t>(shadow->manager_, kManagerManifest, reinterpret_cast<std::uintptr_t>(m));
  *out = std::move(shadow);
  return Status::kOk;
}

bool Shadow::Find(std::uint64_t type, std::uint64_t name, Found* out, std::uint32_t* user) const noexcept {
  const std::int64_t at = Search(manifest_.data(), type, name);
  if (at < 0) return false;
  if (out != nullptr && !ReadLikeTheEngine(manager_, type, name, out)) return false;
  if (user != nullptr) *user = users_[static_cast<std::size_t>(at)];
  return true;
}

}  // namespace nevr_shadow
