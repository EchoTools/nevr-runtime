/* Exact GOT import hooking for arm64 ELF: the Android/Bionic backend of the
 * hook lifecycle in core/hook_lifecycle.h.
 *
 * Windows nevr-runtime patches an instruction prologue in place (MinHook). That
 * does not translate to Android: code pages are not writable and correct
 * patching needs an architecture-specific trampoline. What does translate is an
 * import hook: replace the pointer a module calls through for a symbol it
 * resolves at load time. The pointer lives in `.got`/`.got.plt`, a data page.
 *
 * This reaches only calls a module makes through a dynamic relocation that names
 * a symbol, whether that symbol is defined in another library or in the module
 * itself. It cannot reach a direct call, an inlined call, or a call through a
 * vtable. An internal hook is a row of the build-time libr15 patch (libr15_attach.h, docs/adr/0021).
 *
 * A target is one slot, named by module, symbol and relocation type
 * (R_AARCH64_JUMP_SLOT or R_AARCH64_GLOB_DAT). Nothing is patched unless
 *
 *   - the module is loaded and its optional build ID matches,
 *   - for a JUMP_SLOT, the module is linked BIND_NOW. A lazily bound slot starts
 *     out holding the address of a lazy-binding stub, not of the target, and the
 *     dynamic linker rewrites the slot when it resolves it. Both pinned Quest
 *     libraries are BIND_NOW (`readelf -d`: FLAGS BIND_NOW). How Bionic resolves
 *     lazy slots was not measured here, so a lazy JUMP_SLOT is refused rather
 *     than hooked on a guess,
 *   - exactly one relocation of the requested type names the symbol (or the one
 *     at the pinned link-time address does),
 *   - the relocation has no addend and its slot is an aligned pointer inside a
 *     writable segment of the image,
 *   - the slot holds the expected original, or an address inside an executable
 *     mapping, and does not already hold the hook,
 *   - no other handle in the process owns the slot.
 *
 * Writes are serialized by one process-wide lock and are compare-and-swap: Install
 * stores only over the original it read, Remove only over its own hook. A slot
 * changed by anyone else (a hook chained on top, a module reloaded at the same
 * base) yields kSlotChanged and is left alone. The protection a write restores is
 * read from /proc/self/maps under that lock; the PT_GNU_RELRO range is the fallback when that is
 * unreadable. More than one slot for the symbol and relocation type is refused
 * unless the target pins the slot.
 *
 * Install and Remove log exactly one structured line (hook_log.h) and return a
 * status; a failed Install leaves the slot and its page protection as they were
 * and the caller's original pointer at its entry value, with one exception: if the
 * our entry was stored in the slot (rolled back because the page could not be
 * re-protected, or overwritten by another writer right after), the original stays
 * published, because a thread may already have entered the detour while the hook
 * was live and the original is the real function. A failed re-protect after a
 * failed store reports the store's status, not kRestoreProtectFailed. Tokens never include a secret.
 */
#pragma once

#include <elf.h>

#include <cstddef>
#include <cstdint>
#include <optional>

