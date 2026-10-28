// Host test for the arm64 inline (trampoline) hook backend (inline_hook.h).
//
// The host is x86_64, so the patched words are arm64 encodings that are never executed here. What
// this test checks is the bytes an install leaves behind and the refusals: the PC-relative table on
// fixed encodings, the trampoline's contents, the prologue, alignment and page checks, the registry,
// and the thunk entry point. A detour that runs on a device is the Quest backend's own check
// (docs/adr/0003); this file cannot stand in for it.
//
// Built with -fno-exceptions, like every translation unit that includes callback_thunk.h.
// Run: inline_hook_test [name-filter]

#include <sys/mman.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "arm64_reloc.h"
#include "callback_thunk.h"
#include "hook_install.h"
#include "hook_log.h"
#include "inline_hook.h"
#include "mini_test.h"

namespace sentinel {
// The one test seam into the private Install (inline_hook.h names it as a friend).
struct InlineHookTestAccess {
  static InlineStatus Install(const void* target, const InlinePrologue& expected, void* detour,
                              void** originalOut) {
    return InlineHook::Install(target, expected, detour, originalOut);
  }
  static ProtectFn SetProtect(ProtectFn fn) { return InlineHook::SetProtectForTest(fn); }
};
}  // namespace sentinel

namespace {

using sentinel::InlineHookTestAccess;
using sentinel::InlinePrologue;
using sentinel::InlineStatus;

std::string g_lines;
void CaptureSink(sentinel::LogLevel, const char* line) {
  g_lines += line;
  g_lines += '\n';
}

// A plausible arm64 function entry: STP x29,x30,[sp,#-16]! ; MOV x29,sp ; SUB sp,sp,#0x20 ; STR x19,[sp,#16].
constexpr std::uint32_t kStpFp = 0xA9BF7BFDu;
constexpr std::uint32_t kMovFp = 0x910003FDu;
constexpr std::uint32_t kSubSp = 0xD10083FFu;
constexpr std::uint32_t kStrX19 = 0xF90013F3u;
const InlinePrologue kPrologue{{kStpFp, kMovFp, kSubSp, kStrX19}};

std::size_t Page() { return static_cast<std::size_t>(sysconf(_SC_PAGESIZE)); }

// An anonymous page with `count` words at its start, r-x when `executable`, rw otherwise.
std::uint32_t* MakePage(const std::uint32_t* words, std::size_t count, bool executable) {
  void* p = mmap(nullptr, Page(), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (p == MAP_FAILED) return nullptr;
  std::memcpy(p, words, count * sizeof(std::uint32_t));
  if (executable && mprotect(p, Page(), PROT_READ | PROT_EXEC) != 0) return nullptr;
  return static_cast<std::uint32_t*>(p);
}

std::uint64_t Quad(const std::uint32_t* words) {
  std::uint64_t v = 0;
  std::memcpy(&v, words, sizeof v);
  return v;
}

TEST(PcRelativeEncodingsAreRecognised) {
  // Each of these depends on its own address; the encodings are the architecture's, with zero offsets.
  const std::uint32_t pcRelative[] = {
      0x14000000u,  // B #0
      0x94000000u,  // BL #0
      0x54000000u,  // B.EQ #0
      0xB4000000u,  // CBZ x0, #0
      0x34000000u,  // CBZ w0, #0
      0x36000000u,  // TBZ w0, #0, #0
      0x37000000u,  // TBNZ w0, #0, #0
      0x10000000u,  // ADR x0, #0
      0x90000000u,  // ADRP x0, #0
      0x54000010u,  // BC.cond #0
      0x58000000u,  // LDR x0, #0 (literal)
      0x98000000u,  // LDRSW x0, #0 (literal)
      0x1C000000u,  // LDR s0, #0 (literal, SIMD)
  };
  for (std::uint32_t w : pcRelative) CHECK(sentinel::IsPcRelative(w));
  const std::uint32_t position[] = {
      kStpFp, kMovFp, kSubSp, kStrX19,
      0x91000400u,  // ADD x0, x0, #1
      0xF9400020u,  // LDR x0, [x1]
      0x39400000u,  // LDRB w0, [x1]
      0xD65F03C0u,  // RET
      0xD61F0200u,  // BR x16
      0xD503201Fu,  // NOP
  };
  for (std::uint32_t w : position) CHECK(!sentinel::IsPcRelative(w));
}

TEST(TrampolineConstantsAreTheAarch64Encodings) {
  // LDR x16, #8 and BR x16, from the architecture manual's encodings: Rt = 16 in both.
  CHECK_EQ(sentinel::kLdrX16Literal8, 0x58000050u);
  CHECK_EQ(sentinel::kBrX16, 0xD61F0200u);
  CHECK_EQ(sentinel::kInlinePatchBytes, static_cast<std::size_t>(16));
}

TEST(InstallWritesDetourAndTrampoline) {
  std::uint32_t* page = MakePage(kPrologue.words, 4, true);
  CHECK(page != nullptr);
  if (page == nullptr) return;
  const std::uint64_t detour = 0x0000007812345678ULL;  // any address: it is stored, never called here
  void* original = nullptr;
  g_lines.clear();
  sentinel::SetLogSink(&CaptureSink);
  const InlineStatus st = InlineHookTestAccess::Install(page, kPrologue, reinterpret_cast<void*>(detour), &original);
  sentinel::SetLogSink(nullptr);
  CHECK_EQ(st, InlineStatus::kOk);

  // The patch: LDR x16,#8 ; BR x16 ; .quad detour.
  CHECK_EQ(page[0], sentinel::kLdrX16Literal8);
  CHECK_EQ(page[1], sentinel::kBrX16);
  CHECK_EQ(Quad(&page[2]), detour);

  // The trampoline: the displaced four words, then LDR/BR to target+16.
  CHECK(original != nullptr);
  if (original == nullptr) return;
  auto* t = static_cast<std::uint32_t*>(original);
  for (std::size_t i = 0; i < 4; ++i) CHECK_EQ(t[i], kPrologue.words[i]);
  CHECK_EQ(t[4], sentinel::kLdrX16Literal8);
  CHECK_EQ(t[5], sentinel::kBrX16);
  CHECK_EQ(Quad(&t[6]), static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(page)) + 16);

  CHECK(g_lines.find("\"event\":\"inline_hook\"") != std::string::npos);
  CHECK(g_lines.find("\"status\":\"ok\"") != std::string::npos);
}

TEST(PrologueMismatchChangesNothing) {
  std::uint32_t* page = MakePage(kPrologue.words, 4, true);
  CHECK(page != nullptr);
  if (page == nullptr) return;
  InlinePrologue wrong = kPrologue;
  wrong.words[2] = 0xD503201Fu;  // a NOP where the page holds SUB
  void* original = nullptr;
  const InlineStatus st = InlineHookTestAccess::Install(page, wrong, reinterpret_cast<void*>(0x1000), &original);
  CHECK_EQ(st, InlineStatus::kPrologueMismatch);
  CHECK_EQ(page[0], kStpFp);
  CHECK_EQ(page[1], kMovFp);
  CHECK_EQ(page[2], kSubSp);
  CHECK_EQ(page[3], kStrX19);
  CHECK(original == nullptr);
}

TEST(PcRelativePrologueIsRelocatedIntoTheTrampoline) {
  // The shape of the Quest version label's entry: a compare-and-branch first. CBZ x0, #0x2000 at word 0,
  // then ADRP x1 and a call; the trampoline sits in another mapping, so none of them can be copied as is.
  const std::uint32_t words[4] = {0xB4010000u /* CBZ x0, #0x2000 */, 0xB0000001u /* ADRP x1, #0x1000 */,
                                  0x94000010u /* BL #0x40 */, kStrX19};
  std::uint32_t* page = MakePage(words, 4, true);
  CHECK(page != nullptr);
  if (page == nullptr) return;
  const InlinePrologue prologue{{words[0], words[1], words[2], words[3]}};
  CHECK_EQ(sentinel::CheckInlineTarget(page, prologue), InlineStatus::kOk);
  void* original = nullptr;
  g_lines.clear();
  sentinel::SetLogSink(&CaptureSink);
  const InlineStatus st = InlineHookTestAccess::Install(page, prologue, reinterpret_cast<void*>(0x2000), &original);
  sentinel::SetLogSink(nullptr);
  CHECK_EQ(st, InlineStatus::kOk);
  CHECK(original != nullptr);
  if (original == nullptr) return;

  // The patch is the usual one; the trampoline is exactly what the relocator builds for this address.
  CHECK_EQ(page[0], sentinel::kLdrX16Literal8);
  sentinel::Trampoline expected;
  sentinel::RelocRefusal refusal;
  CHECK(sentinel::BuildTrampoline(reinterpret_cast<std::uintptr_t>(page), prologue.words, expected, refusal));
  const auto* t = static_cast<const std::uint32_t*>(original);
  for (std::size_t i = 0; i < expected.count; ++i) CHECK_EQ(t[i], expected.words[i]);
  // The first word is no longer the CBZ: it is its inverse, CBNZ x0, over the jump.
  CHECK_EQ(t[0], 0xB5000060u);
  CHECK(g_lines.find("\"status\":\"ok\"") != std::string::npos);
}

TEST(PcRelativeBranchIntoThePatchedBytesIsRefusedAndTheLogNamesIt) {
  // B #8 at word 1 lands on word 3: inside the 16 bytes the detour overwrites, so the original word is gone.
  const std::uint32_t words[4] = {kStpFp, 0x14000002u, kSubSp, kStrX19};
  std::uint32_t* page = MakePage(words, 4, true);
  CHECK(page != nullptr);
  if (page == nullptr) return;
  const InlinePrologue prologue{{words[0], words[1], words[2], words[3]}};
  CHECK_EQ(sentinel::CheckInlineTarget(page, prologue), InlineStatus::kPcRelativeInstruction);
  void* original = nullptr;
  g_lines.clear();
  sentinel::SetLogSink(&CaptureSink);
  const InlineStatus st = InlineHookTestAccess::Install(page, prologue, reinterpret_cast<void*>(0x1000), &original);
  sentinel::SetLogSink(nullptr);
  CHECK_EQ(st, InlineStatus::kPcRelativeInstruction);
  CHECK_EQ(page[1], 0x14000002u);
  CHECK(original == nullptr);
  CHECK(g_lines.find("\"status\":\"pc_relative_instruction\"") != std::string::npos);
  CHECK(g_lines.find("\"instruction\":\"B\"") != std::string::npos);
  CHECK(g_lines.find("\"word\":1") != std::string::npos);
  CHECK(g_lines.find("\"encoding\":\"0x0000000014000002\"") != std::string::npos);
  CHECK(g_lines.find("inside the patched bytes") != std::string::npos);
}

TEST(NonExecutablePageIsRefused) {
  std::uint32_t* page = MakePage(kPrologue.words, 4, false);
  CHECK(page != nullptr);
  if (page == nullptr) return;
  void* original = nullptr;
  const InlineStatus st = InlineHookTestAccess::Install(page, kPrologue, reinterpret_cast<void*>(0x1000), &original);
  CHECK_EQ(st, InlineStatus::kPageNotExecutable);
  CHECK_EQ(page[0], kStpFp);
  CHECK(original == nullptr);
}

TEST(AlignmentBadArgumentAndPageStraddleAreRefused) {
  std::uint32_t* page = MakePage(kPrologue.words, 4, true);
  CHECK(page != nullptr);
  if (page == nullptr) return;
  CHECK_EQ(sentinel::CheckInlineTarget(nullptr, kPrologue), InlineStatus::kBadArgument);
  CHECK_EQ(sentinel::CheckInlineTarget(reinterpret_cast<const char*>(page) + 2, kPrologue),
           InlineStatus::kNotWordAligned);
  // The last 8 bytes of a page: the 16 patched bytes would cross into the next page.
  CHECK_EQ(sentinel::CheckInlineTarget(reinterpret_cast<const char*>(page) + Page() - 8, kPrologue),
           InlineStatus::kWindowStraddlesPage);
  CHECK_EQ(sentinel::CheckInlineTarget(page, kPrologue), InlineStatus::kOk);
}

TEST(SecondInstallOfATargetIsRefused) {
  std::uint32_t* page = MakePage(kPrologue.words, 4, true);
  CHECK(page != nullptr);
  if (page == nullptr) return;
  void* first = nullptr;
  CHECK_EQ(InlineHookTestAccess::Install(page, kPrologue, reinterpret_cast<void*>(0x2000), &first), InlineStatus::kOk);
  void* second = nullptr;
  const InlineStatus st = InlineHookTestAccess::Install(page, kPrologue, reinterpret_cast<void*>(0x3000), &second);
  CHECK_EQ(st, InlineStatus::kAlreadyInstalled);
  CHECK(second == nullptr);
  CHECK_EQ(Quad(&page[2]), static_cast<std::uint64_t>(0x2000));
}

// A protection change that fails on demand (the Quest 3 in the field refused the final mprotect(RX) after a
// successful mprotect(RW) on the same page); every other call is the real mprotect.
struct ProtectFault {
  std::uintptr_t page;  // 0: any address
  int prot;             // the protection requested by the calls that fail
  int failures;         // how many matching calls still fail
  int err;
};
ProtectFault g_fault{};

int FaultyProtect(void* addr, std::size_t len, int prot) {
  const auto a = reinterpret_cast<std::uintptr_t>(addr);
  if (g_fault.failures > 0 && prot == g_fault.prot && (g_fault.page == 0 || g_fault.page == a)) {
    --g_fault.failures;
    errno = g_fault.err;
    return -1;
  }
  return mprotect(addr, len, prot);
}

// Arms the fault for the lifetime of one scope; the real mprotect comes back on exit.
class FaultScope {
 public:
  FaultScope(std::uintptr_t page, int prot, int failures) {
    g_fault = ProtectFault{page, prot, failures, EACCES};
    previous_ = InlineHookTestAccess::SetProtect(&FaultyProtect);
  }
  ~FaultScope() { InlineHookTestAccess::SetProtect(previous_); }
  FaultScope(const FaultScope&) = delete;
  FaultScope& operator=(const FaultScope&) = delete;

