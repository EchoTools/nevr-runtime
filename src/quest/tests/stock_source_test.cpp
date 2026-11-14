// The Quest's stock source (quest/cargo/stock_source.h) over a stock tree written under NEVR_TEST_TMP, and the shared core
// (core/stock_source.h) under a memory figure the test chooses: a manifest, a chunk or a script over the one-object share of the
// device's memory is not read (docs/adr/0045 decision 8). No game file is read.
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zstd.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "core/cargo_limits.h"
#include "core/stock_source.h"
#include "quest/cargo/posix_map_fs.h"
#include "quest/cargo/stock_source.h"
#include "quest/tests/test_check.h"

namespace {

constexpr mode_t kDirMode = 0700;
constexpr mode_t kFileMode = 0600;

std::string Tmp() {
  const char* dir = std::getenv("NEVR_TEST_TMP");
  return std::string(dir != nullptr ? dir : "/tmp") + "/stock_source_" + std::to_string(::getpid());
}

void MakeDirs(const std::string& path) {
  for (std::size_t at = 1; at <= path.size(); ++at) {
    if (at == path.size() || path[at] == '/') ::mkdir(path.substr(0, at).c_str(), kDirMode);
  }
}

void WriteFile(const std::string& path, const std::vector<std::uint8_t>& bytes) {
  const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, kFileMode);
  QCHECK(fd >= 0);
  QCHECK(::write(fd, bytes.data(), bytes.size()) == static_cast<ssize_t>(bytes.size()));
  ::close(fd);
}

void Put(std::vector<std::uint8_t>* out, std::size_t at, std::uint64_t value, unsigned width) {
  for (unsigned i = 0; i < width; ++i) (*out)[at + i] = static_cast<std::uint8_t>(value >> (8 * i));
}

std::vector<std::uint8_t> Compress(const std::vector<std::uint8_t>& in) {
  std::vector<std::uint8_t> out(ZSTD_compressBound(in.size()));
  const std::size_t n = ZSTD_compress(out.data(), out.size(), in.data(), in.size(), 3);
  QCHECK(!ZSTD_isError(n));
  out.resize(n);
  return out;
}

struct Rec {
  std::uint64_t type, name;
  std::uint32_t offset, size;
};

// A one-package manifest (docs/reference/package-manifest-format.md) whose records all sit in chunk 0 of `compressed` bytes.
std::vector<std::uint8_t> Manifest(const std::vector<Rec>& recs, std::uint32_t chunk_bytes, std::uint32_t compressed) {
  std::vector<std::uint8_t> body(0xc0 + recs.size() * 0x20 + 0x10, 0);
  Put(&body, 0, 1, 4);
  Put(&body, 0x38, recs.size(), 8);
  Put(&body, 0x78, 0, 8);
  Put(&body, 0xb8, 1, 8);
  for (std::size_t i = 0; i < recs.size(); ++i) {
    const std::size_t at = 0xc0 + i * 0x20;
    Put(&body, at, recs[i].type, 8);
    Put(&body, at + 8, recs[i].name, 8);
    Put(&body, at + 0x10, 0, 4);
    Put(&body, at + 0x14, recs[i].offset, 4);
    Put(&body, at + 0x18, recs[i].size, 4);
  }
  const std::size_t chunk_at = 0xc0 + recs.size() * 0x20;
  Put(&body, chunk_at, 0, 4);
  Put(&body, chunk_at + 4, 0, 4);
  Put(&body, chunk_at + 8, compressed, 4);
  Put(&body, chunk_at + 12, chunk_bytes, 4);
  const std::vector<std::uint8_t> frame = Compress(body);
  std::vector<std::uint8_t> file(24, 0);
  std::memcpy(file.data(), "ZSTD", 4);
  Put(&file, 4, 0x10, 4);
  Put(&file, 8, body.size(), 8);
  Put(&file, 16, frame.size(), 8);
  file.insert(file.end(), frame.begin(), frame.end());
  return file;
}

std::string g_root;
const char* Root() noexcept { return g_root.empty() ? nullptr : g_root.c_str(); }

