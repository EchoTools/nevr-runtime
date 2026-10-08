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
#include "hook_log.h"

namespace sentinel {

namespace {

constexpr std::size_t kSlotSize = sizeof(void*);
static_assert(sizeof(Elf64_Rela) == 24, "ELF64 RELA entry layout");

// ---- process-wide lock, slot ownership, store observer ------------------------

// One mutex covers slot reservation and every slot write. Writes must be
// serialized process-wide, not per handle: two handles on different slots of one
// RELRO page each make the page writable and then read-only again, and without a
// common lock the second handle's mprotect(PROT_READ) can land between the first
// handle's mprotect(PROT_WRITE) and its store. A pthread mutex is used because it
// cannot throw, and this code runs from an ELF constructor.
pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

class Lock {
 public:
  Lock() { pthread_mutex_lock(&g_lock); }
  ~Lock() { pthread_mutex_unlock(&g_lock); }
  Lock(const Lock&) = delete;
  Lock& operator=(const Lock&) = delete;
};

constexpr std::size_t kMaxSlots = 64;
void** g_slots[kMaxSlots] = {};
std::size_t g_slotCount = 0;

std::atomic<StoreObserver> g_storeObserver{nullptr};
std::atomic<ProtectFn> g_protect{nullptr};

int Protect(void* addr, std::size_t length, int prot) {
  const ProtectFn fn = g_protect.load(std::memory_order_acquire);
  return fn != nullptr ? fn(addr, length, prot) : mprotect(addr, length, prot);
}

GotStatus ReserveSlot(void** slot) {
  const Lock lock;
  for (std::size_t i = 0; i < g_slotCount; ++i) {
    if (g_slots[i] == slot) return GotStatus::kAlreadyInstalled;
  }
  if (g_slotCount == kMaxSlots) return GotStatus::kRegistryFull;
  g_slots[g_slotCount++] = slot;
  return GotStatus::kOk;
}

void ReleaseSlot(void** slot) {
  const Lock lock;
  for (std::size_t i = 0; i < g_slotCount; ++i) {
    if (g_slots[i] == slot) {
      g_slots[i] = g_slots[--g_slotCount];
      g_slots[g_slotCount] = nullptr;
      return;
    }
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

// Protection (PROT_*) of the mapping that contains `addr`, from /proc/self/maps,
// or -1 if it cannot be read. Reads with a fixed buffer, no allocation.
int LiveProtection(const void* addr) {
  const int fd = open("/proc/self/maps", O_RDONLY | O_CLOEXEC);
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

// `relroReadOnly` is the fallback when /proc/self/maps is unreadable.
// Replaces `expected` with `value` in `slot`, and only if the slot still holds
// `expected`: a hook someone chained on top, or any other writer, is never
// overwritten. A RELRO page is made writable for the store and protected
// read-only again. The whole sequence runs under the process-wide lock. If the
// page cannot be re-protected the store is undone (compare-and-swap, so only our
// own value is rolled back) and re-protection is retried.
GotStatus WriteSlot(void** slot, void* expected, void* value, bool relroReadOnly,
                    int* savedErrno) {
  const Lock lock;
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
  if (observer != nullptr) observer(slot, value);

  void* seen = expected;
  GotStatus status = GotStatus::kOk;
  if (!__atomic_compare_exchange_n(slot, &seen, value, false, __ATOMIC_RELEASE, __ATOMIC_ACQUIRE)) {
    status = GotStatus::kSlotChanged;
  } else if (__atomic_load_n(slot, __ATOMIC_ACQUIRE) != value) {
    status = GotStatus::kWriteVerifyFailed;  // a writer outside this lock stored after us; theirs stays
  }
  if (restoreReadOnly &&
      Protect(reinterpret_cast<void*>(pages.start), pages.length, PROT_READ) != 0) {
    *savedErrno = errno;
    if (status == GotStatus::kOk) {
      void* ours = value;
      __atomic_compare_exchange_n(slot, &ours, expected, false, __ATOMIC_RELEASE, __ATOMIC_ACQUIRE);
    }
    if (Protect(reinterpret_cast<void*>(pages.start), pages.length, PROT_READ) != 0) {
      char page[19];
      LogFields(LogLevel::kError, "got_hook",
                {{"op", "protect"}, {"status", "page_left_writable"},
                 {"page", HexString(page, pages.start)}});
    }
    return GotStatus::kRestoreProtectFailed;
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
    const auto* cursor = reinterpret_cast<const unsigned char*>(image.base + ph.p_vaddr);
    const unsigned char* const end = cursor + ph.p_memsz;
    while (static_cast<std::size_t>(end - cursor) >= sizeof(Elf64_Nhdr)) {
      Elf64_Nhdr nhdr;
      std::memcpy(&nhdr, cursor, sizeof(nhdr));
      const std::size_t nameLen = (nhdr.n_namesz + 3U) & ~3U;
      const std::size_t descLen = (nhdr.n_descsz + 3U) & ~3U;
      const std::size_t remaining = static_cast<std::size_t>(end - cursor) - sizeof(nhdr);
      if (nameLen > remaining || descLen > remaining - nameLen) break;
      const unsigned char* name = cursor + sizeof(nhdr);
      const unsigned char* desc = name + nameLen;
      if (nhdr.n_type == NT_GNU_BUILD_ID && nhdr.n_namesz == 4 &&
          std::memcmp(name, "GNU", 4) == 0 && nhdr.n_descsz * 2U + 1U <= capacity) {
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

struct Prepared {
  ElfImage image;
  SlotResolution resolution;
  void* original = nullptr;
  char actualBuildId[64] = {};   // filled when a build-ID check fails
};

// The one failure record for an operation. Build IDs and module names are public
// identifiers; nothing here is a credential.
void LogFailure(const char* op, const GotTarget& t, GotStatus status, const char* stage, int err,
                const char* actualBuildId = nullptr) {
  const char* module = t.module != nullptr ? t.module : "(null)";
  const char* symbol = t.symbol != nullptr ? t.symbol : "(null)";
  if (actualBuildId != nullptr) {
    LogFields(LogLevel::kError, "got_hook",
              {{"op", op}, {"status", GotStatusName(status)}, {"stage", stage}, {"module", module},
               {"symbol", symbol}, {"reloc", RelocKindName(t.kind)}, {"errno", err},
               {"expected_build_id", t.buildId != nullptr ? t.buildId : ""},
               {"actual_build_id", actualBuildId[0] != '\0' ? actualBuildId : "(none)"}});
    return;
  }
  LogFields(LogLevel::kError, "got_hook",
            {{"op", op}, {"status", GotStatusName(status)}, {"stage", stage}, {"module", module},
             {"symbol", symbol}, {"reloc", RelocKindName(t.kind)}, {"errno", err}});
}

// Everything that can be checked without writing. On kOk the slot is reserved.
GotStatus Prepare(const GotTarget& target, void* hookFn, ImageLookup lookup, Prepared* out) {
  if (!lookup(target.module, &out->image)) return GotStatus::kModuleNotLoaded;
  if (target.buildId != nullptr) {
    if (!ReadBuildId(out->image, out->actualBuildId, sizeof(out->actualBuildId)) ||
        std::strcmp(out->actualBuildId, target.buildId) != 0) {
      return GotStatus::kBuildIdMismatch;
    }
  }
  out->resolution = ResolveSlot(out->image, target, kNativeRelocs);
  if (out->resolution.status != GotStatus::kOk) return out->resolution.status;

  void* const current = __atomic_load_n(out->resolution.slot, __ATOMIC_ACQUIRE);
  if (current == hookFn) return GotStatus::kAlreadyInstalled;
  if (target.expectedOriginal != nullptr) {
    if (current != target.expectedOriginal) return GotStatus::kOriginalMismatch;
  } else if (current == nullptr || !IsExecutableAddress(current)) {
    return GotStatus::kOriginalImplausible;
  }
  const GotStatus reserved = ReserveSlot(out->resolution.slot);
  if (reserved != GotStatus::kOk) return reserved;
  out->original = current;
  return GotStatus::kOk;
}

}  // namespace

ProtectFn SetProtectFunction(ProtectFn fn) {
  return g_protect.exchange(fn, std::memory_order_acq_rel);
}

StoreObserver SetStoreObserver(StoreObserver observer) {
  return g_storeObserver.exchange(observer, std::memory_order_acq_rel);
}

GotStatus GotHook::Install(const GotTarget& target, void* hookFn, void** originalOut,
                           ImageLookup lookup) {
  if (target.module == nullptr || target.symbol == nullptr || hookFn == nullptr ||
      originalOut == nullptr || lookup == nullptr) {
    LogFailure("install", target, GotStatus::kBadArgument, "arguments", 0);
    return GotStatus::kBadArgument;
  }
  if (installed_) {
    LogFailure("install", target, GotStatus::kAlreadyInstalled, "handle", 0);
    return GotStatus::kAlreadyInstalled;
  }

  GotStatus status = GotStatus::kOk;
  int savedErrno = 0;
  Prepared prepared;
  const nevr::hook::AttachStage stage = nevr::hook::AttachPublished(
      originalOut,
      [&](void** original) {
        status = Prepare(target, hookFn, lookup, &prepared);
        if (status != GotStatus::kOk) return false;
        *original = prepared.original;
        return true;
      },
      [&] {
        status = WriteSlot(prepared.resolution.slot, prepared.original, hookFn,
                           prepared.resolution.restoreReadOnly, &savedErrno);
        return status == GotStatus::kOk;
      },
      [&] { ReleaseSlot(prepared.resolution.slot); },
      [&] { return status == GotStatus::kRestoreProtectFailed; });

  if (stage != nevr::hook::AttachStage::kAttached) {
    LogFailure("install", target, status, nevr::hook::AttachStageName(stage), savedErrno,
               status == GotStatus::kBuildIdMismatch ? prepared.actualBuildId : nullptr);
    return status;
  }
  target_ = target;
  lookup_ = lookup;
  image_ = prepared.image;
  slot_ = prepared.resolution.slot;
  hookFn_ = hookFn;
  original_ = prepared.original;
  restoreReadOnly_ = prepared.resolution.restoreReadOnly;
  installed_ = true;
  char vaddr[19], bias[19];
  LogFields(LogLevel::kInfo, "got_hook",
            {{"op", "install"}, {"status", "ok"}, {"module", target.module},
             {"symbol", target.symbol}, {"reloc", RelocKindName(target.kind)},
             {"slot_vaddr", HexString(vaddr, prepared.resolution.slotVaddr)},
             {"load_bias", HexString(bias, image_.base)}});
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
    // The module is gone or moved: the slot memory is not ours to write. The
    // hook went with it, so the handle is released.
    ReleaseSlot(slot_);
    installed_ = false;
    LogFailure("remove", target_, GotStatus::kModuleChanged, "revalidate", 0);
    return GotStatus::kModuleChanged;
  }
  // Compare-and-swap under the write lock: a hook chained on top of ours, or a
  // module reloaded at the same base, leaves kSlotChanged and the slot untouched.
  int savedErrno = 0;
  const GotStatus status = WriteSlot(slot_, hookFn_, original_, restoreReadOnly_, &savedErrno);
  if (status != GotStatus::kOk) {
    LogFailure("remove", target_, status, "restore", savedErrno);
    return status;
  }
  ReleaseSlot(slot_);
  installed_ = false;
  LogFields(LogLevel::kInfo, "got_hook",
            {{"op", "remove"}, {"status", "ok"}, {"module", target_.module},
             {"symbol", target_.symbol}, {"reloc", RelocKindName(target_.kind)}});
  return GotStatus::kOk;
}

void GotHook::Forget() {
  if (!installed_) return;
  ReleaseSlot(slot_);
  installed_ = false;
  LogFields(LogLevel::kWarn, "got_hook",
            {{"op", "forget"}, {"status", "ok"}, {"module", target_.module},
             {"symbol", target_.symbol}, {"reloc", RelocKindName(target_.kind)}});
}

}  // namespace sentinel
