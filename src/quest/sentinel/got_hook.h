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
 * itself: a PLT or GOT word, or a vtable or factory word that an R_AARCH64_ABS64
 * relocation fills with the symbol's address. It cannot reach a direct call, an
 * inlined call, or a vtable word no relocation names. An internal hook is a row
 * of the build-time libr15 patch (libr15_attach.h, docs/adr/0021).
 *
 * A target is one slot, named by module, symbol and relocation type
 * (R_AARCH64_JUMP_SLOT, R_AARCH64_GLOB_DAT or R_AARCH64_ABS64). Nothing is patched unless
 *
 *   - the module is loaded and its optional build ID matches,
 *   - for a JUMP_SLOT, the module is linked BIND_NOW. A lazily bound slot starts
 *     out holding the address of a lazy-binding stub, not of the target, and the
 *     dynamic linker rewrites the slot when it resolves it. Both pinned Quest
 *     libraries are BIND_NOW (`readelf -d`: FLAGS BIND_NOW). How Bionic resolves
 *     lazy slots was not measured here, so a lazy JUMP_SLOT is refused rather
 *     than hooked on a guess,
 *   - exactly one relocation of the requested type names the symbol (or the one
 *     at the pinned link-time address does). An ABS64 target always pins its slot: a
 *     symbol that a vtable lists has one such word in every vtable that lists it, and the
 *     hook is for one of them. The pin must be a word whose relocation names the symbol
 *     (kSlotOffsetMismatch otherwise); without a pin the target is kAmbiguousRelocation
 *     when the symbol has several words and kPinRequired when it has one,
 *   - the relocation has no addend and its slot is an aligned pointer inside a
 *     writable segment of the image,
 *   - the slot holds the expected original, or an address inside an executable
 *     mapping (the first handler on a slot); a later handler joins the slot's chain,
 *     and the slot must hold the chain's newest hook,
 *   - the handler is not already in the chain, and no entry the chain holds was left
 *     reachable by a failed install (kSlotPoisoned).
 *
 * Writes are serialized by one process-wide lock and are compare-and-swap: Install
 * stores only over the original it read, Remove only over its own hook. A slot
 * changed by anyone else (a hook chained on top, a module reloaded at the same
 * base) yields kSlotChanged and is left alone. The protection a write restores is
 * read from /proc/self/maps under that lock; the PT_GNU_RELRO range is the fallback when that is
 * unreadable. More than one slot for the symbol and relocation type is refused
 * unless the target pins the slot.
 *
 * Several handlers may hold one slot: a chain (docs/adr/0034). The newest handler runs first and reaches the one before
 * it through its original; the oldest reaches the game's function. A handle's original is the value published through
 * the originalOut pointer it was installed with, so a handler calls its original the same way whether or not it is
 * chained. Remove relinks the handler that called the removed one (or, for the newest, restores the slot to what it
 * called), so the others keep running. The chain's order is the order of registration. A handler's install is refused,
 * with a Warning that names the slot, the chain and the reason, when the slot holds a value the chain does not account for.
 *
 * A data word the game reads, such as a heap-size descriptor pointer, is not a hook: GotDataSlot
 * (below) stores a value of ours over it and puts the game's value back, through the same lock and
 * registry.
 *
 * Assembly entries. A few hooks cannot be a CallbackThunk because they read what the thunk's handler cannot see: the caller's
 * x30, sp or a callee-saved register. Such a hook is a reviewed assembly entry (callback_thunk.h rule 7) installed through
 * InstallAsmEntry (hook_install.h) and the friend AsmEntryInstaller. It joins the chain like any hook (it may sit above the
 * handlers already on its slot, and it calls the newest of them), reads the word it calls with acquire semantics at every
 * call, and the manager publishes that word before the slot changes and again when the hook it calls leaves. It acts on the game's own call
 * site, so it must stay the newest hook on its slot: a later install on that slot, a handler's or another entry's, is refused
 * with kSlotHeldByEntry, a Warning that names the refused hook's owner and the chain with the entry marked (asm_entry). The
 * rule ends when the entry leaves the chain: its Remove (which forgets the link when it finds the module gone or moved)
 * or its Forget.
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

// kAbs64: a word that an R_AARCH64_ABS64 relocation names a symbol for (a vtable or factory entry). Its target pins the slot.
enum class RelocKind : std::uint8_t { kJumpSlot, kGlobDat, kAbs64 };

const char* RelocKindName(RelocKind kind);

