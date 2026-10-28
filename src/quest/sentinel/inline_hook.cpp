#include "inline_hook.h"

#include "arm64_reloc.h"
#include "hook_log.h"

#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>

namespace sentinel {
namespace {

// One lock for every inline write and for the registry. Install holds it from the first read of the
// page to the last verified store, so two installs cannot interleave on one page.
pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;
std::uintptr_t g_installed[kMaxInlineHooks] = {};
std::size_t g_installedCount = 0;

// Every protection change goes through g_protect so a host test can make one fail (InlineHookTestAccess); in the
// sentinel it is mprotect.
int RealProtect(void* addr, std::size_t len, int prot) { return mprotect(addr, len, prot); }
ProtectFn g_protect = &RealProtect;

class LockGuard {
 public:
  LockGuard() { pthread_mutex_lock(&g_mutex); }
  ~LockGuard() { pthread_mutex_unlock(&g_mutex); }
  LockGuard(const LockGuard&) = delete;
  LockGuard& operator=(const LockGuard&) = delete;
};

std::uint32_t LoadWord(const void* p) {
  std::uint32_t w = 0;
  std::memcpy(&w, p, sizeof w);
  return w;
}

// The stores into the target and the trampoline are whole aligned words, so a concurrent reader sees
// either the old word or the new one, never a mix within a word.
void StoreWord(void* p, std::uint32_t w) {
  __atomic_store_n(static_cast<std::uint32_t*>(p), w, __ATOMIC_RELEASE);
}

std::size_t PageSize() {
  const long size = sysconf(_SC_PAGESIZE);
  return size > 0 ? static_cast<std::size_t>(size) : 4096;
}

// Permissions of the mapping that holds `addr` (for example "r-xp"), from /proc/self/maps. False when
// the file cannot be read or no mapping holds the address: an unreadable map never reads as writable
// or executable by default.
bool MappingPerms(std::uintptr_t addr, char (&perms)[5]) {
  std::FILE* f = std::fopen("/proc/self/maps", "re");
  if (f == nullptr) return false;
  char line[512];
  bool found = false;
  while (!found && std::fgets(line, sizeof line, f) != nullptr) {
    unsigned long lo = 0;
    unsigned long hi = 0;
    char p[5] = {};
    if (std::sscanf(line, "%lx-%lx %4s", &lo, &hi, p) != 3) continue;
    if (addr >= lo && addr < hi) {
      std::memcpy(perms, p, sizeof perms);
      found = true;
    }
  }
  std::fclose(f);
  return found;
}

bool IsReadExecute(const char (&perms)[5]) {
  return perms[0] == 'r' && perms[1] == '-' && perms[2] == 'x';
}

// The permissions of the page that holds `target` as /proc/self/maps shows them now, "unknown" when they cannot be read.
struct PagePerms {
  char text[8];
};
PagePerms PagePermsNow(const void* target) {
  PagePerms p = {};
  char perms[5] = {};
  if (MappingPerms(reinterpret_cast<std::uintptr_t>(target), perms)) {
    std::memcpy(p.text, perms, sizeof perms);
  } else {
    std::memcpy(p.text, "unknown", sizeof "unknown");
  }
  return p;
}

// One line per outcome. A failed protection change carries `op` (which change) and `err` (its errno, read by the
// caller right after the call) and `pagePerms`, the target page's permissions read right after that call (a caller
// that goes on to change the page again reads them first), so the line says why the page is in the state it is in.
void LogResult(LogLevel level, InlineStatus status, const void* target, const void* trampoline,
               const char* op = nullptr, int err = 0, const char* note = nullptr, int retryErr = 0,
               const PagePerms* pagePerms = nullptr) {
  char tb[19];
  char rb[19];
  LogField fields[8] = {{"", ""}, {"", ""}, {"", ""}, {"", ""}, {"", ""}, {"", ""}, {"", ""}, {"", ""}};
  std::size_t n = 0;
  fields[n++] = LogField("status", InlineStatusName(status));
  fields[n++] = LogField("target", HexString(tb, reinterpret_cast<std::uintptr_t>(target)));
  if (trampoline != nullptr) {
    fields[n++] = LogField("trampoline", HexString(rb, reinterpret_cast<std::uintptr_t>(trampoline)));
  }
  if (op != nullptr) fields[n++] = LogField("op", op);
  if (err != 0) {
    fields[n++] = LogField("errno", err);
    const PagePerms now = pagePerms != nullptr ? *pagePerms : PagePermsNow(target);
    fields[n++] = LogField("page_perms", now.text);
  }
  if (retryErr != 0) fields[n++] = LogField("retry_errno", retryErr);
  if (note != nullptr) fields[n++] = LogField("note", note);
  LogFieldList(level, "inline_hook", fields, n);
}

// The four words of the detour: LDR x16,#8 ; BR x16 ; .quad detour (little-endian 64-bit address).
void BuildDetour(std::uintptr_t detour, std::uint32_t (&out)[4]) {
  out[0] = kLdrX16Literal8;
  out[1] = kBrX16;
  const std::uint64_t addr = detour;
  std::memcpy(&out[2], &addr, sizeof addr);
}

}  // namespace

const char* InlineStatusName(InlineStatus status) {
  switch (status) {
    case InlineStatus::kOk: return "ok";
    case InlineStatus::kBadArgument: return "bad_argument";
    case InlineStatus::kNotWordAligned: return "not_word_aligned";
    case InlineStatus::kWindowStraddlesPage: return "window_straddles_page";
    case InlineStatus::kPrologueMismatch: return "prologue_mismatch";
    case InlineStatus::kPcRelativeInstruction: return "pc_relative_instruction";
    case InlineStatus::kPageNotExecutable: return "page_not_executable";
    case InlineStatus::kAlreadyInstalled: return "already_installed";
    case InlineStatus::kRegistryFull: return "registry_full";
    case InlineStatus::kTrampolineFailed: return "trampoline_failed";
    case InlineStatus::kProtectFailed: return "protect_failed";
    case InlineStatus::kWriteVerifyFailed: return "write_verify_failed";
    case InlineStatus::kRestoreProtectUndone: return "restore_protect_undone";
    case InlineStatus::kRestoreProtectFailed: return "restore_protect_failed";
  }
  return "unknown";
}

bool IsPcRelative(std::uint32_t insn) noexcept { return ClassifyPcRel(insn) != PcRelKind::kNone; }

namespace {

static_assert(kInlinePatchBytes == kRelocWindowBytes, "the relocator's window is the patch's");

// The address and prologue checks, then the trampoline's relocation. On kPcRelativeInstruction `refusal` names
// the word and the instruction; on kOk `tramp` holds the trampoline's words.
InlineStatus CheckAndRelocate(const void* target, const InlinePrologue& expected, Trampoline& tramp,
                              RelocRefusal& refusal) noexcept {
  if (target == nullptr) return InlineStatus::kBadArgument;
  const auto addr = reinterpret_cast<std::uintptr_t>(target);
  if ((addr & 3u) != 0) return InlineStatus::kNotWordAligned;
  if ((addr & (PageSize() - 1)) > PageSize() - kInlinePatchBytes) return InlineStatus::kWindowStraddlesPage;
  for (std::size_t i = 0; i < 4; ++i) {
    const std::uint32_t live = LoadWord(static_cast<const std::uint8_t*>(target) + 4 * i);
    if (live != expected.words[i]) return InlineStatus::kPrologueMismatch;
  }
  if (!BuildTrampoline(addr, expected.words, tramp, refusal)) return InlineStatus::kPcRelativeInstruction;
  return InlineStatus::kOk;
}

void LogRefusal(const void* target, const RelocRefusal& refusal) {
  char tb[19];
  char eb[19];
  LogFields(LogLevel::kError, "inline_hook",
            {{"status", InlineStatusName(InlineStatus::kPcRelativeInstruction)},
             {"target", HexString(tb, reinterpret_cast<std::uintptr_t>(target))},
             {"word", refusal.index},
             {"encoding", HexString(eb, refusal.insn)},
             {"instruction", PcRelName(refusal.kind)},
             {"reason", refusal.reason}});
}

}  // namespace

ProtectFn InlineHook::SetProtectForTest(ProtectFn fn) {
  LockGuard lock;
  const ProtectFn previous = g_protect;
  g_protect = fn != nullptr ? fn : &RealProtect;
  return previous;
}

InlineStatus CheckInlineTarget(const void* target, const InlinePrologue& expected) noexcept {
  Trampoline tramp;
  RelocRefusal refusal;
  return CheckAndRelocate(target, expected, tramp, refusal);
}

InlineStatus InlineHook::Install(const void* target, const InlinePrologue& expected, void* detour,
                                 void** originalOut) {
  if (target == nullptr || detour == nullptr || originalOut == nullptr) {
    LogResult(LogLevel::kError, InlineStatus::kBadArgument, target, nullptr);
    return InlineStatus::kBadArgument;
  }
  LockGuard lock;
  const auto addr = reinterpret_cast<std::uintptr_t>(target);
  // Registry first: a target this process already hooked reads as the detour now, so a prologue check
  // would report a mismatch for it instead of the true reason.
  for (std::size_t i = 0; i < g_installedCount; ++i) {
    if (g_installed[i] == addr) {
      LogResult(LogLevel::kError, InlineStatus::kAlreadyInstalled, target, nullptr);
      return InlineStatus::kAlreadyInstalled;
    }
  }
  if (g_installedCount >= kMaxInlineHooks) {
    LogResult(LogLevel::kError, InlineStatus::kRegistryFull, target, nullptr);
    return InlineStatus::kRegistryFull;
  }
  Trampoline relocated;
  RelocRefusal refusal;
  const InlineStatus pre = CheckAndRelocate(target, expected, relocated, refusal);
  if (pre == InlineStatus::kPcRelativeInstruction) {
    LogRefusal(target, refusal);
    return pre;
  }
  if (pre != InlineStatus::kOk) {
    LogResult(LogLevel::kError, pre, target, nullptr);
    return pre;
  }
  char perms[5] = {};
  if (!MappingPerms(addr, perms) || !IsReadExecute(perms)) {
    LogResult(LogLevel::kError, InlineStatus::kPageNotExecutable, target, nullptr);
    return InlineStatus::kPageNotExecutable;
  }

  // The trampoline: the displaced words, PC-relative ones rewritten (arm64_reloc.h), then LDR x16,#8 ;
  // BR x16 ; .quad target+16 and the literal pool. It is complete and executable before the target can
  // reach it.
  const std::size_t pageSize = PageSize();
  void* const tramp = mmap(nullptr, pageSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (tramp == MAP_FAILED) {
    const int err = errno;
    LogResult(LogLevel::kError, InlineStatus::kTrampolineFailed, target, nullptr, "trampoline_map", err);
    return InlineStatus::kTrampolineFailed;
  }
  const std::size_t trampolineBytes = relocated.count * sizeof(std::uint32_t);
  std::memcpy(tramp, relocated.words, trampolineBytes);
  if (g_protect(tramp, pageSize, PROT_READ | PROT_EXEC) != 0) {
    const int err = errno;
    munmap(tramp, pageSize);
    LogResult(LogLevel::kError, InlineStatus::kTrampolineFailed, target, nullptr, "trampoline_rx", err);
    return InlineStatus::kTrampolineFailed;
  }
  __builtin___clear_cache(static_cast<char*>(tramp), static_cast<char*>(tramp) + trampolineBytes);

  // Publish the original before the commit; restored to its entry value on any failure below.
  void* const previousOriginal = *originalOut;
  *originalOut = tramp;

  auto rollBack = [&](std::uint32_t* patch) {
    // Word 0 is never stored before the commit, so the target still starts with its original
    // instructions; words 1-3 go back to the displaced originals. Caller holds the page writable.
    StoreWord(patch + 3, expected.words[3]);
    StoreWord(patch + 2, expected.words[2]);
    StoreWord(patch + 1, expected.words[1]);
  };

  const std::uintptr_t pageStart = addr & ~(static_cast<std::uintptr_t>(pageSize) - 1);
  if (g_protect(reinterpret_cast<void*>(pageStart), pageSize, PROT_READ | PROT_WRITE) != 0) {
    const int err = errno;
    *originalOut = previousOriginal;
    munmap(tramp, pageSize);
    LogResult(LogLevel::kError, InlineStatus::kProtectFailed, target, nullptr, "target_rw", err);
    return InlineStatus::kProtectFailed;
  }

  std::uint32_t detourWords[4] = {};
  BuildDetour(reinterpret_cast<std::uintptr_t>(detour), detourWords);
  auto* patch = reinterpret_cast<std::uint32_t*>(addr);
  // Words 2-3 and word 1 first: the target still starts with its original word 0, so none of these
  // is reachable yet.
  StoreWord(patch + 3, detourWords[3]);
  StoreWord(patch + 2, detourWords[2]);
  StoreWord(patch + 1, detourWords[1]);
  if (LoadWord(patch + 1) != detourWords[1] || LoadWord(patch + 2) != detourWords[2] ||
      LoadWord(patch + 3) != detourWords[3]) {
    rollBack(patch);
    *originalOut = previousOriginal;
    munmap(tramp, pageSize);
    if (g_protect(reinterpret_cast<void*>(pageStart), pageSize, PROT_READ | PROT_EXEC) != 0) {
      const int err = errno;
      LogResult(LogLevel::kError, InlineStatus::kRestoreProtectFailed, target, nullptr, "target_rx", err,
                "fatal: page left writable and not executable, original bytes in place");
      return InlineStatus::kRestoreProtectFailed;
    }
    LogResult(LogLevel::kError, InlineStatus::kWriteVerifyFailed, target, nullptr);
    return InlineStatus::kWriteVerifyFailed;
  }
  // The commit: word 0 makes the detour reachable. From here the hook may be live, so the target is
  // registered whatever the read-back says, and the trampoline and the published original are kept.
  StoreWord(patch, detourWords[0]);
  const bool committedVerified = LoadWord(patch) == detourWords[0];
  g_installed[g_installedCount++] = addr;

  if (g_protect(reinterpret_cast<void*>(pageStart), pageSize, PROT_READ | PROT_EXEC) != 0) {
    // The page is writable and not executable: the first call into it would fault. Put the original
    // words back while it is still writable (word 0 first, the commit in reverse), forget the hook, and
    // try to make the page executable again with the original bytes in it.
    const int firstErr = errno;
    const PagePerms atFailure = PagePermsNow(target);
    StoreWord(patch, expected.words[0]);
    rollBack(patch);
    --g_installedCount;
    *originalOut = previousOriginal;
    munmap(tramp, pageSize);
    if (g_protect(reinterpret_cast<void*>(pageStart), pageSize, PROT_READ | PROT_EXEC) == 0) {
      __builtin___clear_cache(reinterpret_cast<char*>(patch), reinterpret_cast<char*>(patch) + kInlinePatchBytes);
      LogResult(LogLevel::kError, InlineStatus::kRestoreProtectUndone, target, nullptr, "target_rx", firstErr,
                "hook undone and skipped, original bytes restored, page executable again", 0, &atFailure);
      return InlineStatus::kRestoreProtectUndone;
    }
    const int retryErr = errno;
    LogResult(LogLevel::kError, InlineStatus::kRestoreProtectFailed, target, nullptr, "target_rx", firstErr,
              "fatal: page left writable and not executable, original bytes in place; the first call into it faults",
              retryErr, &atFailure);
    return InlineStatus::kRestoreProtectFailed;
  }
  // The flush takes a writable range; the patch above was written through the page's own mapping.
  char* const patchBegin = const_cast<char*>(static_cast<const char*>(target));
  __builtin___clear_cache(patchBegin, patchBegin + kInlinePatchBytes);

  if (!committedVerified) {
    LogResult(LogLevel::kError, InlineStatus::kWriteVerifyFailed, target, tramp);
    return InlineStatus::kWriteVerifyFailed;
  }

  LogResult(LogLevel::kInfo, InlineStatus::kOk, target, tramp);
  return InlineStatus::kOk;
}

}  // namespace sentinel
