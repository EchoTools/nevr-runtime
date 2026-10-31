#include "peerclamp.h"

#include <atomic>
#include <cstdio>
#include <cstring>

#include "plugin_log.h"
#include "safe_read.h"

namespace q15t {
namespace {

// SBroadcasterData and one peer entry, as CleanupPeers reads them (the source build's peerclamp:55-57; disassembly 0x24ef9d8 ldr w27,[x0,#0x554];
// 0x24efa68 ldr w8,[x19,#0x560]; 0x24efa6c ldr x9,[x19,#0x518]; 0x24efa74 ldr x24,[x19,#0x478]; 0x24efa90 ldr w27,[x8,#8];
// 0x24efa94 ldr x25,[x8,#0x228]; 0x24efa98 add x22,x8,#0x18; 0x24efa30 mov w28,#0x948; 0x24efa84 mov w2,#0x80).
constexpr std::uintptr_t kHead = 0x554, kPerChunk = 0x560, kChunks = 0x518, kNow = 0x478, kSent = 0x470;
constexpr std::uintptr_t kStride = 0x948, kNext = 0x8, kKey = 0x18, kStamp = 0x228;
constexpr std::size_t kKeyLen = 0x80;
constexpr std::size_t kBdSpan = kPerChunk + 4 - kSent, kEntrySpan = kStamp + 8 - kNext;
constexpr std::uint32_t kEnd = 0xffffffffu;
constexpr unsigned kMaxWalk = 4096, kClampLines = 16, kTotalEvery = 100, kTotalLines = 8;
constexpr const char kSymbol[] = "_ZN10NRadEngine16SBroadcasterData12CleanupPeersEv";

using CleanupFn = void (*)(void* bd);

std::atomic<std::uintptr_t> g_real{0};
std::atomic<std::uintptr_t> g_invalid{0};  // &SAddress::kInvalid, read at install
std::atomic<unsigned> g_clamps{0}, g_lines{0}, g_totals{0};
std::atomic<bool> g_capped{false};

void Clamp(std::uintptr_t bd) {
  // One read of the broadcaster fields (+0x470..+0x564). A failed read leaves the span zero (the fields are read from the lowest
  // address up, so a fault before `per` leaves per == 0): per == 0 ends the walk below.
  std::uint8_t b[kBdSpan] = {};
  (void)SafeRead(bd + kSent, b, sizeof b);
  std::uint32_t per, idx;
  std::uint64_t now, sent;
  std::uintptr_t chunks;
  std::memcpy(&sent, b + (kSent - kSent), sizeof sent);
  std::memcpy(&now, b + (kNow - kSent), sizeof now);
  std::memcpy(&chunks, b + (kChunks - kSent), sizeof chunks);
  std::memcpy(&idx, b + (kHead - kSent), sizeof idx);
  std::memcpy(&per, b + (kPerChunk - kSent), sizeof per);
  if (per == 0) return;
  std::uint8_t invalid[kKeyLen];
  if (!SafeRead(g_invalid.load(std::memory_order_relaxed), invalid, sizeof invalid)) return;
  for (unsigned n = 0; idx != kEnd; ++n) {
    if (n == kMaxWalk) {
      if (!g_capped.exchange(true, std::memory_order_relaxed)) {
        const KV f[] = {{"entries", static_cast<long long>(kMaxWalk)}};
        LogNums(NEVR_LOG_WARNING, "peerclamp walk capped", f, 1);
      }
      return;
    }
    // the chunk pointer; an unreadable one is 0 and the entry read below fails
    std::uintptr_t chunk = 0;
    (void)SafeRead(chunks + static_cast<std::uintptr_t>(idx / per) * sizeof(std::uintptr_t), &chunk, sizeof chunk);
    const std::uintptr_t e = chunk + static_cast<std::uintptr_t>(idx % per) * kStride;
    std::uint8_t entry[kEntrySpan];  // +0x8 .. +0x230: next, key, stamp
    if (!SafeRead(e + kNext, entry, sizeof entry)) return;
    std::uint32_t next;
    std::uint64_t stamp;
    std::memcpy(&next, entry + (kNext - kNext), sizeof next);
    std::memcpy(&stamp, entry + (kStamp - kNext), sizeof stamp);
    if (std::memcmp(entry + (kKey - kNext), invalid, kKeyLen) != 0 && stamp > now) {  // an entry the stock loop timeout-tests, stamped in the future
      *reinterpret_cast<std::uint64_t*>(e + kStamp) = now;
      const unsigned c = g_clamps.fetch_add(1, std::memory_order_relaxed) + 1;
      if (g_lines.fetch_add(1, std::memory_order_relaxed) < kClampLines) {
        char st[24], nw[24], d[24], sn[24], ix[16], cn[16];
        std::snprintf(st, sizeof st, "%llu", static_cast<unsigned long long>(stamp));
        std::snprintf(nw, sizeof nw, "%llu", static_cast<unsigned long long>(now));
        std::snprintf(d, sizeof d, "%llu", static_cast<unsigned long long>(stamp - now));
        std::snprintf(sn, sizeof sn, "%llu", static_cast<unsigned long long>(sent));
        std::snprintf(ix, sizeof ix, "%u", idx);
        std::snprintf(cn, sizeof cn, "%u", c);
        const NevrLogField f[] = {{"idx", ix}, {"stamp", st}, {"now", nw}, {"diff", d}, {"c470", sn}, {"clamp", cn}};
        Log(NEVR_LOG_INFO, "peerclamp pulled a peer's last-received stamp down to now", f, 6);
      } else if (c % kTotalEvery == 0 && g_totals.fetch_add(1, std::memory_order_relaxed) < kTotalLines) {
        const KV f[] = {{"clamps", static_cast<long long>(c)}};
        LogNums(NEVR_LOG_INFO, "peerclamp clamps so far", f, 1);
      }
    }
    idx = next;
  }
}

__attribute__((noinline)) void CleanupWrapper(void* bd) {
  Clamp(reinterpret_cast<std::uintptr_t>(bd));
  const auto real = reinterpret_cast<CleanupFn>(g_real.load(std::memory_order_acquire));
  if (real != nullptr) real(bd);
}

void Hex(char* out, std::size_t n, unsigned long long v) { std::snprintf(out, n, "0x%llx", v); }

}  // namespace

InstallResult InstallPeerClampAt(std::uintptr_t base, std::uintptr_t slot_vaddr, std::uintptr_t real_vaddr, std::uintptr_t invalid_vaddr) {
  InstallResult r;
  std::uintptr_t invalid = 0;
  if (!SafeRead(base + invalid_vaddr, &invalid, sizeof invalid) || invalid == 0) {
    char a[24];
    Hex(a, sizeof a, invalid_vaddr);
    const NevrLogField f[] = {{"why", "the kInvalid pointer is unreadable or null"}, {"invalid_vaddr", a}};
    Log(NEVR_LOG_ERROR, "peerclamp refused: nothing changed", f, 2);
    r.reason = "invalid_unreadable";
    return r;
  }
  g_invalid.store(invalid, std::memory_order_relaxed);
  r = InstallImportHook(kSymbol, base, slot_vaddr, reinterpret_cast<std::uintptr_t>(&CleanupWrapper), &g_real, real_vaddr);
  char slot[24], real[24];
  Hex(slot, sizeof slot, slot_vaddr);
  Hex(real, sizeof real, real_vaddr);
  const NevrLogField f[] = {{"slot_vaddr", slot}, {"real_vaddr", real}, {"status", StatusName(r.status)}, {"reason", r.reason}};
  if (r.status == SwapStatus::kOk)
    Log(NEVR_LOG_INFO, "peerclamp on (TEST ONLY): CleanupPeers pulls a peer's last-received stamp above now down to now", f, 4);
  else
    LogSlotRefusal("peerclamp refused: nothing changed (or restore failed, see status)", base, slot_vaddr, real_vaddr, r);
  return r;
}

bool PeerClampWordPinsMatch(std::uintptr_t base) { return CheckWordPins("peerclamp", base, kPeerClampWordPins, kPeerClampWordPinCount); }

InstallResult InstallPeerClamp(std::uintptr_t base) {
  if (!PeerClampWordPinsMatch(base)) {
    InstallResult r;
    r.reason = "word_pin_mismatch";
    return r;
  }
  return InstallPeerClampAt(base, kPeerClampSlot, kPeerClampReal, kPeerClampInvalid);
}

unsigned PeerClampCount() { return g_clamps.load(std::memory_order_relaxed); }

void ResetPeerClampForTest() {
  g_real.store(0);
  g_invalid.store(0);
  g_clamps = g_lines = g_totals = 0;
  g_capped = false;
}

}  // namespace q15t
