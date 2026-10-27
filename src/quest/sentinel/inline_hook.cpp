#include "inline_hook.h"

#include "hook_log.h"

#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>

namespace sentinel {
namespace {

// One lock for every inline write and for the registry. Install holds it from the first read of the
// page to the last verified store, so two installs cannot interleave on one page.
pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;
std::uintptr_t g_installed[kMaxInlineHooks] = {};
std::size_t g_installedCount = 0;

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

void LogResult(LogLevel level, InlineStatus status, const void* target, const void* trampoline) {
  char tb[19];
  char rb[19];
  if (trampoline != nullptr) {
    LogFields(level, "inline_hook",
              {{"status", InlineStatusName(status)},
               {"target", HexString(tb, reinterpret_cast<std::uintptr_t>(target))},
               {"trampoline", HexString(rb, reinterpret_cast<std::uintptr_t>(trampoline))}});
  } else {
    LogFields(level, "inline_hook",
              {{"status", InlineStatusName(status)},
               {"target", HexString(tb, reinterpret_cast<std::uintptr_t>(target))}});
  }
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
    case InlineStatus::kRestoreProtectFailed: return "restore_protect_failed";
  }
  return "unknown";
}

bool IsPcRelative(std::uint32_t insn) noexcept {
  // B and BL: opcode bits 31..26 are 000101 or 100101.
  if ((insn & 0xFC000000u) == 0x14000000u || (insn & 0xFC000000u) == 0x94000000u) return true;
  // B.cond (bit 4 clear).
  if ((insn & 0xFF000010u) == 0x54000000u) return true;
  // CBZ, CBNZ.
  if ((insn & 0x7E000000u) == 0x34000000u) return true;
  // TBZ, TBNZ.
  if ((insn & 0x7E000000u) == 0x36000000u) return true;
  // ADR (bit 31 clear) and ADRP (bit 31 set).
  if ((insn & 0x9F000000u) == 0x10000000u || (insn & 0x9F000000u) == 0x90000000u) return true;
  // LDR, LDRSW and PRFM (literal): bits 29..27 are 011, bits 25..24 are 00.
  if ((insn & 0x3B000000u) == 0x18000000u) return true;
  return false;
}

InlineStatus CheckInlineTarget(const void* target, const InlinePrologue& expected) noexcept {
  if (target == nullptr) return InlineStatus::kBadArgument;
  const auto addr = reinterpret_cast<std::uintptr_t>(target);
  if ((addr & 3u) != 0) return InlineStatus::kNotWordAligned;
  if ((addr & (PageSize() - 1)) > PageSize() - kInlinePatchBytes) return InlineStatus::kWindowStraddlesPage;
  for (std::size_t i = 0; i < 4; ++i) {
    const std::uint32_t live = LoadWord(static_cast<const std::uint8_t*>(target) + 4 * i);
    if (live != expected.words[i]) return InlineStatus::kPrologueMismatch;
  }
  for (std::size_t i = 0; i < 4; ++i) {
    if (IsPcRelative(expected.words[i])) return InlineStatus::kPcRelativeInstruction;
  }
  return InlineStatus::kOk;
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
  const InlineStatus pre = CheckInlineTarget(target, expected);
  if (pre != InlineStatus::kOk) {
    LogResult(LogLevel::kError, pre, target, nullptr);
    return pre;
  }
  char perms[5] = {};
  if (!MappingPerms(addr, perms) || !IsReadExecute(perms)) {
    LogResult(LogLevel::kError, InlineStatus::kPageNotExecutable, target, nullptr);
    return InlineStatus::kPageNotExecutable;
  }

  // The trampoline: the displaced words, then LDR x16,#8 ; BR x16 ; .quad target+16. It is complete
  // and executable before the target can reach it.
  const std::size_t pageSize = PageSize();
  void* const tramp = mmap(nullptr, pageSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (tramp == MAP_FAILED) {
    LogResult(LogLevel::kError, InlineStatus::kTrampolineFailed, target, nullptr);
    return InlineStatus::kTrampolineFailed;
  }
  auto* t = static_cast<std::uint32_t*>(tramp);
  for (std::size_t i = 0; i < 4; ++i) t[i] = expected.words[i];
  t[4] = kLdrX16Literal8;
  t[5] = kBrX16;
  const std::uint64_t resume = addr + kInlinePatchBytes;
  std::memcpy(&t[6], &resume, sizeof resume);
  if (mprotect(tramp, pageSize, PROT_READ | PROT_EXEC) != 0) {
    munmap(tramp, pageSize);
    LogResult(LogLevel::kError, InlineStatus::kTrampolineFailed, target, nullptr);
    return InlineStatus::kTrampolineFailed;
  }
  __builtin___clear_cache(static_cast<char*>(tramp), static_cast<char*>(tramp) + kInlinePatchBytes * 2);

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
  if (mprotect(reinterpret_cast<void*>(pageStart), pageSize, PROT_READ | PROT_WRITE) != 0) {
    *originalOut = previousOriginal;
    munmap(tramp, pageSize);
    LogResult(LogLevel::kError, InlineStatus::kProtectFailed, target, nullptr);
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
    mprotect(reinterpret_cast<void*>(pageStart), pageSize, PROT_READ | PROT_EXEC);
    *originalOut = previousOriginal;
    munmap(tramp, pageSize);
    LogResult(LogLevel::kError, InlineStatus::kWriteVerifyFailed, target, nullptr);
    return InlineStatus::kWriteVerifyFailed;
  }
  // The commit: word 0 makes the detour reachable. From here the hook may be live, so the target is
  // registered whatever the read-back says, and the trampoline and the published original are kept.
  StoreWord(patch, detourWords[0]);
  const bool committedVerified = LoadWord(patch) == detourWords[0];
  g_installed[g_installedCount++] = addr;

  if (mprotect(reinterpret_cast<void*>(pageStart), pageSize, PROT_READ | PROT_EXEC) != 0) {
    // The hook is committed and the page is left writable and not executable. Nothing here can
    // undo that; the caller must treat it as fatal.
    LogResult(LogLevel::kError, InlineStatus::kRestoreProtectFailed, target, tramp);
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