void TheStockSourceReadsTheGamesTreeReadOnly() {
  g_root = Tmp() + "/root";
  const std::string android = g_root + "/_data/5932408047/rad15/android";
  MakeDirs(android + "/manifests");
  MakeDirs(android + "/packages");
  const std::string lib_dir = Tmp() + "/lib";
  MakeDirs(lib_dir);

  std::vector<std::uint8_t> chunk(300);
  for (std::size_t i = 0; i < chunk.size(); ++i) chunk[i] = static_cast<std::uint8_t>(i * 7);
  const std::vector<std::uint8_t> frame = Compress(chunk);
  const std::vector<Rec> recs = {{0x22, 0x9, 100, 50}, {0x11, 0x5, 0, 100}};
  const std::string manifest = android + "/manifests/48037dc70b0ecab2";
  WriteFile(manifest, Manifest(recs, static_cast<std::uint32_t>(chunk.size()), static_cast<std::uint32_t>(frame.size())));
  WriteFile(android + "/packages/48037dc70b0ecab2_0", frame);
  WriteFile(lib_dir + "/lib00000000000000ab.so", {1, 2, 3});

  std::unique_ptr<nevr_maps::StockSource> stock = nevr_quest::maps::MakePosixStockSource(&Root, lib_dir, "");
  std::vector<std::uint8_t> out;
  std::vector<nevr_maps::StockStamp> read_from;
  QCHECK(stock->ReadResource(0x22, 0x9, &out, &read_from));
  QCHECK(out == std::vector<std::uint8_t>(chunk.begin() + 100, chunk.begin() + 150));
  QCHECK(read_from.size() == 2 && read_from[0].path == manifest);
  QCHECK(stock->StampsCurrent(read_from));
  QCHECK(stock->ReadResource(0x11, 0x5, &out, &read_from) && out.size() == 100 && out[3] == 21);
  QCHECK(!stock->ReadResource(0x11, 0x6, &out, &read_from) && out.empty());

  // A pack's manifest replaced the stock one, which it kept as `.stock`: the rebuild reads the kept copy.
  WriteFile(manifest + ".stock", Manifest({{0x33, 0x1, 0, 300}}, static_cast<std::uint32_t>(chunk.size()),
                                          static_cast<std::uint32_t>(frame.size())));
  read_from.clear();
  QCHECK(stock->ReadResource(0x33, 0x1, &out, &read_from) && out == chunk);
  QCHECK(read_from[0].path == manifest + ".stock");
  QCHECK(!stock->ReadResource(0x22, 0x9, &out, &read_from));

  // A stock file that changed after the read is no longer current (must-fail for a stamp that is never compared).
  std::vector<nevr_maps::StockStamp> stamps;
  QCHECK(stock->ReadResource(0x33, 0x1, &out, &stamps));
  std::vector<std::uint8_t> longer = frame;
  longer.push_back(0);
  WriteFile(android + "/packages/48037dc70b0ecab2_0", longer);
  QCHECK(!stock->StampsCurrent(stamps));

  read_from.clear();
  QCHECK(stock->ReadScript(0xab, &out, &read_from) && out == std::vector<std::uint8_t>({1, 2, 3}));
  QCHECK(read_from.size() == 1 && read_from[0].path == lib_dir + "/lib00000000000000ab.so");
  QCHECK(!stock->ReadScript(0xac, &out, &read_from));

  g_root.clear();
  QCHECK(!stock->ReadResource(0x33, 0x1, &out, &read_from));  // no data root yet: nothing is found
}

// The shared core under a memory figure the test sets: one manifest and one package on disk, through the Quest's own file reads.
std::uint64_t g_memory = 0;
bool g_haveMemory = true;
bool TestMemory(std::uint64_t* bytes) noexcept {
  *bytes = g_memory;
  return g_haveMemory;
}

class SharedCore : public nevr_maps::PackageStockSource {
 public:
  SharedCore(std::string manifest, std::string prefix)
      : PackageStockSource(&TestMemory), manifest_(std::move(manifest)), prefix_(std::move(prefix)) {}
  bool ReadScript(std::uint64_t, std::vector<std::uint8_t>* out, std::vector<nevr_maps::StockStamp>* read_from) override {
    return ReadWholeFile(prefix_ + ".script", out, read_from);
  }
  std::uint64_t Share() const { return ObjectBytes(); }

 protected:
  std::vector<nevr_maps::ManifestPlace> Manifests() override { return {{manifest_, prefix_}}; }
  std::unique_ptr<nevr_maps::StockFile> Open(const std::string& path) override {
    class File : public nevr_maps::StockFile {
     public:
      explicit File(int fd) : fd_(fd) {}
      ~File() override { ::close(fd_); }
      bool Stamp(nevr_maps::FileStamp* out) override { return nevr_quest::maps::PosixMapFs().StampOf(fd_, out); }
      bool Read(std::uint64_t offset, void* buffer, std::size_t length) override {
        return ::pread(fd_, buffer, length, static_cast<off_t>(offset)) == static_cast<ssize_t>(length);
      }

     private:
      int fd_;
    };
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    return fd < 0 ? nullptr : std::unique_ptr<nevr_maps::StockFile>(new File(fd));
  }

