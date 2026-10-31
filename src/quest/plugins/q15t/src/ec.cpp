#include "ec.h"

#include <dlfcn.h>
#include <link.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <stdarg.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstring>

#include "ec_core.h"
#include "ec_fld.h"
#include "got_swap.h"
#include "plugin_log.h"
#include "safe_read.h"

// The page lives in a hidden object; `re33_ec_page` is an exported alias of it, for the script libs' dlsym. Why not export the
// storage itself: a default-visibility symbol is preemptible, so if another object already exports `re33_ec_page` in the global
// scope (the source build's own libvrapi.so), every reference to it inside this .so would bind to THAT page through the GOT and the
// plugin would write into someone else's memory (src/quest/tests/q15t/ec_page_test.cpp, second run, showed exactly that). The hidden storage binds
// locally, always.
extern "C" {
alignas(4096) __attribute__((visibility("hidden"))) std::uint8_t q15t_ec_page_storage[4096] = {};
extern __attribute__((visibility("default"), alias("q15t_ec_page_storage"))) std::uint8_t re33_ec_page[4096];
}

namespace q15t {
namespace {

using namespace ecc;

constexpr int kLineCap = 48;
constexpr char kReloadSym[] = "_ZN10NRadEngine8NRadGame4NR1521ReloadEquipmentConfigEv";
constexpr char kUnloadSym[] = "_ZN10NRadEngine8NRadGame4NR1521UnloadEquipmentConfigEv";
constexpr char kWriteLogSym[] = "_ZN10NRadEngine9NWriteLog8WriteLogENS0_9ELogLevelEyPKcz";
constexpr char kWriteLogVSym[] = "_ZN10NRadEngine9NWriteLog9WriteLogVENS0_9ELogLevelEyPKcSt9__va_list";
// the source's presets ("On files ship by name only"): veil + translocator; + point; + overcharge. All store the selected tac
// mod and hide the list on a server without EC.
constexpr char kPlayWords[] = "types=VT equip hide";
constexpr char kPlayPWords[] = "types=VPT equip hide";
constexpr char kPlayOWords[] = "types=VPTO equip hide";

using RecvfromFn = EcRecvfromFn;
using SendtoFn = EcSendtoFn;
std::atomic<std::uintptr_t> g_recvfrom_real{0}, g_sendto_real{0};

pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
State g_st;  // under g_mu
std::atomic<std::uint64_t> g_sends{0}, g_passed{0}, g_consumed{0};
std::atomic<int> g_killLines{0}, g_tokenLines{0}, g_selLines{0};
std::atomic<void (*)(std::uint32_t)> g_droneKilled{nullptr};
constexpr int kNetTicks = 4;
std::atomic<void (*)()> g_netTick[kNetTicks] = {};
std::atomic<int> g_gameFd{-1};           // the game's own socket (from the server pick; -1 = none)
std::atomic<std::uint64_t> g_gameSrc{0}; // that server's address << 16 | port (0 = none)
std::atomic<std::uint32_t> g_gameTid{0}; // the thread its datagrams arrive on
double g_locTokenS = 0;
std::uintptr_t g_base = 0;
bool g_hideWrite = false;   // set before the thread starts
bool g_equipStore = false;  // set before the thread starts
bool g_listOn = false;      // both gate words swapped
bool g_noThread = false;    // test seam
char g_dir_override[256] = "";
bool g_installed = false;

// the selected tac mod: one WriteLog word per lib (0 libr15, 1 libpnsovr, 2 libpnsrad)
constexpr int kSelLibs = 3;
const char* const kSelLib[kSelLibs] = {"/libr15.so", "/libpnsovr.so", "/libpnsrad.so"};
// the result type stands for "whatever WriteLogV leaves in x0": WriteLog's body returns it untouched
using WriteLogVFn = std::uintptr_t (*)(int, unsigned long long, const char*, va_list);
std::atomic<WriteLogVFn> g_logV[kSelLibs] = {};
std::atomic<std::uintptr_t> g_wlReal[kSelLibs] = {};
bool g_selTried[kSelLibs] = {};
EcSelEnv g_env;

GearGate g_gear;
using VoidFn = void (*)();
std::atomic<VoidFn> g_gearReal[2] = {};

template <int I>
void GearWrap() {
  g_gear.Enter();
  const VoidFn real = g_gearReal[I].load(std::memory_order_acquire);
  if (real != nullptr) real();
  g_gear.Leave();
}

struct LockG {
  LockG() { pthread_mutex_lock(&g_mu); }
  ~LockG() { pthread_mutex_unlock(&g_mu); }
};

std::uint64_t (*g_clock)() = nullptr;  // test seam

std::uint64_t Now100() {  // the clock the lifted bodies read
  if (g_clock != nullptr) return g_clock();
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<std::uint64_t>(ts.tv_sec) * 10000000ull + static_cast<std::uint64_t>(ts.tv_nsec) / 100;
}
std::uint32_t Tid() { return static_cast<std::uint32_t>(syscall(SYS_gettid)); }

ssize_t SendtoWrap(int fd, const void* buf, size_t len, int flags, const sockaddr* to, socklen_t tolen) {
  if (to && tolen >= sizeof(sockaddr_in) && to->sa_family == AF_INET) {
    const sockaddr_in* a = reinterpret_cast<const sockaddr_in*>(to);
    LockG l;
    g_st.SawSend(fd, a->sin_addr.s_addr, a->sin_port);
  }
  g_sends.fetch_add(1, std::memory_order_relaxed);
  const auto real = reinterpret_cast<SendtoFn>(g_sendto_real.load(std::memory_order_acquire));
  return real(fd, buf, len, flags, to, tolen);
}

// C2 / C3: the real recvfrom; a datagram of exactly 16 bytes with both magics and a known type that was not peeked is ours and
// is NOT returned: the real recvfrom is called again with the same arguments (a non-blocking socket gives the game the next
// datagram or EAGAIN, a blocking one waits as the game's own call would). Everything else, MSG_PEEK, and a call with
// from != null and fromlen == null pass through untouched.
constexpr int kMaxConsumePerCall = 256;

ssize_t RecvfromWrap(int fd, void* buf, size_t len, int flags, sockaddr* from, socklen_t* fromlen) {
  const auto real = reinterpret_cast<RecvfromFn>(g_recvfrom_real.load(std::memory_order_acquire));
  for (int consumed = 0;; ++consumed) {
    sockaddr_storage own;
    socklen_t ownLen = sizeof own;
    sockaddr* f = from ? from : reinterpret_cast<sockaddr*>(&own);
    socklen_t* fl = from ? fromlen : &ownLen;
    const socklen_t saved = fl ? *fl : 0;
    const ssize_t n = real(fd, buf, len, flags, f, fl);
    Packet p;
    // len: a MSG_TRUNC call can return 16 into a shorter buffer
    // consumed: the datagrams are the network's, so one call never swallows more than kMaxConsumePerCall of them in a row; past
    // that the next one passes through to the game untouched (a flood cannot pin the game's thread in this loop)
    if ((flags & MSG_PEEK) || !fl || len < sizeof(Packet) || consumed >= kMaxConsumePerCall || !Ours(buf, static_cast<long>(n), &p)) {
      if (n >= 0) g_passed.fetch_add(1, std::memory_order_relaxed);
      // only the game's own socket = the game loop (CR15NetGame::UpdateBroadcaster), never another caller of recvfrom; and only
      // on the thread that received a datagram of the current server on it (an fd number can be reused)
      if (fd == g_gameFd.load(std::memory_order_relaxed)) {
        const std::uint32_t tid = Tid();
        if (n > 0 && fl && *fl >= sizeof(sockaddr_in) && f->sa_family == AF_INET) {
          const sockaddr_in* a = reinterpret_cast<const sockaddr_in*>(f);
          const std::uint64_t src = (static_cast<std::uint64_t>(a->sin_addr.s_addr) << 16) | a->sin_port;
          if (src == g_gameSrc.load(std::memory_order_relaxed)) g_gameTid.store(tid, std::memory_order_relaxed);
        }
        if (tid == g_gameTid.load(std::memory_order_relaxed))
          for (auto& t : g_netTick)
            if (void (*const tick)() = t.load(std::memory_order_acquire)) tick();
      }
      return n;
    }
    if (*fl >= sizeof(sockaddr_in) && f->sa_family == AF_INET) {
      const sockaddr_in* a = reinterpret_cast<const sockaddr_in*>(f);
      std::uint32_t killed;
      std::uint64_t tokensBefore, tokensAfter;
      {
        LockG l;
        tokensBefore = g_st.tokens;
        const std::uint64_t now = Now100();
        killed = g_st.Handle(q15t_ec_page_storage, p, a->sin_addr.s_addr, a->sin_port, now, now / 10000);
        tokensAfter = g_st.tokens;
      }
      if (killed) {  // outside the lock: the handler is another module's
        void (*const handler)(std::uint32_t) = g_droneKilled.load(std::memory_order_acquire);
        if (handler) handler(killed - 1);
        if (g_killLines.fetch_add(1) < 16)
          EcFld().N("drone", killed - 1).S("handler", handler ? "yes" : "none (dropped)").Emit(NEVR_LOG_INFO, "ec: K drone killed on the server");
      }
      if (tokensAfter != tokensBefore && g_tokenLines.fetch_add(1) < kLineCap) {
        char t[2] = {static_cast<char>(p.type), 0};
        EcFld().S("type", t).X("id", p.id).N("tokens", static_cast<long long>(tokensAfter)).Emit(NEVR_LOG_INFO, "ec: token received");
      }
    }
    *fl = saved;
    g_consumed.fetch_add(1, std::memory_order_relaxed);
  }
}

// Runs on whichever thread logs the profile update. No lock: two atomic page stores and capped lines.
void SawProfile(const char* json, int lib) {
  char name[24];
  const std::uint64_t sym = SelectedFromJson(json, name, sizeof name);
  if (!name[0]) return;  // a profile update without a tac mod
  if (sym && g_equipStore) StoreSelected(q15t_ec_page_storage, sym);
  if (g_selLines.fetch_add(1) < kLineCap)
    EcFld()
        .S("name", name)
        .X("sym", sym)
        .S("lib", kSelLib[lib] + 1)
        .S("result", !sym ? "not a known tac mod, nothing stored" : g_equipStore ? "stored at +e80" : "seen, NOT stored (noequip word)")
        .Emit(NEVR_LOG_INFO, "ec: sel");
}

// WriteLog's own body in C: va_start, WriteLogV of the same lib, the result passed through, plus one look at the format.
template <int I>
std::uintptr_t WriteLogWrap(int level, unsigned long long flags, const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  if (IsProfileFmt(fmt)) {
    va_list c;
    va_copy(c, ap);
    SawProfile(va_arg(c, const char*), I);
    va_end(c);
  }
  const WriteLogVFn v = g_logV[I].load(std::memory_order_acquire);
  const std::uintptr_t ret = v != nullptr ? v(level, flags, fmt, ap) : 0;  // v is set before the swap; null cannot happen
  va_end(ap);
  return ret;
}
using WriteLogFn = std::uintptr_t (*)(int, unsigned long long, const char*, ...);
const WriteLogFn kWriteLogWrap[kSelLibs] = {WriteLogWrap<0>, WriteLogWrap<1>, WriteLogWrap<2>};

const char* DataDir(char* out, std::size_t n) {
  if (g_dir_override[0] != 0) {
    std::snprintf(out, n, "%s", g_dir_override);
    return access(out, R_OK) == 0 ? out : nullptr;
  }
  char cmd[256] = "";
  FILE* f = std::fopen("/proc/self/cmdline", "re");
  if (f == nullptr) return nullptr;
  const std::size_t r = std::fread(cmd, 1, sizeof cmd - 1, f);
  std::fclose(f);
  cmd[r] = 0;
  if (char* colon = std::strchr(cmd, ':')) *colon = 0;
  if (std::strchr(cmd, '.') == nullptr || std::strchr(cmd, '/') != nullptr || std::strlen(cmd) > 160) return nullptr;
  std::snprintf(out, n, "/sdcard/Android/data/%s/files/q15t", cmd);
  return access(out, R_OK) == 0 ? out : nullptr;
}

bool FileExists(const char* dir, const char* name) {
  char p[400];
  std::snprintf(p, sizeof p, "%s/%s", dir, name);
  return access(p, F_OK) == 0;
}

// <dir>/ec.off (kill file: off, whatever else is there and whatever the build's default words are), else <dir>/ec.on (its first line =
// the TEST words), else ec_preset1.on / ec_preset2.on / ec_preset3.on, else the build's default words.
// false = off. `src` names what switched it on, or "ec.off" when the kill file switched it off.
bool Switch(char* words, std::size_t wordsLen, const char** src) {
  words[0] = 0;
  *src = "";
  char dir[256];
  if (DataDir(dir, sizeof dir) != nullptr) {
    if (FileExists(dir, "ec.off")) {
      *src = "ec.off";
      return false;
    }
    char path[400];
    std::snprintf(path, sizeof path, "%s/ec.on", dir);
    if (FILE* w = std::fopen(path, "re")) {
      if (!std::fgets(words, static_cast<int>(wordsLen), w)) words[0] = 0;
      std::fclose(w);
      *src = "ec.on";
      return true;
    }
    static const struct {
      const char* file;
      const char* words;
    } presets[] = {{"ec_preset1.on", kPlayWords}, {"ec_preset2.on", kPlayPWords}, {"ec_preset3.on", kPlayOWords}};
    for (const auto& p : presets)
      if (FileExists(dir, p.file)) {
        std::snprintf(words, wordsLen, "%s", p.words);
        *src = p.file;
        return true;
      }
  }
#ifdef Q15T_EC_DEFAULT_WORDS
  std::snprintf(words, wordsLen, "%s", Q15T_EC_DEFAULT_WORDS);
  *src = "build default";
  return true;
#else
  return false;
#endif
}

// ---- the selected tac mod, platform libs by name ----
struct FindArg {
  const char* tail;
  std::uintptr_t addr;
  EcLibInfo* out;
};
int FindLibCb(dl_phdr_info* info, size_t, void* data) {
  auto* l = static_cast<FindArg*>(data);
  std::uintptr_t dyn = 0;
  bool inside = false;
  for (int i = 0; i < info->dlpi_phnum; ++i) {
    const auto& ph = info->dlpi_phdr[i];
    if (ph.p_type == PT_DYNAMIC) dyn = info->dlpi_addr + ph.p_vaddr;
    if (ph.p_type == PT_LOAD && l->addr >= info->dlpi_addr + ph.p_vaddr && l->addr < info->dlpi_addr + ph.p_vaddr + ph.p_memsz) inside = true;
  }
  if (l->tail) {
    const char* slash = info->dlpi_name ? std::strrchr(info->dlpi_name, '/') : nullptr;
    if (!slash || std::strcmp(slash, l->tail) != 0) return 0;
  } else if (!inside) {
    return 0;
  }
  l->out->base = info->dlpi_addr;
  l->out->dyn = dyn;
  return 1;
}
bool FindLib(const char* tail, std::uintptr_t addr, EcLibInfo* out) {
  if (g_env.find_lib != nullptr) return g_env.find_lib(tail, addr, out);
  FindArg a{tail, addr, out};
  return dl_iterate_phdr(FindLibCb, &a) == 1;
}
bool NamedAs(std::uintptr_t fn, const char* sym) {
  if (g_env.named_as != nullptr) return g_env.named_as(fn, sym);
  Dl_info di{};
  return fn && dladdr(reinterpret_cast<void*>(fn), &di) && di.dli_sname && !std::strcmp(di.dli_sname, sym) &&
         reinterpret_cast<std::uintptr_t>(di.dli_saddr) == fn;
}

// Refusal of one lib: one WARNING line, that lib's word untouched; the lib counts as tried.
bool SelRefuse(int i, const char* why, std::uintptr_t word_val) {
  EcFld().S("lib", kSelLib[i] + 1).S("why", why).X("word", word_val).Emit(NEVR_LOG_WARNING, "ec: sel refused");
  return true;
}

// True = tried (installed or refused); false = the lib is not loaded yet.
bool HookSelectionByName(int i) {
  EcLibInfo lib;
  if (!FindLib(kSelLib[i], 0, &lib) || lib.base == 0) return false;
  const std::uintptr_t slot = lib.dyn ? JumpSlot(lib.base, lib.dyn, kWriteLogSym) : 0;
  if (!slot) return SelRefuse(i, "no WriteLog jump slot", 0);
  bool ok = true;
  const auto was = Rd<std::uintptr_t>(slot, &ok);
  if (!ok || !NamedAs(was, kWriteLogSym) || !IsWriteLogBody(was)) return SelRefuse(i, "the word is not a WriteLog of the known body", was);
  EcLibInfo owner;  // the lib whose WriteLog the word is bound to: its own WriteLogV word
  std::uintptr_t vword = 0;
  if (FindLib(nullptr, was, &owner) && owner.base && owner.dyn) {
    const std::uintptr_t vslot = JumpSlot(owner.base, owner.dyn, kWriteLogVSym);
    bool ok2 = true;
    if (vslot) vword = Rd<std::uintptr_t>(vslot, &ok2);
    if (!ok2) vword = 0;
  }
  if (!vword || !NamedAs(vword, kWriteLogVSym)) return SelRefuse(i, "the owner of its WriteLog has no WriteLogV word that holds a WriteLogV", was);
  g_logV[i].store(reinterpret_cast<WriteLogVFn>(vword), std::memory_order_release);
  const SwapResult r = SwapWord(slot, was, reinterpret_cast<std::uintptr_t>(kWriteLogWrap[i]));
  if (r.status != SwapStatus::kOk) {
    g_logV[i].store(nullptr, std::memory_order_release);
    EcFld().S("lib", kSelLib[i] + 1).S("status", StatusName(r.status)).Emit(NEVR_LOG_WARNING, "ec: sel refused (swap)");
    return true;
  }
  g_wlReal[i].store(was, std::memory_order_release);
  EcFld().S("lib", kSelLib[i] + 1).X("slot_off", slot - lib.base).S("mode", g_equipStore ? "equip: selections are stored at +e80" : "noequip: logged only")
      .Emit(NEVR_LOG_INFO, "ec: sel watching");
  return true;
}

// libr15: the pinned slot, content-checked (the body is the 25 words ReVault shows; WriteLogV's own slot holds WriteLogV).
InstallResult InstallSelectionPinned(std::uintptr_t base) {
  InstallResult r;
  bool ok = true;
  const auto vword = Rd<std::uintptr_t>(base + kEcSlotWriteLogV, &ok);
  if (!ok || vword != base + kEcFnWriteLogV) {
    r.status = SwapStatus::kUnexpectedOriginal;
    r.reason = "writelogv_slot_not_writelogv";
    return r;
  }
  if (!IsWriteLogBody(base + kEcFnWriteLog)) {
    r.status = SwapStatus::kUnexpectedOriginal;
    r.reason = "writelog_body_not_known";
    return r;
  }
  g_logV[0].store(reinterpret_cast<WriteLogVFn>(vword), std::memory_order_release);
  r = InstallImportHook("NRadEngine::NWriteLog::WriteLog", base, kEcSlotWriteLog, reinterpret_cast<std::uintptr_t>(kWriteLogWrap[0]), &g_wlReal[0],
                        kEcFnWriteLog);
  if (r.status != SwapStatus::kOk) g_logV[0].store(nullptr, std::memory_order_release);
  return r;
}

void ReportHook(const char* hook, const InstallResult& r) {
  EcFld().S("hook", hook).S("status", StatusName(r.status)).S("reason", r.reason).Emit(r.status == SwapStatus::kOk ? NEVR_LOG_INFO : NEVR_LOG_ERROR,
                                                                                    "ec: hook");
}

// Puts a word back: our wrapper -> the real function (checked swap). Used to make a pair "both or none".
void Undo(std::uintptr_t base, std::uintptr_t slot_vaddr, std::uintptr_t wrapper, std::uintptr_t real) {
  const SwapResult u = SwapWord(base + slot_vaddr, wrapper, real);
  if (u.status != SwapStatus::kOk)
    EcFld().X("slot", slot_vaddr).S("status", StatusName(u.status)).Emit(NEVR_LOG_ERROR, "ec: could not put a word back");
}

// recvfrom + sendto: both or none.
InstallResult InstallNet(std::uintptr_t base) {
  const auto rw = reinterpret_cast<std::uintptr_t>(&RecvfromWrap);
  const auto sw = reinterpret_cast<std::uintptr_t>(&SendtoWrap);
  InstallResult r1 = InstallImportHook("recvfrom", base, kEcSlotRecvfrom, rw, &g_recvfrom_real);
  ReportHook("recvfrom", r1);
  if (r1.status != SwapStatus::kOk) return r1;
  InstallResult r2 = InstallImportHook("sendto", base, kEcSlotSendto, sw, &g_sendto_real);
  ReportHook("sendto", r2);
  if (r2.status != SwapStatus::kOk) {
    Undo(base, kEcSlotRecvfrom, rw, g_recvfrom_real.load());
    g_recvfrom_real.store(0);
    return r2;
  }
  return r1;
}

// The hide's gate: Reload / Unload of the equipment config, both or none. False = the hide stays off.
bool InstallGearGate(std::uintptr_t base) {
  const auto w0 = reinterpret_cast<std::uintptr_t>(&GearWrap<0>);
  const auto w1 = reinterpret_cast<std::uintptr_t>(&GearWrap<1>);
  std::atomic<std::uintptr_t> real0{0}, real1{0};
  InstallResult a = InstallImportHook(kReloadSym, base, kEcSlotReloadEquip, w0, &real0, kEcFnReloadEquip);
  ReportHook("NR15::ReloadEquipmentConfig (gear gate)", a);
  if (a.status != SwapStatus::kOk) return false;
  g_gearReal[0].store(reinterpret_cast<VoidFn>(real0.load()), std::memory_order_release);
  InstallResult b = InstallImportHook(kUnloadSym, base, kEcSlotUnloadEquip, w1, &real1, kEcFnUnloadEquip);
  ReportHook("NR15::UnloadEquipmentConfig (gear gate)", b);
  if (b.status != SwapStatus::kOk) {
    Undo(base, kEcSlotReloadEquip, w0, real0.load());
    return false;
  }
  g_gearReal[1].store(reinterpret_cast<VoidFn>(real1.load()), std::memory_order_release);
  return true;
}

void SendPacket(const Dest& d, std::uint8_t type, std::uint32_t id) {
  const Packet p = {kMagic, type, {0, 0, 0}, id, kMagic2};
  sockaddr_in to{};
  to.sin_family = AF_INET;
  to.sin_addr.s_addr = d.addr;
  to.sin_port = d.port;
  const auto real = reinterpret_cast<SendtoFn>(g_sendto_real.load(std::memory_order_acquire));
  if (real != nullptr) real(d.sock, &p, sizeof p, 0, reinterpret_cast<const sockaddr*>(&to), sizeof to);
}

bool Pow2(std::uint32_t n) { return n && !(n & (n - 1)); }

// The source's Main loop locals.
struct Loop {
  std::uint32_t seenUse[4] = {};
  std::uint32_t seenSteps = 0, seenLocShown = 0, seenVeils = 0, seenBarriers = 0, seenSkips = 0;
  std::uint64_t nextPick = 0, nextHello = 0, nextSummary = 0, nextLocToken = 0, nextSel = 0, nextList = 0;
  TacList tacList;
  int listWas = 0;
  bool keepWas = false;
  std::uint64_t selWas = 0, selChangedMs = 0;
  Dest logged = {};
  bool answered = false, pageAns = false;
  int lines = 0, summaries = 0, locLines = 0;
  std::uint32_t pid = 0;
};
Loop g_loop;

void LoopInit() {
  g_loop = Loop();
  std::uint8_t* const page = q15t_ec_page_storage;
  for (int k = 0; k < 4; ++k) g_loop.seenUse[k] = Load32(page + UseOf(k));
  g_loop.seenSteps = Load32(page + kDroneBlock + kDroneSteps);
  g_loop.seenLocShown = Load32(page + kLocTok + 0x10);
  g_loop.seenVeils = Load32(page + kVeilTok + 0x10);
  g_loop.seenBarriers = Load32(page + kVeilTok + 0x14);
  g_loop.seenSkips = Load32(page + kEquipSkips);
  g_loop.pid = static_cast<std::uint32_t>(getpid());
}

void* Main(void*) {
  for (;;) {
    usleep(5000);
    EcStep(Now100());
  }
  return nullptr;
}

}  // namespace

std::uint8_t* EcPage() { return q15t_ec_page_storage; }

bool EcDataFileExists(const char* name) {
  char dir[256];
  return DataDir(dir, sizeof dir) != nullptr && FileExists(dir, name);
}

bool EcReadDataLine(const char* name, char* out, std::size_t n) {
  char dir[256];
  if (n == 0 || DataDir(dir, sizeof dir) == nullptr) return false;
  char path[400];
  std::snprintf(path, sizeof path, "%s/%s", dir, name);
  FILE* f = std::fopen(path, "re");
  if (f == nullptr) return false;
  out[0] = 0;
  if (std::fgets(out, static_cast<int>(n), f) == nullptr) out[0] = 0;
  std::fclose(f);
  if (char* nl = std::strpbrk(out, "\r\n")) *nl = 0;
  return true;
}

EcPageState PublishEcPage() {
  Dl_info di{};
  const char* path = dladdr(q15t_ec_page_storage, &di) && di.dli_fname ? di.dli_fname : nullptr;
  bool reopened = false;
  if (path != nullptr) {
    // the extra reference is kept for the process's life (the plugin is never unmapped)
    reopened = dlopen(path, RTLD_NOW | RTLD_NOLOAD | RTLD_GLOBAL) != nullptr;
  }
  void* const seen = dlsym(RTLD_DEFAULT, "re33_ec_page");
  const EcPageState st = seen == nullptr ? EcPageState::kInvisible : seen == static_cast<void*>(q15t_ec_page_storage) ? EcPageState::kVisible : EcPageState::kOther;
  char a[24], b[24];
  std::snprintf(a, sizeof a, "%p", static_cast<void*>(q15t_ec_page_storage));
  std::snprintf(b, sizeof b, "%p", seen);
  EcFld f;
  f.S("page", a).S("global_lookup", b).S("reopened_global", reopened ? "1" : "0")
      .S("state", st == EcPageState::kVisible ? "visible" : st == EcPageState::kInvisible ? "invisible" : "other")
      .X("rva_start", kPageRva).X("rva_end", kPageRva + kPageSize);
  if (st == EcPageState::kVisible)
    f.Emit(NEVR_LOG_INFO, "ec: page published (script libs find re33_ec_page by dlsym)");
  else if (st == EcPageState::kInvisible)
    f.Emit(NEVR_LOG_WARNING, "ec: page NOT visible to dlsym(RTLD_DEFAULT): EC scripts decide as on a PC without the EC network client");
  else
    f.Emit(NEVR_LOG_ERROR, "ec: another re33_ec_page is already global: the scripts use that one, not this plugin's");
  return st;
}

InstallResult InstallEc(std::uintptr_t base) {
  char words[256];
  const char* src = "";
  if (!Switch(words, sizeof words, &src)) {
    PublishEcPage();
    if (std::strcmp(src, "ec.off") == 0)
      EcFld().S("switch", "off").S("kill_file", "ec.off").Emit(NEVR_LOG_INFO, "ec: off (ec.off is in the data directory; the stock recvfrom/sendto stay in place)");
    else
      EcFld().S("switch", "off").S("hint", "ec.on / ec_preset1.on / ec_preset2.on / ec_preset3.on in the data directory").Emit(NEVR_LOG_INFO, "ec: off");
    InstallResult r;
    r.reason = "off";
    return r;
  }
  EcFld().S("switch", src).S("words", words).Emit(NEVR_LOG_INFO, "ec: on");
  return InstallEcWithWords(base, words);
}

InstallResult InstallEcWithWords(std::uintptr_t base, const char* words) {
  InstallResult out;
  if (g_installed) {
    out.reason = "already_installed";
    return out;
  }
  PublishEcPage();
  g_base = base;
  g_locTokenS = LocTokenFromWords(words);
  {
    LockG l;
    g_st.types = TypesFromWords(words);  // 'T' only unless the TEST word types=<VPTO letters>
  }
  g_hideWrite = WholeWord(words, "hide");
  g_equipStore = EquipFromWords(words);
  EcFld().X("types_mask", g_st.types).S("hide_write", g_hideWrite ? "1" : "0").S("equip_store", g_equipStore ? "1" : "0")
      .N("loctoken_s", static_cast<long long>(g_locTokenS)).Emit(NEVR_LOG_INFO, "ec: words");

  out = InstallNet(base);
  if (out.status != SwapStatus::kOk) {
    EcFld().S("why", out.reason[0] ? out.reason : StatusName(out.status)).Emit(NEVR_LOG_ERROR, "ec: refused: no net hook; no thread, the page stays zeroed");
    return out;
  }
  const InstallResult sel = InstallSelectionPinned(base);
  ReportHook("NWriteLog::WriteLog (selected tac mod, libr15)", sel);
  g_selTried[0] = true;  // refused or installed: not retried (the pinned word does not change)
  g_listOn = InstallGearGate(base);
  if (!g_listOn) EcFld().Emit(NEVR_LOG_WARNING, "ec: tac mods: gear gate not installed; the list is left alone");
  LoopInit();
  g_installed = true;
  EcFld().X("recvfrom_slot", kEcSlotRecvfrom).X("sendto_slot", kEcSlotSendto).N("loctoken_s", static_cast<long long>(g_locTokenS))
      .Emit(NEVR_LOG_INFO, "ec: on: recvfrom / sendto swapped; hello every 2 s");
  if (!g_noThread) {
    pthread_t th;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&th, &attr, Main, nullptr) != 0)
      EcFld().Emit(NEVR_LOG_ERROR, "ec: thread not started: the hooks count but nothing is sent");
    pthread_attr_destroy(&attr);
  }
  return out;
}