namespace sentinel {

enum class RelocKind : std::uint8_t { kJumpSlot, kGlobDat };

const char* RelocKindName(RelocKind kind);

// The two relocation type numbers a backend scans for. The production value is
// the host architecture's; tests and fixture checks pass AArch64's explicitly so
// an AArch64 image can be resolved on any host.
struct RelocNumbers {
  std::uint32_t jumpSlot;
  std::uint32_t globDat;
};

constexpr RelocNumbers kAarch64Relocs{R_AARCH64_JUMP_SLOT, R_AARCH64_GLOB_DAT};
constexpr RelocNumbers kX86_64Relocs{R_X86_64_JUMP_SLOT, R_X86_64_GLOB_DAT};
#if defined(__aarch64__)
constexpr RelocNumbers kNativeRelocs = kAarch64Relocs;
#elif defined(__x86_64__)
constexpr RelocNumbers kNativeRelocs = kX86_64Relocs;
#else
#error "got_hook supports aarch64 (Quest) and x86_64 (host tests) only"
#endif

enum class GotStatus : std::uint8_t {
  kOk,
  kBadArgument,
  kAlreadyInstalled,       // this handle, another handle, or the slot already holds the hook
  kModuleNotLoaded,
  kNoDynamicSegment,
  kMalformedDynamic,       // a dynamic table is missing, unsupported or outside the image
  kBuildIdMismatch,
  kSymbolNotFound,         // no relocation of any covered type names the symbol
  kWrongRelocationType,    // the symbol is only relocated under the other type
  kAmbiguousRelocation,    // several slots match and no link-time address pins one
  kSlotOffsetMismatch,     // the pinned link-time address is not a matching relocation
  kLazyBinding,            // JUMP_SLOT in a module not linked BIND_NOW (see above)
  kUnsupportedAddend,
  kSlotMisaligned,
  kSlotOutsideImage,       // not inside a writable PT_LOAD
  kOriginalMismatch,       // slot value differs from the expected original
  kOriginalImplausible,    // slot value is null or not in an executable mapping
  kProtectFailed,
  kWriteVerifyFailed,
  kRestoreProtectFailed,
  kNotInstalled,
  kModuleChanged,          // Remove: the module unloaded, moved or no longer resolves the slot
  kSlotChanged,            // the slot no longer holds the value this write expected (chained hook, reload)
  kRegistryFull,           // the registry has no room for this hook (kRegistryCapacity, kCriticalReserve below)
  kSlotPoisoned,           // a failed Install left our entry possibly reachable through this slot
};

// Stable tokens for log lines and tests.
const char* GotStatusName(GotStatus status);

// The registry of reserved slots: one entry for every slot a handle holds, process-wide. Its size is a budget, not a guess:
// hook_budget.h lists the handles every file declares, a static_assert there keeps the sum plus its headroom inside
// kRegistryCapacity, and tools/tests/test_quest_hook_budget.py checks the list against the source.
inline constexpr std::size_t kRegistryCapacity = 256;

// The room only a kCritical hook may use. A hook the game cannot be played without (the login, the matchmaking redirect and
// the login's prerequisites) takes its slot from the whole registry; every other hook stops kCriticalReserve entries short of
// it, so no number of normal hooks, the crash handler's among them, can leave a critical one without a slot. Those hooks
// install after the game loads libpnsrad* (the dlopen hook), long after the constructor filled the rest.
inline constexpr std::size_t kCriticalReserve = 40;

enum class HookPriority : std::uint8_t { kNormal, kCritical };

// A second build ID a target may carry for a module (ADR 0021 section 12). The libr15 the game runs is a patched copy of the
// stock file whose live GNU note is a derived ID, while a plugin's addresses are properties of the stock file: the caller
// that knows which is which sets this function, and `Prepare` accepts a target whose `buildId` equals the live note OR
// the alias of the live note. The function gets the module's live note (lowercase hex) at every install and returns the
// alias, or nullptr for none; it must be fast, must not throw and must return a string that outlives the call (the
// backend knows nothing about libr15: this is the only place the identity rule enters). Unset: only the live note is
// accepted. Returns the previous function.
using BuildIdAliasFn = const char* (*)(const char* liveNote);
BuildIdAliasFn SetBuildIdAlias(BuildIdAliasFn fn);

// A live build ID that a service-side target (GotTarget::serviceSide) accepts in place of its own (#1737). Attach sets it
// once, to the stock ID, when the live libr15 is a pinned file run in service-only mode: then only the service-side hooks
// install on it, and every other libr15 hook refuses as build_id_mismatch. nullptr (the default): no such ID.
void SetServiceSideBuildId(const char* liveNote) noexcept;

// What to hook. Every string must outlive the handle (string literals).
struct GotTarget {
  const char* module;   // soname, or a path whose last segment is the soname
  const char* symbol;   // exact dynstr name, no @VERSION suffix
  RelocKind kind;
  const char* buildId;  // lowercase hex GNU build ID, or nullptr to skip
  std::optional<std::uint64_t> slotVaddr;  // pinned link-time address of the slot
  const void* expectedOriginal;            // exact slot value, or nullptr to require plausibility
  HookPriority priority = HookPriority::kNormal;  // kCritical: may use the registry's reserve (kCriticalReserve)
  bool serviceSide = false;  // a service-side hook (sign-in, matchmaking, redirect): see SetServiceSideBuildId

