// map_sets (CDN_DESIGN Q1): PC3's file registration over a held POSIX descriptor, its refusals, the mount observer, and the
// registry's platform types. The descriptors are temp files under NEVR_TEST_TMP; no game file is read.

#include <fcntl.h>
#include <unistd.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include <zstd.h>

#include "core/resource_set_registry.h"
#include "quest/sentinel/hook_log.h"
#include "quest/sentinel/map_sets.h"
#include "quest/tests/test_check.h"

namespace {

namespace rs = nevr_resource_sets;
namespace ms = nevr_quest::map_sets;

const std::string kAbcSha = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";

void QuietSink(sentinel::LogLevel, const char*) {}

std::string TempFile(const char* name, const std::string& bytes) {
  const char* dir = std::getenv("NEVR_TEST_TMP");
  const std::string path = std::string(dir != nullptr ? dir : "/tmp") + "/map_sets_" + name;
  const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
  QCHECK(fd >= 0);
  QCHECK(::write(fd, bytes.data(), bytes.size()) == static_cast<ssize_t>(bytes.size()));
  ::close(fd);
  return path;
}

bool StillOpen(int fd) { return ::fcntl(fd, F_GETFD) != -1; }

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
  QCHECK(ms::RegisterFileResourceSet("arena", fd, "map:arena@1", kAbcSha, true).detail == "bad_owner");
  QCHECK(ms::RegisterFileResourceSet("map:", fd, "map:arena@1", kAbcSha, true).detail == "bad_owner");
  QCHECK(ms::RegisterFileResourceSet("map:arena", fd, "map:\"arena", kAbcSha, true).detail == "bad_label");
  QCHECK(ms::RegisterFileResourceSet("map:arena", fd, std::string(129, 'a'), kAbcSha, true).detail == "bad_label");
  QCHECK(ms::RegisterFileResourceSet("map:arena", fd, "map:arena@1", "ABC", true).detail == "bad_pin");
  QCHECK(ms::RegisterFileResourceSet("map:arena", -1, "map:arena@1", kAbcSha, true).detail == "bad_handle");
  const rs::Outcome hash = ms::RegisterFileResourceSet("map:arena", fd, "map:arena@1", std::string(64, '0'), true);
  QCHECK(hash.refusal == rs::Refusal::kHash);
  QCHECK(hash.detail.find(kAbcSha) != std::string::npos);
  // The pin matches, and three bytes are no resource set.
  QCHECK(ms::RegisterFileResourceSet("map:arena", fd, "map:arena@1", kAbcSha, true).refusal == rs::Refusal::kFormat);
  QCHECK(StillOpen(fd));
  QCHECK(ms::Held() == 0);
  ::close(fd);
}

void AnEmptyFileAndADirectoryAreFormat() {
  ms::ResetForTest();
  const int empty = ::open(TempFile("empty", "").c_str(), O_RDONLY);
  QCHECK(ms::RegisterFileResourceSet("map:arena", empty, "map:arena@1", "", false).detail == "empty_file");
  QCHECK(StillOpen(empty));
  ::close(empty);
  const char* dir = std::getenv("NEVR_TEST_TMP");
  const int d = ::open(dir != nullptr ? dir : "/tmp", O_RDONLY | O_DIRECTORY);
  QCHECK(ms::RegisterFileResourceSet("map:arena", d, "map:arena@1", "", false).detail == "not_a_file");
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

// The valid-set fixture (synthetic): an Android level root, its archive listing one record, and that record.
void AnAndroidTypedMapSetMountsAndItsRecordsAreServed() {
  ms::ResetForTest();
  published = 0;
  g_published = nullptr;
  const std::uint64_t level = 0x7691d78d414c5701ULL;  // mpl_arena_a_lowspec
  const std::uint64_t component = 0x1111222233334444ULL, part = 0x55;
  std::string archive(4 + 4 + 16, '\0');
  PutLe(&archive, 0, 4, 0);  // no dependencies
  PutLe(&archive, 4, 4, 1);
  PutLe(&archive, 8, 8, component);
  PutLe(&archive, 16, 8, part);
  const std::string set = BuildSet({{{rs::kAndroidTypes.archive, level}, archive},
                                    {{component, part}, std::string(300, 'c')},
                                    {{rs::kAndroidTypes.level_root, level}, std::string(64, 'r')}});
  const int fd = ::open(TempFile("valid", set).c_str(), O_RDONLY);
  QCHECK(fd >= 0);
  QCHECK(ms::RegisterFileResourceSet("map:arena", fd, "map:arena@1.0", "", false).refusal == rs::Refusal::kNone);
  QCHECK(ms::Held() == 1);
  rs::StockView stock;
  stock.has = &NoStock;
  stock.records = 1;
  const rs::MountReport report = ms::Mount(stock, &Publish);
  QCHECK(report.sets == 1 && report.refused == 0 && report.levels == 1);
  QCHECK(published == 1 && g_published != nullptr);
  const std::vector<std::uint64_t> levels = ms::MountedLevelNames();
  QCHECK(levels.size() == 1 && levels[0] == level);
  nevr_shadow::Found found;
  QCHECK(g_published->Find(rs::kAndroidTypes.level_root, level, &found) && found.size == 64);
  QCHECK(g_published->Find(component, part, &found) && found.size == 300);
  QCHECK(!g_published->Find(rs::kWin10Types.level_root, level, &found));  // the PC's types are not the Quest's
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
  QCHECK(ms::RegisterFileResourceSet("map:arena", fd, "map:arena@1", "", false).refusal == rs::Refusal::kTooLate);
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
  QCHECK(ms::RegisterFileResourceSet("map:arena", fd, "map:arena@1", "", false).refusal == rs::Refusal::kTooLate);
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
  sentinel::SetLogSink(&QuietSink);
  TheWin10TypesAreThePcsAndTheAndroidTypesDiffer();
  TheArgumentRefusalsAreThePcsAndTheCallerKeepsTheDescriptor();
  AnEmptyFileAndADirectoryAreFormat();
  AMountWithNothingHeldCallsTheObserverOncePublishesNothingAndClosesRegistration();
  AbandonCallsTheObserverWithAnEmptyReportAndLaterRegistrationsAreTooLate();
  DropMapIsForMapOwnersBeforeTheMountOnly();
  AnAndroidTypedMapSetMountsAndItsRecordsAreServed();
  ms::ResetForTest();
  return quest_test::Failures() == 0 ? 0 : 1;
}
