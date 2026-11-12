#include "quest/combat/decals.h"

#include <dlfcn.h>
#include <pthread.h>
#include <sys/stat.h>
#include <time.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <new>

#include "quest/combat/combat_level_targets.h"
#include "quest/combat/decals_targets.h"
#include "quest/combat/legacy_switch.h"
#include "quest/combat/safe_read.h"
#include "quest/sentinel/hook_install.h"
#include "quest/sentinel/hook_log.h"

namespace nevr_quest::combat::decals {

namespace {

using sentinel::LogLevel;

// Engine layout the module reads and writes (docs/quest/combat/decals.md, "Engine facts").
constexpr std::uintptr_t kSysAllocator = 0x30;   // CR15NetBulletCS: its CAllocator*
constexpr std::uintptr_t kSysSpace = 0x80;       // CR15NetBulletCS: the CncaGameSpace that owns it
constexpr std::uintptr_t kSpaceLevel = level::targets::kGamespaceLevelOffset;  // CncaGameSpace: the CSymbol64 of the level it loaded
constexpr std::size_t kNodeBytes = 0x2048;       // sizeof(CDynamicMeshNode)
constexpr std::uintptr_t kNodeMesh = 0xa8;       // CDynamicMeshNode: its CGDynamicMesh
constexpr std::uintptr_t kNodeVertexCount = 0x98, kNodeIndexCount = 0xa0;
// The mesh's render state: Initialize copies the pool's {raster, depth, blend, pass}, whose blend is 0 (off) for material type 15.
constexpr std::uintptr_t kNodeBlend = kNodeMesh + 0x320 + 0x700 + 8;
constexpr std::size_t kVtDestructor = 0x0, kVtLink = 0x58, kVtVisible = 0x60;  // CDynamicMeshNode vtable byte offsets
constexpr std::size_t kVtAlloc = 0x10, kVtFree = 0x38;                         // CAllocator vtable byte offsets
constexpr int kNodeLineCap = 64;

pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;  // the bullet systems of several game spaces update on their own threads
Table g_table;
Host g_host;
Api g_api;
std::uint16_t g_idx[12 * kMaxNodeQuads];
sentinel::GotHook g_updateHook;
sentinel::GotHook g_shutdownHook;
std::atomic<bool> g_on{false};
std::atomic<int> g_node_lines{0};

std::int64_t NowMs() {
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<std::int64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

template <typename F>
F Vt(void* obj, std::size_t byte_offset) {
  return (*reinterpret_cast<F* const*>(obj))[byte_offset / sizeof(void*)];
}

void SetVisible(std::uintptr_t node, std::uint32_t on) {
  Vt<void (*)(void*, std::uint32_t)>(reinterpret_cast<void*>(node), kVtVisible)(reinterpret_cast<void*>(node), on);
}

// ---- the real operations ---------------------------------------------------------------------------------------------

bool OpLevelOf(void*, std::uintptr_t sys, std::uint64_t* level) {
  bool ok = true;
  const auto space = SafeRead<std::uintptr_t>(sys + kSysSpace, &ok);
  if (!ok || space == 0) return false;
  const auto l = SafeRead<std::uint64_t>(space + kSpaceLevel, &ok);
  if (!ok || l == 0 || l == ~0ull) return false;  // the constructor leaves -1 there until LoadLevel stores the level's symbol
  *level = l;
  return true;
}

std::uintptr_t OpCreate(void*, std::uintptr_t sys, const Group& g, int* rc, std::uintptr_t* tried, bool* blended) {
  const Api& a = g_api;
  *rc = -1;
  *tried = 0;
  *blended = false;
  if (*a.gs_initialized == 0 || *a.mem_context == nullptr) return 0;  // graphics are not up yet: the next try
  bool ok = true;
  void* const alloc = reinterpret_cast<void*>(SafeRead<std::uintptr_t>(sys + kSysAllocator, &ok));
  if (!ok || alloc == nullptr) return 0;
  void* const prev = a.mem_current();
  a.mem_set(alloc);
  void* const node = Vt<void* (*)(void*, std::size_t)>(alloc, kVtAlloc)(alloc, kNodeBytes);
  if (node == nullptr) {
    a.mem_set(prev);
    return 0;
  }
  *tried = reinterpret_cast<std::uintptr_t>(node);
  a.ctor(node);
  Vt<void (*)(void*, void*)>(node, kVtLink)(node, nullptr);
  Vt<void (*)(void*, std::uint32_t)>(node, kVtVisible)(node, 0);
  const VertexFormat fmt = a.vertex_format();
  void** const ctx = *a.mem_context;
  const MeshInit in{g.mat, 4ull * g.n, 12ull * g.n, 2, 1, 0, 0, ctx[0], ctx[1], g_idx, nullptr};
  char* const mesh = static_cast<char*>(node) + kNodeMesh;
  *rc = a.init(mesh, &fmt, &in);
  if (*rc != 0) {  // the node is destroyed and freed again
    a.shutdown(node);
    Vt<void (*)(void*)>(node, kVtDestructor)(node);
    Vt<void (*)(void*, void*)>(alloc, kVtFree)(alloc, node);
    a.mem_set(prev);
    return 0;
  }
  *reinterpret_cast<std::uint64_t*>(static_cast<char*>(node) + kNodeVertexCount) = 4ull * g.n;
  *reinterpret_cast<std::uint64_t*>(static_cast<char*>(node) + kNodeIndexCount) = 0;
  a.mem_set(prev);
  // Initialize leaves the blend of the pool the material type maps to: 0, off, which draws the atlas cells as opaque squares. The
  // group's own blend is written over that 0 and over nothing else.
  volatile std::uint32_t* const b = reinterpret_cast<volatile std::uint32_t*>(static_cast<char*>(node) + kNodeBlend);
  const std::uint32_t was = *b;
  if (was == 0) {
    *b = g.blend;
    *blended = true;
  }
  return reinterpret_cast<std::uintptr_t>(node);
}

void OpDestroy(void*, std::uintptr_t sys, std::uintptr_t nodeAddr) {
  const Api& a = g_api;
  void* const node = reinterpret_cast<void*>(nodeAddr);
  bool ok = true;
  void* const alloc = reinterpret_cast<void*>(SafeRead<std::uintptr_t>(sys + kSysAllocator, &ok));
  if (!ok || alloc == nullptr) {  // the system's allocator is gone: nothing can free the node
    sentinel::LogFields(sentinel::LogLevel::kWarn, "combat_decals_node_destroy_failed",
                        {{"system", static_cast<long long>(sys)}, {"node", static_cast<long long>(nodeAddr)},
                         {"msg", "a node could not be destroyed because its system's allocator is unreadable; the engine keeps it"}});
    return;
  }
  Vt<void (*)(void*, std::uint32_t)>(node, kVtVisible)(node, 0);
  a.shutdown(node);
  void* const prev = a.mem_current();
  a.mem_set(alloc);
  Vt<void (*)(void*)>(node, kVtDestructor)(node);  // takes it out of the scene graph and the update lists
  Vt<void (*)(void*, void*)>(alloc, kVtFree)(alloc, node);
  a.mem_set(prev);
}

bool OpDraw(void*, std::uintptr_t nodeAddr, const Group& g) {
  const Api& a = g_api;
  void* const mesh = reinterpret_cast<void*>(nodeAddr + kNodeMesh);
  Vtx* const v = static_cast<Vtx*>(a.map(mesh));
  if (v == nullptr) return false;
  FillVertices(g, v);
  a.unmap(mesh);
  *reinterpret_cast<std::uint64_t*>(nodeAddr + kNodeIndexCount) = 12ull * g.n;
  SetVisible(nodeAddr, 1);
  return true;
}

void OpHide(void*, std::uintptr_t nodeAddr) { SetVisible(nodeAddr, 0); }

// The create operation, with the line each node gets (a failure is a Warning).
std::uintptr_t OpCreateLogged(void* user, std::uintptr_t sys, const Group& g, int* rc, std::uintptr_t* tried, bool* blended) {
  const std::uintptr_t node = OpCreate(user, sys, g, rc, tried, blended);
  if (g_node_lines.fetch_add(1, std::memory_order_relaxed) < kNodeLineCap) {
    char m[19];
    sentinel::LogFields(node != 0 ? sentinel::LogLevel::kInfo : sentinel::LogLevel::kWarn, "combat_decals_node",
                        {{"made", node != 0 ? 1 : 0}, {"system", static_cast<long long>(sys)}, {"node", static_cast<long long>(*tried)},
                         {"material", sentinel::HexString(m, g.mat)}, {"quads", g.n}, {"rc", *rc},
                         {"blend", *blended ? static_cast<long long>(g.blend) : 0},
                         {"msg", node != 0 ? "a decal node was made for a group of the level"
                                           : "a decal node was not made (rc is the engine's answer, 0 when the graphics were not up yet); the group stays off"}});
  }
  return node;
}

// ---- the table -------------------------------------------------------------------------------------------------------

// Reads a whole file into a new buffer; false (and nothing owned) when it is absent, unreadable or over the cap.
bool ReadFile(const char* path, std::uint8_t** out, std::size_t* len, const char** why) {
  struct stat st;
  if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
    *why = "absent";
    return false;
  }
  if (st.st_size < 0 || static_cast<std::size_t>(st.st_size) > kMaxFileBytes) {
    *why = "too_large";
    return false;
  }
  const std::size_t n = static_cast<std::size_t>(st.st_size);
  std::uint8_t* buf = new (std::nothrow) std::uint8_t[n == 0 ? 1 : n];
  if (buf == nullptr) {
    *why = "no_memory";
    return false;
  }
  FILE* f = std::fopen(path, "rbe");
  const std::size_t got = f != nullptr ? std::fread(buf, 1, n, f) : 0;
  if (f != nullptr) std::fclose(f);
  if (f == nullptr || got != n) {
    delete[] buf;
    *why = "unreadable";
    return false;
  }
  *out = buf;
  *len = n;
  return true;
}

// The candidate paths, in order, for a run that is not a test: the imported combat plugin data directory, then the two places the content pack's layout
// puts an effects table. Each is `<root>/decals.bin`; returns the count.
int CandidatePaths(char (*out)[400], int max) {
  int n = 0;
  char dir[300];
  if (n < max && legacy::Directory(dir, sizeof dir)) std::snprintf(out[n++], 400, "%s/decals.bin", dir);
  char pkg[200];
  if (legacy::PackageName(pkg, sizeof pkg)) {
    if (n < max) std::snprintf(out[n++], 400, "/sdcard/Android/data/%s/files/afk/decals.bin", pkg);
    if (n < max) std::snprintf(out[n++], 400, "/sdcard/Android/media/%s/files/afk/decals.bin", pkg);
  }
  return n;
}

bool Resolve(Api* a) {
  void* const h = dlopen("libr15.so", RTLD_NOW | RTLD_NOLOAD);
  if (h == nullptr) return false;
  auto S = [h](const char* name) { return dlsym(h, name); };
  a->ctor = reinterpret_cast<void (*)(void*)>(S("_ZN10NRadEngine16CDynamicMeshNodeC1Ev"));
  a->vertex_format = reinterpret_cast<VertexFormat (*)()>(S("_ZN10NRadEngine15SDynamicMeshVtx12VertexFormatEv"));
  a->init = reinterpret_cast<int (*)(void*, const VertexFormat*, const MeshInit*)>(
      S("_ZN10NRadEngine13CGDynamicMesh10InitializeERKNS_14CGVertexFormatERKNS_17SGDynamicMeshInitE"));
  a->map = reinterpret_cast<void* (*)(void*)>(S("_ZN10NRadEngine13CGDynamicMesh11MapVerticesEv"));
  a->unmap = reinterpret_cast<void (*)(void*)>(S("_ZN10NRadEngine13CGDynamicMesh13UnmapVerticesEv"));
  a->shutdown = reinterpret_cast<void (*)(void*)>(S("_ZN10NRadEngine16CDynamicMeshNode8ShutdownEv"));
  a->mem_current = reinterpret_cast<void* (*)()>(S("_ZN10NRadEngine14CMemoryContext7CurrentEv"));
  a->mem_set = reinterpret_cast<void (*)(void*)>(S("_ZN10NRadEngine14CMemoryContext10SetCurrentEPNS_10CAllocatorE"));
  a->mem_context = reinterpret_cast<void***>(S("_ZN10NRadEngine14CMemoryContext7contextE"));
  a->gs_initialized = reinterpret_cast<const std::uint32_t*>(S("_ZN10NRadEngine3CGS11initializedE"));
  return true;
}

bool Complete(const Api& a) {
  return a.ctor && a.vertex_format && a.init && a.map && a.unmap && a.shutdown && a.mem_current && a.mem_set && a.mem_context &&
         a.gs_initialized;
}

// ---- the handlers ----------------------------------------------------------------------------------------------------

using UpdateSig = void(std::uintptr_t sys);
using ShutdownSig = std::uint64_t(std::uint64_t self);
struct UpdateTag {};
struct ShutdownTag {};
using UpdateThunk = sentinel::CallbackThunk<UpdateTag, UpdateSig>;
using ShutdownThunk = sentinel::CallbackThunk<ShutdownTag, ShutdownSig>;

// A game space that has nothing to draw (an arena level, or no readable level yet) is skipped for kQuickSkipCalls updates without the module's lock, the
// clock or a read: one entry per bullet system, kept by the update that looked at it. The arena frame is then a few relaxed loads and a branch. A skipped
// space is looked at again after the skip, so a space that becomes a combat level is found within kQuickSkipCalls updates (and the Host's own once-a-second
// rule still bounds how often its level is read).
constexpr std::uint32_t kQuickSkipCalls = 64;
std::atomic<std::uintptr_t> g_quickSys[kMaxSystems] = {};
std::atomic<std::uint32_t> g_quickSkip[kMaxSystems] = {};

bool QuickSkip(std::uintptr_t sys) noexcept {
  for (int i = 0; i < kMaxSystems; ++i) {
    if (g_quickSys[i].load(std::memory_order_relaxed) != sys) continue;
    const std::uint32_t n = g_quickSkip[i].load(std::memory_order_relaxed);
    if (n == 0) return false;
    g_quickSkip[i].store(n - 1, std::memory_order_relaxed);
    return true;
  }
  return false;
}

// Under g_mu.
void QuickSet(std::uintptr_t sys, bool quiet) noexcept {
  int free = -1;
  for (int i = 0; i < kMaxSystems; ++i) {
    const std::uintptr_t have = g_quickSys[i].load(std::memory_order_relaxed);
    if (have == sys) {
      g_quickSkip[i].store(quiet ? kQuickSkipCalls : 0, std::memory_order_relaxed);
      return;
    }
    if (have == 0 && free < 0) free = i;
  }
  if (quiet && free >= 0) {
    g_quickSkip[free].store(kQuickSkipCalls, std::memory_order_relaxed);
    g_quickSys[free].store(sys, std::memory_order_relaxed);
  }
}

// Under g_mu.
void QuickDrop(std::uintptr_t sys) noexcept {
  for (int i = 0; i < kMaxSystems; ++i) {
    if (g_quickSys[i].load(std::memory_order_relaxed) == sys) {
      g_quickSkip[i].store(0, std::memory_order_relaxed);
      g_quickSys[i].store(0, std::memory_order_relaxed);
    }
  }
}

// The game's own update first: the nodes are drawn from the state it leaves.
void OnUpdate(UpdateThunk::Fn original, std::uintptr_t sys) noexcept {
  original(sys);
  if (!g_on.load(std::memory_order_acquire)) return;
  if (QuickSkip(sys)) return;
  pthread_mutex_lock(&g_mu);
  g_host.Tick(sys, NowMs());
  QuickSet(sys, g_host.Quiet(sys));
  pthread_mutex_unlock(&g_mu);
}

// A virtual method: x0 = this, and the earlier handler's or the game's value is returned untouched.
std::uint64_t OnShutdown(ShutdownThunk::Fn original, std::uint64_t self) noexcept {
  if (g_on.load(std::memory_order_acquire)) {
    pthread_mutex_lock(&g_mu);
    g_host.Shutdown(static_cast<std::uintptr_t>(self));  // our nodes go before the system does
    QuickDrop(static_cast<std::uintptr_t>(self));
    pthread_mutex_unlock(&g_mu);
  }
  return original(self);
}

NEVR_HOOK_RECORD(kUpdateRecord, UpdateThunk, &OnUpdate);
NEVR_HOOK_RECORD(kShutdownRecord, ShutdownThunk, &OnShutdown);

ModuleResult Refused(const Switches& switches, const char* reason, LogLevel level, const char* msg) noexcept {
  sentinel::LogFields(level, "combat_decals_install",
                      {{"result", level == LogLevel::kInfo ? "off" : "refused"}, {"status", reason}, {"reason", reason},
                       {"decals_flip_u", switches.flipU ? 1 : 0}, {"decals_flip_v", switches.flipV ? 1 : 0},
                       {"msg", msg}});
  return {false, reason, level == LogLevel::kInfo};
}

void WarnLegacy(const char* file, const char* key) noexcept {
  if (!legacy::Present(file)) return;
  sentinel::LogFields(LogLevel::kWarn, "combat_decals_legacy_switch",
                      {{"file", file}, {"replaced_by", key},
                       {"msg", "the plugin's switch file is no longer read and changes nothing; set the combat key in config.yaml instead"}});
}

}  // namespace

ModuleResult InstallAt(const Switches& switches, sentinel::ImageLookup lookup, const char* tablePath, const Api* api) noexcept {
  WarnLegacy("decals.off", "combat.decals: false");
  WarnLegacy("decalflipu.on", "combat.decals_flip_u: true");
  WarnLegacy("decalflipv.on", "combat.decals_flip_v: true");
  if (!switches.on) {
    sentinel::LogFields(LogLevel::kInfo, "combat_decals_install",
                        {{"result", "off"}, {"status", "off_config"}, {"reason", "off_config"}, {"switch", "combat.decals"},
                         {"decals_flip_u", switches.flipU ? 1 : 0}, {"decals_flip_v", switches.flipV ? 1 : 0},
                         {"msg", "combat.decals is false: nothing is drawn and no hook is installed"}});
    return {false, "off_config", true};
  }
  // A second install would reset the live state (the node records, the counters) and, when its slot check then refuses, switch the module off: refuse
  // before anything is touched, with the first install still running.
  if (g_on.load(std::memory_order_acquire))
    return Refused(switches, "already_installed", LogLevel::kWarn, "decals is already installed; the running install is left as it is");
  // 1. the table
  char found[400] = "";
  char searched[1300] = "";
  std::uint8_t* bytes = nullptr;
  std::size_t len = 0;
  const char* why = "absent";
  const char* problem = nullptr;  // the first file that exists and could not be used: not the same as no file
  if (tablePath != nullptr) {
    std::snprintf(searched, sizeof searched, "%s", tablePath);
    if (ReadFile(tablePath, &bytes, &len, &why)) {
      std::snprintf(found, sizeof found, "%s", tablePath);
    } else if (std::strcmp(why, "absent") != 0) {
      problem = why;
    }
  } else {
    char paths[3][400];
    const int n = CandidatePaths(paths, 3);
    for (int i = 0; i < n; ++i) {
      std::size_t used = std::strlen(searched);
      std::snprintf(searched + used, sizeof searched - used, "%s%s", i == 0 ? "" : " ", paths[i]);
      if (bytes != nullptr) continue;
      if (ReadFile(paths[i], &bytes, &len, &why)) {
        std::snprintf(found, sizeof found, "%.399s", paths[i]);  // each path was written into 400 bytes
      } else if (problem == nullptr && std::strcmp(why, "absent") != 0) {
        problem = why;
      }
    }
  }
  if (bytes == nullptr && problem != nullptr) {
    sentinel::LogFields(LogLevel::kError, "combat_decals_install",
                        {{"result", "refused"}, {"status", "table_unreadable"}, {"reason", "table_unreadable"}, {"why", problem},
                         {"decals_flip_u", switches.flipU ? 1 : 0}, {"decals_flip_v", switches.flipV ? 1 : 0},
                         {"msg", "a decals.bin exists but cannot be read, so nothing is drawn"}});
    return {false, "table_unreadable"};
  }
  if (bytes == nullptr) {
    sentinel::LogFields(LogLevel::kInfo, "combat_decals_install",
                        {{"result", "off"}, {"status", "no_table"}, {"reason", "no_table"}, {"why", why},
                         {"decals_flip_u", switches.flipU ? 1 : 0}, {"decals_flip_v", switches.flipV ? 1 : 0},
                         {"msg", "no readable decals.bin: the module draws nothing without the content pack's table and installs no hook"}});
    return {false, "no_table", true};
  }
  Table table;
  const LoadResult lr = ParseTable(bytes, len, switches.flipU, switches.flipV, &table);
  delete[] bytes;
  if (lr != LoadResult::kOk || table.count == 0) {
    const char* reason = lr != LoadResult::kOk ? LoadResultName(lr) : "no_groups";
    sentinel::LogFields(LogLevel::kError, "combat_decals_install",
                        {{"result", "refused"}, {"status", lr != LoadResult::kOk ? "bad_table" : "empty_table"}, {"reason", reason}, {"bytes", len},
                         {"decals_flip_u", switches.flipU ? 1 : 0}, {"decals_flip_v", switches.flipV ? 1 : 0},
                         {"msg", "decals.bin is not a usable table, so nothing is drawn"}});
    FreeTable(&table);
    return {false, lr != LoadResult::kOk ? "bad_table" : "empty_table"};
  }
  // 2. the engine calls
  Api resolved;
  const Api* use = api;
  if (use == nullptr) {
    if (!Resolve(&resolved) || !Complete(resolved)) {
      sentinel::LogFields(LogLevel::kError, "combat_decals_install",
                          {{"result", "refused"}, {"status", "engine_export_missing"}, {"reason", "engine_export_missing"},
                           {"decals_flip_u", switches.flipU ? 1 : 0}, {"decals_flip_v", switches.flipV ? 1 : 0},
                           {"msg", "libr15.so is not loaded or lacks an export the draw path needs (not the pinned build?), so nothing is drawn"}});
      FreeTable(&table);
      return {false, "engine_export_missing"};
    }
    use = &resolved;
  } else if (!Complete(*use)) {
    FreeTable(&table);
    return Refused(switches, "engine_export_missing", LogLevel::kError, "the engine calls given are incomplete, so nothing is drawn");
  }
  sentinel::ElfImage image{};
  if (lookup == nullptr || !lookup(sentinel::pinned::kLibR15, &image)) {
    FreeTable(&table);
    return Refused(switches, "module_not_loaded", LogLevel::kError, "libr15.so is not loaded, so no hook can be installed and nothing is drawn");
  }
  const std::uintptr_t base = image.base;
  // 3. the state: set before either handler can run
  FreeTable(&g_table);
  g_table = table;
  g_api = *use;
  FillIndices(g_idx);
  g_host = Host();
  g_host.table = &g_table;
  g_host.ops.level_of = &OpLevelOf;
  g_host.ops.create = &OpCreateLogged;
  g_host.ops.destroy = &OpDestroy;
  g_host.ops.draw = &OpDraw;
  g_host.ops.hide = &OpHide;
  g_node_lines.store(0);
  g_on.store(true, std::memory_order_release);
  // 4. Shutdown first (a node must never outlive its system), then the update that makes nodes
  ShutdownThunk::Arm(kShutdownRecord);
  const sentinel::GotStatus shutdownStatus = sentinel::InstallThunk<ShutdownThunk>(g_shutdownHook, ShutdownTarget(base), lookup);
  sentinel::LogFields(shutdownStatus == sentinel::GotStatus::kOk ? LogLevel::kInfo : LogLevel::kError, "combat_decals_hook",
                      {{"hook", "shutdown"}, {"status", sentinel::GotStatusName(shutdownStatus)}, {"slot", "vtable_word_chained"}});
  if (shutdownStatus != sentinel::GotStatus::kOk) {
    g_on.store(false, std::memory_order_release);
    return {false, sentinel::GotStatusName(shutdownStatus)};
  }
  UpdateThunk::Arm(kUpdateRecord);
  const sentinel::GotStatus updateStatus = sentinel::InstallThunk<UpdateThunk>(g_updateHook, UpdateTarget(base), lookup);
  sentinel::LogFields(updateStatus == sentinel::GotStatus::kOk ? LogLevel::kInfo : LogLevel::kError, "combat_decals_hook",
                      {{"hook", "update"}, {"status", sentinel::GotStatusName(updateStatus)}, {"slot", "glob_dat"}});
  if (updateStatus != sentinel::GotStatus::kOk) {
    g_on.store(false, std::memory_order_release);  // the Shutdown handler stays in and passes through
    return {false, sentinel::GotStatusName(updateStatus)};
  }
  sentinel::LogFields(LogLevel::kInfo, "combat_decals_table",
                      {{"path", found}, {"groups", g_table.count}, {"quads", g_table.total_quads}, {"groups_cut_to_160_quads", g_table.truncated_groups},
                       {"trailing_bytes", g_table.trailing_bytes}, {"flip_u", switches.flipU ? 1 : 0}, {"flip_v", switches.flipV ? 1 : 0}});
  sentinel::LogFields(LogLevel::kInfo, "combat_decals_install",
                      {{"result", "installed"}, {"status", "installed"}, {"reason", "ok"}, {"switch", "combat.decals"},
                       {"decals_flip_u", switches.flipU ? 1 : 0}, {"decals_flip_v", switches.flipV ? 1 : 0},
                       {"msg", "the static level decals are drawn from the table in dynamic-mesh nodes, in combat levels only"}});
  return {true, "installed"};
}

ModuleResult Install(const Switches& switches) noexcept { return InstallAt(switches, sentinel::FindLoadedImage, nullptr, nullptr); }

const Counters& CountersForTest() noexcept { return g_host.counters; }

void ResetForTest() noexcept {
  g_on.store(false);
  g_updateHook.Forget();
  g_shutdownHook.Forget();
  UpdateThunk::Reset();
  ShutdownThunk::Reset();
  FreeTable(&g_table);
  g_host = Host();
  g_api = Api();
  g_node_lines.store(0);
  for (int i = 0; i < kMaxSystems; ++i) {
    g_quickSys[i].store(0);
    g_quickSkip[i].store(0);
  }
}

}  // namespace nevr_quest::combat::decals