  GotTarget(const char* moduleName, const char* symbolName, RelocKind relocKind,
            const char* build = nullptr, std::optional<std::uint64_t> pinnedSlot = std::nullopt,
            const void* original = nullptr)
      : module(moduleName),
        symbol(symbolName),
        kind(relocKind),
        buildId(build),
        slotVaddr(pinnedSlot),
        expectedOriginal(original) {}

  // The same target, marked as a service-side hook (SetServiceSideBuildId).
  GotTarget ServiceSide() const {
    GotTarget copy = *this;
    copy.serviceSide = true;
    return copy;
  }

  // The same target, taken from the registry's reserve when the registry is otherwise full.
  GotTarget Critical() const {
    GotTarget copy = *this;
    copy.priority = HookPriority::kCritical;
    return copy;
  }
};

// A loaded (or synthetic) ELF64 image: load bias and program headers.
struct ElfImage {
  std::uintptr_t base = 0;
  const Elf64_Phdr* phdr = nullptr;
  std::size_t phnum = 0;
  char name[256] = {};
};

// Finds a loaded module by soname or path tail via dl_iterate_phdr.
bool FindLoadedImage(const char* module, ElfImage* out);

// How a handle finds its module. Production uses FindLoadedImage; a test passes
// a lookup that returns an image it built in memory.
using ImageLookup = bool (*)(const char* module, ElfImage* out);

// Lowercase hex of the NT_GNU_BUILD_ID note into `out`; false if absent.
bool ReadBuildId(const ElfImage& image, char* out, std::size_t capacity);

struct SlotResolution {
  GotStatus status = GotStatus::kModuleNotLoaded;
  std::uint64_t slotVaddr = 0;      // link-time address of the slot
  void** slot = nullptr;            // runtime address (base applied)
  bool restoreReadOnly = false;     // slot page is RELRO: protect read-only after a write
};

// Resolves a target to its slot inside `image` without reading or writing the
// slot. Works on any ELF64 image whose dynamic pointers are either link-time
// (Bionic) or already relocated (glibc); pure apart from reading `image`.
SlotResolution ResolveSlot(const ElfImage& image, const GotTarget& target,
                           const RelocNumbers& relocs);

// Gives back poisoned reservations (slots where a failed Install left our entry possibly
// reachable) that lie in [begin, begin + length) AND whose address /proc/self/maps shows as
// unmapped, i.e. the module that owned them is gone. A poisoned slot on a live module is never
// released, so a retry cannot publish a foreign hook that chains our entry as the original
// and build a call cycle; it is refused with kSlotPoisoned. Nothing in the sentinel calls this:
// the game's libraries are never unloaded, so there is no unload hook point to call it from.
// A poisoned slot occupies one registry entry for the rest of the process, and a
// module reloaded at the same address gets kSlotPoisoned until this has released it.
//
// Residual window, not closed: a foreign writer that reads our entry inside the store-to-
// rollback window and stores its own chain after a SUCCESSFUL rollback leaves a hook that
// calls our entry through a slot we released rather than poisoned.
void ReleasePoisonedSlotsIn(const void* begin, std::size_t length);

// What the registry holds and what it turned away, for the start-up summary and the self-check. A refusal is
// kRegistryFull: the hook was NOT installed and the game runs its own code in that slot.
struct RegistryRefusal {
  char module[64];
  char symbol[48];
  bool critical;
};
inline constexpr std::size_t kRefusalLedgerSize = 8;

struct RegistryStats {
  std::size_t capacity = kRegistryCapacity;
  std::size_t criticalReserve = kCriticalReserve;
  std::size_t used = 0;             // entries held now (poisoned ones included)
  std::size_t highWater = 0;        // the most entries held at once, this process
  std::size_t refused = 0;          // installs turned away for want of room, this process
  std::size_t refusedCritical = 0;  // of those, how many were critical
  std::size_t ledgerCount = 0;      // entries of `lost` that are filled (the first kRefusalLedgerSize refusals)
  RegistryRefusal lost[kRefusalLedgerSize] = {};
};
RegistryStats RegistrySnapshot();

// Test seam: empties the registry's counters and the refusal ledger. Slots held by live handles stay held.
void ResetRegistryCountersForTest();

// Test seam: how /proc/self/maps is opened (default: open(2) on that path). A test returns a
// descriptor whose reads fail or end at once to show that unreadable maps never read as
// "unmapped". Returns the previous opener.
using MapsOpener = int (*)();
MapsOpener SetMapsOpener(MapsOpener opener);

// Test seam. Called with the process-wide write lock held, around the
// compare-and-swap store of Install and Remove. Production code leaves it unset. Returns the previous observer.
// kBeforeStore: after the page is writable, before the compare-and-swap.
// kAfterStore: only when the compare-and-swap succeeded, before the read-back.
enum class StorePhase { kBeforeStore, kAfterStore };
using StoreObserver = void (*)(void** slot, void* value, StorePhase phase);
StoreObserver SetStoreObserver(StoreObserver observer);

// Test seam for the page-protection call (default: mprotect). Used to make a
// re-protect fail deterministically. Returns the previous function.
using ProtectFn = int (*)(void* addr, std::size_t length, int prot);
ProtectFn SetProtectFunction(ProtectFn fn);

// One installed (or empty) hook. Not copyable. Install and Remove on one handle
// must not race each other; the callbacks the hook redirects may run on any
// thread at any time.
//
// A handle has no destructor on purpose. A hook is process-lifetime state: its slot
// stays redirected, and its slot reservation stays held, when the handle goes out
// of scope. Un-hooking from a destructor would run during static destruction while
// other threads may still be inside the detour. Call Remove() (or Forget(), to drop
// the reservation without writing) before the handle is destroyed if the hook must
// not outlive it.
class GotHook {
 public:
  GotHook() = default;
  GotHook(const GotHook&) = delete;
  GotHook& operator=(const GotHook&) = delete;