void EcStep(std::uint64_t now100) {
  Loop& L = g_loop;
  std::uint8_t* const page = q15t_ec_page_storage;
  const std::uint64_t nowMs = now100 / 10000;
  if (nowMs >= L.nextSel) {
    L.nextSel = nowMs + 1000;
    for (int i = 1; i < kSelLibs; ++i)
      if (!g_selTried[i]) g_selTried[i] = HookSelectionByName(i);
  }
  const std::uint32_t skips = Load32(page + kEquipSkips);
  if (skips != L.seenSkips) {
    L.seenSkips = skips;
    if (L.lines < kLineCap) {
      ++L.lines;
      EcFld().X("selected", Load64(page + kEquipSel)).N("skipped_so_far", skips).Emit(NEVR_LOG_INFO, "ec: a tac mod that is not the selected one was used: its node skipped");
    }
  }
  Use uses[4];
  Dest d{};
  bool haveDest, ans;
  int n;
  {
    LockG l;
    n = WatchUses(g_st, page, L.seenUse, static_cast<std::uint32_t>(nowMs), L.pid, now100, uses);
    if (nowMs >= L.nextPick) {
      L.nextPick = nowMs + 1000;
      g_st.PickServer();
    }
    haveDest = g_st.Server(&d);
    ans = g_st.Answered(nowMs);
  }
  g_gameSrc.store(haveDest ? (static_cast<std::uint64_t>(d.addr) << 16) | d.port : 0, std::memory_order_relaxed);
  g_gameFd.store(haveDest ? d.sock : -1, std::memory_order_relaxed);  // cleared when no server is picked
  for (int i = 0; i < n; ++i) {
    if (haveDest)
      for (int rep = 0; rep < 2; ++rep) SendPacket(d, uses[i].type, uses[i].id);  // twice, as upstream
    if (L.lines < kLineCap) {
      ++L.lines;
      char t[2] = {static_cast<char>(uses[i].type), 0};
      EcFld().S("type", t).X("id", uses[i].id).S("sent", haveDest ? "twice to the server" : "no (no server seen yet)").Emit(NEVR_LOG_INFO, "ec: used here");
    }
  }
  if (nowMs >= L.nextHello) {
    L.nextHello = nowMs + 2000;
    if (haveDest) {
      SendPacket(d, 'H', 0);
      if ((d.addr != L.logged.addr || d.port != L.logged.port) && L.lines < kLineCap) {
        ++L.lines;
        L.logged = d;
        char ip[24];
        const std::uint8_t* a = reinterpret_cast<const std::uint8_t*>(&d.addr);
        std::snprintf(ip, sizeof ip, "%u.%u.%u.%u", a[0], a[1], a[2], a[3]);
        EcFld().S("addr", ip).N("port", ntohs(d.port)).N("fd", d.sock).Emit(NEVR_LOG_INFO, "ec: server (the game's own connection)");
      }
    }
  }
  if (g_listOn && nowMs >= L.nextList) {
    L.nextList = nowMs + 250;
    std::uint64_t count = 0;
    int r = 0;
    // one of ours selected = the list stays whole (with the hide word only; +e80 is 0 with `noequip`)
    const std::uint64_t sel = Load64(page + kEquipSel);
    const bool keep = g_hideWrite && !ans && OurSelected(sel);
    if (sel != L.selWas) {
      L.selWas = sel;
      L.selChangedMs = nowMs;  // SFD-1: a cut waits till the selection is still
    }
    {
      bool ok = true;
      const auto list = Rd<std::uintptr_t>(g_base + kEcTacModList, &ok);
      g_gear.Lock();
      if (ok) r = TacModsFor(L.tacList, list, ans, g_hideWrite, &count, g_gear.gen.load(std::memory_order_relaxed), keep, HideSettled(nowMs, L.selChangedMs));
      g_gear.Unlock();
    }
    if (keep != L.keepWas) {
      L.keepWas = keep;
      if (L.lines < kLineCap) {
        ++L.lines;
        EcFld().X("selected", sel).Emit(NEVR_LOG_INFO, keep ? "ec: tac mods: an EC tac mod is selected: the list stays whole on this server"
                                                            : "ec: tac mods: no EC tac mod kept: the hide rule applies again");
      }
    }
    if (r != L.listWas) {
      L.listWas = r;
      if (r && L.lines < kLineCap) {
        ++L.lines;
        EcFld().S("server", r == 1 ? "EC" : "no EC")
            .S("effect", !g_hideWrite ? (r == 1 ? "would be shown again (no hide word, nothing written)" : "would be hidden (no hide word, nothing written)")
                         : r == 1 ? "shown" : r == 3 ? "shown again: one of them is selected" : "hidden")
            .N("tac_mods", static_cast<long long>(count))
            .Emit(NEVR_LOG_INFO, "ec: tac mods: smoke / buff / point");
      }
    }
  }
  if (ans != L.pageAns) {  // not under the line cap
    L.pageAns = ans;
    Store32(page + kQuestEcServer, ans ? 1 : 0);
  }
  if (ans != L.answered && L.lines < kLineCap) {
    ++L.lines;
    L.answered = ans;
    EcFld().Emit(NEVR_LOG_INFO, ans ? "ec: answer seen: the server has EC" : "ec: answer lost: no 'A' for 6 s");
  }
  if (g_locTokenS > 0 && nowMs >= L.nextLocToken) {
    const bool first = L.nextLocToken == 0;
    L.nextLocToken = nowMs + static_cast<std::uint64_t>(g_locTokenS * 1000);
    if (!first) {
      AddToken(page + kLocTok, now100);
      if (L.locLines < kLineCap) {
        ++L.locLines;
        EcFld().N("count", Load32(page + kLocTok)).Emit(NEVR_LOG_INFO, "ec: TEST loctoken: one LOC token added");
      }
    }
  }
  const std::uint32_t steps = Load32(page + kDroneBlock + kDroneSteps);
  if (steps != L.seenSteps) {
    if ((Pow2(steps) || steps < L.seenSteps) && L.lines < kLineCap) {
      ++L.lines;
      EcFld().N("steps", steps).Emit(NEVR_LOG_INFO, "ec: 221a prelude steps");
    }
    L.seenSteps = steps;
  }
  const std::uint32_t shown = Load32(page + kLocTok + 0x10);
  if (shown != L.seenLocShown) {
    L.seenLocShown = shown;
    if (L.locLines < kLineCap) {
      ++L.locLines;
      EcFld().N("shown", shown).Emit(NEVR_LOG_INFO, "ec: a translocator puck took a T token: effect shown");
    }
  }
  const std::uint32_t veils = Load32(page + kVeilTok + 0x10), barriers = Load32(page + kVeilTok + 0x14);
  if (veils != L.seenVeils || barriers != L.seenBarriers) {
    L.seenVeils = veils;
    L.seenBarriers = barriers;
    if (L.lines < kLineCap) {
      ++L.lines;
      EcFld().N("veil", veils).N("barrier", barriers).Emit(NEVR_LOG_INFO, "ec: wall decided");
    }
  }
  if (nowMs >= L.nextSummary) {
    L.nextSummary = nowMs + 10000;
    if (L.summaries < 60) {
      ++L.summaries;
      std::uint64_t tokens, dropped, kills, answers, medi;
      {
        LockG l;
        tokens = g_st.tokens;
        dropped = g_st.dropped;
        kills = g_st.kills;
        answers = g_st.answers;
        medi = g_st.medi;
      }
      EcFld f;
      f.N("sendto", static_cast<long long>(g_sends.load())).N("passed", static_cast<long long>(g_passed.load()))
          .N("consumed", static_cast<long long>(g_consumed.load())).N("A", static_cast<long long>(answers)).N("tokens", static_cast<long long>(tokens))
          .N("dropped", static_cast<long long>(dropped)).N("K", static_cast<long long>(kills)).S("server", haveDest ? "seen" : "none");
      f.Emit(NEVR_LOG_INFO, "ec: sum");
      (void)medi;
    }
  }
}