 private:
  std::string manifest_, prefix_;
};

void NothingOverTheMemoryShareIsRead() {
  const std::string dir = Tmp() + "/memory";
  MakeDirs(dir);
  std::vector<std::uint8_t> chunk(4000, 9);
  const std::vector<std::uint8_t> frame = Compress(chunk);
  const std::vector<std::uint8_t> manifest = Manifest({{0x44, 0x1, 0, 4000}}, 4000, static_cast<std::uint32_t>(frame.size()));
  WriteFile(dir + "/m", manifest);
  WriteFile(dir + "/p_0", frame);
  WriteFile(dir + "/p.script", chunk);  // a stock script of the chunk's size
  std::vector<std::uint8_t> out;
  std::vector<nevr_maps::StockStamp> read_from;

  // The share is the one-object share of cargo_limits.h: half the memory the device can still give.
  g_memory = 1u << 30;
  {
    SharedCore core(dir + "/m", dir + "/p");
    QCHECK(core.Share() == g_memory / nevr_cargo::kMemoryShareForObject);
    QCHECK(core.ReadResource(0x44, 0x1, &out, &read_from) && out == chunk);
    QCHECK(core.ReadScript(0, &out, &read_from) && out == chunk);
  }
  // A share that holds the manifest's file and body but not the 4000-byte chunk: the manifest parses, the resource is not read.
  const std::uint64_t body = 0xc0 + 0x20 + 0x10;
  g_memory = nevr_cargo::kMemoryShareForObject * (manifest.size() + body + 1);
  {
    SharedCore core(dir + "/m", dir + "/p");
    QCHECK(core.Share() < 4000);
    QCHECK(!core.ReadResource(0x44, 0x1, &out, &read_from));
    QCHECK(out.empty());
    QCHECK(read_from.size() >= 1 && read_from[0].path == dir + "/m");  // the manifest was consulted
    QCHECK(!core.ReadScript(0, &out, &read_from) && out.empty());       // a script over the share is not read either
  }
  // A share smaller than the manifest's file and body: the manifest itself is not read.
  g_memory = nevr_cargo::kMemoryShareForObject * (manifest.size() + body - 1);
  {
    SharedCore core(dir + "/m", dir + "/p");
    read_from.clear();
    QCHECK(!core.ReadResource(0x44, 0x1, &out, &read_from));
    QCHECK(read_from.size() == 1);  // the package was never opened
  }
  // A resource whose chunk is smaller than the manifest's body: a share that holds the chunk but not the manifest's file and body
  // leaves the manifest unread, so the resource is not read although its chunk would fit.
  std::vector<std::uint8_t> small(100, 7);
  const std::vector<std::uint8_t> small_frame = Compress(small);
  const std::vector<std::uint8_t> small_manifest = Manifest({{0x55, 0x2, 0, 100}}, 100, static_cast<std::uint32_t>(small_frame.size()));
  WriteFile(dir + "/n", small_manifest);
  WriteFile(dir + "/q_0", small_frame);
  g_memory = nevr_cargo::kMemoryShareForObject * (small_manifest.size() + body + 1);
  {
    SharedCore core(dir + "/n", dir + "/q");
    QCHECK(core.ReadResource(0x55, 0x2, &out, &read_from) && out == small);
  }
  g_memory = nevr_cargo::kMemoryShareForObject * (small_manifest.size() + body - 1);
  {
    SharedCore core(dir + "/n", dir + "/q");
    QCHECK(core.Share() > small.size());
    QCHECK(!core.ReadResource(0x55, 0x2, &out, &read_from));
  }
  // No memory figure: nothing is read, and nothing is guessed in its place.
  g_haveMemory = false;
  g_memory = 1u << 30;
  {
    SharedCore core(dir + "/m", dir + "/p");
    QCHECK(core.Share() == 0);
    QCHECK(!core.ReadResource(0x44, 0x1, &out, &read_from));
  }
  g_haveMemory = true;
}

}  // namespace

int main() {
  TheStockSourceReadsTheGamesTreeReadOnly();
  NothingOverTheMemoryShareIsRead();
  nevr_quest::maps::PosixMapFs().RemoveTree(Tmp());
  return quest_test::Failures() == 0 ? 0 : 1;
}