 private:
  sentinel::ProtectFn previous_;
};

// The permissions /proc/self/maps shows for the mapping that holds `addr` ("r-xp"), "" when none does.
std::string PermsOf(const void* addr) {
  std::FILE* f = std::fopen("/proc/self/maps", "re");
  if (f == nullptr) return "";
  char line[512];
  std::string perms;
  const auto a = reinterpret_cast<std::uintptr_t>(addr);
  while (perms.empty() && std::fgets(line, sizeof line, f) != nullptr) {
    unsigned long lo = 0;
    unsigned long hi = 0;
    char p[5] = {};
    if (std::sscanf(line, "%lx-%lx %4s", &lo, &hi, p) == 3 && a >= lo && a < hi) perms = p;
  }
  std::fclose(f);
  return perms;
}

void CheckOriginalPrologue(const std::uint32_t* page) {
  CHECK_EQ(page[0], kStpFp);
  CHECK_EQ(page[1], kMovFp);
  CHECK_EQ(page[2], kSubSp);
  CHECK_EQ(page[3], kStrX19);
}

TEST(AFailedRestoreOfExecuteTakesTheHookBackOutAndTheSecondTryLeavesThePageExecutable) {
  std::uint32_t* page = MakePage(kPrologue.words, 4, true);
  CHECK(page != nullptr);
  if (page == nullptr) return;
  void* original = nullptr;
  InlineStatus st = InlineStatus::kOk;
  g_lines.clear();
  sentinel::SetLogSink(&CaptureSink);
  {
    FaultScope fault(reinterpret_cast<std::uintptr_t>(page), PROT_READ | PROT_EXEC, 1);
    st = InlineHookTestAccess::Install(page, kPrologue, reinterpret_cast<void*>(0x1000), &original);
    CHECK_EQ(g_fault.failures, 0);  // the install did reach the final restore
  }
  sentinel::SetLogSink(nullptr);
  CHECK_EQ(st, InlineStatus::kRestoreProtectUndone);
  CHECK_EQ(PermsOf(page), std::string("r-xp"));
  CheckOriginalPrologue(page);
  CHECK(original == nullptr);  // the published original went back to its entry value
  CHECK(g_lines.find("\"status\":\"restore_protect_undone\"") != std::string::npos);
  CHECK(g_lines.find("\"level\":\"error\"") != std::string::npos);
  CHECK(g_lines.find("\"op\":\"target_rx\"") != std::string::npos);
  CHECK(g_lines.find("\"errno\":13") != std::string::npos);
  CHECK(g_lines.find("\"page_perms\":\"rw-p\"") != std::string::npos);  // the state the failed call left
  CHECK(g_lines.find("\"status\":\"ok\"") == std::string::npos);

  // The registry let go of the target: the same page installs cleanly now.
  void* again = nullptr;
  CHECK_EQ(InlineHookTestAccess::Install(page, kPrologue, reinterpret_cast<void*>(0x2000), &again), InlineStatus::kOk);
  CHECK_EQ(Quad(&page[2]), static_cast<std::uint64_t>(0x2000));
  CHECK_EQ(PermsOf(page), std::string("r-xp"));
}

TEST(WhenTheSecondTryFailsTooTheOriginalBytesAreInPlaceAndTheLogSaysItIsFatal) {
  std::uint32_t* page = MakePage(kPrologue.words, 4, true);
  CHECK(page != nullptr);
  if (page == nullptr) return;
  void* original = nullptr;
  InlineStatus st = InlineStatus::kOk;
  g_lines.clear();
  sentinel::SetLogSink(&CaptureSink);
  {
    FaultScope fault(reinterpret_cast<std::uintptr_t>(page), PROT_READ | PROT_EXEC, 2);
    st = InlineHookTestAccess::Install(page, kPrologue, reinterpret_cast<void*>(0x1000), &original);
  }
  sentinel::SetLogSink(nullptr);
  CHECK_EQ(st, InlineStatus::kRestoreProtectFailed);
  CheckOriginalPrologue(page);
  CHECK(original == nullptr);
  CHECK_EQ(PermsOf(page), std::string("rw-p"));  // nothing here can make it executable again
  CHECK(g_lines.find("\"status\":\"restore_protect_failed\"") != std::string::npos);
  CHECK(g_lines.find("\"level\":\"error\"") != std::string::npos);
  CHECK(g_lines.find("\"errno\":13") != std::string::npos);
  CHECK(g_lines.find("\"retry_errno\":13") != std::string::npos);
  CHECK(g_lines.find("fatal") != std::string::npos);
  CHECK_EQ(mprotect(page, Page(), PROT_READ | PROT_EXEC), 0);  // leave the test process clean
}

TEST(EveryFailedProtectionChangeLogsItsErrnoAndThePagePermissions) {
  // The target page cannot be made writable: nothing was touched and the line says why.
  std::uint32_t* page = MakePage(kPrologue.words, 4, true);
  CHECK(page != nullptr);
  if (page == nullptr) return;
  void* original = nullptr;
  g_lines.clear();
  sentinel::SetLogSink(&CaptureSink);
  InlineStatus st = InlineStatus::kOk;
  {
    FaultScope fault(reinterpret_cast<std::uintptr_t>(page), PROT_READ | PROT_WRITE, 1);
    st = InlineHookTestAccess::Install(page, kPrologue, reinterpret_cast<void*>(0x1000), &original);
  }
  CHECK_EQ(st, InlineStatus::kProtectFailed);
  CHECK(g_lines.find("\"op\":\"target_rw\"") != std::string::npos);
  CHECK(g_lines.find("\"errno\":13") != std::string::npos);
  CHECK(g_lines.find("\"page_perms\":\"r-xp\"") != std::string::npos);
  CheckOriginalPrologue(page);

  // The trampoline page cannot be made executable (the first r-x request of an install).
  g_lines.clear();
  {
    FaultScope fault(0, PROT_READ | PROT_EXEC, 1);
    st = InlineHookTestAccess::Install(page, kPrologue, reinterpret_cast<void*>(0x1000), &original);
  }
  sentinel::SetLogSink(nullptr);
  CHECK_EQ(st, InlineStatus::kTrampolineFailed);
  CHECK(g_lines.find("\"op\":\"trampoline_rx\"") != std::string::npos);
  CHECK(g_lines.find("\"errno\":13") != std::string::npos);
  CheckOriginalPrologue(page);
  CHECK(original == nullptr);
}

TEST(ASuccessfulInstallLogsNoErrno) {
  std::uint32_t* page = MakePage(kPrologue.words, 4, true);
  CHECK(page != nullptr);
  if (page == nullptr) return;
  void* original = nullptr;
  g_lines.clear();
  sentinel::SetLogSink(&CaptureSink);
  const InlineStatus st = InlineHookTestAccess::Install(page, kPrologue, reinterpret_cast<void*>(0x1000), &original);
  sentinel::SetLogSink(nullptr);
  CHECK_EQ(st, InlineStatus::kOk);
  CHECK(g_lines.find("errno") == std::string::npos);
  CHECK_EQ(PermsOf(page), std::string("r-xp"));
}

// A thunk for the entry-point check; the tag makes its statics its own.
struct InlineTestTag {};
using InlineTestThunk = sentinel::CallbackThunk<InlineTestTag, int(int)>;

TEST(ThunkEntryBecomesTheDetourAndItsOriginalIsTheTrampoline) {
  std::uint32_t* page = MakePage(kPrologue.words, 4, true);
  CHECK(page != nullptr);
  if (page == nullptr) return;
  const InlineStatus st = sentinel::InstallInlineThunk<InlineTestThunk>(page, kPrologue);
  CHECK_EQ(st, InlineStatus::kOk);
  CHECK_EQ(page[0], sentinel::kLdrX16Literal8);
  CHECK_EQ(Quad(&page[2]), static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(InlineTestThunk::EntryAddress())));
  CHECK(*InlineTestThunk::OriginalOut() != nullptr);
  CHECK(reinterpret_cast<void*>(InlineTestThunk::Original()) == *InlineTestThunk::OriginalOut());
}

}  // namespace

int main(int argc, char** argv) { return mini_test::RunAll(argc, argv); }
