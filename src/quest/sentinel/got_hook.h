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
 * vtable. An internal hook needs a different backend (docs/adr/0003).
 *
 * A target is one slot, named by module, symbol and relocation type
 * (R_AARCH64_JUMP_SLOT or R_AARCH64_GLOB_DAT). Nothing is patched unless
 *
 *   - the module is loaded and its optional build ID matches,
 *   - for a JUMP_SLOT, the module is linked BIND_NOW (a lazy slot holds a
 *     resolver stub that overwrites the slot on first call, undoing the hook),
 *   - exactly one relocation of the requested type names the symbol (or the one
 *     at the pinned link-time address does),
 *   - the relocation has no addend and its slot is an aligned pointer inside a
 *     writable segment of the image,
 *   - the slot holds the expected original, or an address inside an executable
 *     mapping, and does not already hold the hook,
 *   - no other handle in the process owns the slot.
 *
 * Install and Remove log exactly one structured line (hook_log.h) and return a
 * status; a failed Install leaves the slot, its page protection and the caller's
 * original pointer as they were. Tokens never include a secret.
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
  kLazyBinding,            // JUMP_SLOT in a module not linked BIND_NOW: the resolver would overwrite the hook
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
  kSlotChanged,            // Remove: the slot no longer holds our hook (someone chained on top)
};

// Stable tokens for log lines and tests.
const char* GotStatusName(GotStatus status);

// What to hook. Every string must outlive the handle (string literals).
struct GotTarget {
  const char* module;   // soname, or a path whose last segment is the soname
  const char* symbol;   // exact dynstr name, no @VERSION suffix
  RelocKind kind;
  const char* buildId;  // lowercase hex GNU build ID, or nullptr to skip
  std::optional<std::uint64_t> slotVaddr;  // pinned link-time address of the slot
  const void* expectedOriginal;            // exact slot value, or nullptr to require plausibility

  GotTarget(const char* moduleName, const char* symbolName, RelocKind relocKind,
            const char* build = nullptr, std::optional<std::uint64_t> pinnedSlot = std::nullopt,
            const void* original = nullptr)
      : module(moduleName),
        symbol(symbolName),
        kind(relocKind),
        buildId(build),
        slotVaddr(pinnedSlot),
        expectedOriginal(original) {}
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

// One installed (or empty) hook. Not copyable. Install and Remove on one handle
// must not race each other; the callbacks the hook redirects may run on any
// thread at any time.
class GotHook {
 public:
  GotHook() = default;
  GotHook(const GotHook&) = delete;
  GotHook& operator=(const GotHook&) = delete;

  // Redirects the slot to `hookFn` and stores the previous slot value in
  // `*originalOut` before the slot changes. `*originalOut` is restored to its
  // entry value on failure.
  GotStatus Install(const GotTarget& target, void* hookFn, void** originalOut,
                    ImageLookup lookup = FindLoadedImage);

  // Writes the original value back and releases the slot. The pointer previously
  // published through `originalOut` stays valid: it is the real function.
  GotStatus Remove();

  bool installed() const { return installed_; }

 private:
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
