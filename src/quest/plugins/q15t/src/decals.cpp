#include "decals.h"

#include <dlfcn.h>
#include <pthread.h>
#include <sys/stat.h>
#include <time.h>

#include <atomic>
#include <cstdio>
#include <cstring>

#include "module_switch.h"
#include "plugin_log.h"
#include "safe_read.h"

namespace q15t {
namespace {

using namespace decals;

// Engine layout the module reads and writes (docs/quest/q15t/modules/decals.md, "Engine facts").
constexpr std::uintptr_t kSysAllocator = 0x30;   // CR15NetBulletCS: its CAllocator*
constexpr std::uintptr_t kSysSpace = 0x80;       // CR15NetBulletCS: the CncaGameSpace that owns it
constexpr std::uintptr_t kSpaceLevel = 0x58;     // CncaGameSpace: the CSymbol64 of the level it loaded
constexpr std::size_t kNodeBytes = 0x2048;       // sizeof(CDynamicMeshNode)
constexpr std::uintptr_t kNodeMesh = 0xa8;       // CDynamicMeshNode: its CGDynamicMesh
constexpr std::uintptr_t kNodeVertexCount = 0x98, kNodeIndexCount = 0xa0;
// The mesh's render state: Initialize copies the pool's {raster, depth, blend, pass}, whose blend is 0 (off) for material type 15.
constexpr std::uintptr_t kNodeBlend = kNodeMesh + 0x320 + 0x700 + 8;
constexpr std::size_t kVtDestructor = 0x0, kVtLink = 0x58, kVtVisible = 0x60;  // CDynamicMeshNode vtable byte offsets
constexpr std::size_t kVtAlloc = 0x10, kVtFree = 0x38;                         // CAllocator vtable byte offsets
constexpr int kNodeLineCap = 64;

constexpr const char kUpdateSymbol[] = "_ZN10NRadEngine8NRadGame15CR15NetBulletCS14UpdateMovementEv";

pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;  // the bullet systems of several game spaces update on their own threads
Table g_table;
Host g_host;
DecalsApi g_api;
std::uint16_t g_idx[12 * kMaxNodeQuads];
std::atomic<std::uintptr_t> g_real_update{0}, g_real_shutdown{0};
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
  const auto space = Rd<std::uintptr_t>(sys + kSysSpace, &ok);
  if (!ok || space == 0) return false;
  const auto l = Rd<std::uint64_t>(space + kSpaceLevel, &ok);
  if (!ok || l == 0 || l == ~0ull) return false;  // the constructor leaves -1 there until LoadLevel stores the level's symbol
  *level = l;
  return true;
}

std::uintptr_t OpCreate(void*, std::uintptr_t sys, const Group& g, int* rc, std::uintptr_t* tried, bool* blended) {
  const DecalsApi& a = g_api;
  *rc = -1;
  *tried = 0;
  *blended = false;
  if (*a.gs_initialized == 0 || *a.mem_context == nullptr) return 0;  // graphics are not up yet: the next try
  bool ok = true;
  void* const alloc = reinterpret_cast<void*>(Rd<std::uintptr_t>(sys + kSysAllocator, &ok));
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
  const DecalsVertexFormat fmt = a.vertex_format();
  void** const ctx = *a.mem_context;
  const DecalsMeshInit in{g.mat, 4ull * g.n, 12ull * g.n, 2, 1, 0, 0, ctx[0], ctx[1], g_idx, nullptr};
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
  const DecalsApi& a = g_api;
  void* const node = reinterpret_cast<void*>(nodeAddr);
  bool ok = true;
  void* const alloc = reinterpret_cast<void*>(Rd<std::uintptr_t>(sys + kSysAllocator, &ok));
  if (!ok || alloc == nullptr) {  // the system's allocator is gone: nothing can free the node
    char n[24], s[24];
    const NevrLogField f[] = {{"system", Hex(s, sizeof s, sys)}, {"node", Hex(n, sizeof n, nodeAddr)}};
    Log(NEVR_LOG_WARNING, "decals: a node could not be destroyed, its system's allocator is unreadable; the engine keeps it", f, 2);
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
  const DecalsApi& a = g_api;
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
    char n[24], m[24], q[24], r[24], bl[24], s[24];
    const NevrLogField f[] = {{"system", Hex(s, sizeof s, sys)},
                              {"node", Hex(n, sizeof n, *tried)},
                              {"material", Hex(m, sizeof m, g.mat)},
                              {"quads", Dec(q, sizeof q, g.n)},
                              {"rc", Dec(r, sizeof r, *rc)},
                              {"blend", Dec(bl, sizeof bl, *blended ? static_cast<long long>(g.blend) : 0)}};
    Log(node != 0 ? NEVR_LOG_INFO : NEVR_LOG_WARNING,
        node != 0 ? "decals: node made" : "decals: node not made (rc is the engine's answer, 0 when the graphics were not up yet; the group stays off)",
        f, 6);
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

// The candidate paths, in order, for a run that is not a test: the plugin data directory, then the two places the content pack's layout
// puts an effects table. Each is `<root>/decals.bin`; returns the count.
int CandidatePaths(char (*out)[400], int max) {
  int n = 0;
  char dir[300];
  if (n < max && ModuleDataDir(dir, sizeof dir)) std::snprintf(out[n++], 400, "%s/decals.bin", dir);
  char pkg[200];
  if (ModulePackageName(pkg, sizeof pkg)) {
    if (n < max) std::snprintf(out[n++], 400, "/sdcard/Android/data/%s/files/afk/decals.bin", pkg);
    if (n < max) std::snprintf(out[n++], 400, "/sdcard/Android/media/%s/files/afk/decals.bin", pkg);
  }
  return n;
}

bool Resolve(DecalsApi* a, std::uintptr_t* update_fn) {
  void* const h = dlopen("libr15.so", RTLD_NOW | RTLD_NOLOAD);
  if (h == nullptr) return false;
  auto S = [h](const char* name) { return dlsym(h, name); };
  a->ctor = reinterpret_cast<void (*)(void*)>(S("_ZN10NRadEngine16CDynamicMeshNodeC1Ev"));
  a->vertex_format = reinterpret_cast<DecalsVertexFormat (*)()>(S("_ZN10NRadEngine15SDynamicMeshVtx12VertexFormatEv"));
  a->init = reinterpret_cast<int (*)(void*, const DecalsVertexFormat*, const DecalsMeshInit*)>(
      S("_ZN10NRadEngine13CGDynamicMesh10InitializeERKNS_14CGVertexFormatERKNS_17SGDynamicMeshInitE"));
  a->map = reinterpret_cast<void* (*)(void*)>(S("_ZN10NRadEngine13CGDynamicMesh11MapVerticesEv"));
  a->unmap = reinterpret_cast<void (*)(void*)>(S("_ZN10NRadEngine13CGDynamicMesh13UnmapVerticesEv"));
  a->shutdown = reinterpret_cast<void (*)(void*)>(S("_ZN10NRadEngine16CDynamicMeshNode8ShutdownEv"));
  a->mem_current = reinterpret_cast<void* (*)()>(S("_ZN10NRadEngine14CMemoryContext7CurrentEv"));
  a->mem_set = reinterpret_cast<void (*)(void*)>(S("_ZN10NRadEngine14CMemoryContext10SetCurrentEPNS_10CAllocatorE"));
  a->mem_context = reinterpret_cast<void***>(S("_ZN10NRadEngine14CMemoryContext7contextE"));
  a->gs_initialized = reinterpret_cast<const std::uint32_t*>(S("_ZN10NRadEngine3CGS11initializedE"));
  *update_fn = reinterpret_cast<std::uintptr_t>(S(kUpdateSymbol));
  return true;
}

bool Complete(const DecalsApi& a) {
  return a.ctor && a.vertex_format && a.init && a.map && a.unmap && a.shutdown && a.mem_current && a.mem_set && a.mem_context &&
         a.gs_initialized;
}

// ---- the wrappers ----------------------------------------------------------------------------------------------------

void UpdateWrapper(std::uintptr_t sys) {
  const auto real = reinterpret_cast<void (*)(std::uintptr_t)>(g_real_update.load(std::memory_order_acquire));
  if (real != nullptr) real(sys);  // the game's own update first: the nodes are drawn from the state it leaves
  if (!g_on.load(std::memory_order_acquire)) return;
  pthread_mutex_lock(&g_mu);
  g_host.Tick(sys, NowMs());
  pthread_mutex_unlock(&g_mu);
}

// A virtual method: x0 = this, and the earlier wrapper or the real function's value is returned untouched.
std::uint64_t ShutdownWrapper(std::uint64_t self) {
  if (g_on.load(std::memory_order_acquire)) {
    pthread_mutex_lock(&g_mu);
    g_host.Shutdown(static_cast<std::uintptr_t>(self));  // our nodes go before the system does
    pthread_mutex_unlock(&g_mu);
  }
  const auto real = reinterpret_cast<std::uint64_t (*)(std::uint64_t)>(g_real_shutdown.load(std::memory_order_acquire));
  return real != nullptr ? real(self) : 0;  // not reachable: the slot is swapped only after g_real_shutdown is set
}

InstallResult Refused(const char* reason, SwapStatus status = SwapStatus::kNullSlot) {
  InstallResult r;
  r.status = status;
  r.reason = reason;
  return r;
}

}  // namespace

InstallResult InstallDecalsAt(std::uintptr_t base, std::uintptr_t update_slot, std::uintptr_t update_real, std::uintptr_t shutdown_slot,
                              std::uintptr_t shutdown_real, const char* table_path, const DecalsApi* api) {
  if (ModuleSwitchPresent("decals.off")) {
    const NevrLogField f[] = {{"why", "decals.off present in the q15t data directory"}};
    Log(NEVR_LOG_INFO, "decals off", f, 1);
    return Refused("off_file");
  }
  // A second install would reset the live state (the node records, the counters) and, when its slot check then refuses, switch the
  // module off: refuse before anything is touched, with the first install still running.
  if (g_on.load(std::memory_order_acquire)) {
    const NevrLogField f[] = {{"why", "decals is already installed; the running install is left as it is"}};
    Log(NEVR_LOG_WARNING, "decals refused: already installed", f, 1);
    return Refused("already_installed", SwapStatus::kUnexpectedOriginal);
  }
  // 1. the table
  char found[400] = "";
  char searched[1300] = "";
  std::uint8_t* bytes = nullptr;
  std::size_t len = 0;
  const char* why = "absent";
  const char* problem = nullptr;  // the first file that exists and could not be used: not the same as no file
  if (table_path != nullptr) {
    std::snprintf(searched, sizeof searched, "%s", table_path);
    if (ReadFile(table_path, &bytes, &len, &why)) {
      std::snprintf(found, sizeof found, "%s", table_path);
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
    const NevrLogField f[] = {{"searched", searched}, {"why", problem}};
    Log(NEVR_LOG_ERROR, "decals refused: a decals.bin exists but cannot be read, nothing is drawn", f, 2);
    return Refused("table_unreadable");
  }
  if (bytes == nullptr) {
    const NevrLogField f[] = {{"searched", searched}, {"why", why}};
    Log(NEVR_LOG_INFO, "decals off: no readable decals.bin (the module draws nothing without the content pack's table)", f, 2);
    return Refused("off");
  }
  const bool flip_u = ModuleSwitchPresent("decalflipu.on"), flip_v = ModuleSwitchPresent("decalflipv.on");
  Table table;
  const LoadResult lr = ParseTable(bytes, len, flip_u, flip_v, &table);
  delete[] bytes;
  if (lr != LoadResult::kOk || table.count == 0) {
    char l[24];
    const NevrLogField f[] = {{"path", found}, {"reason", lr != LoadResult::kOk ? LoadResultName(lr) : "no_groups"}, {"bytes", Dec(l, sizeof l, static_cast<long long>(len))}};
    Log(NEVR_LOG_ERROR, "decals refused: decals.bin is not a usable table, nothing is drawn", f, 3);
    FreeTable(&table);
    return Refused(lr != LoadResult::kOk ? "bad_table" : "empty_table");
  }
  // 2. the engine calls
  DecalsApi resolved;
  std::uintptr_t update_fn = 0;
  const DecalsApi* use = api;
  if (use == nullptr) {
    if (!Resolve(&resolved, &update_fn) || !Complete(resolved)) {
      const NevrLogField f[] = {{"path", found}};
      Log(NEVR_LOG_ERROR, "decals refused: libr15.so is not loaded or lacks an export the draw path needs (not the pinned build?)", f, 1);
      FreeTable(&table);
      return Refused("engine_export_missing");
    }
    use = &resolved;
    if (update_fn == 0) {
      const NevrLogField f[] = {{"symbol", kUpdateSymbol}};
      Log(NEVR_LOG_ERROR, "decals refused: CR15NetBulletCS::UpdateMovement is not exported (not the pinned build?)", f, 1);
      FreeTable(&table);
      return Refused("update_symbol_missing");
    }
    if (update_real == 0) update_real = update_fn != 0 ? update_fn - base : 0;
    if (update_fn != 0 && update_fn != base + update_real) {  // the pinned vaddr and the exported name must be one function
      char want[24], got[24];
      const NevrLogField f[] = {{"symbol", kUpdateSymbol}, {"exported_at", Hex(got, sizeof got, update_fn)},
                                {"pinned_at", Hex(want, sizeof want, base + update_real)}};
      Log(NEVR_LOG_ERROR, "decals refused: CR15NetBulletCS::UpdateMovement is not where the pin says (not the pinned build?)", f, 3);
      FreeTable(&table);
      return Refused("update_symbol_mismatch");
    }
  } else if (!Complete(*use)) {
    FreeTable(&table);
    return Refused("engine_export_missing");
  }
  if (update_real == 0) {
    const NevrLogField f[] = {{"symbol", kUpdateSymbol}};
    Log(NEVR_LOG_ERROR, "decals refused: CR15NetBulletCS::UpdateMovement is not exported, so its slot cannot be checked", f, 1);
    FreeTable(&table);
    return Refused("update_symbol_missing");
  }
  // 3. the state: set before either wrapper can run
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
  const InstallResult rs = InstallChainedImportHook(base, shutdown_slot, reinterpret_cast<std::uintptr_t>(&ShutdownWrapper), &g_real_shutdown,
                                                    shutdown_real);
  char sl[24], ss[24];
  {
    const NevrLogField f[] = {{"hook", "CR15NetBulletCS::Shutdown (vtable word, chained)"}, {"slot_vaddr", Hex(sl, sizeof sl, shutdown_slot)},
                              {"status", StatusName(rs.status)}, {"reason", rs.reason}};
    Log(rs.status == SwapStatus::kOk ? NEVR_LOG_INFO : NEVR_LOG_ERROR, "decals shutdown wrapper", f, 4);
  }
  if (rs.status != SwapStatus::kOk) {
    g_on.store(false, std::memory_order_release);
    return rs;
  }
  const InstallResult ru = InstallImportHook("CR15NetBulletCS::UpdateMovement", base, update_slot,
                                             reinterpret_cast<std::uintptr_t>(&UpdateWrapper), &g_real_update, update_real);
  {
    const NevrLogField f[] = {{"hook", "CR15NetBulletCS::UpdateMovement (data slot)"}, {"slot_vaddr", Hex(ss, sizeof ss, update_slot)},
                              {"status", StatusName(ru.status)}, {"reason", ru.reason}};
    Log(ru.status == SwapStatus::kOk ? NEVR_LOG_INFO : NEVR_LOG_ERROR, "decals update wrapper", f, 4);
  }
  if (ru.status != SwapStatus::kOk) {
    g_on.store(false, std::memory_order_release);  // the Shutdown wrapper stays in and passes through
    return ru;
  }
  char gcount[24], qcount[24], tcount[24], trail[24];
  const NevrLogField f[] = {{"path", found},
                            {"groups", Dec(gcount, sizeof gcount, g_table.count)},
                            {"quads", Dec(qcount, sizeof qcount, g_table.total_quads)},
                            {"groups_cut_to_160_quads", Dec(tcount, sizeof tcount, g_table.truncated_groups)},
                            {"trailing_bytes", Dec(trail, sizeof trail, static_cast<long long>(g_table.trailing_bytes))},
                            {"flip_u", flip_u ? "1" : "0"},
                            {"flip_v", flip_v ? "1" : "0"}};
  Log(NEVR_LOG_INFO, "decals on: the static level decals are drawn from the table in dynamic-mesh nodes", f, 7);
  InstallResult ok;
  ok.status = SwapStatus::kOk;
  ok.real_resolved = true;
  return ok;
}

InstallResult InstallDecals(std::uintptr_t base) {
  return InstallDecalsAt(base, kDecalsUpdateSlot, kDecalsUpdateReal, kDecalsShutdownSlot, kDecalsShutdownReal, nullptr, nullptr);
}

const Counters& DecalsCountersForTest() { return g_host.counters; }

void ResetDecalsForTest() {
  g_on.store(false);
  g_real_update.store(0);
  g_real_shutdown.store(0);
  FreeTable(&g_table);
  g_host = Host();
  g_api = DecalsApi();
  g_node_lines.store(0);
}

}  // namespace q15t
