// map_sets (quest/sentinel/map_sets.h): the shared map-set registration over a held POSIX descriptor, its refusals, the mapping
// released before the registration returns, the mount observer, and the registry's Android types. The descriptors are temp files
// under NEVR_TEST_TMP; no game file is read. Every record is read back as the JSON the sentinel log writes.

#include <fcntl.h>
#include <unistd.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <zstd.h>

#include "core/resource_set_registry.h"
#include "quest/data/sha256.h"
#include "quest/sentinel/hook_log.h"
#include "quest/sentinel/map_sets.h"
#include "quest/tests/test_check.h"

namespace {

namespace rs = nevr_resource_sets;
namespace ms = nevr_quest::map_sets;

constexpr char kAbcSha[] = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
constexpr mode_t kFileMode = 0600;

std::vector<std::string> g_lines;
void Sink(sentinel::LogLevel, const char* line) { g_lines.emplace_back(line); }

// The value of `key` in a JSON record line, as text ("<absent>" when the line has no such key).
std::string Field(const std::string& line, const char* key) {
  const std::string needle = std::string("\"") + key + "\":";
  const std::size_t at = line.find(needle);
  if (at == std::string::npos) return "<absent>";
  const std::size_t begin = at + needle.size();
  if (line[begin] == '"') return line.substr(begin + 1, line.find('"', begin + 1) - begin - 1);
  return line.substr(begin, line.find_first_of(",}", begin) - begin);
}

// The last record of `event`.
std::string Last(const char* event) {
  const std::string needle = std::string("\"event\":\"") + event + "\"";
  for (auto it = g_lines.rbegin(); it != g_lines.rend(); ++it) {
    if (it->find(needle) != std::string::npos) return *it;
  }
  return std::string();
}

std::string TempPath(const char* name) {
  const char* dir = std::getenv("NEVR_TEST_TMP");
  return std::string(dir != nullptr ? dir : "/tmp") + "/map_sets_" + std::to_string(::getpid()) + "_" + name;
}

std::string TempFile(const char* name, const std::string& bytes) {
  const std::string path = TempPath(name);
  const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, kFileMode);
  QCHECK(fd >= 0);
  QCHECK(::write(fd, bytes.data(), bytes.size()) == static_cast<ssize_t>(bytes.size()));
  ::close(fd);
  return path;
}

bool StillOpen(int fd) { return ::fcntl(fd, F_GETFD) != -1; }

// True when the process maps `path` now (/proc/self/maps names a file mapping by its path).
bool Mapped(const std::string& path) {
  std::ifstream maps("/proc/self/maps");
  std::stringstream text;
  text << maps.rdbuf();
  return text.str().find(path) != std::string::npos;
}

rs::MapFileRequest Request(std::string owner = "map:arena", std::string label = "map:arena@1", std::string sha256 = "",
                           bool rehash = false) {
  rs::MapFileRequest r;
  r.owner = std::move(owner);
  r.label = std::move(label);
  r.sha256 = std::move(sha256);
  r.rehash = rehash;
  return r;
}

int observed = 0;
std::size_t observedSets = 99;
void Observer(const rs::MountReport& report) {
  ++observed;
  observedSets = report.sets;
}

void TheWin10TypesAreThePcsAndTheAndroidTypesDiffer() {
  QCHECK(rs::kWin10Types.archive == rs::kArchiveType);
  QCHECK(rs::kWin10Types.level_root == rs::kLevelRootType);
  QCHECK(rs::kWin10Types.script == rs::kScriptType);
  QCHECK(rs::kWin10Types.texture == rs::kTextureType);
  QCHECK(rs::kWin10Types.texture_gpu == rs::kTextureGpuType);
  QCHECK(rs::kAndroidTypes.texture == 0xe2efe7289d5985b8ULL);      // cargo_manifest.h kAndroidArm64 buffer 0
  QCHECK(rs::kAndroidTypes.texture_gpu == 0x489bb35d53ca50e9ULL);  // and buffer 1
  QCHECK(rs::kAndroidTypes.archive != rs::kArchiveType);
  QCHECK(rs::kAndroidTypes.level_root != rs::kLevelRootType);
  QCHECK(rs::kAndroidTypes.script != rs::kScriptType);
}

