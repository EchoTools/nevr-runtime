// #2161: a plugin that carries a resource set in its own file and registers it from its init, for test_behavioral's
// PluginResourceSets tests. The set is built at compile time into the .nevrres section (constant data, no relocation),
// the way nevr/pc/plugin_resources.h embeds one: a level (kLevel) with its root, its archive and one mesh, in one
// chunk that is a zstd frame of one raw block.
//
// The set also carries one override (kOverrideType:kOverrideName, 8 bytes of 0x55), which the host registers when it
// mounts the set.
//
// The init registers the set, then tries four refusals the host must make: the same set again (a key the first holds:
// NEVR_RESOURCES_COLLIDES), a block of writable data (NEVR_RESOURCES_NOT_IN_FILE, writable_section), a read-only block
// with a relocated pointer in it (NEVR_RESOURCES_NOT_IN_FILE, relocated) and the set under a label with a newline
// (NEVR_RESOURCES_BAD_ARGUMENT, bad_label). The statuses are exported for the test to read.

#include <windows.h>

#include <array>
#include <cstdint>
#include <cstring>

#include "nevr/pc/plugin_resources.h"
#include "nevr/plugin_interface.h"

namespace {

constexpr std::uint64_t kLevel = 0x0123456789abcdefULL;
constexpr std::uint64_t kMeshType = 0x1111111111111111ULL;
constexpr std::uint64_t kMesh = 0x2222222222222222ULL;
constexpr std::uint64_t kArchiveType = 0x2a41cf1c1d9e5d32ULL;
constexpr std::uint64_t kLevelRootType = 0xe8e38d7781a338a6ULL;
constexpr std::uint64_t kOverrideType = 0x5555555555555555ULL;
constexpr std::uint64_t kOverrideName = 0x6666666666666666ULL;

// The chunk, decompressed: the root (16 bytes of 0x11) at 0, the archive at 16 (no dependencies, two entries, an empty
// type table, a closing 0: 48 bytes), the mesh (16 bytes of 0x22) at 64.
constexpr std::size_t kPlain = 80;
constexpr std::size_t kFrame = 4 + 1 + 4 + 3 + kPlain;
constexpr std::size_t kHeader = 48, kRecords = 3, kTable = kHeader + kRecords * 0x20;  // chunk table at 144
constexpr std::size_t kOverrideTable = kTable + 0x10;                                 // one override at 160
constexpr std::size_t kFrameAt = kOverrideTable + 0x18;                               // frame at 184
constexpr std::size_t kOverrideAt = kFrameAt + kFrame;                                // the override's 8 bytes
constexpr std::size_t kSize = kOverrideAt + 8;

constexpr void Put(std::array<unsigned char, kSize>& a, std::size_t at, std::uint64_t v, int bytes) {
  for (int i = 0; i < bytes; ++i) a[at + i] = static_cast<unsigned char>(v >> (8 * i));
}

constexpr std::array<unsigned char, kSize> BuildSet() {
  std::array<unsigned char, kSize> a{};
  const char magic[8] = {'N', 'E', 'V', 'R', 'R', 'E', 'S', '1'};
  for (int i = 0; i < 8; ++i) a[i] = static_cast<unsigned char>(magic[i]);
  Put(a, 0x08, kHeader, 4);
  Put(a, 0x0c, kRecords, 4);
  Put(a, 0x10, 1, 4);
  Put(a, 0x14, 1, 4);
  Put(a, 0x18, kHeader, 8);
  Put(a, 0x20, kTable, 8);
  Put(a, 0x28, kOverrideTable, 8);
  Put(a, kOverrideTable, kOverrideType, 8);
  Put(a, kOverrideTable + 8, kOverrideName, 8);
  Put(a, kOverrideTable + 0x10, kOverrideAt, 4);
  Put(a, kOverrideTable + 0x14, 8, 4);
  for (std::size_t i = 0; i < 8; ++i) a[kOverrideAt + i] = 0x55;
  const std::uint64_t types[3] = {kLevelRootType, kArchiveType, kMeshType};
  const std::uint64_t names[3] = {kLevel, kLevel, kMesh};
  const std::uint32_t offsets[3] = {0, 16, 64};
  const std::uint32_t sizes[3] = {16, 48, 16};
  for (int r = 0; r < 3; ++r) {
    const std::size_t at = kHeader + static_cast<std::size_t>(r) * 0x20;
    Put(a, at, types[r], 8);
    Put(a, at + 8, names[r], 8);
    Put(a, at + 0x10, 0, 4);
    Put(a, at + 0x14, offsets[r], 4);
    Put(a, at + 0x18, sizes[r], 4);
    Put(a, at + 0x1c, 16, 4);
  }
  Put(a, kTable + 4, kFrameAt, 4);
  Put(a, kTable + 8, kFrame, 4);
  Put(a, kTable + 12, kPlain, 4);
  // The frame: magic, single segment with a 4-byte content size, one last raw block of kPlain bytes.
  std::size_t f = kFrameAt;
  a[f++] = 0x28; a[f++] = 0xB5; a[f++] = 0x2F; a[f++] = 0xFD; a[f++] = 0xA0;
  Put(a, f, kPlain, 4);
  f += 4;
  Put(a, f, (static_cast<std::uint64_t>(kPlain) << 3) | 1u, 3);
  f += 3;
  for (std::size_t i = 0; i < 16; ++i) a[f + i] = 0x11;
  std::size_t arc = f + 16;
  Put(a, arc, 0, 4);       // no dependencies
  Put(a, arc + 4, 2, 4);   // two entries
  Put(a, arc + 8, kMeshType, 8);
  Put(a, arc + 16, kMesh, 8);
  Put(a, arc + 24, kLevelRootType, 8);
  Put(a, arc + 32, kLevel, 8);
  Put(a, arc + 40, 0, 4);  // empty type table
  Put(a, arc + 44, 0, 4);  // closing 0
  for (std::size_t i = 0; i < 16; ++i) a[f + 64 + i] = 0x22;
  return a;
}

NEVR_RESOURCE_SET_SECTION const std::array<unsigned char, kSize> kSet = BuildSet();

// Data holding a pointer: the loader relocates it, so its file bytes differ from the mapped ones. One copy is writable
// (.data), one read-only, in the set's own section.
struct WithPointer {
  const void* self;
  unsigned char pad[56];
};
WithPointer g_withPointer = {&g_withPointer, {}};
extern const WithPointer kRelocated;
NEVR_RESOURCE_SET_SECTION const WithPointer kRelocated = {&kRelocated, {}};

constexpr int kRegistrations = 6;
int32_t g_status[kRegistrations] = {-99, -99, -99, -99, -99, -99};

}  // namespace

