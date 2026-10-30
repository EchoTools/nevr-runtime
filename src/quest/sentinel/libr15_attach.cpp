#include "libr15_attach.h"

#include <fcntl.h>
#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

#include "arm64_reloc.h"
#include "credits_text.h"
#include "hook_log.h"
#include "mapping_perms.h"
#include "pinned_targets.h"
#include "process_end_hook.h"
#include "quest/data/sha256.h"
#include "quest/patch/libr15_patch_identity.h"
#include "quest/patch/libr15_patch_table.h"
#include "quest/patch/stub_codec.h"

namespace nevr_quest::libr15_patch {
namespace {

using sentinel::ElfImage;
using sentinel::LogField;
using sentinel::LogLevel;

constexpr std::size_t kInlineWindowWords = patch::kStubWords;
constexpr std::size_t kBuildIdText = patch::kBuildIdBytes * 2 + 1;

// One lock for Attach, the activation and the record below. Nothing here is constructed before main: the record is
// zero-initialised data, the lock a constant initialiser (tools/check_quest_static_init.sh).
pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;

struct RowRecord {
  void** slot;         // the run-time address of the row's slot
  void* trampoline;    // the row's trampoline (inline rows only)
  bool activated;
};
bool g_verified = false;
char g_verifiedBuildId[kBuildIdText] = {};
std::uintptr_t g_base = 0;
std::size_t g_pageSize = 0;
RowRecord g_rows[patch::kRowCount] = {};

class LockGuard {
 public:
  LockGuard() { pthread_mutex_lock(&g_mutex); }
  ~LockGuard() { pthread_mutex_unlock(&g_mutex); }
  LockGuard(const LockGuard&) = delete;
  LockGuard& operator=(const LockGuard&) = delete;
};

void DefaultEnd(int exitCode, const char* reason) noexcept { process_end::EndWithExitCode(exitCode, reason); }

// The file's sha256, read in chunks (never the whole 58 MB at once).
bool DefaultDigest(const char* path, char* out) noexcept {
  const int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) return false;
  data::Sha256 sha;
  static unsigned char chunk[64 * 1024];  // under g_mutex: Attach is the only caller
  bool ok = true;
  for (;;) {
    const ssize_t got = read(fd, chunk, sizeof chunk);
    if (got == 0) break;
    if (got < 0) {
      if (errno == EINTR) continue;
      ok = false;
      break;
    }
    sha.Update(chunk, static_cast<std::size_t>(got));
  }
  close(fd);
  if (!ok) return false;
  const std::string hex = sha.Hex();
  if (hex.size() != 64) return false;
  std::memcpy(out, hex.c_str(), 65);
  return true;
}

// The libr15 hooks that do not install on a service-only file (#1737); each logs its own refusal
// (`build_id_mismatch` for a GOT hook, `not_verified` for a row or the credits slot).
constexpr const char* kServiceOnlySkipped =
    "inline_rows,credits_text,clock,frame_dispatch,login_prompt,social,obb_skip,process_end,crash_first,permission_wait,"
    "vk_ext_window,os_build_gate,texture_quality,render_config,menu_cards,wizard_metric,splash_skip,ui_texture_mips,"
    "pns_provider";
constexpr const char* kServiceOnlyKept = "tstring_redirect,create_connection_redirect,dlopen(login,matchmaking)";

std::size_t PageSize() {
  const long size = sysconf(_SC_PAGESIZE);
  return size > 0 ? static_cast<std::size_t>(size) : 4096;
}

std::uint32_t LoadWord(std::uintptr_t addr) {
  std::uint32_t w = 0;
  std::memcpy(&w, reinterpret_cast<const void*>(addr), sizeof w);
  return w;
}

// [vaddr, vaddr + length) lies wholly in the file-backed part of one PT_LOAD of the live program headers.
bool InFileBackedLoad(const ElfImage& image, std::uint64_t vaddr, std::uint64_t length) {
  for (std::size_t i = 0; i < image.phnum; ++i) {
    const Elf64_Phdr& p = image.phdr[i];
    if (p.p_type != PT_LOAD) continue;
    if (vaddr >= p.p_vaddr && length <= p.p_filesz && vaddr - p.p_vaddr <= p.p_filesz - length) return true;
  }
  return false;
}

// Check (c): the 8-byte slot lies in a writable PT_LOAD, in its bss part, and not in PT_GNU_RELRO (a store there would
// fault once the linker has protected it).
bool SlotInWritableBss(const ElfImage& image, std::uint64_t slotVaddr) {
  constexpr std::uint64_t kSlot = patch::kSlotBytes;
  bool inBss = false;
  for (std::size_t i = 0; i < image.phnum; ++i) {
    const Elf64_Phdr& p = image.phdr[i];
    if (p.p_type == PT_GNU_RELRO) {
      if (slotVaddr < p.p_vaddr + p.p_memsz && p.p_vaddr < slotVaddr + kSlot) return false;
      continue;
    }
    if (p.p_type != PT_LOAD || (p.p_flags & PF_W) == 0) continue;
    if (slotVaddr < p.p_vaddr) continue;
    const std::uint64_t into = slotVaddr - p.p_vaddr;
    if (into >= p.p_filesz && kSlot <= p.p_memsz && into <= p.p_memsz - kSlot) inBss = true;
  }
  return inBss;
}

// What a failed row check says: the token, which check, and the words or addresses that disagree.
struct RowFailure {
  const char* reason = nullptr;
  const char* check = nullptr;
  const char* id = nullptr;
  std::string ownedId;  // an extra row's id, which the options own
  int site = -1;
  int word = -1;
  std::uint32_t live = 0;
  std::uint32_t expected = 0;
  std::uint64_t liveSlot = 0;
  std::uint64_t expectedSlot = 0;
  bool hasWords = false;
  bool hasSlots = false;
};

bool Fail(RowFailure* f, const patch::Row& row, const char* reason, const char* check) {
  f->reason = reason;
  f->check = check;
  f->id = row.id;
  return false;
}

bool FailWords(RowFailure* f, const patch::Row& row, const char* reason, const char* check, int site, int word,
               std::uint32_t live, std::uint32_t expected) {
  Fail(f, row, reason, check);
  f->site = site;
  f->word = word;
  f->live = live;
  f->expected = expected;
  f->hasWords = true;
  return false;
}

bool FailSlot(RowFailure* f, const patch::Row& row, int site, std::uint64_t live, std::uint64_t expected) {
  Fail(f, row, kReasonRowSlot, "slot");
  f->site = site;
  f->liveSlot = live;
  f->expectedSlot = expected;
  f->hasSlots = true;
  return false;
}

// Checks (a), (b), (c) of one inline row: the stub the table implies is what is live, it decodes to the row's slot at
// its run-time address, and the slot is in the writable bss.
bool CheckInlineRow(const ElfImage& image, const patch::Row& row, RowFailure* f) {
  const patch::Site& site = row.sites[0];
  const std::uint64_t siteVa = site.Vaddr();
  const std::uint64_t slotVa = row.SlotVaddr();
  patch::Stub expected{};
  if (!patch::EncodeSlotStub(siteVa, slotVa, &expected) || !InFileBackedLoad(image, siteVa, 4 * kInlineWindowWords)) {
    return Fail(f, row, kReasonRowStub, "stub");
  }
  std::uint32_t live[patch::kStubWords] = {};
  for (std::size_t w = 0; w < patch::kStubWords; ++w) {
    live[w] = LoadWord(image.base + siteVa + 4 * w);
    if (live[w] != expected.words[w]) {
      return FailWords(f, row, kReasonRowStub, "stub", 0, static_cast<int>(w), live[w], expected.words[w]);
    }
  }
  std::uint64_t decoded = 0;
  if (!patch::DecodeSlotStub(live, image.base + siteVa, &decoded) || decoded != image.base + slotVa) {
    return FailSlot(f, row, 0, decoded, image.base + slotVa);
  }
  if (!SlotInWritableBss(image, slotVa)) return Fail(f, row, kReasonRowSegment, "segment");
  return true;
}

// The same for the credits row: each of its two ADRP+ADD pairs became ADRP+LDR through the row's slot.
bool CheckCreditsRow(const ElfImage& image, const patch::Row& row, RowFailure* f) {
  const std::uint64_t slotVa = row.SlotVaddr();
  for (std::size_t pair = 0; pair < row.siteCount / 2; ++pair) {
    const int adrpSite = static_cast<int>(2 * pair);
    const patch::Site& adrp = row.sites[adrpSite];
    const patch::Site& ldr = row.sites[adrpSite + 1];
    patch::SlotLoadPair expected{};
    if (!patch::EncodeSlotLoadPair(adrp.Vaddr(), slotVa, adrp.words[0] & 0x1Fu, &expected) ||
        !InFileBackedLoad(image, adrp.Vaddr(), 4) || !InFileBackedLoad(image, ldr.Vaddr(), 4)) {
      return Fail(f, row, kReasonRowStub, "stub");
    }
    const std::uint32_t liveAdrp = LoadWord(image.base + adrp.Vaddr());
    const std::uint32_t liveLdr = LoadWord(image.base + ldr.Vaddr());
    if (liveAdrp != expected.adrp) return FailWords(f, row, kReasonRowStub, "stub", adrpSite, 0, liveAdrp, expected.adrp);
    if (liveLdr != expected.ldr) return FailWords(f, row, kReasonRowStub, "stub", adrpSite + 1, 0, liveLdr, expected.ldr);
    unsigned rd = 0;
    std::uint64_t decoded = 0;
    if (!patch::DecodeSlotLoadPair(liveAdrp, liveLdr, image.base + adrp.Vaddr(), &rd, &decoded) ||
        decoded != image.base + slotVa) {
      return FailSlot(f, row, adrpSite, decoded, image.base + slotVa);
    }
  }
  if (!SlotInWritableBss(image, slotVa)) return Fail(f, row, kReasonRowSegment, "segment");
  return true;
}

// A word-patch row (extra rows, ADR section 11): the live words are the replacements.
bool CheckWordRow(const ElfImage& image, const patch::WordPatchRow& row, RowFailure* f) {
  f->reason = kReasonRowWords;
  f->check = "words";
  f->ownedId = row.id;
  for (std::size_t i = 0; i < row.edits.size(); ++i) {
    const patch::WordEdit& edit = row.edits[i];
    f->site = static_cast<int>(edit.offset);
    f->word = static_cast<int>(i);
    f->expected = patch::WordValue(edit.word);
    f->hasWords = true;
    if (row.revaultVa < patch::kRevaultBias) return false;
    const std::uint64_t va = row.revaultVa - patch::kRevaultBias + edit.offset;
    if (!InFileBackedLoad(image, va, 4)) return false;
    f->live = LoadWord(image.base + va);
    if (f->live != f->expected) return false;
  }
  return true;
}

void LogRowFailure(const RowFailure& f) {
  char liveHex[19];
  char expectedHex[19];
  char liveSlotHex[19];
  char expectedSlotHex[19];
  LogField fields[10] = {{"", ""}, {"", ""}, {"", ""}, {"", ""}, {"", ""}, {"", ""}, {"", ""}, {"", ""}, {"", ""}, {"", ""}};
  std::size_t n = 0;
  fields[n++] = LogField("status", "refused");
  fields[n++] = LogField("reason", f.reason);
  fields[n++] = LogField("id", f.id != nullptr ? f.id : f.ownedId.c_str());
  fields[n++] = LogField("check", f.check);
  if (f.hasWords) {
    fields[n++] = LogField("site", f.site);
    fields[n++] = LogField("live", sentinel::HexString(liveHex, f.live));
    fields[n++] = LogField("expected", sentinel::HexString(expectedHex, f.expected));
  }
  if (f.hasSlots) {
    fields[n++] = LogField("live_slot", sentinel::HexString(liveSlotHex, f.liveSlot));
    fields[n++] = LogField("expected_slot", sentinel::HexString(expectedSlotHex, f.expectedSlot));
  }
  sentinel::LogFieldList(LogLevel::kError, "libr15_patch", fields, n);
}

void LogRefusal(const char* reason, const char* buildId) {
  sentinel::LogFields(LogLevel::kError, "libr15_patch",
                      {{"status", "refused"}, {"reason", reason}, {"build_id", buildId != nullptr ? buildId : "(none)"},
                       {"stock_build_id", patch::kLibR15StockBuildId}});
}

AttachResult Refused(AttachStatus status, const char* reason) {
  AttachResult r;
  r.status = status;
  r.reason = reason;
  return r;
}

void ReleaseTrampolines(std::size_t pageSize) {
  for (RowRecord& r : g_rows) {
    if (r.trampoline != nullptr) munmap(r.trampoline, pageSize);
    r = RowRecord{};
  }
}

// One trampoline in its own anonymous page. On failure logs the errno and the site page's permissions, and
// returns the failing operation's name (otherwise nullptr).
const char* MakeTrampoline(const ElfImage& image, const patch::Row& row, const AttachOptions& options, std::size_t pageSize,
                           void** out, int* errnoOut) {
  const patch::Site& site = row.sites[0];
  const std::uint32_t window[kInlineWindowWords] = {site.words[0], site.words[1], site.words[2]};
  sentinel::Trampoline relocated;
  sentinel::RelocRefusal refusal{};
  if (!sentinel::BuildTrampoline(image.base + site.Vaddr(), window, relocated, refusal)) {
    *errnoOut = 0;
    return "trampoline_relocate";
  }
  const MapFn map = options.map != nullptr ? options.map : &mmap;
  const ProtectFn protect = options.protect != nullptr ? options.protect : &mprotect;
  void* const page = map(nullptr, pageSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (page == MAP_FAILED) {
    *errnoOut = errno;
    return "trampoline_map";
  }
  const std::size_t bytes = relocated.count * sizeof(std::uint32_t);
  std::memcpy(page, relocated.words, bytes);
  if (protect(page, pageSize, PROT_READ | PROT_EXEC) != 0) {
    *errnoOut = errno;
    munmap(page, pageSize);
    return "trampoline_rx";
  }
  __builtin___clear_cache(static_cast<char*>(page), static_cast<char*>(page) + bytes);
  *out = page;
  return nullptr;
}

void LogTrampolineFailure(const ElfImage& image, const patch::Row& row, const char* op, int err) {
  char tb[19];
  const sentinel::PermsText perms = sentinel::PermsTextOf(image.base + row.sites[0].Vaddr());
  sentinel::LogFields(LogLevel::kError, "libr15_patch",
                      {{"status", "refused"}, {"reason", kReasonTrampolineFailed}, {"id", row.id}, {"op", op},
                       {"target", sentinel::HexString(tb, image.base + row.sites[0].Vaddr())}, {"errno", err},
                       {"page_perms", perms.text}});
}

}  // namespace

const char* AttachStatusName(AttachStatus status) {
  switch (status) {
    case AttachStatus::kVerified: return "verified";
    case AttachStatus::kAlreadyVerified: return "already_verified";
    case AttachStatus::kModuleNotLoaded: return "module_not_loaded";
    case AttachStatus::kNoBuildId: return "no_build_id";
    case AttachStatus::kNotPatched: return "not_patched";
    case AttachStatus::kBuildIdMismatch: return "build_id_mismatch";
    case AttachStatus::kRowFailed: return "row_failed";
    case AttachStatus::kTrampolineFailed: return "trampoline_failed";
    case AttachStatus::kPinnedServiceOnly: return "pinned_service_only";
  }
  return "unknown";
}

const char* ActivateStatusName(ActivateStatus status) {
  switch (status) {
    case ActivateStatus::kOk: return "ok";
    case ActivateStatus::kBadArgument: return "bad_argument";
    case ActivateStatus::kNotVerified: return "not_verified";
    case ActivateStatus::kUnknownRow: return "unknown_row";
    case ActivateStatus::kNotInlineRow: return "not_inline_row";
    case ActivateStatus::kAlreadyActivated: return "already_activated";
  }
  return "unknown";
}

bool Verified() noexcept { return __atomic_load_n(&g_verified, __ATOMIC_ACQUIRE); }

const char* VerifiedBuildId() noexcept { return Verified() ? g_verifiedBuildId : nullptr; }

AttachResult Attach(const AttachOptions& options) {
  LockGuard lock;
  if (g_verified) {
    LogRefusal("already_verified", g_verifiedBuildId);
    return Refused(AttachStatus::kAlreadyVerified, "already_verified");
  }

  const char* const module = options.module != nullptr ? options.module : sentinel::pinned::kLibR15;
  ElfImage image;
  if (options.lookup == nullptr || !options.lookup(module, &image)) {
    LogRefusal("module_not_loaded", nullptr);
    return Refused(AttachStatus::kModuleNotLoaded, "module_not_loaded");
  }
  char liveId[kBuildIdText + 24] = {};
  if (!sentinel::ReadBuildId(image, liveId, sizeof liveId)) {
    LogRefusal("no_build_id", nullptr);
    return Refused(AttachStatus::kNoBuildId, "no_build_id");
  }
  if (std::strcmp(liveId, patch::kLibR15StockBuildId) == 0) {
    char digest[65] = {};
    const FileDigestFn digestFn = options.digest != nullptr ? options.digest : &DefaultDigest;
    if (image.name[0] != '\0' && digestFn(image.name, digest)) {
      for (const char* pinned : kLibR15ServiceOnlySha256) {
        if (std::strcmp(digest, pinned) != 0) continue;
        char prefix[13] = {};
        std::memcpy(prefix, digest, 12);
        sentinel::SetServiceSideBuildId(patch::kLibR15StockBuildId);
        sentinel::LogFields(LogLevel::kInfo, "libr15_pinned_accept",
                            {{"status", "accepted"}, {"sha256_prefix", prefix}, {"build_id", liveId},
                             {"mode", "service_only"}, {"kept", kServiceOnlyKept}, {"skipped", kServiceOnlySkipped},
                             {"consequence", "the file is not ours: none of our libr15 patches is applied; sign-in, "
                                             "matchmaking and the server redirect are"}});
        return Refused(AttachStatus::kPinnedServiceOnly, kReasonPinnedServiceOnly);
      }
    }
    LogRefusal(kReasonNotPatched, liveId);
    return Refused(AttachStatus::kNotPatched, kReasonNotPatched);
  }
  const std::string patchedId = patch::PatchedBuildId(options.extraRows);
  if (patchedId != liveId) {
    LogRefusal(kReasonBuildIdMismatch, liveId);
    return Refused(AttachStatus::kBuildIdMismatch, kReasonBuildIdMismatch);
  }

  const EndFn end = options.end != nullptr ? options.end : &DefaultEnd;

  // Every row is checked before anything is stored.
  for (std::size_t i = 0; i < patch::kRowCount; ++i) {
    const patch::Row& row = patch::kRows[i];
    RowFailure failure;
    const bool ok = row.kind == patch::RowKind::kInlineHook ? CheckInlineRow(image, row, &failure)
                                                            : CheckCreditsRow(image, row, &failure);
    if (ok) continue;
    LogRowFailure(failure);
    end(process_end::kFailureExitCode, failure.reason);
    AttachResult r = Refused(AttachStatus::kRowFailed, failure.reason);
    r.rowId = row.id;
    return r;
  }
  for (const patch::WordPatchRow& row : options.extraRows) {
    RowFailure failure;
    if (CheckWordRow(image, row, &failure)) continue;
    LogRowFailure(failure);
    end(process_end::kFailureExitCode, failure.reason);
    AttachResult r = Refused(AttachStatus::kRowFailed, failure.reason);
    r.rowId = row.id;
    return r;
  }

  // Every trampoline next, still without a store: a failure here leaves every slot as the file has it.
  const std::size_t pageSize = PageSize();
  RowRecord made[patch::kRowCount] = {};
  for (std::size_t i = 0; i < patch::kRowCount; ++i) {
    const patch::Row& row = patch::kRows[i];
    made[i].slot = reinterpret_cast<void**>(image.base + row.SlotVaddr());
    if (row.kind != patch::RowKind::kInlineHook) continue;
    int err = 0;
    const char* const failedOp = MakeTrampoline(image, row, options, pageSize, &made[i].trampoline, &err);
    if (failedOp == nullptr) continue;
    for (RowRecord& earlier : made) {
      if (earlier.trampoline != nullptr) munmap(earlier.trampoline, pageSize);
    }
    LogTrampolineFailure(image, row, failedOp, err);
    end(process_end::kFailureExitCode, kReasonTrampolineFailed);
    AttachResult r = Refused(AttachStatus::kTrampolineFailed, kReasonTrampolineFailed);
    r.rowId = row.id;
    return r;
  }

  // The stores: each slot gets its trampoline (the credits slot the game's own blob), so every patched function runs
  // as stock until a hook is activated.
  for (std::size_t i = 0; i < patch::kRowCount; ++i) {
    const patch::Row& row = patch::kRows[i];
    void* const value = row.kind == patch::RowKind::kInlineHook
                            ? made[i].trampoline
                            : reinterpret_cast<void*>(image.base + credits_text::kBlobVaddr);
    __atomic_store_n(made[i].slot, value, __ATOMIC_RELEASE);
    g_rows[i] = made[i];
  }
  g_base = image.base;
  g_pageSize = pageSize;
  std::memcpy(g_verifiedBuildId, patchedId.c_str(), kBuildIdText);
  __atomic_store_n(&g_verified, true, __ATOMIC_RELEASE);

  char slotArea[19];
  LogField fields[8] = {{"", ""}, {"", ""}, {"", ""}, {"", ""}, {"", ""}, {"", ""}, {"", ""}, {"", ""}};
  std::size_t n = 0;
  fields[n++] = LogField("status", "verified");
  fields[n++] = LogField("build_id", g_verifiedBuildId);
  fields[n++] = LogField("stock_build_id", patch::kLibR15StockBuildId);
  fields[n++] = LogField("rows", patch::kRowCount);
  fields[n++] = LogField("slot_area", sentinel::HexString(slotArea, patch::kSlotAreaVaddr));
  if (!options.extraRows.empty()) {
    fields[n++] = LogField("extra_rows", options.extraRows.size());
    fields[n++] = LogField("extra_rows_sha256", options.extraRowsSha256.c_str());
  }
  sentinel::LogFieldList(LogLevel::kInfo, "libr15_patch", fields, n);

  AttachResult r;
  r.status = AttachStatus::kVerified;
  return r;
}

std::uintptr_t RowSiteAddress(const char* rowId) noexcept {
  if (rowId == nullptr || !Verified()) return 0;
  for (std::size_t i = 0; i < patch::kRowCount; ++i) {
    if (std::strcmp(patch::kRows[i].id, rowId) == 0) return g_base + patch::kRows[i].sites[0].Vaddr();
  }
  return 0;
}

void** CreditsSlot() noexcept {
  if (!Verified()) return nullptr;
  for (std::size_t i = 0; i < patch::kRowCount; ++i) {
    if (patch::kRows[i].kind == patch::RowKind::kCreditsPair) return g_rows[i].slot;
  }
  return nullptr;
}

ActivateStatus RowActivation::ActivateRow(const char* rowId, void* detour, void** originalOut) {
  auto refuse = [rowId](ActivateStatus status) {
    sentinel::LogFields(LogLevel::kError, "inline_hook",
                        {{"status", ActivateStatusName(status)}, {"id", rowId != nullptr ? rowId : "(null)"}});
    return status;
  };
  if (rowId == nullptr || detour == nullptr || originalOut == nullptr) return refuse(ActivateStatus::kBadArgument);
  LockGuard lock;
  if (!g_verified) return refuse(ActivateStatus::kNotVerified);
  std::size_t index = patch::kRowCount;
  for (std::size_t i = 0; i < patch::kRowCount; ++i) {
    if (std::strcmp(patch::kRows[i].id, rowId) == 0) index = i;
  }
  if (index == patch::kRowCount) return refuse(ActivateStatus::kUnknownRow);
  const patch::Row& row = patch::kRows[index];
  if (row.kind != patch::RowKind::kInlineHook) return refuse(ActivateStatus::kNotInlineRow);
  RowRecord& record = g_rows[index];
  if (record.activated) return refuse(ActivateStatus::kAlreadyActivated);

  // The original is published before the detour can be reached.
  *originalOut = record.trampoline;
  __atomic_store_n(record.slot, detour, __ATOMIC_RELEASE);
  record.activated = true;

  char tb[19];
  char sb[19];
  char rb[19];
  const std::uintptr_t target = g_base + row.sites[0].Vaddr();
  const sentinel::PermsText perms = sentinel::PermsTextOf(target);
  sentinel::LogFields(LogLevel::kInfo, "inline_hook",
                      {{"status", "ok"}, {"id", row.id}, {"target", sentinel::HexString(tb, target)},
                       {"slot", sentinel::HexString(sb, reinterpret_cast<std::uintptr_t>(record.slot))},
                       {"trampoline", sentinel::HexString(rb, reinterpret_cast<std::uintptr_t>(record.trampoline))},
                       {"page_perms", perms.text}});
  return ActivateStatus::kOk;
}

void ResetForTest() noexcept {
  LockGuard lock;
  if (g_pageSize != 0) ReleaseTrampolines(g_pageSize);
  g_verified = false;
  std::memset(g_verifiedBuildId, 0, sizeof g_verifiedBuildId);
  g_base = 0;
  g_pageSize = 0;
}

}  // namespace nevr_quest::libr15_patch