void TheArgumentRefusalsAreThePcsAndTheCallerKeepsTheDescriptor() {
  ms::ResetForTest();
  const std::string path = TempFile("abc", "abc");
  const int fd = ::open(path.c_str(), O_RDONLY);
  QCHECK(ms::RegisterFileResourceSet(Request("arena", "map:arena@1", kAbcSha, true), fd).detail == "bad_owner");
  QCHECK(ms::RegisterFileResourceSet(Request("map:", "map:arena@1", kAbcSha, true), fd).detail == "bad_owner");
  QCHECK(ms::RegisterFileResourceSet(Request("map:arena", "map:\"arena", kAbcSha, true), fd).detail == "bad_label");
  QCHECK(ms::RegisterFileResourceSet(Request("map:arena", std::string(rs::kMaxMapLabelBytes + 1, 'a'), kAbcSha, true), fd).detail ==
         "bad_label");
  QCHECK(ms::RegisterFileResourceSet(Request("map:arena", "map:arena@1", "ABC", true), fd).detail == "bad_pin");
  QCHECK(ms::RegisterFileResourceSet(Request("map:arena", "map:arena@1", kAbcSha, true), -1).detail == "bad_handle");
  const rs::Outcome hash = ms::RegisterFileResourceSet(Request("map:arena", "map:arena@1", std::string(64, '0'), true), fd);
  QCHECK(hash.refusal == rs::Refusal::kHash);
  QCHECK(hash.detail == std::string("sha256 ") + kAbcSha + ", pinned " + std::string(64, '0'));
  const std::string record = Last("map_set");
  QCHECK(Field(record, "result") == "refused");
  QCHECK(Field(record, "reason") == "hash_mismatch");
  QCHECK(Field(record, "owner") == "map:arena");
  QCHECK(Field(record, "rehashed") == "true");
  // The pin matches, and three bytes are no resource set.
  QCHECK(ms::RegisterFileResourceSet(Request("map:arena", "map:arena@1", kAbcSha, true), fd).refusal == rs::Refusal::kFormat);
  QCHECK(StillOpen(fd));
  QCHECK(ms::Held() == 0);
  QCHECK(!Mapped(path));
  ::close(fd);
}

void AnEmptyFileAndADirectoryAreFormat() {
  ms::ResetForTest();
  const int empty = ::open(TempFile("empty", "").c_str(), O_RDONLY);
  QCHECK(ms::RegisterFileResourceSet(Request(), empty).detail == "empty_file");
  QCHECK(StillOpen(empty));
  ::close(empty);
  const char* dir = std::getenv("NEVR_TEST_TMP");
  const int d = ::open(dir != nullptr ? dir : "/tmp", O_RDONLY | O_DIRECTORY);
  QCHECK(ms::RegisterFileResourceSet(Request(), d).detail == "not_a_file");
  ::close(d);
}

bool NoStock(void*, std::uint64_t, std::uint64_t) { return false; }
int published = 0;
const nevr_shadow::Shadow* g_published = nullptr;
void Publish(const nevr_shadow::Shadow* shadow) {
  ++published;
  g_published = shadow;
}

void PutLe(std::string* out, std::size_t at, unsigned width, std::uint64_t value) {
  for (unsigned i = 0; i < width; ++i) (*out)[at + i] = static_cast<char>(value >> (8 * i));
}