extern "C" __declspec(dllexport) NevrPluginInfo NevrPluginGetInfo() {
  return NevrPluginInfo{"test_plugin_resources", "#2161 resource set fixture", 1, 0, 0};
}

extern "C" __declspec(dllexport) uint32_t NevrPluginGetApiVersion() { return NEVR_PLUGIN_API_VERSION; }

// args {"fail":"1"} makes the init fail after it registered the set, so the host must drop the set.
extern "C" __declspec(dllexport) int NevrPluginInitEx(const NevrGameContext* ctx, const char* args) {
  g_status[0] = NevrRegisterEmbeddedResources(ctx, kSet.data(), kSet.size(), "fixture levels").status;
  g_status[1] = NevrRegisterEmbeddedResources(ctx, kSet.data(), kSet.size(), "the same set again").status;
  g_status[2] = NevrRegisterEmbeddedResources(ctx, &g_withPointer, sizeof(g_withPointer), "writable").status;
  g_status[3] = NevrRegisterEmbeddedResources(ctx, &kRelocated, sizeof(kRelocated), "relocated").status;
  g_status[4] = NevrRegisterEmbeddedResources(ctx, kSet.data(), kSet.size(), "two\nlines").status;
  // Bytes in another module's image (kernel32's): never the plugin's own file.
  const auto* elsewhere = reinterpret_cast<const unsigned char*>(GetModuleHandleA("kernel32.dll"));
  g_status[5] = NevrRegisterEmbeddedResources(ctx, elsewhere + 0x1000, 64, "elsewhere").status;
  const bool fail = args != nullptr && std::strstr(args, "\"fail\":\"1\"") != nullptr;
  return fail ? 1 : 0;
}

// A shutdown export, so the loader releases this DLL after a failed init instead of keeping it for the process: a
// kept copy would answer the next test's status reads.
extern "C" __declspec(dllexport) void NevrPluginShutdown() {}

// Test reads: the status of registration `which` (0 the set, 1 the repeat, 2 the writable block, 3 the relocated
// block, 4 the bad label, 5 another module's bytes), and the set's bytes.
extern "C" __declspec(dllexport) int32_t NevrTestPluginResourcesStatus(uint32_t which) {
  return which < static_cast<uint32_t>(kRegistrations) ? g_status[which] : -98;
}
extern "C" __declspec(dllexport) const unsigned char* NevrTestPluginResourcesSet(uint64_t* size) {
  *size = kSet.size();
  return kSet.data();
}
