#include "boosterpool.h"

#include <atomic>
#include <cstdio>

#include "got_swap.h"
#include "import_hook.h"
#include "plugin_log.h"
#include "safe_read.h"
#include "word_pins.h"

namespace q15t {
namespace {

// ELF vaddrs of the pinned build (disassembly VA = ELF + 0x100000, as levelname.cpp; the GOT slot is from the disassembly's adrp target)
constexpr std::uintptr_t kSlot = 0x36cd1f8;       // GOT word (JUMP_SLOT) of the PLT stub at disassembly 0xfeb620 (AllocateExclusive calls it)
constexpr std::uintptr_t kFn = 0x18037c4;         // CncaGameSpace::AllocatePooledActor(CSymbol64) (disassembly 0x19037c4)
constexpr std::uintptr_t kAppend = 0xf31ea0;      // CSymbol64::Append (disassembly 0x1031ea0, a PLT stub)
constexpr std::uintptr_t kPlainName = 0x37af868;  // DAT_038af868: the booster pool actor's plain CSymbol64 (disassembly 0x38af868)
constexpr char kSym[] = "_ZN10NRadEngine13CncaGameSpace19AllocatePooledActorENS_9CSymbol64E";
constexpr char kSuffix[] = "_lowspec";
constexpr std::uint64_t kKeptLogFirst = 16;

// SActor: the 16-byte value AllocatePooledActor returns in x0:x1
struct ActorPair {
  std::uint64_t id;
  std::uint64_t gen;
};
using AllocFn = ActorPair (*)(void* st, std::uint64_t name);
using AppendFn = std::uint64_t (*)(std::uint64_t name, const char* suffix, std::uint32_t a, std::uint32_t b);

std::atomic<std::uintptr_t> g_real{0}, g_append{0}, g_plain_addr{0};
std::atomic<std::uint64_t> g_kept{0}, g_unreadable{0};
std::atomic<bool> g_installed{false};

void LogKept(std::uint64_t name, std::uint64_t n) {
  char nb[24], cb[24];
  std::snprintf(nb, sizeof nb, "0x%llx", static_cast<unsigned long long>(name));
  std::snprintf(cb, sizeof cb, "%llu", static_cast<unsigned long long>(n));
  const NevrLogField f[] = {{"name", nb}, {"n", cb}, {"answer", "plain name"}};
  Log(NEVR_LOG_INFO, "boosterpool actor kept its plain name", f, 3);
}

// this = st (the game space's ST), name = the CSymbol64 the caller asks for. Only the one name the stock builds under the lowspec
// bit is changed: the plain name, which is what the patched function asks for. The stock's Append is still run, the same way
// the stock runs it, so the answer is the same name the stock computes.
__attribute__((noinline)) ActorPair Wrapper(void* st, std::uint64_t name) {
  const auto real = reinterpret_cast<AllocFn>(g_real.load(std::memory_order_acquire));
  const auto append = reinterpret_cast<AppendFn>(g_append.load(std::memory_order_acquire));
  if (real == nullptr || append == nullptr) return ActorPair{~std::uint64_t{0}, 0};  // the game's own "no actor" value
  std::uint64_t plain = 0;
  if (!SafeRead(g_plain_addr.load(std::memory_order_acquire), &plain, sizeof plain)) {  // on any doubt the real call, untouched
    g_unreadable.fetch_add(1, std::memory_order_relaxed);
    return real(st, name);
  }
  const std::uint64_t suffixed = append(plain, kSuffix, 1, 1);
  if (name != suffixed) return real(st, name);
  const std::uint64_t n = g_kept.fetch_add(1, std::memory_order_relaxed) + 1;
  if (n <= kKeptLogFirst) LogKept(name, n);
  return real(st, plain);
}

}  // namespace

InstallResult InstallBoosterPool(std::uintptr_t base) {
  InstallResult out;
  if (base == 0) {
    out.reason = "no_base";
    const NevrLogField f[] = {{"why", "no_base"}};
    Log(NEVR_LOG_ERROR, "boosterpool refused", f, 1);
    return out;
  }
  if (g_installed.load(std::memory_order_acquire)) {
    out.status = SwapStatus::kUnexpectedOriginal;
    out.reason = "already_installed";
    const NevrLogField f[] = {{"why", "already_installed"}};
    Log(NEVR_LOG_WARNING, "boosterpool refused", f, 1);
    return out;
  }
  g_append.store(base + kAppend, std::memory_order_release);
  g_plain_addr.store(base + kPlainName, std::memory_order_release);
  const InstallResult r = InstallImportHook(kSym, base, kSlot, reinterpret_cast<std::uintptr_t>(&Wrapper), &g_real, kFn);
  if (r.status == SwapStatus::kOk || r.status == SwapStatus::kRestoreFailed) {
    g_installed.store(true, std::memory_order_release);  // the wrapper is in, even when the page protection was not put back
    char sb[24], fb[24], pb[24];
    std::snprintf(sb, sizeof sb, "0x%llx", static_cast<unsigned long long>(kSlot));
    std::snprintf(fb, sizeof fb, "0x%llx", static_cast<unsigned long long>(kFn));
    std::snprintf(pb, sizeof pb, "0x%llx", static_cast<unsigned long long>(kPlainName));
    if (r.status == SwapStatus::kRestoreFailed) {
      const NevrLogField f[] = {{"slot_vaddr", sb}, {"status", StatusName(r.status)}};
      Log(NEVR_LOG_ERROR, "boosterpool: on, but the slot page's old protection could not be restored", f, 2);
    } else {
      const NevrLogField f[] = {{"slot_vaddr", sb}, {"fn_vaddr", fb}, {"plain_name_vaddr", pb}};
      Log(NEVR_LOG_INFO, "boosterpool installed", f, 3);
    }
  } else {
    // a refused swap writes nothing (got_swap.h), so the stock allocation runs as it is
    g_append.store(0, std::memory_order_release);
    g_plain_addr.store(0, std::memory_order_release);
    LogSlotRefusal("boosterpool refused", base, kSlot, kFn, r);
  }
  return r;
}

std::uint64_t BoosterPoolKeptCount() { return g_kept.load(std::memory_order_relaxed); }
std::uint64_t BoosterPoolUnreadableCount() { return g_unreadable.load(std::memory_order_relaxed); }

std::uintptr_t BoosterPoolWrapperForTest() { return reinterpret_cast<std::uintptr_t>(&Wrapper); }
void BoosterPoolTargetsForTest(std::uintptr_t* real, std::uintptr_t* append, std::uintptr_t* plain_addr) {
  *real = g_real.load();
  *append = g_append.load();
  *plain_addr = g_plain_addr.load();
}
void SetBoosterPoolTargetsForTest(std::uintptr_t real, std::uintptr_t append, std::uintptr_t plain_addr) {
  g_real.store(real);
  g_append.store(append);
  g_plain_addr.store(plain_addr);
}
void ResetBoosterPoolForTest() {
  g_real.store(0);
  g_append.store(0);
  g_plain_addr.store(0);
  g_kept.store(0);
  g_unreadable.store(0);
  g_installed.store(false);
}

}  // namespace q15t