// A NEVRRES1 set, each record in its own zstd chunk (core/tests/map_derive_test.cpp BuildSet); records sorted by key.
std::string BuildSet(const std::vector<std::pair<std::pair<std::uint64_t, std::uint64_t>, std::string>>& records) {
  std::vector<std::string> frames;
  for (const auto& r : records) {
    std::string frame(ZSTD_compressBound(r.second.size()), '\0');
    frame.resize(ZSTD_compress(&frame[0], frame.size(), r.second.data(), r.second.size(), 1));
    frames.push_back(frame);
  }
  const std::size_t n = records.size();
  std::string set(48 + n * 0x20 + n * 0x10, '\0');
  std::memcpy(&set[0], "NEVRRES1", 8);
  PutLe(&set, 8, 4, 48);
  PutLe(&set, 0x0c, 4, n);
  PutLe(&set, 0x10, 4, n);
  PutLe(&set, 0x18, 8, 48);
  PutLe(&set, 0x20, 8, 48 + n * 0x20);
  PutLe(&set, 0x28, 8, set.size());
  std::uint64_t at = set.size();
  for (std::size_t i = 0; i < n; ++i) {
    const std::size_t r = 48 + i * 0x20, c = 48 + n * 0x20 + i * 0x10;
    PutLe(&set, r, 8, records[i].first.first);
    PutLe(&set, r + 8, 8, records[i].first.second);
    PutLe(&set, r + 0x10, 4, i);
    PutLe(&set, r + 0x18, 4, records[i].second.size());
    PutLe(&set, r + 0x1c, 4, 16);
    PutLe(&set, c + 4, 4, at);
    PutLe(&set, c + 8, 4, frames[i].size());
    PutLe(&set, c + 12, 4, records[i].second.size());
    at += frames[i].size();
  }
  for (const std::string& f : frames) set += f;
  return set;
}

constexpr std::uint64_t kLevel = 0x7691d78d414c5701ULL;  // mpl_arena_a_lowspec
constexpr std::uint64_t kComponent = 0x1111222233334444ULL, kPart = 0x55;

// The valid-set fixture (synthetic): an Android level root, its archive listing one record, and that record.
std::string ValidSet() {
  std::string archive(4 + 4 + 16, '\0');
  PutLe(&archive, 0, 4, 0);  // no dependencies
  PutLe(&archive, 4, 4, 1);
  PutLe(&archive, 8, 8, kComponent);
  PutLe(&archive, 16, 8, kPart);
  return BuildSet({{{rs::kAndroidTypes.archive, kLevel}, archive},
                   {{kComponent, kPart}, std::string(300, 'c')},
                   {{rs::kAndroidTypes.level_root, kLevel}, std::string(64, 'r')}});
}

void AnAndroidTypedMapSetMountsAndItsRecordsAreServed() {
  ms::ResetForTest();
  published = 0;
  g_published = nullptr;
  const std::string set = ValidSet();
  const std::string path = TempFile("valid", set);
  const int fd = ::open(path.c_str(), O_RDONLY);
  QCHECK(fd >= 0);
  QCHECK(ms::RegisterFileResourceSet(Request("map:arena", "map:arena@1.0", nevr_quest::data::Sha256Hex(set.data(), set.size()),
                                             true),
                                     fd)
             .refusal == rs::Refusal::kNone);
  const std::string record = Last("map_set");
  QCHECK(Field(record, "result") == "registered");
  QCHECK(Field(record, "label") == "map:arena@1.0");
  // The registry keeps what it read; the mapping it read through is gone, and the descriptor is the registry's.
  QCHECK(!Mapped(path));
  QCHECK(StillOpen(fd));
  QCHECK(ms::Held() == 1);
  rs::StockView stock;
  stock.has = &NoStock;
  stock.records = 1;
  const rs::MountReport report = ms::Mount(stock, &Publish);
  QCHECK(report.sets == 1 && report.refused == 0 && report.levels == 1);
  QCHECK(published == 1 && g_published != nullptr);
  QCHECK(Field(Last("map_set_mount"), "result") == "mounted");
  const std::vector<std::uint64_t> levels = ms::MountedLevelNames();
  QCHECK(levels.size() == 1 && levels[0] == kLevel);
  nevr_shadow::Found found;
  QCHECK(g_published->Find(rs::kAndroidTypes.level_root, kLevel, &found) && found.size == 64);
  QCHECK(g_published->Find(kComponent, kPart, &found) && found.size == 300);
  QCHECK(found.handle == fd);  // the engine reads the set through the held descriptor
  QCHECK(!g_published->Find(rs::kWin10Types.level_root, kLevel, &found));  // the PC's types are not the Quest's
}

