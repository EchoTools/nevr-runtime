#include "menu_cards_hook.h"

#include <atomic>
#include <cstddef>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "got_hook.h"
#include "hook_install.h"
#include "hook_log.h"
#include "friends_panel_swap.h"
#include "menu_cards_swap.h"
#include "netgun_bullets_swap.h"
#include "outside_game_call.h"
#include "pinned_targets.h"
#include "quest/data/sha256.h"
#include "runtime/patch/menu_cards.h"
#include "runtime/patch/netgun_bullets.h"

namespace nevr_quest::menu_cards {

namespace {

using Thunk = sentinel::pinned::LibR15AsyncResourceIOCallbackThunk;

sentinel::GotHook g_hook;
std::atomic<void* const*> g_contextSlot{nullptr};
std::atomic<const char*> g_pin{nevr_menu_cards::kQuestStockPageSha256};
std::atomic<unsigned long long> g_deliveries{0};
std::atomic<unsigned long long> g_pages{0};
std::atomic<unsigned long long> g_swapped{0};
std::atomic<PictureServer> g_pictureServer{nullptr};

// Two bounded diagnostics over finished primary buffers. A key is claimed by fetch_add; two threads racing on the same new
// key can each log it once, which only costs a line.
//   classes: the first delivery of each resource class (kClassLines of them), so a run shows what the Quest calls its
//            resource classes whatever else is loaded around them;
//   band:    the first delivery of each (class, name) whose primary buffer is as large as a UI page, so the page shows up
//            under whatever name and class the Quest gives it.
struct SeenPair {
  std::atomic<std::uint64_t> classSymbol{0};
  std::atomic<std::uint64_t> nameSymbol{0};
};
SeenPair g_seenClasses[kClassLines];
std::atomic<unsigned> g_seenClassCount{0};
SeenPair g_seenBand[kBandLines];
std::atomic<unsigned> g_seenBandCount{0};

// What FreeResource will release buffer 0 through: the allocator at context + 0x10, or null before the game has one.
void* ReleaseAllocator() noexcept {
  void* const* const slot = g_contextSlot.load(std::memory_order_acquire);
  if (slot == nullptr) return nullptr;
  const unsigned char* const context = static_cast<const unsigned char*>(*slot);
  if (context == nullptr) return nullptr;
  return Load<void*>(context, kContextAllocatorOffset);
}

// True the first time `key` is offered (while the table has room), recording it.
bool Claim(SeenPair* table, std::atomic<unsigned>& count, unsigned capacity, const Delivery& d, bool byClassOnly) noexcept {
  const unsigned known = count.load(std::memory_order_acquire);
  for (unsigned i = 0; i < known && i < capacity; ++i) {
    if (table[i].classSymbol.load(std::memory_order_relaxed) == d.classSymbol &&
        (byClassOnly || table[i].nameSymbol.load(std::memory_order_relaxed) == d.nameSymbol)) {
      return false;
    }
  }
  const unsigned slot = count.fetch_add(1, std::memory_order_acq_rel);
  if (slot >= capacity) return false;
  table[slot].classSymbol.store(d.classSymbol, std::memory_order_relaxed);
  table[slot].nameSymbol.store(d.nameSymbol, std::memory_order_relaxed);
  return true;
}

void NoteResource(const Delivery& d) noexcept {
  char classHex[19], nameHex[19];
  if (Claim(g_seenClasses, g_seenClassCount, kClassLines, d, /*byClassOnly=*/true)) {
    sentinel::LogFields(sentinel::LogLevel::kInfo, "menu_cards_class",
                        {{"class", sentinel::HexString(classHex, d.classSymbol)},
                         {"name", sentinel::HexString(nameHex, d.nameSymbol)},
                         {"bytes", d.size}});
  }
  if (d.size >= kBandMin && d.size <= kBandMax &&
      Claim(g_seenBand, g_seenBandCount, kBandLines, d, /*byClassOnly=*/false)) {
    sentinel::LogFields(sentinel::LogLevel::kInfo, "menu_cards_resource",
                        {{"class", sentinel::HexString(classHex, d.classSymbol)},
                         {"name", sentinel::HexString(nameHex, d.nameSymbol)},
                         {"bytes", d.size}});
  }
}

// The page served for the pinned stock page: built once, kept for the life of the process (the game keeps reading
// what it was given, and a later load copies from here again). Heap objects that are never freed, so no static
// destructor runs under a game thread.
struct Cache {
  std::mutex mutex;
  std::string stockSha256;
  std::vector<unsigned char> bytes;
};
std::atomic<Cache*> g_cache{nullptr};

Cache& TheCache() noexcept {
  Cache* c = g_cache.load(std::memory_order_acquire);
  if (c != nullptr) return *c;
  Cache* const fresh = new Cache();
  if (g_cache.compare_exchange_strong(c, fresh, std::memory_order_acq_rel)) return *fresh;
  delete fresh;
  return *c;
}

}  // namespace

struct Served {
  const unsigned char* bytes = nullptr;
  std::size_t size = 0;
  const char* why = "none";  // a fixed token when bytes is null
};

// Hashes the delivered page, compares it with the pin and builds the page with the cards. Runs before any game code
// (the handler calls the game's allocator only after it returns) and calls none: it may allocate, and carries a
// personality (outside_game_call.h).
NEVR_OUTSIDE_GAME_CALL Served PrepareCards(const unsigned char* page, std::size_t size, std::uint64_t classSymbol,
                                           std::uint64_t nameSymbol) noexcept {
  const std::string sha = nevr_quest::data::Sha256Hex(page, size);
  const char* const pin = g_pin.load(std::memory_order_acquire);
  const char* const pinState = pin[0] == '\0' ? "unset" : (sha == pin ? "match" : "mismatch");
  char classHex[19], nameHex[19];
  sentinel::LogFields(sentinel::LogLevel::kInfo, "menu_cards_page",
                      {{"class", sentinel::HexString(classHex, classSymbol)},
                       {"name", sentinel::HexString(nameHex, nameSymbol)},
                       {"bytes", size},
                       {"sha256", sha.c_str()},
                       {"pin", pinState}});

  Cache& cache = TheCache();
  std::lock_guard<std::mutex> lock(cache.mutex);
  if (!cache.bytes.empty() && cache.stockSha256 == sha) return {cache.bytes.data(), cache.bytes.size(), "none"};

  const nevr_menu_cards::Result r = nevr_menu_cards::Patch(page, size, sha, pin, nevr_menu_cards::Cards());
  if (r.verdict == nevr_menu_cards::Verdict::NotStock) return {nullptr, 0, pin[0] == '\0' ? "pin_unset" : "page_not_pinned"};
  if (r.verdict == nevr_menu_cards::Verdict::Refused) return {nullptr, 0, nevr_menu_page::StatusName(r.status)};
  cache.bytes = r.bytes;
  cache.stockSha256 = sha;
  return {cache.bytes.data(), cache.bytes.size(), "none"};
}

namespace {

// The net-gun resources of the five combat levels (#1478, the Quest twin of #1420). The PC rewrite decides the bytes: a
// pinned stock buffer is served with its rows and slots added, through the same allocator check as the page. Any other
// buffer is left alone and says why (netgun_bullets_skip). Nothing is cached: SwapIn copies the served bytes into the
// game's own buffer, so the rewrite's vector is not needed after this returns. As for the page, the game's original
// buffer is not freed here.
void OfferNetgun(unsigned char* data, const Delivery& d, nevr_netgun_bullets::Resource res) noexcept {
  const std::size_t size = static_cast<std::size_t>(d.size);
  const std::string sha = nevr_quest::data::Sha256Hex(d.buffer, size);
  const nevr_netgun_bullets::Result r = nevr_netgun_bullets::Decide(res, d.buffer, size, sha, nevr_netgun_bullets::StockPins(res));
  char nameHex[19];
  const char* const resource = nevr_netgun_bullets::ResourceName(res);
  const char* const name = sentinel::HexString(nameHex, d.nameSymbol);
  if (r.verdict == nevr_netgun_bullets::Verdict::NotStock) {
    sentinel::LogFields(sentinel::LogLevel::kInfo, "netgun_bullets_skip",
                        {{"resource", resource}, {"name", name}, {"bytes", d.size}, {"sha256", sha.c_str()}, {"reason", "not_pinned"}});
    return;
  }
  if (r.verdict == nevr_netgun_bullets::Verdict::Refused) {
    sentinel::LogFields(sentinel::LogLevel::kWarn, "netgun_bullets_skip",
                        {{"resource", resource}, {"name", name}, {"bytes", d.size}, {"reason", nevr_netgun_bullets::StatusName(r.status)}});
    return;
  }
  const Swap swap = SwapIn(data, r.bytes.data(), r.bytes.size(), ReleaseAllocator());
  if (swap != Swap::Swapped) {
    sentinel::LogFields(sentinel::LogLevel::kWarn, "netgun_bullets_skip",
                        {{"resource", resource}, {"name", name}, {"bytes", d.size}, {"reason", SwapName(swap)}});
    return;
  }
  sentinel::LogFields(sentinel::LogLevel::kInfo, "netgun_bullets_swap",
                      {{"resource", resource}, {"name", name}, {"bytes_before", d.size}, {"bytes_after", r.bytes.size()},
                       {"rows_before", r.entriesBefore}, {"rows_after", r.entriesAfter}});
}

// The FRIENDS panel's resources (#1535, the Quest twin of the PC transforms): built from the delivered bytes by the
// shared builders, before any game code runs (it allocates; outside_game_call.h). False, with the reason, when refused.
NEVR_OUTSIDE_GAME_CALL bool PrepareFriends(friends_panel::Kind kind, const unsigned char* data, std::size_t size,
                                           std::vector<std::uint8_t>* out, std::string* why) noexcept {
  return friends_panel::Build(kind, data, size, out, why);
}

// Serves the panel's resource in place of the delivered one. As for the net-gun resources nothing is cached: SwapIn copies
// the served bytes into a buffer from the game's own allocator, and the game's original buffer is not freed here.
void OfferFriends(unsigned char* data, const Delivery& d, friends_panel::Kind kind) noexcept {
  std::vector<std::uint8_t> built;
  std::string why;
  if (!PrepareFriends(kind, d.buffer, static_cast<std::size_t>(d.size), &built, &why)) {
    sentinel::LogFields(sentinel::LogLevel::kWarn, "friends_panel_skip",
                        {{"resource", friends_panel::KindName(kind)}, {"bytes", d.size}, {"reason", why.c_str()}});
    return;
  }
  const Swap swap = SwapIn(data, built.data(), built.size(), ReleaseAllocator());
  if (swap != Swap::Swapped) {
    sentinel::LogFields(sentinel::LogLevel::kWarn, "friends_panel_skip",
                        {{"resource", friends_panel::KindName(kind)}, {"bytes", d.size}, {"reason", SwapName(swap)}});
    return;
  }
  sentinel::LogFields(sentinel::LogLevel::kInfo, "friends_panel_swap",
                      {{"resource", friends_panel::KindName(kind)}, {"bytes_before", d.size}, {"bytes_after", built.size()}});
}

// Runs before the game's callback, on whichever thread finished the read. Never throws (the unit has no exceptions).
void Offer(unsigned char* data) noexcept {
  g_deliveries.fetch_add(1, std::memory_order_relaxed);
  if (data == nullptr) return;
  const Delivery d = Inspect(data);
  // A FRIENDS panel picture is copied over its slot's payload, whichever buffer carries it (#1535).
  const PictureServer pictures = g_pictureServer.load(std::memory_order_acquire);
  if (pictures != nullptr && d.status == block::kStatusDone && d.buffer != nullptr && d.size != 0 &&
      pictures(d.nameSymbol, d.buffer, d.size)) {
    return;
  }
  if (!IsFinishedPrimary(d)) return;
  NoteResource(d);
  nevr_netgun_bullets::Resource netgun = nevr_netgun_bullets::Resource::Particles;
  if (netgun_bullets::IsNetgunDelivery(d, &netgun)) {
    OfferNetgun(data, d, netgun);
    return;
  }
  const friends_panel::Kind friends = friends_panel::Classify(d);
  if (friends != friends_panel::Kind::kNone) {
    OfferFriends(data, d, friends);
    return;
  }
  // The page is the CR15UIPage2CR resource under mnu_master_lowspec. The name is also the main-menu level's, so about sixty
  // resources of other classes arrive under it (headset run 2): only the class picks the page out, and the rest are
  // neither touched nor logged as pages.
  if (d.classSymbol != nevr_menu_cards::kPageType || d.nameSymbol != nevr_menu_cards::kQuestPageName) return;
  g_pages.fetch_add(1, std::memory_order_relaxed);
  if (d.size > nevr_menu_page::kMaxResourceBytes) {
    sentinel::LogFields(sentinel::LogLevel::kWarn, "menu_cards_skip", {{"reason", "page_too_large"}, {"bytes", d.size}});
    return;
  }
  const Served served = PrepareCards(d.buffer, static_cast<std::size_t>(d.size), d.classSymbol, d.nameSymbol);
  if (served.bytes == nullptr) {
    sentinel::LogFields(sentinel::LogLevel::kInfo, "menu_cards_skip", {{"reason", served.why}});
    return;
  }
  const Swap swap = SwapIn(data, served.bytes, served.size, ReleaseAllocator());
  if (swap != Swap::Swapped) {
    sentinel::LogFields(sentinel::LogLevel::kWarn, "menu_cards_skip", {{"reason", SwapName(swap)}});
    return;
  }
  g_swapped.fetch_add(1, std::memory_order_relaxed);
  sentinel::LogFields(sentinel::LogLevel::kInfo, "menu_cards_swap",
                      {{"result", "swapped"}, {"bytes_before", d.size}, {"bytes_after", served.size}});
}

// CResourceManager::AsyncResourceIOCallback. The cards go in first; the game's callback always runs, on the block as it
// is then, and its result is returned as it came.
std::uint64_t Handler(Thunk::Fn original, unsigned char* data) noexcept {
  Offer(data);
  return original(data);
}

NEVR_HOOK_RECORD(kHook, Thunk, &Handler);

}  // namespace

void SetPictureServer(PictureServer server) noexcept { g_pictureServer.store(server, std::memory_order_release); }

bool Install() noexcept {
  sentinel::ElfImage image;
  const bool found = sentinel::FindLoadedImage(sentinel::pinned::kLibR15, &image);
  if (found) {
    g_contextSlot.store(reinterpret_cast<void* const*>(image.base + sentinel::pinned::kMemoryContextVaddr),
                        std::memory_order_release);
  }
  Thunk::Arm(kHook);
  const sentinel::GotStatus status = sentinel::InstallThunk<Thunk>(g_hook, sentinel::pinned::LibR15AsyncResourceIOCallback());
  const bool ok = status == sentinel::GotStatus::kOk;
  char baseHex[19];
  sentinel::LogFields(ok ? sentinel::LogLevel::kInfo : sentinel::LogLevel::kError, "menu_cards_install",
                      {{"result", ok ? "installed" : "failed"},
                       {"slot", sentinel::GotStatusName(status)},
                       {"pin", nevr_menu_cards::kQuestStockPageSha256[0] == '\0' ? "unset" : "set"},
                       {"load_base", sentinel::HexString(baseHex, found ? static_cast<std::uint64_t>(image.base) : 0)}});
  return ok;
}

void ArmForTest(const char* pinHex, void* const* contextSlot) noexcept {
  g_pin.store(pinHex, std::memory_order_release);
  g_contextSlot.store(contextSlot, std::memory_order_release);
  Thunk::Arm(kHook);
}

void ResetForTest() noexcept {
  g_pictureServer.store(nullptr, std::memory_order_release);
  g_deliveries.store(0, std::memory_order_relaxed);
  g_pages.store(0, std::memory_order_relaxed);
  g_swapped.store(0, std::memory_order_relaxed);
  g_seenClassCount.store(0, std::memory_order_relaxed);
  g_seenBandCount.store(0, std::memory_order_relaxed);
  for (SeenPair& p : g_seenClasses) {
    p.classSymbol.store(0, std::memory_order_relaxed);
    p.nameSymbol.store(0, std::memory_order_relaxed);
  }
  for (SeenPair& p : g_seenBand) {
    p.classSymbol.store(0, std::memory_order_relaxed);
    p.nameSymbol.store(0, std::memory_order_relaxed);
  }
  g_pin.store(nevr_menu_cards::kQuestStockPageSha256, std::memory_order_release);
  g_contextSlot.store(nullptr, std::memory_order_release);
  Cache* const old = g_cache.exchange(nullptr, std::memory_order_acq_rel);
  delete old;  // tests only: no game thread is running
}

Totals CurrentTotals() noexcept {
  return {g_deliveries.load(std::memory_order_relaxed), g_pages.load(std::memory_order_relaxed),
          g_swapped.load(std::memory_order_relaxed)};
}

}  // namespace nevr_quest::menu_cards