  // Writes the original value back and releases the slot. The pointer previously
  // published through `originalOut` stays valid: it is the real function.
  GotStatus Remove();

  // Drops the handle without writing: releases the slot reservation and clears
  // the state. For a module that was unloaded and loaded again at the same base,
  // where Remove would report kSlotChanged for a slot that now holds a fresh
  // value. Does nothing if not installed.
  void Forget();

  bool installed() const { return installed_; }

 private:
  // The raw install, taking any function pointer: redirects the slot to `hookFn` and stores
  // the previous slot value in `*originalOut` before the slot changes; `*originalOut` is
  // restored to its entry value on failure (see the notes at the top of this file for the
  // one exception). Private on purpose: production code installs a CallbackThunk through
  // InstallThunk (hook_install.h), which keeps every hook inside the frame sensor's reach;
  // tests use GotHookTestAccess.
  friend struct ThunkInstaller;
  friend struct GotHookTestAccess;
  GotStatus Install(const GotTarget& target, void* hookFn, void** originalOut,
                    ImageLookup lookup = FindLoadedImage);

  bool installed_ = false;
  GotTarget target_{"", "", RelocKind::kJumpSlot};
  ElfImage image_{};
  void** slot_ = nullptr;
  void* hookFn_ = nullptr;
  void* original_ = nullptr;
  bool restoreReadOnly_ = false;
  ImageLookup lookup_ = FindLoadedImage;
};

}  // namespace sentinel
