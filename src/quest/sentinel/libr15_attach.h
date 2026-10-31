/* The sentinel's run-time half of the build-time libr15 patch (docs/adr/0021, sections 3 to 7, 10 and 11).
 *
 * The patcher (src/quest/patch) rewrote libr15.so before it was packaged: each inline site starts with
 * `adrp x16 ; ldr x16,[x16,#n] ; br x16` through an 8-byte slot in the tail of .bss, the credits ADRP+ADD pairs load
 * their blob address from a slot, and the file carries its own GNU build ID. At run time this file only ever writes
 * DATA: no mprotect, no mmap and no store on any page of the image's code, on any headset. (A Quest on the current
 * Horizon OS denies `execmod`, a private file mapping written and then made executable; ADR 0021, Context.)
 *
 * Attach(), the first libr15 step:
 *   1. finds libr15 and reads its GNU build ID;
 *   2. the stock ID -> `libr15_not_patched`, any ID but PatchedBuildId(extra) -> `build_id_mismatch`: nothing is
 *      installed and the game runs as it came (Attach returns, the process is not ended);
 *   3. the patched ID -> checks EVERY row before it stores anything:
 *        (a) `libr15_patch_row_stub`:    the live words at each site are exactly what the table implies (the stub, or the
 *                                        ADRP+LDR pair), so a stock site or a stub from another table is refused;
 *        (b) `libr15_patch_row_slot`:    decoding the live words, as they sit at their run-time address, names exactly
 *                                        base + the row's slot (a load bias that is not page aligned fails here);
 *        (c) `libr15_patch_row_segment`: the slot lies in a writable PT_LOAD of the live program headers, in its bss
 *                                        part (at or after p_vaddr + p_filesz), and outside PT_GNU_RELRO;
 *        a word-patch row of the extra rows (section 11): `libr15_patch_row_words`, the live words are the replacements.
 *      A failing row logs its id and the check, ends the process through the process-end call with that reason (the
 *      file is ours, so a mismatch is a build defect and an unfilled slot would crash later) and returns kRowFailed;
 *   4. only then builds one trampoline per inline row, from the TABLE's stock words and never from live bytes
 *      (BuildTrampoline, a 3-word window), each in its own fresh anonymous page: mmap RW, copy, mprotect RX, flush.
 *      A failure (`libr15_patch_trampoline_failed`) logs the errno and the page permissions, unmaps what was made, ends
 *      the process and returns kTrampolineFailed, with no slot written;
 *   5. stores every trampoline's address in its row's slot (and, in the credits row's slot, base + the blob's
 *      address, the game's own text). From that moment every patched function behaves as stock: a hook that is
 *      refused or not wanted stays pass-through. Verified() is then true and one `libr15_patch` line says so.
 *
 * Activation (a hook's install) is one atomic store of the detour into the row's slot. It is reached only through
 * sentinel::InstallRowThunk (hook_install.h), which takes a CallbackThunk.
 *
 * Built with -fno-exceptions, like the rest of the hook path. Nothing here has a namespace-scope constructor.
 */
#pragma once

#include <sys/types.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "got_hook.h"
#include "quest/patch/libr15_patch_table.h"

namespace sentinel {
struct RowThunkInstaller;
}