void AWithdrawnMapClosesItsDescriptorAndItsKeysAreFree() {
  ms::ResetForTest();
  const int fd = ::open(TempFile("withdrawn", ValidSet()).c_str(), O_RDONLY);
  QCHECK(ms::RegisterFileResourceSet(Request(), fd).ok());
  QCHECK(ms::HoldsKey(rs::kAndroidTypes.level_root, kLevel));
  ms::Withdraw("map:arena");
  QCHECK(!StillOpen(fd));
  QCHECK(!ms::HoldsKey(rs::kAndroidTypes.level_root, kLevel));
  QCHECK(ms::Held() == 0);
  QCHECK(Field(Last("map_set"), "result") == "withdrawn");
}

void AMountWithNothingHeldCallsTheObserverOncePublishesNothingAndClosesRegistration() {
  ms::ResetForTest();
  observed = 0;
  published = 0;
  ms::SetMountObserver(&Observer);
  rs::StockView stock;
  stock.has = &NoStock;
  stock.records = 1;
  QCHECK(ms::Mount(stock, &Publish).sets == 0);
  QCHECK(observed == 1 && observedSets == 0);
  QCHECK(published == 0);
  QCHECK(ms::Mount(stock, &Publish).sets == 0);
  QCHECK(observed == 1);
  const int fd = ::open(TempFile("late", "abc").c_str(), O_RDONLY);
  QCHECK(ms::RegisterFileResourceSet(Request(), fd).refusal == rs::Refusal::kTooLate);
  QCHECK(StillOpen(fd));
  ::close(fd);
}

void AbandonCallsTheObserverWithAnEmptyReportAndLaterRegistrationsAreTooLate() {
  ms::ResetForTest();
  observed = 0;
  ms::SetMountObserver(&Observer);
  ms::Abandon();
  QCHECK(observed == 1 && observedSets == 0);
  const int fd = ::open(TempFile("abandoned", "abc").c_str(), O_RDONLY);
  QCHECK(ms::RegisterFileResourceSet(Request(), fd).refusal == rs::Refusal::kTooLate);
  ::close(fd);
}

void DropMapIsForMapOwnersBeforeTheMountOnly() {
  rs::Registry registry(1000, rs::kAndroidTypes);
  QCHECK(registry.DropMap("levels").empty());
  QCHECK(registry.DropMap("map:arena").empty());
  QCHECK(registry.Abandon().empty());
  QCHECK(registry.DropMap("map:arena").empty());
}

}  // namespace

int main() {
  sentinel::SetLogSink(&Sink);
  TheWin10TypesAreThePcsAndTheAndroidTypesDiffer();
  TheArgumentRefusalsAreThePcsAndTheCallerKeepsTheDescriptor();
  AnEmptyFileAndADirectoryAreFormat();
  AMountWithNothingHeldCallsTheObserverOncePublishesNothingAndClosesRegistration();
  AbandonCallsTheObserverWithAnEmptyReportAndLaterRegistrationsAreTooLate();
  DropMapIsForMapOwnersBeforeTheMountOnly();
  AWithdrawnMapClosesItsDescriptorAndItsKeysAreFree();
  AnAndroidTypedMapSetMountsAndItsRecordsAreServed();
  ms::ResetForTest();
  return quest_test::Failures() == 0 ? 0 : 1;
}
