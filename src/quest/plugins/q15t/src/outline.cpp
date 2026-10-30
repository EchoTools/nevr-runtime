#include "outline.h"

#include <dlfcn.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <new>

#include "module_switch.h"
#include "plugin_log.h"
#include "safe_read.h"

namespace q15t {
namespace {

using DdsFn = void (*)(std::uintptr_t, std::uint32_t, const void*, std::uint32_t);

constexpr std::uint64_t kDebugMeshLines = 32;  // per skinned mesh narrative, Debug, the first ones only

std::atomic<std::uintptr_t> g_real_scene{0};
std::uintptr_t g_base = 0;
OutlinePins g_pins{};
OutlineFns g_fns{};
bool g_noteam = true;  // set at install, before the slot is swapped; read only afterwards
std::atomic<bool> g_switch_logged{false};
std::atomic<std::uint64_t> g_calls{0}, g_skinned{0}, g_merged{0}, g_refused{0}, g_team{0};

// Interned merged formats: a pipeline built from one may keep its pointer, so they live for the process.
std::mutex g_merged_mu;
std::uint8_t* g_merged_set[kMergedMax] = {};
std::size_t g_merged_n = 0;
bool g_merged_full_logged = false;
std::atomic<bool> g_too_many_logged{false};

void Hex(char* out, std::size_t n, std::uint64_t v) { std::snprintf(out, n, "0x%llx", static_cast<unsigned long long>(v)); }
void Dec(char* out, std::size_t n, std::uint64_t v) { std::snprintf(out, n, "%llu", static_cast<unsigned long long>(v)); }

// True when the element is one the compute buffer supplies: stream 1, usage position (0), normal (2), tangent (3) or 0xb.
bool FromCompute(const std::uint8_t* e) {
  const std::uint8_t usage = e[0], stream = e[6];
  return stream == 1 && (usage == 0 || usage == 2 || usage == 3 || usage == 0xb);
}

// The format the main pass binds for a skinned mesh, built the way CGComputeVertexBuffer::Bind builds it: each element of the scene
// format in order, the compute-supplied ones replaced by the compute format's element of the same usage. nullptr: pass the scene format.
const void* MergedFormat(const void* scene_fmt, const void* compute_fmt) {
  const std::uint64_t n = g_fns.count_used(scene_fmt);
  if (n > kFormatElements) {  // AppendElement has no bound: a 37th element would overwrite the count
    if (!g_too_many_logged.exchange(true)) {
      char cnt[24];
      Dec(cnt, sizeof cnt, n);
      const NevrLogField f[] = {{"elements", cnt}, {"capacity", "36"}};
      Log(NEVR_LOG_WARNING, "outline: a skinned mesh's scene format has more elements than a format holds; its outline keeps the scene format", f, 2);
    }
    return nullptr;
  }
  alignas(16) std::uint8_t built[kFormatBytes + 0x40] = {};  // the game's constructor may touch past 0x128: headroom
  g_fns.fmt_ctor(built);
  for (std::uint64_t i = 0; i < n; ++i) {
    alignas(8) std::uint8_t e[16] = {};  // SVertexElement: 8 bytes
    g_fns.get_index(scene_fmt, i, e);
    if (FromCompute(e)) g_fns.get_usage(compute_fmt, e[0], e);
    g_fns.append(built, e);
  }
  std::lock_guard<std::mutex> lock(g_merged_mu);
  for (std::size_t i = 0; i < g_merged_n; ++i)
    if (std::memcmp(g_merged_set[i], built, kFormatBytes) == 0) return g_merged_set[i];
  if (g_merged_n >= kMergedMax) {
    if (!g_merged_full_logged) {
      const NevrLogField f[] = {{"interned", "256"}};
      Log(NEVR_LOG_WARNING, "outline: the merged-format store is full; later skinned meshes keep the scene format", f, 1);
      g_merged_full_logged = true;
    }
    return nullptr;
  }
  auto* keep = new (std::nothrow) std::uint8_t[kFormatBytes];
  if (keep == nullptr) return nullptr;
  std::memcpy(keep, built, kFormatBytes);
  g_merged_set[g_merged_n++] = keep;
  return keep;
}

template <typename T>
T At(std::uintptr_t addr) {  // a plain read, as the game's own walk makes it
  T v;
  std::memcpy(&v, reinterpret_cast<const void*>(addr), sizeof v);
  return v;
}

void LogRenderSwitch() {
  std::uint32_t v = 0;
  const bool ok = SafeRead(g_base + g_pins.render_switch, &v, sizeof v);
  char val[24];
  if (ok)
    Dec(val, sizeof val, v);
  else
    std::snprintf(val, sizeof val, "unreadable");
  const NevrLogField f[] = {{"render_switch", val}, {"meaning", "0 = the renderer made no outline pools, so no outline draws at all"}};
  Log(NEVR_LOG_INFO, "outline: renderer outline switch at the first scene outline call", f, 2);
}

void LogCount(std::uint64_t n) {
  char c[24], s[24], m[24], r[24], t[24];
  Dec(c, sizeof c, n);
  Dec(s, sizeof s, g_skinned.load(std::memory_order_relaxed));
  Dec(m, sizeof m, g_merged.load(std::memory_order_relaxed));
  Dec(r, sizeof r, g_refused.load(std::memory_order_relaxed));
  Dec(t, sizeof t, g_team.load(std::memory_order_relaxed));
  const NevrLogField f[] = {{"scene_calls", c}, {"skinned_meshes", s}, {"merged", m}, {"merge_refused", r}, {"team_outline_dropped", t}};
  Log(NEVR_LOG_INFO, "outline: skinned meshes got the main pass's vertex format, so their outlines draw", f, 5);
}

// CGScene::SetOutlineMode, rebuilt: the stock walk (ReVault libr15 0x19c5140..0x19c5228) with the skinned format and the team rule.
// Runs once per scene per outline change (model init and the model-outline script node), never per frame.
void SceneWrapper(std::uintptr_t scene, std::uint32_t mode) {
  const std::uint64_t call = g_calls.fetch_add(1, std::memory_order_relaxed) + 1;
  if (!g_switch_logged.exchange(true)) LogRenderSwitch();
  const auto dds_fn = reinterpret_cast<DdsFn>(g_base + g_pins.dds_fn);
  const std::uint64_t meshes = At<std::uint64_t>(scene + kSceneMeshCount);
  const auto res = At<std::uintptr_t>(scene + kSceneRes);
  for (std::uint64_t i = 0; i < meshes; ++i) {
    const std::uintptr_t mesh = At<std::uintptr_t>(scene + kSceneMeshes) + i * kMeshStride;
    const std::uint64_t sets = At<std::uint64_t>(mesh + kMeshDdsCount);
    if (sets == 0) continue;
    const std::uintptr_t row = At<std::uintptr_t>(res + kResRows) + i * kRowStride;
    const void* fmt = reinterpret_cast<const void*>(At<std::uintptr_t>(res + kResFormats) +
                                                    static_cast<std::uintptr_t>(At<std::uint32_t>(row + kRowFormat)) * kFormatStride);
    const std::uintptr_t prims =
        At<std::uintptr_t>(res + kResPrims) + static_cast<std::uintptr_t>(At<std::uint32_t>(row + kRowPrim)) * kPrimStride + kPrimType;
    const void* compute = reinterpret_cast<const void*>(mesh + kMeshComputeFmt);
    const bool skinned = At<std::uint32_t>(mesh + kMeshSkinned) != 0 && g_fns.count_used(compute) != 0;
    const void* use = fmt;
    std::uint32_t m = mode;
    if (skinned) {
      const std::uint64_t k = g_skinned.fetch_add(1, std::memory_order_relaxed);
      const void* merged = MergedFormat(fmt, compute);
      if (merged != nullptr) {
        use = merged;
        g_merged.fetch_add(1, std::memory_order_relaxed);
      } else {
        g_refused.fetch_add(1, std::memory_order_relaxed);
      }
      if (g_noteam && mode == kModeTeam) {
        m = kModeNone;
        g_team.fetch_add(sets, std::memory_order_relaxed);
      }
      if (k < kDebugMeshLines) {
        char mi[24], md[24], ms[24];
        Dec(mi, sizeof mi, i);
        Dec(md, sizeof md, mode);
        Dec(ms, sizeof ms, m);
        const NevrLogField f[] = {{"mesh", mi}, {"mode", md}, {"mode_passed", ms}, {"format", merged != nullptr ? "merged" : "scene"}};
        Log(NEVR_LOG_DEBUG, "outline: skinned mesh", f, 4);
      }
    }
    const std::uintptr_t first = At<std::uintptr_t>(mesh + kMeshDds);
    for (std::uint64_t k = 0; k < sets; ++k) dds_fn(first + k * kDdsStride, m, use, At<std::uint32_t>(prims + k * kPrimStride));
  }
  if ((call & (call - 1)) == 0) LogCount(call);  // 1, 2, 4, ... : a summary that thins out as scenes load
}

InstallResult Refuse(const char* reason, const char* detail, SwapStatus status) {
  InstallResult r;
  r.status = status;
  r.reason = reason;
  const NevrLogField f[] = {{"reason", reason}, {"detail", detail}, {"status", StatusName(status)}};
  Log(NEVR_LOG_ERROR, "outline refused: nothing changed, outlines stay stock", f, 3);
  return r;
}

}  // namespace

bool ResolveOutlineFns(std::uintptr_t base, OutlineFns* out) {
  void* h = dlopen("libr15.so", RTLD_NOW | RTLD_NOLOAD);
  if (h == nullptr) return false;
  void* ctor = dlsym(h, "_ZN10NRadEngine14CGVertexFormatC1Ev");
  void* count = dlsym(h, "_ZNK10NRadEngine14CGVertexFormat9CountUsedEv");
  void* get_i = dlsym(h, "_ZNK10NRadEngine14CGVertexFormat10GetElementEyRNS0_14SVertexElementE");
  void* get_u = dlsym(h, "_ZNK10NRadEngine14CGVertexFormat10GetElementENS0_6EUsageERNS0_14SVertexElementE");
  void* app = dlsym(h, "_ZN10NRadEngine14CGVertexFormat13AppendElementERKNS0_14SVertexElementE");
  if (!ctor || !count || !get_i || !get_u || !app) return false;
  // The four Bind calls (ReVault libr15 0x26bdcc8..0x26bdd28) reach these vaddrs; GetElement(EUsage) is resolved by name only.
  const auto at = [base](void* p, std::uintptr_t va) { return reinterpret_cast<std::uintptr_t>(p) == base + va; };
  if (!at(ctor, 0x2809098) || !at(count, 0x2809438) || !at(get_i, 0x28094a8) || !at(app, 0x280a4a8)) return false;
  out->fmt_ctor = reinterpret_cast<void (*)(void*)>(ctor);
  out->count_used = reinterpret_cast<std::uint64_t (*)(const void*)>(count);
  out->get_index = reinterpret_cast<void (*)(const void*, std::uint64_t, void*)>(get_i);
  out->get_usage = reinterpret_cast<void (*)(const void*, std::uint32_t, void*)>(get_u);
  out->append = reinterpret_cast<void (*)(void*, const void*)>(app);
  return true;
}

InstallResult InstallOutlineAt(std::uintptr_t base, const OutlinePins& pins, const WordPin* words, std::size_t n_words,
                               const OutlineFns& fns) {
  if (ModuleSwitchPresent("outline.off")) {
    const NevrLogField f[] = {{"why", "outline.off present in the q15t data directory"}};
    Log(NEVR_LOG_INFO, "outline off: skinned meshes keep the scene format and teammates keep the team outline", f, 1);
    InstallResult r;
    r.reason = "off_file";
    return r;
  }
  if (base == 0) return Refuse("no_base", "", SwapStatus::kNullSlot);
  if (!fns.fmt_ctor || !fns.count_used || !fns.get_index || !fns.get_usage || !fns.append)
    return Refuse("vertex_format_fns", "a CGVertexFormat function is missing or not at its pinned address", SwapStatus::kNullSlot);
  if (!CheckWordPins("outline", base, words, n_words, "nothing changed, outlines stay stock")) {
    InstallResult r;
    r.status = SwapStatus::kUnexpectedOriginal;
    r.reason = "word_pin";
    return r;
  }
  std::uintptr_t dds_word = 0;
  if (!SafeRead(base + pins.dds_slot, &dds_word, sizeof dds_word)) return Refuse("dds_slot_unreadable", "", SwapStatus::kUnmapped);
  if (dds_word != base + pins.dds_fn)
    return Refuse("dds_slot", "CGDrawDataSet::SetOutlineMode's slot does not hold the game's function", SwapStatus::kUnexpectedOriginal);

  g_base = base;
  g_pins = pins;
  g_fns = fns;
  g_noteam = !ModuleSwitchPresent("outline_noteam.off");
  const InstallResult r =
      InstallImportHook("_ZN10NRadEngine7CGScene14SetOutlineModeENS_16EMeshOutlineModeE", base, pins.scene_slot,
                        reinterpret_cast<std::uintptr_t>(&SceneWrapper), &g_real_scene, pins.scene_fn);
  char slot[24], real[24];
  Hex(slot, sizeof slot, pins.scene_slot);
  Hex(real, sizeof real, pins.scene_fn);
  const NevrLogField f[] = {{"event", "q15t_outline_install"}, {"hook", "CGScene::SetOutlineMode"}, {"slot_vaddr", slot},
                            {"real_vaddr", real}, {"status", StatusName(r.status)}, {"reason", r.reason},
                            {"skinned_format", "merged"}, {"team_outline", g_noteam ? "dropped on skinned meshes" : "kept (outline_noteam.off)"}};
  if (r.status != SwapStatus::kOk) {
    Log(NEVR_LOG_ERROR, "outline hook refused: outlines stay stock", f, 8);
    return r;
  }
  Log(NEVR_LOG_INFO, "outline on: scene outline walks give skinned meshes the main pass's format", f, 8);
  return r;
}

InstallResult InstallOutline(std::uintptr_t base) {
  OutlineFns fns{};
  if (!ResolveOutlineFns(base, &fns)) fns = OutlineFns{};  // InstallOutlineAt refuses an incomplete table, loudly
  return InstallOutlineAt(base, kOutlinePinned, kOutlineWalkWords, sizeof kOutlineWalkWords / sizeof kOutlineWalkWords[0], fns);
}

OutlineCounters OutlineGetCounters() {
  return {g_calls.load(), g_skinned.load(), g_merged.load(), g_refused.load(), g_team.load()};
}

void ResetOutlineForTest() {
  g_real_scene.store(0);
  g_switch_logged.store(false);
  g_calls = g_skinned = g_merged = g_refused = g_team = 0;
  std::lock_guard<std::mutex> lock(g_merged_mu);
  for (std::size_t i = 0; i < g_merged_n; ++i) delete[] g_merged_set[i];
  g_merged_n = 0;
  g_merged_full_logged = false;
  g_too_many_logged.store(false);
}

std::uintptr_t OutlineWrapperForTest() { return reinterpret_cast<std::uintptr_t>(&SceneWrapper); }

}  // namespace q15t