namespace nevr_quest::libr15_patch {

enum class AttachStatus : std::uint8_t {
  kVerified,           // every row checked, every slot filled
  kAlreadyVerified,    // a second Attach in one process; nothing is touched
  kModuleNotLoaded,    // the lookup found no libr15
  kNoBuildId,          // the image has no readable GNU build-ID note
  kNotPatched,         // the live build ID is the stock one (`libr15_not_patched`); nothing installed
  kBuildIdMismatch,    // any other build ID (`build_id_mismatch`); nothing installed
  kRowFailed,          // a row failed check (a), (b), (c) or the word check; the process-end call was made
  kTrampolineFailed,   // a trampoline could not be built (`libr15_patch_trampoline_failed`); the process-end call was made
  kPinnedServiceOnly,  // the stock build ID and a file sha256 in kLibR15ServiceOnlySha256 (#1737): no row is checked or
                       // filled, Verified() stays false, and only the service-side GOT hooks may install
};

// The libr15 files the runtime runs in service-only mode (#1737): a reference build's own byte-patched libr15, which keeps
// the stock build ID. Only the file with exactly this sha256 is accepted; every other stock-ID file is libr15_not_patched.
// In this mode the runtime keeps sign-in, matchmaking and the server redirect (the GotTarget::ServiceSide hooks) and
// installs none of its libr15 patches: no row, no credits slot, no other libr15 GOT hook.
inline constexpr const char* kLibR15ServiceOnlySha256[] = {
    "6e9db28d612019b0e5b2e2ab515e1f9bd15d1a8f8d345060160e3172864540e1",  // #1737
};

// Stable tokens for log lines and tests.
const char* AttachStatusName(AttachStatus status);

// The reason tokens of ADR 0021 section 7 (the `reason` of a refusal line and the process-end call).
inline constexpr const char* kReasonNotPatched = "libr15_not_patched";
inline constexpr const char* kReasonBuildIdMismatch = "build_id_mismatch";
inline constexpr const char* kReasonRowStub = "libr15_patch_row_stub";
inline constexpr const char* kReasonRowSlot = "libr15_patch_row_slot";
inline constexpr const char* kReasonRowSegment = "libr15_patch_row_segment";
inline constexpr const char* kReasonRowWords = "libr15_patch_row_words";
inline constexpr const char* kReasonTrampolineFailed = "libr15_patch_trampoline_failed";
inline constexpr const char* kReasonPinnedServiceOnly = "libr15_pinned_service_only";

// The calls Attach makes, replaceable by a test (nullptr in AttachOptions: the real one).
using ProtectFn = int (*)(void* addr, std::size_t length, int prot);  // mprotect's signature: a trampoline page only
using MapFn = void* (*)(void* addr, std::size_t length, int prot, int flags, int fd, off_t offset);  // mmap's
using EndFn = void (*)(int exitCode, const char* reason) noexcept;  // process_end::EndWithExitCode's
// The sha256 of the file at `path` as 64 lowercase hex characters into out[65]; false when it cannot be read.
using FileDigestFn = bool (*)(const char* path, char* out) noexcept;

struct AttachOptions {
  const char* module = nullptr;                  // nullptr: pinned::kLibR15 ("libr15.so")
  sentinel::ImageLookup lookup = sentinel::FindLoadedImage;
  std::vector<patch::WordPatchRow> extraRows;    // ADR section 11; empty for the public build
  std::string extraRowsSha256;                   // the digest of the extra-rows file as given; logged, the rows are not
  ProtectFn protect = nullptr;
  MapFn map = nullptr;
  EndFn end = nullptr;
  FileDigestFn digest = nullptr;  // nullptr: the file read in 64 KiB chunks through data::Sha256
};

struct AttachResult {
  AttachStatus status = AttachStatus::kModuleNotLoaded;
  std::string reason;  // the token a refusal logged; empty on kVerified
  std::string rowId;   // the row a kRowFailed or kTrampolineFailed names
};

AttachResult Attach(const AttachOptions& options);

// True once Attach returned kVerified in this process. The plugin identity (ADR section 12) reads these two.
bool Verified() noexcept;
// The patched build ID (40 lowercase hex characters) that Attach verified, or nullptr when it did not.
const char* VerifiedBuildId() noexcept;

// The run-time address of the patched function a row names (base + the row's first site), or 0 when Attach has not
// verified this process's libr15 or the row is unknown. A hook file logs it as the `target` of its install line.
std::uintptr_t RowSiteAddress(const char* rowId) noexcept;

// The address of the credits row's 8-byte slot (the data the two patched ADRP+LDR pairs load the credits blob's address
// from, ADR section 8), or nullptr unless Attach verified this process's libr15. Attach stored base + the game's own blob
// address there; credits_text replaces it with one atomic store.
void** CreditsSlot() noexcept;

enum class ActivateStatus : std::uint8_t {
  kOk,
  kBadArgument,        // a null row id, detour or originalOut
  kNotVerified,        // Attach has not verified this process's libr15
  kUnknownRow,
  kNotInlineRow,       // the credits row (its slot is filled by credits_text) or a word patch
  kAlreadyActivated,
};
const char* ActivateStatusName(ActivateStatus status);

struct AttachTestAccess;

class RowActivation {
 public:
  RowActivation() = delete;

 private:
  friend struct sentinel::RowThunkInstaller;
  friend struct AttachTestAccess;

  // Publishes the row's trampoline in *originalOut, then stores `detour` in the row's slot with one release store.
  // Refused (a named status and one `inline_hook` error line) when not verified, when the row is unknown or not an
  // inline row, or when it was activated already; then *originalOut and the slot are untouched.
  static ActivateStatus ActivateRow(const char* rowId, void* detour, void** originalOut);
};

// Forgets the process-wide record and unmaps the trampoline pages. Tests only; the slots in the image are the
// test's own to reset.
void ResetForTest() noexcept;

}  // namespace nevr_quest::libr15_patch