bool EcOnNetTick(void (*tick)()) {
  for (auto& t : g_netTick) {
    void (*empty)() = nullptr;
    if (t.compare_exchange_strong(empty, tick, std::memory_order_acq_rel)) return true;
  }
  return false;
}
void EcOnDroneKilled(void (*handler)(std::uint32_t)) { g_droneKilled.store(handler, std::memory_order_release); }

EcStats EcStatsNow() {
  EcStats s{};
  s.sends = g_sends.load();
  s.passed = g_passed.load();
  s.consumed = g_consumed.load();
  {
    LockG l;
    s.answers = g_st.answers;
    s.tokens = g_st.tokens;
    s.dropped = g_st.dropped;
    s.kills = g_st.kills;
    s.medi = g_st.medi;
    s.types = g_st.types;
    Dest d{};
    s.have_server = g_st.Server(&d);
    s.server_addr = d.addr;
    s.server_port = d.port;
    s.server_fd = s.have_server ? d.sock : -1;
    s.answered = g_st.Answered(Now100() / 10000);
  }
  s.hide_write = g_hideWrite;
  s.equip_store = g_equipStore;
  s.list_on = g_listOn;
  return s;
}

// ---- test seams ----
void SetEcLibcForTest(EcRecvfromFn recv, EcSendtoFn send) {
  g_recvfrom_real.store(reinterpret_cast<std::uintptr_t>(recv));
  g_sendto_real.store(reinterpret_cast<std::uintptr_t>(send));
}
ssize_t EcRecvfromForTest(int fd, void* buf, size_t len, int flags, sockaddr* from, socklen_t* fromlen) {
  return RecvfromWrap(fd, buf, len, flags, from, fromlen);
}
ssize_t EcSendtoForTest(int fd, const void* buf, size_t len, int flags, const sockaddr* to, socklen_t tolen) {
  return SendtoWrap(fd, buf, len, flags, to, tolen);
}
void SetEcNoThreadForTest(bool on) { g_noThread = on; }
void SetEcClockForTest(std::uint64_t (*clock100)()) { g_clock = clock100; }
void SetEcDirForTest(const char* dir) { std::snprintf(g_dir_override, sizeof g_dir_override, "%s", dir != nullptr ? dir : ""); }
void SetEcLogVForTest(int lib, std::uintptr_t fn) { g_logV[lib].store(reinterpret_cast<WriteLogVFn>(fn)); }
std::uintptr_t EcWriteLogWrapForTest(int lib) { return reinterpret_cast<std::uintptr_t>(kWriteLogWrap[lib]); }
std::uintptr_t EcGearWrapForTest(int which) {
  return which == 0 ? reinterpret_cast<std::uintptr_t>(&GearWrap<0>) : reinterpret_cast<std::uintptr_t>(&GearWrap<1>);
}
void SetEcGearRealForTest(int which, void (*fn)()) { g_gearReal[which].store(fn); }
void SetEcSelEnvForTest(const EcSelEnv& env) { g_env = env; }
bool EcSelTriedForTest(int lib) { return g_selTried[lib]; }
std::uint32_t EcGearGenForTest() { return g_gear.gen.load(); }
void EcResetForTest() {
  {
    LockG l;
    g_st = State();
  }
  std::memset(q15t_ec_page_storage, 0, sizeof q15t_ec_page_storage);
  g_sends = g_passed = g_consumed = 0;
  g_killLines = g_tokenLines = g_selLines = 0;
  g_droneKilled = nullptr;
  for (auto& t : g_netTick) t = nullptr;
  g_gameFd = -1;
  g_gameSrc = 0;
  g_gameTid = 0;
  g_locTokenS = 0;
  g_base = 0;
  g_hideWrite = g_equipStore = g_listOn = false;
  g_installed = false;
  g_recvfrom_real = 0;
  g_sendto_real = 0;
  for (int i = 0; i < kSelLibs; ++i) {
    g_logV[i] = nullptr;
    g_wlReal[i] = 0;
    g_selTried[i] = false;
  }
  g_env = EcSelEnv();
  g_gearReal[0] = nullptr;
  g_gearReal[1] = nullptr;
  g_loop = Loop();
}

}  // namespace q15t
