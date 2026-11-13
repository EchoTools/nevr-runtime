#include "quest/data/boot_stub.h"

#include <cstdio>

namespace nevr_quest::data::boot_stub {

namespace {

// One zstd frame (magic, single segment, content size 0xc0, one compressed block, no checksum) of the body below: 0xc0 bytes, all
// zero but the block size 0x80000 at +0x04. Made with zstandard 0.25 at level 3 and decoded by libzstd in boot_stub_test, which
// compares the decoded bytes with the body field by field, so a change to this array that decodes to another body fails there.
constexpr unsigned char kFrame[] = {0x28, 0xb5, 0x2f, 0xfd, 0x20, 0xc0, 0x65, 0x00, 0x00, 0x20, 0x00,
                                    0x00, 0x08, 0x00, 0x02, 0x00, 0x35, 0x40, 0xd8, 0x02, 0x58};

constexpr std::uint64_t kAlignment = 0x10;      // the shipped manifests' alignment word
constexpr std::uint64_t kBodyBytes = 0xc0;      // the decompressed length: the whole header, every count 0

void PutU32(std::string& out, std::uint32_t v) {
  for (int i = 0; i < 4; ++i) out.push_back(static_cast<char>((v >> (8 * i)) & 0xff));
}

void PutU64(std::string& out, std::uint64_t v) {
  for (int i = 0; i < 8; ++i) out.push_back(static_cast<char>((v >> (8 * i)) & 0xff));
}

}  // namespace

std::string SetFileName(std::uint64_t set_id) {
  char buf[17];
  std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(set_id));
  return buf;
}

std::string ManifestsDir(const std::string& root) {
  return root + "/_data/" + kVersionFolder + "/" + kPlatformPath + "/manifests";
}

std::string ManifestPath(const std::string& root, std::uint64_t set_id) {
  return ManifestsDir(root) + "/" + SetFileName(set_id);
}

std::string StubFileBytes() {
  std::string out = "ZSTD";
  PutU32(out, static_cast<std::uint32_t>(kAlignment));
  PutU64(out, kBodyBytes);
  PutU64(out, sizeof(kFrame));
  out.append(reinterpret_cast<const char*>(kFrame), sizeof(kFrame));
  return out;
}

}  // namespace nevr_quest::data::boot_stub
