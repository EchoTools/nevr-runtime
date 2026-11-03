#include "got_hook.h"

#include <fcntl.h>
#include <link.h>  // dl_iterate_phdr, struct dl_phdr_info
#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>

#include "core/hook_lifecycle.h"
#include "hook_budget.h"
#include "hook_log.h"

namespace sentinel {

namespace {

constexpr std::size_t kSlotSize = sizeof(void*);
static_assert(sizeof(Elf64_Rela) == 24, "ELF64 RELA entry layout");

// ---- process-wide lock, the chain registry, store observer ----------------------

// One mutex covers the registry and every slot write. Writes must be
// serialized process-wide, not per handle: two handles on different slots of one
// RELRO page each make the page writable and then read-only again, and without a
// common lock the second handle's mprotect(PROT_READ) can land between the first
// handle's mprotect(PROT_WRITE) and its store. A pthread mutex is used because it
// cannot throw, and this code runs from an ELF constructor.
//
// The lock is never held across a call into the loader (dl_iterate_phdr, dlopen): a thread that holds the loader's lock
// can be the one installing a hook (a dlopen runs an initializer), so the order is loader lock, then this one.
pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

class Lock {
 public:
  Lock() { pthread_mutex_lock(&g_lock); }
  ~Lock() { pthread_mutex_unlock(&g_lock); }
  Lock(const Lock&) = delete;
  Lock& operator=(const Lock&) = delete;
};

// The chain registry: one entry per handle that holds a slot, in registration order (g_links[0] is the oldest). The
// entries of one slot form its chain: the newest runs first, and each entry's `original` is what its hook calls, the entry
// before it or, for the oldest, the game's function. `original` is the value published through `originalOut`; it is kept
// here so a chain can be read and relinked under the lock. `owner` is a copy: an entry can outlive its handle (a poisoned
// one stays reachable), so the owner's text cannot be the handle's.
constexpr std::size_t kOwnerCapacity = 48;
constexpr std::size_t kNone = static_cast<std::size_t>(-1);

struct Link {
  void** slot = nullptr;
  void* hookFn = nullptr;
  void* original = nullptr;
  void** originalOut = nullptr;
  char owner[kOwnerCapacity] = {};
  // Our entry was stored in this slot and may still be reachable through it (another writer overwrote or chained it): it
  // is never given back by a handle, so a retry cannot publish that foreign hook as the original and build a call cycle.
  bool poisoned = false;
};
Link g_links[kRegistryCapacity] = {};
std::size_t g_linkCount = 0;
std::size_t g_highWater = 0;
std::size_t g_refused = 0;
std::size_t g_refusedCritical = 0;
RegistryRefusal g_ledger[kRefusalLedgerSize] = {};
std::size_t g_ledgerCount = 0;

std::atomic<StoreObserver> g_storeObserver{nullptr};
std::atomic<ProtectFn> g_protect{nullptr};
std::atomic<BuildIdAliasFn> g_buildIdAlias{nullptr};
std::atomic<const char*> g_serviceSideBuildId{nullptr};

int Protect(void* addr, std::size_t length, int prot) {
  const ProtectFn fn = g_protect.load(std::memory_order_acquire);
  return fn != nullptr ? fn(addr, length, prot) : mprotect(addr, length, prot);
}

void CopyName(char* out, std::size_t capacity, const char* text) {
  std::snprintf(out, capacity, "%s", text != nullptr ? text : "(null)");
}

void CopyOwner(char* out, const char* owner) {
  std::snprintf(out, kOwnerCapacity, "%s", owner != nullptr ? owner : "(unnamed)");
}

// ---- the chain, under the lock ---------------------------------------------------

// The oldest entry on `slot`, or kNone.
std::size_t OldestOn(void** slot) {
  for (std::size_t i = 0; i < g_linkCount; ++i) {
    if (g_links[i].slot == slot) return i;
  }
  return kNone;
}

// The newest entry on `slot`, or kNone.
std::size_t NewestOn(void** slot) {
  for (std::size_t i = g_linkCount; i-- > 0;) {
    if (g_links[i].slot == slot) return i;
  }
  return kNone;
}

// The entry that follows `index` on its slot (the next newer one), or kNone when `index` is the newest there.
std::size_t NewerOn(std::size_t index) {
  for (std::size_t i = index + 1; i < g_linkCount; ++i) {
    if (g_links[i].slot == g_links[index].slot) return i;
  }
  return kNone;
}

std::size_t FindLink(void** slot, void* hookFn) {
  for (std::size_t i = 0; i < g_linkCount; ++i) {
    if (g_links[i].slot == slot && g_links[i].hookFn == hookFn) return i;
  }
  return kNone;
}

bool AnyPoisonedOn(void** slot) {
  for (std::size_t i = 0; i < g_linkCount; ++i) {
    if (g_links[i].slot == slot && g_links[i].poisoned) return true;
  }
  return false;
}

// Removes entry `index` and keeps the registration order of the rest.
void EraseLinkAt(std::size_t index) {
  for (std::size_t i = index; i + 1 < g_linkCount; ++i) g_links[i] = g_links[i + 1];
  g_links[g_linkCount - 1] = Link{};
  --g_linkCount;
}

void EraseLinkLocked(void** slot, void* hookFn) {
  const std::size_t index = FindLink(slot, hookFn);
  if (index != kNone) EraseLinkAt(index);
}

void PoisonLinkLocked(void** slot, void* hookFn) {
  const std::size_t index = FindLink(slot, hookFn);
  if (index != kNone) g_links[index].poisoned = true;
}

// The refusal ledger (the first kRefusalLedgerSize names) and the counters, for a refusal for want of room.
void RecordRefusalLocked(const GotTarget& target) {
  const bool critical = target.priority == HookPriority::kCritical;
  ++g_refused;
  if (critical) ++g_refusedCritical;
  if (g_ledgerCount < kRefusalLedgerSize) {
    RegistryRefusal& lost = g_ledger[g_ledgerCount++];
    CopyName(lost.module, sizeof(lost.module), target.module);
    CopyName(lost.symbol, sizeof(lost.symbol), target.symbol);
    lost.critical = critical;
  }
}

// ---- module lookup ----------------------------------------------------------

// Does `path` (possibly a full install path) end in `soName` as a whole path
// segment? "libpnsrad.so" must not match "libpnsradmatchmaking.so".
bool NameMatches(const char* path, const char* soName) {
  if (path == nullptr || soName == nullptr) return false;
  const std::size_t pathLen = std::strlen(path);
  const std::size_t nameLen = std::strlen(soName);
  if (nameLen == 0 || nameLen > pathLen) return false;
  if (std::strcmp(path + (pathLen - nameLen), soName) != 0) return false;
  return pathLen == nameLen || path[pathLen - nameLen - 1] == '/';
}

struct FindContext {
  const char* wanted;
  ElfImage* out;
  bool found;
};

int FindCallback(struct dl_phdr_info* info, std::size_t /*size*/, void* data) {
  auto* ctx = static_cast<FindContext*>(data);
  if (!NameMatches(info->dlpi_name, ctx->wanted)) return 0;
  ctx->out->base = info->dlpi_addr;
  ctx->out->phdr = info->dlpi_phdr;
  ctx->out->phnum = info->dlpi_phnum;
  std::strncpy(ctx->out->name, info->dlpi_name, sizeof(ctx->out->name) - 1);
  ctx->out->name[sizeof(ctx->out->name) - 1] = '\0';
  ctx->found = true;
  return 1;
}

struct ExecContext {
  std::uintptr_t addr;
  bool executable;
};

int ExecCallback(struct dl_phdr_info* info, std::size_t /*size*/, void* data) {
  auto* ctx = static_cast<ExecContext*>(data);
  for (std::size_t i = 0; i < info->dlpi_phnum; ++i) {
    const Elf64_Phdr& ph = info->dlpi_phdr[i];
    if (ph.p_type != PT_LOAD || (ph.p_flags & PF_X) == 0) continue;
    const std::uintptr_t start = info->dlpi_addr + ph.p_vaddr;
    if (ctx->addr >= start && ctx->addr - start < ph.p_memsz) {
      ctx->executable = true;
      return 1;
    }
  }
  return 0;
}

bool IsExecutableAddress(const void* p) {
  ExecContext ctx{reinterpret_cast<std::uintptr_t>(p), false};
  dl_iterate_phdr(ExecCallback, &ctx);
  return ctx.executable;
}

// ---- page protection --------------------------------------------------------

struct PageRange {
  std::uintptr_t start;
  std::size_t length;
};

PageRange PagesOf(const void* slot) {
  const std::uintptr_t page = static_cast<std::uintptr_t>(sysconf(_SC_PAGESIZE));
  const std::uintptr_t addr = reinterpret_cast<std::uintptr_t>(slot);
  const std::uintptr_t start = addr & ~(page - 1);
  const std::uintptr_t end = (addr + kSlotSize + page - 1) & ~(page - 1);
  return {start, static_cast<std::size_t>(end - start)};
}

// Opens the maps file; a test points it elsewhere to provoke read failures.
int OpenMapsDefault() { return open("/proc/self/maps", O_RDONLY | O_CLOEXEC); }
std::atomic<MapsOpener> g_mapsOpener{nullptr};
int OpenMaps() {
  const MapsOpener fn = g_mapsOpener.load(std::memory_order_acquire);
  return fn != nullptr ? fn() : OpenMapsDefault();
}

enum class Mapped { kYes, kNo, kUnknown };

// Whether `addr` lies in a mapping. A positive match is conclusive after its line;
// a negative answer needs a complete pass. A failed open, a read error, or an end
// of file before any data (a live process always has mappings) says "unknown".
Mapped IsMapped(const void* addr) {
  const int fd = OpenMaps();
  if (fd < 0) return Mapped::kUnknown;
  const std::uintptr_t want = reinterpret_cast<std::uintptr_t>(addr);
  char chunk[1024];
  char line[160];
  std::size_t len = 0;
  bool readAny = false;
  Mapped result = Mapped::kUnknown;
  bool found = false;
  bool completePass = false;
  auto consume = [&]() {
    line[len] = '\0';
    len = 0;
    unsigned long long lo = 0, hi = 0;
    if (std::sscanf(line, "%llx-%llx", &lo, &hi) == 2 && want >= lo && want < hi) found = true;
  };
  for (;;) {
    const ssize_t n = read(fd, chunk, sizeof(chunk));
    if (n < 0) {
      if (errno == EINTR) continue;
      break;  // a read error: unknown
    }
    if (n == 0) {
      completePass = readAny;  // end of file after data
      break;
    }
    readAny = true;
    for (ssize_t i = 0; i < n; ++i) {
      if (chunk[i] != '\n') {
        if (len < sizeof(line) - 1) line[len++] = chunk[i];
      } else {
        consume();
        if (found) {
          close(fd);
          return Mapped::kYes;
        }
      }
    }
  }
  if (completePass && len > 0) consume();  // a last line with no newline
  close(fd);
  if (found) return Mapped::kYes;  // seeing the address is conclusive even on a short pass
  if (completePass) result = Mapped::kNo;
  return result;
}

// Protection (PROT_*) of the mapping that contains `addr`, from /proc/self/maps,
// or -1 if it cannot be read. Reads with a fixed buffer, no allocation.
int LiveProtection(const void* addr) {
  const int fd = OpenMaps();
  if (fd < 0) return -1;
  const std::uintptr_t want = reinterpret_cast<std::uintptr_t>(addr);
  char chunk[1024];
  char line[160];
  std::size_t len = 0;
  int result = -1;
  for (;;) {
    const ssize_t n = read(fd, chunk, sizeof(chunk));
    if (n <= 0) break;
    for (ssize_t i = 0; i < n && result < 0; ++i) {
      const char c = chunk[i];
      if (c != '\n') {
        if (len < sizeof(line) - 1) line[len++] = c;
        continue;
      }
      line[len] = '\0';
      len = 0;
      unsigned long long lo = 0, hi = 0;
      char perms[8] = {};
      if (std::sscanf(line, "%llx-%llx %7s", &lo, &hi, perms) == 3 && want >= lo && want < hi) {
        result = (perms[0] == 'r' ? PROT_READ : 0) | (perms[1] == 'w' ? PROT_WRITE : 0) |
                 (perms[2] == 'x' ? PROT_EXEC : 0);
      }
    }
    if (result >= 0) break;
  }
  close(fd);
  return result;
}

// What a write left behind. kStored: our value was stored (and, if the write then failed,
// rolled back by us). kStoredMayBeLive: it was stored and another writer's value now sits
// in the slot, possibly chaining it.
enum class StoreOutcome { kNotStored, kStored, kStoredMayBeLive };

// Replaces `expected` with `value` in `slot`, and only if the slot still holds
// `expected`: a hook someone chained on top, or any other writer, is never
// overwritten. A RELRO page is made writable for the store and protected
// read-only again. The caller holds the process-wide lock for the whole sequence.
// If the page cannot be re-protected the store is undone (compare-and-swap, so
// only our own value is rolled back) and re-protection is retried. `relroReadOnly`
// is the fallback for the protection to restore when /proc/self/maps is unreadable.
GotStatus WriteSlotLocked(void** slot, void* expected, void* value, bool relroReadOnly,
                          int* savedErrno, StoreOutcome* outcome) {
  *outcome = StoreOutcome::kNotStored;
  if (__atomic_load_n(slot, __ATOMIC_ACQUIRE) != expected) return GotStatus::kSlotChanged;
  const PageRange pages = PagesOf(slot);
  // The protection to restore is read here, under the lock: another handle on the
  // same page may have it writable right now, and a value sampled earlier could
  // describe that transient state.
  const int live = LiveProtection(slot);
  const bool restoreReadOnly = live >= 0 ? (live & PROT_WRITE) == 0 : relroReadOnly;
  if (restoreReadOnly &&
      Protect(reinterpret_cast<void*>(pages.start), pages.length, PROT_READ | PROT_WRITE) != 0) {
    *savedErrno = errno;
    return GotStatus::kProtectFailed;
  }
  const StoreObserver observer = g_storeObserver.load(std::memory_order_acquire);
  if (observer != nullptr) observer(slot, value, StorePhase::kBeforeStore);

  void* seen = expected;
  GotStatus status = GotStatus::kOk;
  // After a successful Install the entry is live by design; the outcome below says what
  // a FAILED install left behind.
  if (!__atomic_compare_exchange_n(slot, &seen, value, false, __ATOMIC_RELEASE, __ATOMIC_ACQUIRE)) {
    status = GotStatus::kSlotChanged;  // our value was never stored
  } else {
    *outcome = StoreOutcome::kStored;
    if (observer != nullptr) observer(slot, value, StorePhase::kAfterStore);
    if (__atomic_load_n(slot, __ATOMIC_ACQUIRE) != value) {
      // A writer outside this lock stored after us; theirs stays, and may chain our entry.
      status = GotStatus::kWriteVerifyFailed;
      *outcome = StoreOutcome::kStoredMayBeLive;
    }
  }
  if (restoreReadOnly &&
      Protect(reinterpret_cast<void*>(pages.start), pages.length, PROT_READ) != 0) {
    *savedErrno = errno;
    char page[19];
    if (status == GotStatus::kOk) {
      // Undo our own store only; if someone changed the slot since (for example
      // chained our entry), their value stays and the hook is still live.
      void* ours = value;
      if (!__atomic_compare_exchange_n(slot, &ours, expected, false, __ATOMIC_RELEASE,
                                       __ATOMIC_ACQUIRE)) {
        *outcome = StoreOutcome::kStoredMayBeLive;  // their value stays, and may chain our entry
        LogFields(LogLevel::kError, "got_hook",
                  {{"op", "rollback"}, {"status", "rollback_cas_failed"},
                   {"page", HexString(page, pages.start)}});
      }
    }
    if (Protect(reinterpret_cast<void*>(pages.start), pages.length, PROT_READ) != 0) {
      LogFields(LogLevel::kError, "got_hook",
                {{"op", "protect"}, {"status", "page_left_writable"},
                 {"page", HexString(page, pages.start)}});
    }
    // A failed re-protect does not hide an earlier, more specific failure.
    return status == GotStatus::kOk ? GotStatus::kRestoreProtectFailed : status;
  }
  return status;
}

// ---- dynamic section --------------------------------------------------------

bool InImage(const ElfImage& image, std::uint64_t span, std::uintptr_t addr, std::size_t len) {
  if (addr < image.base) return false;
  const std::uint64_t off = addr - image.base;
  return off <= span && len <= span - off;
}

// A DT_* pointer is a link-time vaddr on Bionic and an already-relocated
// address on glibc. A vaddr is below the load span; a relocated pointer is at or
// above the load bias.
std::uintptr_t DynPtr(const ElfImage& image, std::uint64_t span, std::uint64_t value) {
  if (image.base != 0 && value >= image.base && value - image.base < span) {
    return static_cast<std::uintptr_t>(value);
  }
  return image.base + static_cast<std::uintptr_t>(value);
}

struct RelaTable {
  const Elf64_Rela* entries = nullptr;
  std::size_t count = 0;
};

// What the relocation scan keeps. Streaming, so the number of slots a symbol has
// is unbounded: the first slot and whether a different one exists (enough to tell
// "one" from "several") and, when the target pins a slot, whether that slot matched.
struct Matches {
  std::uint64_t first = 0;
  std::int64_t firstAddend = 0;
  std::size_t distinct = 0;  // saturates at 2
  bool pinnedFound = false;
  std::int64_t pinnedAddend = 0;
  bool any = false;
  bool otherKind = false;
};

}  // namespace

const char* RelocKindName(RelocKind kind) {
  return kind == RelocKind::kJumpSlot ? "jump_slot" : "glob_dat";
}

const char* GotStatusName(GotStatus status) {
  switch (status) {
    case GotStatus::kOk:                   return "ok";
    case GotStatus::kBadArgument:          return "bad_argument";
    case GotStatus::kAlreadyInstalled:     return "already_installed";
    case GotStatus::kModuleNotLoaded:      return "module_not_loaded";
    case GotStatus::kNoDynamicSegment:     return "no_dynamic_segment";
    case GotStatus::kMalformedDynamic:     return "malformed_dynamic";
    case GotStatus::kBuildIdMismatch:      return "build_id_mismatch";
    case GotStatus::kSymbolNotFound:       return "symbol_not_found";
    case GotStatus::kWrongRelocationType:  return "wrong_relocation_type";
    case GotStatus::kAmbiguousRelocation:  return "ambiguous_relocation";
    case GotStatus::kSlotOffsetMismatch:   return "slot_offset_mismatch";
    case GotStatus::kLazyBinding:          return "lazy_binding";
    case GotStatus::kUnsupportedAddend:    return "unsupported_addend";
    case GotStatus::kSlotMisaligned:       return "slot_misaligned";
    case GotStatus::kSlotOutsideImage:     return "slot_outside_image";
    case GotStatus::kOriginalMismatch:     return "original_mismatch";
    case GotStatus::kOriginalImplausible:  return "original_implausible";
    case GotStatus::kProtectFailed:        return "protect_failed";
    case GotStatus::kWriteVerifyFailed:    return "write_verify_failed";
    case GotStatus::kRestoreProtectFailed: return "restore_protect_failed";
    case GotStatus::kNotInstalled:         return "not_installed";
    case GotStatus::kModuleChanged:        return "module_changed";
    case GotStatus::kSlotChanged:          return "slot_changed";
    case GotStatus::kRegistryFull:         return "registry_full";
    case GotStatus::kSlotPoisoned:         return "slot_poisoned";
  }
  return "unknown";
}

bool FindLoadedImage(const char* module, ElfImage* out) {
  if (module == nullptr || out == nullptr) return false;
  FindContext ctx{module, out, false};
  dl_iterate_phdr(FindCallback, &ctx);
  return ctx.found;
}

bool ReadBuildId(const ElfImage& image, char* out, std::size_t capacity) {
  if (out == nullptr || capacity == 0) return false;
  for (std::size_t i = 0; i < image.phnum; ++i) {
    const Elf64_Phdr& ph = image.phdr[i];
    if (ph.p_type != PT_NOTE) continue;
    // Program headers describe the loaded image, but a PT_NOTE need not itself be mapped.
    // Only inspect notes wholly contained in a loaded segment before forming a pointer.
    bool loaded = false;
    for (std::size_t j = 0; j < image.phnum; ++j) {
      const Elf64_Phdr& load = image.phdr[j];
      if (load.p_type == PT_LOAD && ph.p_vaddr >= load.p_vaddr &&
          ph.p_vaddr - load.p_vaddr <= load.p_memsz &&
          ph.p_memsz <= load.p_memsz - (ph.p_vaddr - load.p_vaddr)) {
        loaded = true;
        break;
      }
    }
    if (!loaded || ph.p_vaddr > UINTPTR_MAX - image.base ||
        ph.p_memsz > UINTPTR_MAX - (image.base + ph.p_vaddr)) continue;
    const auto* cursor = reinterpret_cast<const unsigned char*>(image.base + ph.p_vaddr);
    const unsigned char* const end = cursor + ph.p_memsz;
    while (static_cast<std::size_t>(end - cursor) >= sizeof(Elf64_Nhdr)) {
      Elf64_Nhdr nhdr;
      std::memcpy(&nhdr, cursor, sizeof(nhdr));
      const std::size_t nameLen = (static_cast<std::size_t>(nhdr.n_namesz) + 3U) & ~std::size_t{3};
      const std::size_t descLen = (static_cast<std::size_t>(nhdr.n_descsz) + 3U) & ~std::size_t{3};
      const std::size_t remaining = static_cast<std::size_t>(end - cursor) - sizeof(nhdr);
      if (nameLen > remaining || descLen > remaining - nameLen) break;
      const unsigned char* name = cursor + sizeof(nhdr);
      const unsigned char* desc = name + nameLen;
      if (nhdr.n_type == NT_GNU_BUILD_ID && nhdr.n_namesz == 4 &&
          std::memcmp(name, "GNU", 4) == 0 && nhdr.n_descsz <= (capacity - 1) / 2) {
        static const char kHex[] = "0123456789abcdef";
        for (std::size_t b = 0; b < nhdr.n_descsz; ++b) {
          out[b * 2] = kHex[desc[b] >> 4];
          out[b * 2 + 1] = kHex[desc[b] & 0xF];
        }
        out[nhdr.n_descsz * 2] = '\0';
        return true;
      }
      cursor = desc + descLen;
    }
  }
  return false;
}

SlotResolution ResolveSlot(const ElfImage& image, const GotTarget& target,
                           const RelocNumbers& relocs) {
  SlotResolution result;
  auto fail = [&result](GotStatus status) {
    result.status = status;
    return result;
  };

  std::uint64_t span = 0;
  const Elf64_Phdr* dynamicPh = nullptr;
  const Elf64_Phdr* relroPh = nullptr;
  for (std::size_t i = 0; i < image.phnum; ++i) {
    const Elf64_Phdr& ph = image.phdr[i];
    if (ph.p_type == PT_LOAD && ph.p_vaddr + ph.p_memsz > span) span = ph.p_vaddr + ph.p_memsz;
    if (ph.p_type == PT_DYNAMIC) dynamicPh = &ph;
    if (ph.p_type == PT_GNU_RELRO) relroPh = &ph;
  }
  if (dynamicPh == nullptr) return fail(GotStatus::kNoDynamicSegment);
  if (dynamicPh->p_vaddr + dynamicPh->p_memsz > span) return fail(GotStatus::kMalformedDynamic);

  const auto* dyn = reinterpret_cast<const Elf64_Dyn*>(image.base + dynamicPh->p_vaddr);
  const std::size_t dynCount = dynamicPh->p_memsz / sizeof(Elf64_Dyn);
  std::uint64_t strtab = 0, symtab = 0, jmprel = 0, rela = 0;
  std::uint64_t strsz = 0, jmprelSize = 0, relaSize = 0, relaEnt = sizeof(Elf64_Rela);
  std::uint64_t pltRelType = DT_RELA;
  std::uint64_t flags = 0, flags1 = 0;
  bool haveStrtab = false, haveSymtab = false;
  for (std::size_t i = 0; i < dynCount && dyn[i].d_tag != DT_NULL; ++i) {
    const std::uint64_t value = dyn[i].d_un.d_val;
    switch (dyn[i].d_tag) {
      case DT_STRTAB:   strtab = value; haveStrtab = true; break;
      case DT_SYMTAB:   symtab = value; haveSymtab = true; break;
      case DT_STRSZ:    strsz = value; break;
      case DT_JMPREL:   jmprel = value; break;
      case DT_PLTRELSZ: jmprelSize = value; break;
      case DT_PLTREL:   pltRelType = value; break;
      case DT_RELA:     rela = value; break;
      case DT_RELASZ:   relaSize = value; break;
      case DT_RELAENT:  relaEnt = value; break;
      case DT_FLAGS:    flags = value; break;
      case DT_FLAGS_1:  flags1 = value; break;
      default: break;
    }
  }
  if (!haveStrtab || !haveSymtab || strsz == 0) return fail(GotStatus::kMalformedDynamic);
  if (relaEnt != sizeof(Elf64_Rela) || (jmprel != 0 && pltRelType != DT_RELA)) {
    return fail(GotStatus::kMalformedDynamic);
  }

  const std::uintptr_t strBase = DynPtr(image, span, strtab);
  const std::uintptr_t symBase = DynPtr(image, span, symtab);
  if (!InImage(image, span, strBase, strsz)) return fail(GotStatus::kMalformedDynamic);
  const auto* strings = reinterpret_cast<const char*>(strBase);
  const auto* symbols = reinterpret_cast<const Elf64_Sym*>(symBase);

  RelaTable tables[2];
  if (jmprel != 0 && jmprelSize != 0) {
    const std::uintptr_t at = DynPtr(image, span, jmprel);
    if (jmprelSize % sizeof(Elf64_Rela) != 0 || !InImage(image, span, at, jmprelSize)) {
      return fail(GotStatus::kMalformedDynamic);
    }
    tables[0] = {reinterpret_cast<const Elf64_Rela*>(at), jmprelSize / sizeof(Elf64_Rela)};
  }
  if (rela != 0 && relaSize != 0) {
    const std::uintptr_t at = DynPtr(image, span, rela);
    if (relaSize % sizeof(Elf64_Rela) != 0 || !InImage(image, span, at, relaSize)) {
      return fail(GotStatus::kMalformedDynamic);
    }
    tables[1] = {reinterpret_cast<const Elf64_Rela*>(at), relaSize / sizeof(Elf64_Rela)};
  }

  const bool bindNow = (flags & DF_BIND_NOW) != 0 || (flags1 & DF_1_NOW) != 0;
  if (target.kind == RelocKind::kJumpSlot && !bindNow) return fail(GotStatus::kLazyBinding);

  const std::uint32_t wantedType =
      target.kind == RelocKind::kJumpSlot ? relocs.jumpSlot : relocs.globDat;
  const std::uint32_t otherType =
      target.kind == RelocKind::kJumpSlot ? relocs.globDat : relocs.jumpSlot;
  const std::size_t symbolLen = std::strlen(target.symbol);

  Matches matches;
  for (const RelaTable& table : tables) {
    for (std::size_t i = 0; i < table.count; ++i) {
      const Elf64_Rela& rel = table.entries[i];
      const std::uint32_t type = ELF64_R_TYPE(rel.r_info);
      if (type != wantedType && type != otherType) continue;
      const std::uint32_t symIndex = ELF64_R_SYM(rel.r_info);
      if (symIndex == 0) continue;
      const std::uintptr_t symAt = symBase + static_cast<std::uintptr_t>(symIndex) * sizeof(Elf64_Sym);
      if (!InImage(image, span, symAt, sizeof(Elf64_Sym))) return fail(GotStatus::kMalformedDynamic);
      const std::uint32_t nameOffset = symbols[symIndex].st_name;
      if (nameOffset >= strsz || symbolLen + 1 > strsz - nameOffset) continue;
      if (std::memcmp(strings + nameOffset, target.symbol, symbolLen + 1) != 0) continue;
      if (type != wantedType) {
        matches.otherKind = true;
        continue;
      }
      matches.any = true;
      if (target.slotVaddr.has_value()) {
        if (rel.r_offset == *target.slotVaddr) {
          matches.pinnedFound = true;
          matches.pinnedAddend = rel.r_addend;
        }
      } else if (matches.distinct == 0) {
        matches.first = rel.r_offset;
        matches.firstAddend = rel.r_addend;
        matches.distinct = 1;
      } else if (rel.r_offset != matches.first) {
        matches.distinct = 2;
      }
    }
  }

  if (!matches.any) {
    return fail(matches.otherKind ? GotStatus::kWrongRelocationType : GotStatus::kSymbolNotFound);
  }
  std::uint64_t chosenVaddr = 0;
  std::int64_t chosenAddend = 0;
  if (target.slotVaddr.has_value()) {
    if (!matches.pinnedFound) return fail(GotStatus::kSlotOffsetMismatch);
    chosenVaddr = *target.slotVaddr;
    chosenAddend = matches.pinnedAddend;
  } else {
    if (matches.distinct > 1) return fail(GotStatus::kAmbiguousRelocation);
    chosenVaddr = matches.first;
    chosenAddend = matches.firstAddend;
  }
  if (chosenAddend != 0) return fail(GotStatus::kUnsupportedAddend);

  const std::uint64_t vaddr = chosenVaddr;
  if (vaddr % kSlotSize != 0) return fail(GotStatus::kSlotMisaligned);
  bool writable = false;
  for (std::size_t i = 0; i < image.phnum; ++i) {
    const Elf64_Phdr& ph = image.phdr[i];
    if (ph.p_type == PT_LOAD && (ph.p_flags & PF_W) != 0 && vaddr >= ph.p_vaddr &&
        vaddr - ph.p_vaddr <= ph.p_memsz && kSlotSize <= ph.p_memsz - (vaddr - ph.p_vaddr)) {
      writable = true;
    }
  }
  if (!writable) return fail(GotStatus::kSlotOutsideImage);

  if (relroPh != nullptr) {
    const std::uint64_t page = static_cast<std::uint64_t>(sysconf(_SC_PAGESIZE));
    const std::uint64_t relroStart = relroPh->p_vaddr & ~(page - 1);
    const std::uint64_t relroEnd = (relroPh->p_vaddr + relroPh->p_memsz + page - 1) & ~(page - 1);
    const std::uint64_t slotPage = vaddr & ~(page - 1);
    result.restoreReadOnly = slotPage >= relroStart && slotPage < relroEnd;
  }
  result.slotVaddr = vaddr;
  result.slot = reinterpret_cast<void**>(image.base + static_cast<std::uintptr_t>(vaddr));
  result.status = GotStatus::kOk;
  return result;
}

namespace {

// The chain's decision, the store and its rollback run as one step under the lock; the module reads happen before it,
// with no lock, in Resolve.
struct Prepared {
  ElfImage image;
  SlotResolution resolution;
  void* observed = nullptr;     // the slot's value when it was resolved
  bool plausible = false;       // `observed` is in an executable mapping (asked only when there is no expected original)
  char actualBuildId[64] = {};  // filled when a build-ID check fails
};

// Where a joined entry sits: `position` counts the chain from its oldest entry (1), `length` is the chain's length
// once the entry has joined.
struct Joined {
  std::size_t position = 0;
  std::size_t length = 0;
};

// The longest owner text a refusal line carries: the chain's owners, joined.
constexpr std::size_t kChainTextCapacity = 256;
// The most fields an install refusal line carries (the two build-ID fields included).
constexpr std::size_t kInstallRefusalFields = 13;

// The one failure record of a remove. Module names and build IDs are public identifiers; nothing here is a credential.
void LogFailure(const char* op, const GotTarget& t, GotStatus status, const char* stage, int err) {
  const char* module = t.module != nullptr ? t.module : "(null)";
  const char* symbol = t.symbol != nullptr ? t.symbol : "(null)";
  LogFields(LogLevel::kError, "got_hook",
            {{"op", op}, {"status", GotStatusName(status)}, {"stage", stage}, {"module", module},
             {"symbol", symbol}, {"reloc", RelocKindName(t.kind)}, {"errno", err}});
}

// What a refusal means, in one sentence, for the log's `reason` field.
const char* RefusalReason(GotStatus status) {
  switch (status) {
    case GotStatus::kAlreadyInstalled:
      return "this handle already holds a hook, or the handler is already in the chain of this slot";
    case GotStatus::kSlotPoisoned:
      return "a failed install left an entry reachable through this slot; it is refused until the module unloads";
    case GotStatus::kOriginalMismatch:
      return "the slot does not hold the value its chain expects";
    case GotStatus::kOriginalImplausible:
      return "the slot holds no address in an executable mapping";
    case GotStatus::kSlotChanged:
      return "the slot changed between the check and the store";
    case GotStatus::kRegistryFull:
      return "the hook registry has no room for this hook";
    case GotStatus::kBuildIdMismatch:
      return "the module's build ID is not the one the target expects";
    default:
      return "the hook is not installed";
  }
}

// The owners of `slot`'s chain, oldest first, joined by '>' ("(none)" for a slot with no entry), into `out`. Returns the
// number of entries. Takes the lock: never call it with the lock held.
long long DescribeChain(void** slot, char* out, std::size_t capacity) {
  const Lock lock;
  std::size_t used = 0;
  long long count = 0;
  out[0] = '\0';
  for (std::size_t i = 0; i < g_linkCount; ++i) {
    if (g_links[i].slot != slot) continue;
    if (used + 1 >= capacity) break;
    const char* const separator = count == 0 ? "" : ">";
    const int wrote = std::snprintf(out + used, capacity - used, "%s%s%s", separator, g_links[i].owner,
                                    g_links[i].poisoned ? "(poisoned)" : "");
    if (wrote > 0) {
      const std::size_t n = static_cast<std::size_t>(wrote);
      used += n < capacity - used ? n : capacity - used - 1;
    }
    ++count;
  }
  if (count == 0) std::snprintf(out, capacity, "%s", "(none)");
  return count;
}

// The refusal of an install: one Warning line that names the slot, the chain on it and the reason (logging.md Rule 6).
// `slot` is null when the module or its slot did not resolve; `actualBuildId` is set only for a build-ID mismatch.
void LogInstallRefusal(const GotTarget& t, GotStatus status, const char* stage, int err, void** slot,
                       const char* actualBuildId) {
  char slotText[19] = "(unresolved)";
  char chain[kChainTextCapacity] = "(none)";
  long long length = 0;
  if (slot != nullptr) {
    HexString(slotText, reinterpret_cast<std::uintptr_t>(slot));
    length = DescribeChain(slot, chain, sizeof(chain));
  }
  // LogField has no default constructor, so the array is initialised whole; the two build-ID fields are logged only for a
  // build-ID refusal (the count below).
  const LogField fields[kInstallRefusalFields] = {
      LogField("op", "install"),
      LogField("status", GotStatusName(status)),
      LogField("reason", RefusalReason(status)),
      LogField("stage", stage),
      LogField("module", t.module),
      LogField("symbol", t.symbol),
      LogField("reloc", RelocKindName(t.kind)),
      LogField("errno", err),
      LogField("slot", slotText),
      LogField("chain", chain),
      LogField("chain_length", length),
      LogField("expected_build_id", t.buildId != nullptr ? t.buildId : ""),
      LogField("actual_build_id", actualBuildId != nullptr && actualBuildId[0] != '\0' ? actualBuildId : "(none)"),
  };
  const std::size_t n = actualBuildId != nullptr ? kInstallRefusalFields : kInstallRefusalFields - 2;
  LogFieldList(LogLevel::kWarn, "got_hook", fields, n);
}

// The refusal for want of room: one Warning line that names the hook that lost, its slot and chain, and what that costs.
// The registry is a budget (hook_budget.h): a line like this one means the budget no longer covers the build, which is a
// defect to fix, not a condition to ride out.
void LogRegistryFull(const GotTarget& t, void** slot) {
  const RegistryStats stats = RegistrySnapshot();
  const bool critical = t.priority == HookPriority::kCritical;
  char slotText[19] = "(unresolved)";
  char chain[kChainTextCapacity] = "(none)";
  long long length = 0;
  if (slot != nullptr) {
    HexString(slotText, reinterpret_cast<std::uintptr_t>(slot));
    length = DescribeChain(slot, chain, sizeof(chain));
  }
  LogFields(LogLevel::kWarn, "got_hook",
            {{"op", "install"}, {"status", "registry_full"}, {"reason", RefusalReason(GotStatus::kRegistryFull)},
             {"module", t.module}, {"symbol", t.symbol}, {"reloc", RelocKindName(t.kind)},
             {"priority", critical ? "critical" : "normal"}, {"slot", slotText}, {"chain", chain},
             {"chain_length", length},
             {"capacity", static_cast<long long>(stats.capacity)}, {"used", static_cast<long long>(stats.used)},
             {"critical_reserve", static_cast<long long>(stats.criticalReserve)},
             {"refused_total", static_cast<long long>(stats.refused)},
             {"consequence", "this hook is not installed; the game keeps its own code in the slot"}});
}

// A target's build ID is accepted when it equals the module's live note, or the alias the registered function gives
// that note (SetBuildIdAlias). The alias is asked at every install, with the note read for it, so what it maps can
// change only with what it reads.
bool BuildIdAccepted(const char* liveNote, const GotTarget& target) {
  const char* const targetBuildId = target.buildId;
  if (std::strcmp(liveNote, targetBuildId) == 0) return true;
  if (target.serviceSide) {
    const char* const service = g_serviceSideBuildId.load(std::memory_order_acquire);
    if (service != nullptr && std::strcmp(liveNote, service) == 0) return true;
  }
  const BuildIdAliasFn alias = g_buildIdAlias.load(std::memory_order_acquire);
  if (alias == nullptr) return false;
  const char* const mapped = alias(liveNote);
  return mapped != nullptr && mapped[0] != '\0' && std::strcmp(mapped, targetBuildId) == 0;
}

// Everything that reads the module. It runs with no lock held (see g_lock): dl_iterate_phdr takes the loader's lock.
GotStatus Resolve(const GotTarget& target, ImageLookup lookup, Prepared* out) {
  if (!lookup(target.module, &out->image)) return GotStatus::kModuleNotLoaded;
  if (target.buildId != nullptr) {
    if (!ReadBuildId(out->image, out->actualBuildId, sizeof(out->actualBuildId)) ||
        !BuildIdAccepted(out->actualBuildId, target)) {
      return GotStatus::kBuildIdMismatch;
    }
  }
  out->resolution = ResolveSlot(out->image, target, kNativeRelocs);
  if (out->resolution.status != GotStatus::kOk) return out->resolution.status;
  out->observed = __atomic_load_n(out->resolution.slot, __ATOMIC_ACQUIRE);
  out->plausible = target.expectedOriginal == nullptr && out->observed != nullptr && IsExecutableAddress(out->observed);
  return GotStatus::kOk;
}

// Under the lock, with no call into the loader: decides whether `hookFn` may join the chain of `p.resolution.slot`, and
// registers its entry. On kOk, `*expectedSlot` is the value the store must find (the game's value for a first handler,
// the chain's newest hook otherwise), `*newOriginal` is the value the new hook calls, and the entry is registered; the
// caller publishes `*newOriginal` through `originalOut` before it stores.
GotStatus JoinLocked(const Prepared& p, const GotTarget& target, void* hookFn, void** originalOut,
                     void** expectedSlot, void** newOriginal, Joined* joined) {
  void** const slot = p.resolution.slot;
  if (AnyPoisonedOn(slot)) return GotStatus::kSlotPoisoned;
  if (FindLink(slot, hookFn) != kNone) return GotStatus::kAlreadyInstalled;
  const std::size_t oldest = OldestOn(slot);
  if (oldest == kNone) {
    // A first handler: the game's value must still be in the slot, as it was when the module was resolved.
    void* const current = __atomic_load_n(slot, __ATOMIC_ACQUIRE);
    if (current != p.observed) return GotStatus::kSlotChanged;
    if (current == hookFn) return GotStatus::kAlreadyInstalled;
    if (target.expectedOriginal != nullptr) {
      if (current != target.expectedOriginal) return GotStatus::kOriginalMismatch;
    } else if (!p.plausible) {
      return GotStatus::kOriginalImplausible;
    }
    *expectedSlot = current;
    *newOriginal = current;
  } else {
    // A later handler: the slot must hold the chain's newest hook, and the chain's base must be the expected original.
    void* const newest = g_links[NewestOn(slot)].hookFn;
    if (__atomic_load_n(slot, __ATOMIC_ACQUIRE) != newest) return GotStatus::kOriginalMismatch;
    if (target.expectedOriginal != nullptr && g_links[oldest].original != target.expectedOriginal) {
      return GotStatus::kOriginalMismatch;
    }
    *expectedSlot = newest;
    *newOriginal = newest;
  }
  const bool critical = target.priority == HookPriority::kCritical;
  const std::size_t limit = critical ? kRegistryCapacity : kRegistryCapacity - kCriticalReserve;
  if (g_linkCount >= limit) {
    RecordRefusalLocked(target);
    return GotStatus::kRegistryFull;
  }
  std::size_t onSlot = 0;
  for (std::size_t i = 0; i < g_linkCount; ++i) {
    if (g_links[i].slot == slot) ++onSlot;
  }
  Link& entry = g_links[g_linkCount++];
  entry = Link{};
  entry.slot = slot;
  entry.hookFn = hookFn;
  entry.original = *newOriginal;
  entry.originalOut = originalOut;
  CopyOwner(entry.owner, target.owner);
  if (g_linkCount > g_highWater) g_highWater = g_linkCount;
  joined->position = onSlot + 1;
  joined->length = onSlot + 1;
  return GotStatus::kOk;
}

// Under the lock, the slot not written: takes the entry for (slot, hookFn) out of its chain. The entry that calls it (the
// next newer one on the slot) is relinked to what it called, so the rest of the chain keeps running. Used where the slot
// is not ours to write: a module that is gone or moved, or a handle dropped with Forget.
void ForgetLinkLocked(void** slot, void* hookFn) {
  const std::size_t index = FindLink(slot, hookFn);
  if (index == kNone) return;
  const std::size_t newer = NewerOn(index);
  if (newer != kNone) {
    g_links[newer].original = g_links[index].original;
    nevr::hook::PublishPointer(g_links[newer].originalOut, g_links[index].original);
  }
  EraseLinkAt(index);
}

// Under the lock: takes the entry for (slot, hookFn) out of its chain. The newest entry is removed by storing the value it
// calls back into the slot, compare-and-swap, so a hook someone chained on top leaves kSlotChanged and the entry stays. An
// older entry is removed by relinking the entry that calls it: that one now calls what the removed one called, and the
// slot is not written.
GotStatus UnlinkLocked(void** slot, void* hookFn, bool restoreReadOnly, int* savedErrno, StoreOutcome* outcome) {
  *outcome = StoreOutcome::kNotStored;
  const std::size_t index = FindLink(slot, hookFn);
  if (index == kNone) return GotStatus::kNotInstalled;
  void* const calls = g_links[index].original;
  const std::size_t newer = NewerOn(index);
  if (newer == kNone) {
    const GotStatus status = WriteSlotLocked(slot, hookFn, calls, restoreReadOnly, savedErrno, outcome);
    if (status != GotStatus::kOk) return status;
  } else {
    g_links[newer].original = calls;
    nevr::hook::PublishPointer(g_links[newer].originalOut, calls);
  }
  EraseLinkAt(index);
  return GotStatus::kOk;
}

}  // namespace

void SetServiceSideBuildId(const char* liveNote) noexcept {
  g_serviceSideBuildId.store(liveNote, std::memory_order_release);
}

BuildIdAliasFn SetBuildIdAlias(BuildIdAliasFn fn) {
  return g_buildIdAlias.exchange(fn, std::memory_order_acq_rel);
}

ProtectFn SetProtectFunction(ProtectFn fn) {
  return g_protect.exchange(fn, std::memory_order_acq_rel);
}

void ReleasePoisonedSlotsIn(const void* begin, std::size_t length) {
  const Lock lock;
  const std::uintptr_t lo = reinterpret_cast<std::uintptr_t>(begin);
  for (std::size_t i = 0; i < g_linkCount;) {
    const std::uintptr_t at = reinterpret_cast<std::uintptr_t>(g_links[i].slot);
    if (!g_links[i].poisoned || at < lo || at - lo >= length) {
      ++i;
      continue;
    }
    // Released only when the address is verifiably unmapped; unreadable maps keep the entry.
    const Mapped mapped = IsMapped(g_links[i].slot);
    char where[19];
    if (mapped == Mapped::kNo) {
      LogFields(LogLevel::kInfo, "got_hook",
                {{"op", "release_poisoned"}, {"status", "released"}, {"slot", HexString(where, at)}});
      EraseLinkAt(i);
    } else {
      LogFields(LogLevel::kError, "got_hook",
                {{"op", "release_poisoned"},
                 {"status", mapped == Mapped::kYes ? "still_mapped" : "mapping_unknown"},
                 {"slot", HexString(where, at)}});
      ++i;
    }
  }
}

RegistryStats RegistrySnapshot() {
  const Lock lock;
  RegistryStats stats;
  stats.used = g_linkCount;
  stats.highWater = g_highWater;
  stats.refused = g_refused;
  stats.refusedCritical = g_refusedCritical;
  stats.ledgerCount = g_ledgerCount;
  for (std::size_t i = 0; i < g_ledgerCount; ++i) stats.lost[i] = g_ledger[i];
  return stats;
}

void ResetRegistryCountersForTest() {
  const Lock lock;
  g_highWater = g_linkCount;
  g_refused = 0;
  g_refusedCritical = 0;
  g_ledgerCount = 0;
  for (RegistryRefusal& entry : g_ledger) entry = RegistryRefusal{};
}

MapsOpener SetMapsOpener(MapsOpener opener) {
  return g_mapsOpener.exchange(opener, std::memory_order_acq_rel);
}

StoreObserver SetStoreObserver(StoreObserver observer) {
  return g_storeObserver.exchange(observer, std::memory_order_acq_rel);
}

GotStatus GotHook::Install(const GotTarget& target, void* hookFn, void** originalOut,
                           ImageLookup lookup) {
  if (target.module == nullptr || target.symbol == nullptr || hookFn == nullptr ||
      originalOut == nullptr || lookup == nullptr) {
    LogInstallRefusal(target, GotStatus::kBadArgument, "arguments", 0, nullptr, nullptr);
    return GotStatus::kBadArgument;
  }
  if (installed_) {
    LogInstallRefusal(target, GotStatus::kAlreadyInstalled, "handle", 0, nullptr, nullptr);
    return GotStatus::kAlreadyInstalled;
  }

  Prepared prepared;
  GotStatus status = Resolve(target, lookup, &prepared);
  if (status != GotStatus::kOk) {
    LogInstallRefusal(target, status, nevr::hook::AttachStageName(nevr::hook::AttachStage::kCreateFailed), 0,
                      nullptr, status == GotStatus::kBuildIdMismatch ? prepared.actualBuildId : nullptr);
    return status;
  }

  void** const slot = prepared.resolution.slot;
  void* expected = nullptr;
  void* newOriginal = nullptr;
  Joined joined;
  int savedErrno = 0;
  StoreOutcome outcome = StoreOutcome::kNotStored;
  nevr::hook::AttachStage stage = nevr::hook::AttachStage::kAttached;
  {
    // The decision, the store and any rollback are one step under the lock: no other handler may join or leave the chain
    // between them, because a relink would then leave this handler calling a hook that is no longer in the chain.
    const Lock lock;
    stage = nevr::hook::AttachPublished(
        originalOut,
        [&](void** original) {
          status = JoinLocked(prepared, target, hookFn, originalOut, &expected, &newOriginal, &joined);
          if (status != GotStatus::kOk) return false;
          *original = newOriginal;
          return true;
        },
        [&] {
          status = WriteSlotLocked(slot, expected, hookFn, prepared.resolution.restoreReadOnly, &savedErrno,
                                   &outcome);
          return status == GotStatus::kOk;
        },
        [&] {
          // An entry whose store may still be reachable stays in the chain, poisoned; any other failure takes it out.
          if (outcome == StoreOutcome::kStoredMayBeLive) {
            PoisonLinkLocked(slot, hookFn);
          } else {
            EraseLinkLocked(slot, hookFn);
          }
        },
        // Once our entry has been stored in the slot, a thread may have jumped into the detour before any rollback; the
        // original (the real function) must stay published for it.
        [&] { return outcome != StoreOutcome::kNotStored; });
  }

  if (stage != nevr::hook::AttachStage::kAttached) {
    mayBeLive_ = outcome == StoreOutcome::kStoredMayBeLive;
    if (status == GotStatus::kRegistryFull) {
      LogRegistryFull(target, slot);
      return status;
    }
    LogInstallRefusal(target, status, nevr::hook::AttachStageName(stage), savedErrno, slot,
                      status == GotStatus::kBuildIdMismatch ? prepared.actualBuildId : nullptr);
    return status;
  }
  target_ = target;
  lookup_ = lookup;
  image_ = prepared.image;
  slot_ = slot;
  hookFn_ = hookFn;
  restoreReadOnly_ = prepared.resolution.restoreReadOnly;
  installed_ = true;
  mayBeLive_ = false;
  // The slot's previous value, as an offset from the module's load bias, so a build can be compared with the
  // previous one (#1530): the same function sits at the same offset in two builds of the same module.
  char vaddr[19], bias[19], originalOffset[19];
  LogFields(LogLevel::kInfo, "got_hook",
            {{"op", "install"}, {"status", "ok"}, {"module", target.module},
             {"symbol", target.symbol}, {"reloc", RelocKindName(target.kind)},
             {"slot_vaddr", HexString(vaddr, prepared.resolution.slotVaddr)},
             {"load_bias", HexString(bias, image_.base)},
             {"original_offset",
              HexString(originalOffset, reinterpret_cast<std::uintptr_t>(newOriginal) - image_.base)},
             {"owner", target.owner != nullptr ? target.owner : "(unnamed)"},
             {"position", static_cast<long long>(joined.position)},
             {"chain_length", static_cast<long long>(joined.length)}});
  return GotStatus::kOk;
}

GotStatus GotHook::Remove() {
  if (!installed_) {
    LogFields(LogLevel::kWarn, "got_hook", {{"op", "remove"}, {"status", "not_installed"}});
    return GotStatus::kNotInstalled;
  }
  ElfImage now;
  SlotResolution again;
  const bool present = lookup_(target_.module, &now) && now.base == image_.base;
  if (present) again = ResolveSlot(now, target_, kNativeRelocs);
  if (!present || again.status != GotStatus::kOk || again.slot != slot_) {
    // The module is gone or moved: the slot memory is not ours to write. The hook went with it, so its entry is released
    // and the handle with it.
    {
      const Lock lock;
      ForgetLinkLocked(slot_, hookFn_);
    }
    installed_ = false;
    LogFailure("remove", target_, GotStatus::kModuleChanged, "revalidate", 0);
    return GotStatus::kModuleChanged;
  }
  // Compare-and-swap under the write lock (UnlinkLocked): a hook chained on top of ours, or a module reloaded at the same
  // base, leaves kSlotChanged and the slot untouched.
  int savedErrno = 0;
  StoreOutcome outcome = StoreOutcome::kNotStored;
  GotStatus status = GotStatus::kOk;
  {
    const Lock lock;
    status = UnlinkLocked(slot_, hookFn_, restoreReadOnly_, &savedErrno, &outcome);
  }
  if (status == GotStatus::kNotInstalled) {
    // The registry no longer has this handle's entry, so there is nothing left to hold: the handle is released.
    installed_ = false;
    LogFailure("remove", target_, status, "registry", 0);
    return status;
  }
  if (status != GotStatus::kOk) {
    LogFailure("remove", target_, status, "restore", savedErrno);
    return status;
  }
  installed_ = false;
  LogFields(LogLevel::kInfo, "got_hook",
            {{"op", "remove"}, {"status", "ok"}, {"module", target_.module},
             {"symbol", target_.symbol}, {"reloc", RelocKindName(target_.kind)}});
  return GotStatus::kOk;
}

void GotHook::Forget() {
  if (!installed_) return;
  {
    const Lock lock;
    ForgetLinkLocked(slot_, hookFn_);
  }
  installed_ = false;
  LogFields(LogLevel::kWarn, "got_hook",
            {{"op", "forget"}, {"status", "ok"}, {"module", target_.module},
             {"symbol", target_.symbol}, {"reloc", RelocKindName(target_.kind)}});
}

GotStatus PluginHandlerInstaller::Install(GotHook& hook, const GotTarget& target, void* hookFn, void** originalOut,
                                          ImageLookup lookup) {
  return hook.Install(target, hookFn, originalOut, lookup);
}

}  // namespace sentinel