// The relocation type numbers a backend scans for. The production value is
// the host architecture's; tests and fixture checks pass AArch64's explicitly so
// an AArch64 image can be resolved on any host.
struct RelocNumbers {
  std::uint32_t jumpSlot;
  std::uint32_t globDat;
  std::uint32_t abs64;
};

constexpr RelocNumbers kAarch64Relocs{R_AARCH64_JUMP_SLOT, R_AARCH64_GLOB_DAT, R_AARCH64_ABS64};
constexpr RelocNumbers kX86_64Relocs{R_X86_64_JUMP_SLOT, R_X86_64_GLOB_DAT, R_X86_64_64};
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
  kAlreadyInstalled,       // this handle already holds a hook, or the handler is already in the slot's chain
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
  kOriginalMismatch,       // slot value differs from the expected original (on a chain: from its newest hook)
  kOriginalImplausible,    // slot value is null or not in an executable mapping
  kProtectFailed,
  kWriteVerifyFailed,
  kRestoreProtectFailed,
  kNotInstalled,
  kModuleChanged,          // Remove: the module unloaded, moved or no longer resolves the slot
  kSlotChanged,            // the slot no longer holds the value this write expected (chained hook, reload)
  kRegistryFull,           // the registry has no room for this hook (kRegistryCapacity, kCriticalReserve below)
  kSlotPoisoned,           // a failed Install left our entry possibly reachable through this slot
  kPinRequired,            // an ABS64 target names its slot by a pinned link-time address, and this one has none
  kSlotClaimed,            // the slot already holds a registry entry a data write may not share, or a data write holds it
  kExecutableMapping,      // a data write's slot names a function or is in an executable mapping, or its new value is a code address (a code pointer is a hook)
  kSlotHeldByEntry,        // the slot's newest hook is an assembly entry, which must stay the newest (see "Assembly entries" above GotHook)
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
  const char* owner = nullptr;  // who installs the hook, for the chain shown on a refusal; nullptr reads as "(unnamed)"

  // constexpr: a GotHook holds a GotTarget, and a namespace-scope array of GotHook (diag/hwdump_install.cpp) must be
  // constant-initialized; check-android-static-init fails on a dynamic initializer there.
  constexpr GotTarget(const char* moduleName, const char* symbolName, RelocKind relocKind,
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

  // The same target, named as the installer of its hook. `name` must outlive the handle.
  GotTarget Owned(const char* name) const {
    GotTarget copy = *this;
    copy.owner = name;
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
  bool inExecutableSegment = false; // the slot's PT_LOAD is executable as well as writable (a data write refuses it)
  std::uint8_t symbolType = 0;      // ELF64_ST_TYPE of the symbol the slot's relocation names (a data write needs STT_OBJECT)
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

// Test seam: the relocation numbers Install, Write and Remove resolve slots with (default: the host architecture's, kNativeRelocs).
// A test that resolves a real AArch64 image on an x86-64 host passes kAarch64Relocs; the pointer must stay valid while it is set.
// Returns the previous setting (nullptr: the default).
const RelocNumbers* SetRelocNumbers(const RelocNumbers* numbers);

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
  constexpr GotHook() = default;  // constexpr for the same reason as GotTarget's constructor
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
  // True when a failed Install left its handler reachable through the slot: the store was live before the rollback, so the
  // entry is poisoned (kSlotPoisoned for the next install). The handler's code must then stay mapped for the life of the
  // process; the plugin service keeps the handle held (slot_hook.cpp).
  bool mayBeLive() const { return mayBeLive_; }

 private:
  // The raw install, taking any function pointer: joins `hookFn` to the slot's chain and redirects the
  // slot to it. Before the slot changes, `*originalOut` is published with the value hookFn calls (the
  // previous slot value, or the chain's newest hook); on failure it is restored to its entry value (see
  // the notes at the top of this file for the one exception). Private on purpose: production code
  // installs a CallbackThunk through InstallThunk (hook_install.h), which keeps every hook inside the frame
  // sensor's reach; tests use GotHookTestAccess, the plugin service uses PluginHandlerInstaller and a reviewed assembly entry
  // uses AsmEntryInstaller (hook_install.h; callback_thunk.h rule 7).
  friend struct ThunkInstaller;
  friend struct GotHookTestAccess;
  friend struct PluginHandlerInstaller;
  friend struct AsmEntryInstaller;
  GotStatus Install(const GotTarget& target, void* hookFn, void** originalOut,
                    ImageLookup lookup = FindLoadedImage);
  // Install for an assembly entry: the same chain join, with the entry marked so that no later hook joins above it.
  // `nextOut` is the word the entry (or its continuation) loads with acquire semantics at every call.
  GotStatus InstallEntry(const GotTarget& target, void* entryFn, void** nextOut, ImageLookup lookup);
  GotStatus InstallImpl(const GotTarget& target, void* hookFn, void** originalOut, ImageLookup lookup, bool asmEntry);

  bool installed_ = false;
  bool mayBeLive_ = false;
  GotTarget target_{"", "", RelocKind::kJumpSlot};
  ElfImage image_{};
  void** slot_ = nullptr;
  void* hookFn_ = nullptr;
  bool restoreReadOnly_ = false;
  ImageLookup lookup_ = FindLoadedImage;
};

// A data word the game reads, pointed at a value of ours (docs/adr/0046 rule 2; docs/adr/0019 section 5 (iii)): the heap-size
// descriptor pointers a libr15 GLOB_DAT slot holds are the case. It is not a hook: there is no thunk, no original to call and no
// chain, so one handle is the slot's only owner, and the store is outside ADR 0034's hook rules (rule 6). It is a write the manager
// makes, so the page-protection call stays in this file (N151).
//
// The slot resolves as a hook's does (GotTarget: module, symbol, kind, build ID, optional pin; `expectedOriginal` is not read, the
// `expected` argument of Write is the check) and takes one registry entry, a normal one unless the target is Critical(). The kind
// is kGlobDat: a slot that holds the address of a data symbol (ADR 0046 rule 2); any other kind is kBadArgument. Write stores only
// over `expected`, under the process-wide lock, and refuses:
//   - a slot whose relocation does not name a data object (anything but STT_OBJECT: a function, an ifunc, or a symbol with no type
//     such as a weak reference nothing defined, which the loader fills with a function): it holds a code pointer, which is a
//     hook's business, installed with InstallThunk so the chain sees it (kExecutableMapping, stage `symbol_type`). The symbol
//     decides, not the address: libr15's .rodata lies inside its executable segment, so a data object's address is an address in
//     an executable segment, and the heap descriptors the first user writes over are such objects;
//   - a `value` that is an address in an executable segment of a loaded module: a code pointer stored here would skip the chain
//     (kExecutableMapping, stage `new_value`). `value` is therefore writable storage of the caller's, not a const object the
//     linker places in an R E segment;
//   - a slot in an executable mapping: its live protection in /proc/self/maps has x, or its PT_LOAD is flagged executable
//     (kExecutableMapping, stages `store` and `segment`);
//   - a slot that already holds a registry entry, a hook's chain or another data write (kSlotClaimed); a hook that asks for a
//     slot a data write holds is refused the same way;
//   - a slot that does not hold `expected` (kOriginalMismatch).
// One Debug line records a write and one a restore; a refusal is one Warning line that names the slot and the stage (logging.md
// Rules 6 and 13). A failed Write holds no entry and leaves the slot as it found it, except that a store another writer
// overwrote right after ours is theirs. It never poisons an entry, where ADR 0019 section 5 (iii) does for a failed restore: a data
// word has no chain through which a stored value could be called, so there is nothing to keep unreachable.
class GotDataSlot {
 public:
  constexpr GotDataSlot() = default;
  GotDataSlot(const GotDataSlot&) = delete;
  GotDataSlot& operator=(const GotDataSlot&) = delete;

  // Stores `value` in the slot `target` names, if it holds `expected` now. Neither may be null.
  GotStatus Write(const GotTarget& target, const void* expected, void* value, ImageLookup lookup = FindLoadedImage);

  // Stores `expected` back, only over `value`, and releases the slot. A slot someone else has changed since is left alone
  // (kSlotChanged) and the handle stays written; Forget() drops it. A module that has gone or moved releases the handle
  // without a write (kModuleChanged).
  GotStatus Restore();

  // Drops the handle without writing, as GotHook::Forget does. Does nothing if not written.
  void Forget();

  bool written() const { return written_; }

 private:
  bool written_ = false;
  GotTarget target_{"", "", RelocKind::kJumpSlot};
  ElfImage image_{};
  void** slot_ = nullptr;
  void* value_ = nullptr;
  bool restoreReadOnly_ = false;
  ImageLookup lookup_ = FindLoadedImage;
};

// The plugin hook service's install (plugin/slot_hook.cpp): GotHook::Install for a handler a plugin supplies at run
// time. Plugin code is outside the frame sensor's build, so the handler is a plain function the host does not thunk;
// callback_thunk.h rule 4 names this as its one exception to InstallThunk.
struct PluginHandlerInstaller {
  static GotStatus Install(GotHook& hook, const GotTarget& target, void* hookFn, void** originalOut,
                           ImageLookup lookup = FindLoadedImage);
};

}  // namespace sentinel
