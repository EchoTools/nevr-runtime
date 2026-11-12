// Host test for the cargo limits (src/core/cargo_limits.*; docs/adr/0045-cdn-texture-delivery.md): every limit is computed from the
// device's facts and moves with them; a missing fact makes the limits "not ok" with its name; no limit is a constant.
//
// Built and run by `just test-quest-data`.

#include <malloc.h>

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>

#include <nlohmann/json.hpp>

#include "core/cargo_limits.h"
#include "core/cargo_manifest.h"
#include "quest/tests/test_check.h"

namespace {

namespace nc = nevr_cargo;

nc::DeviceFacts Facts(std::uint64_t memory, std::uint64_t free_bytes, std::uint64_t fd_limit, std::uint64_t fds_open) {
  nc::DeviceFacts f;
  f.have_free = true;
  f.free_bytes = free_bytes;
  f.have_fds = true;
  f.fd_soft_limit = fd_limit;
  f.fds_open = fds_open;
  f.have_memory = true;
  f.memory_available = memory;
  return f;
}

constexpr std::uint64_t kGiB = std::uint64_t{1} << 30;
constexpr std::uint64_t kEntry = 400;

void EveryLimitIsTheDevicesNumberThroughItsShare() {
  const nc::Limits l = nc::ComputeLimits(Facts(3 * kGiB, 10 * kGiB, 1024, 64), kEntry);
  QCHECK(l.ok);
  QCHECK(l.manifest_text_bytes == 3 * kGiB / (nc::kMemoryShareForManifest * (nc::kManifestBufferCopies + nc::kJsonExpansion)));
  QCHECK(l.max_entries == 3 * kGiB / nc::kMemoryShareForEntries / kEntry);
  QCHECK(l.max_object_bytes == 3 * kGiB / nc::kMemoryShareForObject);
  QCHECK(l.storage_budget == 10 * kGiB - 10 * kGiB / nc::kStorageShareKept);
  QCHECK(l.max_store_files == (1024 - 64) / nc::kDescriptorShareForStore);
  QCHECK(l.store_file_bytes == nc::kFormatMaxStoreBytes);  // 10 GiB free: the format's 4 GiB is the bound
  QCHECK(l.index_bytes == nc::kIndexBytesFor(l.max_entries));
}

void TheLimitsMoveWithTheDevice() {
  const nc::Limits a = nc::ComputeLimits(Facts(2 * kGiB, 8 * kGiB, 1000, 100), kEntry);
  const nc::Limits b = nc::ComputeLimits(Facts(4 * kGiB, 16 * kGiB, 1900, 100), kEntry);
  QCHECK(a.ok && b.ok);
  QCHECK(b.manifest_text_bytes == 2 * a.manifest_text_bytes);
  QCHECK(b.max_entries == 2 * a.max_entries);
  QCHECK(b.max_object_bytes == 2 * a.max_object_bytes);
  QCHECK(b.storage_budget == 2 * a.storage_budget);
  QCHECK(b.max_store_files == 2 * a.max_store_files);
  // Nothing equals the numbers the first design wrote down (2,048 entries, 1 MiB of manifest, 512 MiB of blobs, one file per object).
  QCHECK(a.max_entries != 2048 && a.manifest_text_bytes != (std::uint64_t{1} << 20) && a.storage_budget != (std::uint64_t{512} << 20));
}

void AStoreFileNeverPassesWhatThePartitionCanHold() {
  const nc::Limits small = nc::ComputeLimits(Facts(3 * kGiB, 1 * kGiB, 1024, 64), kEntry);
  QCHECK(small.ok);
  QCHECK(small.store_file_bytes == 1 * kGiB);  // the partition's free space is below the format's bound
  QCHECK(small.store_file_bytes <= nc::kFormatMaxStoreBytes);
}

void TheObjectBoundNeverPassesTheEnginesWord() {
  const nc::Limits big = nc::ComputeLimits(Facts(64 * kGiB, 10 * kGiB, 1024, 64), kEntry);
  QCHECK(big.ok);
  QCHECK(big.max_object_bytes == nc::kFormatMaxObjectBytes);  // 32 GiB would not fit a u32 size
}

void AMissingFactIsNamedAndNothingIsGuessed() {
  nc::DeviceFacts f = Facts(3 * kGiB, 10 * kGiB, 1024, 64);
  f.have_memory = false;
  nc::Limits l = nc::ComputeLimits(f, kEntry);
  QCHECK(!l.ok);
  QCHECK(std::string(l.reason) == "no_memory_fact");
  QCHECK(l.manifest_text_bytes == 0 && l.max_entries == 0);
  f = Facts(3 * kGiB, 10 * kGiB, 1024, 64);
  f.have_free = false;
  l = nc::ComputeLimits(f, kEntry);
  QCHECK(!l.ok && std::string(l.reason) == "no_free_space_fact");
  f = Facts(3 * kGiB, 10 * kGiB, 1024, 64);
  f.have_fds = false;
  l = nc::ComputeLimits(f, kEntry);
  QCHECK(!l.ok && std::string(l.reason) == "no_descriptor_fact");
}

void ADeviceWithNoRoomSaysSo() {
  nc::Limits l = nc::ComputeLimits(Facts(3 * kGiB, 10 * kGiB, 100, 100), kEntry);  // every descriptor is taken
  QCHECK(!l.ok && std::string(l.reason) == "no_descriptors");
  l = nc::ComputeLimits(Facts(3 * kGiB, 10 * kGiB, 100, 150), kEntry);  // open above the limit: no underflow into a huge budget
  QCHECK(!l.ok && std::string(l.reason) == "no_descriptors");
  l = nc::ComputeLimits(Facts(3 * kGiB, 0, 1024, 64), kEntry);
  QCHECK(!l.ok && std::string(l.reason) == "no_free_space");
  l = nc::ComputeLimits(Facts(0, 10 * kGiB, 1024, 64), kEntry);
  QCHECK(!l.ok && std::string(l.reason) == "no_memory");
  l = nc::ComputeLimits(Facts(3 * kGiB, 10 * kGiB, 1024, 64), 0);
  QCHECK(!l.ok && std::string(l.reason) == "no_entry_size");
}

void AnUnlimitedDescriptorLimitDoesNotOverflow() {
  const nc::Limits l = nc::ComputeLimits(Facts(3 * kGiB, 10 * kGiB, ~std::uint64_t{0}, 64), kEntry);
  QCHECK(l.ok);
  QCHECK(l.max_store_files > 1000000);
}

// The expansion the memory share assumes is measured, on the real all-assets manifest when the machine has it and on a synthetic
// one of the same shape when it does not: the parsed form must stay under kJsonExpansion times the text.
void TheJsonExpansionTheShareAssumesIsHeld() {
  using json = nlohmann::ordered_json;
  std::string text;
  const char* real = std::getenv("NEVR_CARGO_REAL_MANIFEST");
  if (real != nullptr) {
    std::ifstream f(real, std::ios::binary);
    text.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
  }
  if (text.empty()) {
    json entries = json::array();
    for (int i = 0; i < 40000; ++i) {
      entries.push_back(json{{"platform", "android-arm64"}, {"type", "0xd9952dd5b4f69aec"}, {"name", "0x8120755c6da7eb41"},
                             {"format", "zstd-frame"}, {"sha256", std::string(64, 'a')}, {"size", 206078 + i}, {"bytes", 217748}, {"align", 256}});
    }
    text = json{{"schema", "x"}, {"labels", {{"game", {{"entries", entries}}}}}}.dump();
  }
  const std::size_t before = mallinfo2().uordblks;
  const json doc = json::parse(text.begin(), text.end(), nullptr, false);
  const std::size_t after = mallinfo2().uordblks;
  QCHECK(!doc.is_discarded());
  const double ratio = static_cast<double>(after - before) / static_cast<double>(text.size());
  std::printf("cargo_limits_test: parsed form is %.2f times the text (%zu bytes of text); the share assumes %llu\n", ratio, text.size(),
              static_cast<unsigned long long>(nc::kJsonExpansion));
  QCHECK(ratio < static_cast<double>(nc::kJsonExpansion));
}

}  // namespace

int main() {
  EveryLimitIsTheDevicesNumberThroughItsShare();
  TheLimitsMoveWithTheDevice();
  AStoreFileNeverPassesWhatThePartitionCanHold();
  TheObjectBoundNeverPassesTheEnginesWord();
  AMissingFactIsNamedAndNothingIsGuessed();
  ADeviceWithNoRoomSaysSo();
  AnUnlimitedDescriptorLimitDoesNotOverflow();
  TheJsonExpansionTheShareAssumesIsHeld();
  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "cargo_limits_test: %d check(s) failed\n", quest_test::Failures());
    return 1;
  }
  std::printf("cargo_limits_test: all checks passed\n");
  return 0;
}
