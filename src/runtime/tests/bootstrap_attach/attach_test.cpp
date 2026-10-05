// Attach-time MinHook proof-gate harness
// (docs/design/2026-10-05-bugsplat-nevr-split.md, Implementation step 2 and the
// "Proof gate" paragraph).
//
// This executable stands in for echovr.exe. It exports an event recorder and a
// synthetic game image: a VirtualAlloc'd PE32+ header block with the supported
// TimeDateStamp and, at RVA 0x116720, a function that begins with the REAL
// 18-byte PreprocessCommandLine prologue and then calls a C body that records
// the call and returns ExpectedResult(game). The probe host (BugSplat64.dll)
// validates and MinHooks it from DLL_PROCESS_ATTACH; MinHook therefore relocates
// and executes the real prologue instructions in its trampoline.
//
// Built twice:
//   test_bootstrap_attach.exe        suite + dynamic-load child scenarios
//                                    (LoadLibraryW of the host while other threads run)
//   test_bootstrap_attach_static.exe statically imports the host, the way the game
//                                    does: host attach runs inside process start,
//                                    before main and before this image's CRT init.
//
// Every child validates, from the recorded events (index order is a
// linearization: each record takes its slot with InterlockedIncrement):
//   - host attach: enter < image ok < prologue ok < hook enabled (JMP written) < exit,
//     with nevr.dll NOT loaded at attach exit;
//   - no runtime operation (load, DllMain, GetApi, initialize, dispatch) before
//     attach returned — and, where the scenario allows it, before LoadLibrary /
//     main saw the return;
//   - every call issued executed the original exactly once, with the exact
//     argument, and returned the exact result;
//   - per thread, before -> original -> after nesting is balanced;
//   - load/API/init failures degrade to original-only calls and never dispatch.

#include "runtime/tests/bootstrap_attach/abi.h"

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

using namespace BootstrapProbe;

namespace {

struct EventRecord {
  LONG event;
  DWORD thread;
  LONGLONG qpc;
  std::uint64_t payload;
};

// Plain zero-initialised storage only: the static-import build records from the
// host's DllMain, before this image's CRT has run.
// 2^20 x 32 B = 32 MiB of .bss: the hammer scenario records every call it
// makes across the whole attach window.
constexpr LONG kEventCapacity = 1 << 20;
EventRecord g_events[kEventCapacity];
volatile LONG g_event_count = 0;
volatile LONG g_event_overflow = 0;

enum ImageKind : LONG { kImageSupported = 0, kImageWrongTimestamp = 1, kImageWrongPrologue = 2 };
volatile LONG g_image_kind = kImageSupported;
BYTE* g_image = nullptr;
volatile LONG g_attach_hold_ms = 0;
constexpr SIZE_T kImageSize = 0x118000;
constexpr SIZE_T kPage = 0x1000;

}  // namespace

extern "C" __declspec(dllexport) void BootstrapProbeRecordEvent(LONG event, std::uint64_t payload) {
  LARGE_INTEGER now;
  QueryPerformanceCounter(&now);
  const LONG index = InterlockedIncrement(&g_event_count) - 1;
  if (index < 0 || index >= kEventCapacity) {
    InterlockedExchange(&g_event_overflow, 1);
    return;
  }
  g_events[index] = EventRecord{event, GetCurrentThreadId(), now.QuadPart, payload};
}

// The body behind the synthetic prologue. Called from generated machine code.
extern "C" std::uint64_t BootstrapProbeOriginalBody(void* game) {
  BootstrapProbeRecordEvent(kOriginal, reinterpret_cast<std::uintptr_t>(game));
  return ExpectedResult(game);
}

// Disjoint target for the runtime fixture's own MinHook instance.
extern "C" __declspec(dllexport) __attribute__((noinline)) std::uint64_t
BootstrapProbeSecondTarget(std::uint64_t value) {
  BootstrapProbeRecordEvent(kSecondOriginal, value);
  return value + 1;
}

namespace {

// Writes the synthetic image. CRT-free (byte loops, kernel32 only) because the
// static-import build reaches it from the host's DllMain.
BYTE* BuildGameImage(LONG kind) {
  auto* base = static_cast<BYTE*>(
      VirtualAlloc(nullptr, kImageSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
  if (base == nullptr) return nullptr;
  constexpr LONG kNtOffset = 0x80;
  auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
  dos->e_magic = IMAGE_DOS_SIGNATURE;
  dos->e_lfanew = kNtOffset;
  auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + kNtOffset);
  nt->Signature = IMAGE_NT_SIGNATURE;
  nt->FileHeader.Machine = IMAGE_FILE_MACHINE_AMD64;
  nt->FileHeader.NumberOfSections = 1;
  nt->FileHeader.TimeDateStamp =
      kind == kImageWrongTimestamp ? kSupportedTimestamp + 1 : kSupportedTimestamp;
  nt->FileHeader.SizeOfOptionalHeader = sizeof(IMAGE_OPTIONAL_HEADER64);
  nt->OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR64_MAGIC;
  nt->OptionalHeader.SizeOfHeaders = kPage;
  nt->OptionalHeader.SizeOfImage = static_cast<DWORD>(kImageSize);

  BYTE* code = base + kPreprocessRva;
  SIZE_T at = 0;
  for (const unsigned char byte : kPreprocessPrologue) code[at++] = byte;
  if (kind == kImageWrongPrologue) code[kPrologueMutationOffset] = 0xb2;
  // mov rcx, rdi ; mov rax, imm64 ; call rax ; add rsp, 0xa8 ; pop rdi ; pop rbp ; ret
  // Entry RSP is 8 mod 16; push, push, sub 0xa8 leaves it 16-aligned with the
  // 32-byte shadow space inside the frame the real prologue allocated.
  const unsigned char mid[] = {0x48, 0x89, 0xf9, 0x48, 0xb8};
  for (const unsigned char byte : mid) code[at++] = byte;
  std::uintptr_t body = reinterpret_cast<std::uintptr_t>(&BootstrapProbeOriginalBody);
  for (int shift = 0; shift < 64; shift += 8) code[at++] = static_cast<BYTE>(body >> shift);
  const unsigned char tail[] = {0xff, 0xd0, 0x48, 0x81, 0xc4, 0xa8, 0x00,
                                0x00, 0x00, 0x5f, 0x5d, 0xc3};
  for (const unsigned char byte : tail) code[at++] = byte;

  DWORD old_protection = 0;
  BYTE* code_page = base + (kPreprocessRva & ~(kPage - 1));
  if (VirtualProtect(base, kPage, PAGE_READONLY, &old_protection) == FALSE ||
      VirtualProtect(code_page, kPage, PAGE_EXECUTE_READ, &old_protection) == FALSE) {
    return nullptr;
  }
  FlushInstructionCache(GetCurrentProcess(), code, at);
  return base;
}

}  // namespace

extern "C" __declspec(dllexport) DWORD BootstrapProbeAttachHoldMs() {
  return static_cast<DWORD>(g_attach_hold_ms);
}

extern "C" __declspec(dllexport) HMODULE BootstrapProbeGameImage() {
  if (g_image == nullptr) g_image = BuildGameImage(g_image_kind);
  return reinterpret_cast<HMODULE>(g_image);
}

namespace {

[[noreturn]] void Fail(const char* message);

NevrPreprocessCommandLineFn Target() {
  return reinterpret_cast<NevrPreprocessCommandLineFn>(g_image + kPreprocessRva);
}

void CallTarget(std::uintptr_t raw_game) {
  void* game = reinterpret_cast<void*>(raw_game);
  BootstrapProbeRecordEvent(kCallIssued, raw_game);
  if (Target()(game) != ExpectedResult(game)) BootstrapProbeRecordEvent(kBadResult, raw_game);
}

const char* EventName(LONG event) {
  static const char* const kNames[kEventLimit] = {
      "?", "host.attach_enter", "host.image_accepted", "host.image_rejected",
      "host.prologue_accepted", "host.prologue_rejected", "host.hook_enabled",
      "host.hook_failed", "host.attach_exit", "host.load_begin", "host.load_end",
      "host.reentry_bypass", "runtime.dll_attach", "runtime.get_api",
      "runtime.initialized", "runtime.before", "original", "runtime.after",
      "call.issued", "call.bad_result", "main.entered", "host.loadlibrary_returned",
      "runtime.second_hook_enabled", "second.detour", "second.original",
      "host.attach_held"};
  return event > 0 && event < kEventLimit ? kNames[event] : "?";
}

std::vector<EventRecord> Snapshot() {
  if (InterlockedCompareExchange(&g_event_overflow, 0, 0) != 0) Fail("event buffer overflowed");
  const LONG count = InterlockedCompareExchange(&g_event_count, 0, 0);
  return std::vector<EventRecord>(g_events, g_events + count);
}

constexpr std::size_t kNone = static_cast<std::size_t>(-1);

std::size_t First(const std::vector<EventRecord>& events, LONG event) {
  for (std::size_t index = 0; index < events.size(); ++index) {
    if (events[index].event == event) return index;
  }
  return kNone;
}

LONG Count(const std::vector<EventRecord>& events, LONG event) {
  LONG count = 0;
  for (const EventRecord& record : events) count += record.event == event ? 1 : 0;
  return count;
}

void PrintTimeline(const std::vector<EventRecord>& events, const char* label) {
  LARGE_INTEGER frequency;
  QueryPerformanceFrequency(&frequency);
  const std::size_t origin = First(events, kHostAttachEnter);
  const LONGLONG origin_qpc = origin == kNone ? events.front().qpc : events[origin].qpc;
  std::printf("bootstrap attach test: [%s] timeline (first occurrence; t = us since host.attach_enter; "
              "#idx = global record order)\n", label);
  for (LONG event = 1; event < kEventLimit; ++event) {
    const std::size_t index = First(events, event);
    if (index == kNone) continue;
    const double micros = static_cast<double>(events[index].qpc - origin_qpc) * 1e6 /
                          static_cast<double>(frequency.QuadPart);
    const unsigned long long record_index = index;
    std::printf("  #%-6llu t=%10.1f  tid=%-6lu  %-28s count=%ld payload=0x%llx\n", record_index, micros,
                static_cast<unsigned long>(events[index].thread), EventName(event),
                static_cast<long>(Count(events, event)),
                static_cast<unsigned long long>(events[index].payload));
  }
}

void Fail(const char* message) {
  std::fprintf(stderr, "bootstrap attach test: FAIL: %s\n", message);
  const LONG count = InterlockedCompareExchange(&g_event_count, 0, 0);
  if (count > 0 && count <= kEventCapacity) {
    std::fflush(stdout);
    PrintTimeline(std::vector<EventRecord>(g_events, g_events + count), "failure");
  }
  std::fflush(stdout);
  std::fflush(stderr);
  ExitProcess(1);
  __builtin_unreachable();
}

void Require(bool condition, const char* message) {
  if (!condition) Fail(message);
}

// Per-thread nesting: before pushes a frame, original completes the innermost
// open frame (or is a lone original when no frame is open: unhooked/degraded),
// after closes it. Returns the number of lone originals.
LONG CheckFrames(const std::vector<EventRecord>& events) {
  struct Frame {
    std::uint64_t game;
    bool original_seen;
  };
  std::map<DWORD, std::vector<Frame>> stacks;
  LONG lone = 0;
  for (const EventRecord& record : events) {
    std::vector<Frame>& stack = stacks[record.thread];
    if (record.event == kRuntimeBefore) {
      stack.push_back(Frame{record.payload, false});
    } else if (record.event == kOriginal) {
      if (stack.empty()) {
        ++lone;
      } else {
        Require(!stack.back().original_seen, "original ran twice inside one dispatched call");
        Require(stack.back().game == record.payload, "original saw a different game pointer than before");
        stack.back().original_seen = true;
      }
    } else if (record.event == kRuntimeAfter) {
      Require(!stack.empty(), "runtime.after without an open runtime.before on its thread");
      Require(stack.back().original_seen, "runtime.after ran before the original");
      Require(record.payload == ExpectedResult(reinterpret_cast<const void*>(stack.back().game)),
              "runtime.after saw a result other than the original's");
      stack.pop_back();
    }
  }
  for (const auto& entry : stacks) Require(entry.second.empty(), "a dispatched call never reached runtime.after");
  return lone;
}

struct Expectation {
  bool hooked = true;                  // image + prologue accepted, detour enabled
  bool dispatched = true;              // runtime loaded, validated and initialized
  std::uint64_t load_outcome = kLoadOk;  // low 32 bits of host.load_end payload
  bool static_import = false;
  bool load_may_precede_return = false;  // hammer: a racing thread may trigger the load
  bool load_begin_inside_attach = false;  // held attach: the racing load MUST start inside DllMain
  enum class Lone { kNone, kAtLeastOne, kExactlyOne, kEveryCall } lone = Lone::kNone;
  bool expect_reentry_bypass = false;
  bool second_hook_called = false;
  LONG min_threads_parked_in_load = 0;  // concurrent-first
};

void ValidateHostAttach(const std::vector<EventRecord>& events, const Expectation& expect,
                        std::size_t* attach_exit) {
  Require(Count(events, kHostAttachEnter) == 1 && Count(events, kHostAttachExit) == 1,
          "host attach did not run exactly once");
  const std::size_t enter = First(events, kHostAttachEnter);
  *attach_exit = First(events, kHostAttachExit);
  Require(enter < *attach_exit, "host attach exit recorded before enter");
  Require(events[*attach_exit].payload == 0, "nevr.dll was loaded before host DllMain returned");
  const BYTE first_target_byte = g_image[kPreprocessRva];
  if (expect.hooked) {
    const std::size_t image = First(events, kHostImageAccepted);
    const std::size_t prologue = First(events, kHostPrologueAccepted);
    const std::size_t enabled = First(events, kHostHookEnabled);
    Require(image != kNone && prologue != kNone && enabled != kNone,
            "host did not accept image, accept prologue and enable the hook");
    Require(enter < image && image < prologue && prologue < enabled && enabled < *attach_exit,
            "host attach steps out of order");
    Require(events[enabled].payload == 0xE9 && first_target_byte == 0xE9,
            "rendezvous does not start with a JMP rel32 after enable (partial install?)");
  } else {
    Require(Count(events, kHostHookEnabled) == 0, "host hooked a rejected image");
    Require(first_target_byte == kPreprocessPrologue[0], "rejected target bytes were modified");
    Require(Count(events, kHostImageRejected) + Count(events, kHostPrologueRejected) == 1,
            "rejection was not recorded exactly once");
  }
}

void ValidateNoEarlyRuntime(const std::vector<EventRecord>& events, const Expectation& expect,
                            std::size_t attach_exit) {
  const std::size_t returned =
      First(events, expect.static_import ? kMainEntered : kHostLoadReturned);
  Require(returned != kNone && attach_exit < returned,
          "attach return was not observed after host attach exit");
  // host.load_begin is the HOST deciding to load (it can race attach exit when
  // another thread is already calling the game); everything below is the load
  // itself or code inside nevr.dll, and must follow attach exit.
  const LONG runtime_events[] = {kHostLoadEnd, kHostReentryBypass,
                                 kRuntimeDllAttach, kRuntimeGetApi, kRuntimeInitialized,
                                 kRuntimeBefore, kRuntimeAfter, kRuntimeSecondHookEnabled};
  const std::size_t load_begin = First(events, kHostLoadBegin);
  if (expect.load_begin_inside_attach) {
    // Bounded by prologue_accepted, not hook_enabled: MH_EnableHook makes the JMP
    // live and resumes frozen threads BEFORE it returns, so a racing caller can
    // enter the detour before the host records hook_enabled (observed under
    // Wine). That is safe only because the host publishes the trampoline and the
    // host context before enabling.
    Require(load_begin != kNone && First(events, kHostPrologueAccepted) < load_begin &&
                load_begin < attach_exit,
            "held attach: no thread entered the detour while host DllMain held the loader lock");
  } else if (load_begin != kNone && !expect.load_may_precede_return) {
    Require(load_begin > attach_exit && load_begin > returned,
            "host began loading the runtime before attach return was observed");
  }
  const bool strict = expect.static_import || !expect.load_may_precede_return;
  for (std::size_t index = 0; index < events.size(); ++index) {
    for (const LONG event : runtime_events) {
      if (events[index].event != event) continue;
      Require(index > attach_exit && events[index].qpc >= events[attach_exit].qpc,
              "runtime operation recorded before host DllMain attach returned");
      if (strict) Require(index > returned, "runtime operation before the attach return was observed");
    }
  }
}

void ValidateDispatch(const std::vector<EventRecord>& events, const Expectation& expect) {
  const LONG calls = Count(events, kCallIssued);
  Require(calls > 0, "scenario issued no calls");
  Require(Count(events, kOriginal) == calls, "original did not run exactly once per call");
  Require(Count(events, kBadResult) == 0, "a caller received a result other than the original's");
  // A lone original is a call that reached the original with no dispatch frame
  // open: unhooked (before attach / rejected image), degraded, or the bypassed
  // same-thread re-entry during runtime load.
  const LONG lone = CheckFrames(events);
  switch (expect.lone) {
    case Expectation::Lone::kNone:
      Require(lone == 0, "a call reached the original without runtime dispatch");
      break;
    case Expectation::Lone::kAtLeastOne:
      Require(lone > 0, "hammer thread never ran the unhooked original before attach");
      break;
    case Expectation::Lone::kExactlyOne:
      Require(lone == 1, "expected exactly one undispatched original call");
      break;
    case Expectation::Lone::kEveryCall:
      Require(lone == calls, "a degraded or unhooked call was dispatched");
      break;
  }
  Require((Count(events, kHostReentryBypass) == 1) == expect.expect_reentry_bypass,
          "same-thread re-entry during runtime load was not bypassed exactly as expected");

  if (!expect.hooked) {
    Require(Count(events, kHostLoadBegin) == 0 && Count(events, kRuntimeDllAttach) == 0 &&
                GetModuleHandleW(L"nevr.dll") == nullptr,
            "runtime was loaded although the host rejected the image");
    Require(Count(events, kRuntimeBefore) == 0, "dispatch without a hook");
    return;
  }
  Require(Count(events, kHostLoadBegin) == 1 && Count(events, kHostLoadEnd) == 1,
          "runtime load attempted other than exactly once");
  const std::size_t load_end = First(events, kHostLoadEnd);
  const std::uint64_t outcome = events[load_end].payload;
  if ((outcome & 0xFFFFFFFFull) != expect.load_outcome) {
    std::fprintf(stderr, "load outcome 0x%llx, expected 0x%llx\n",
                 static_cast<unsigned long long>(outcome),
                 static_cast<unsigned long long>(expect.load_outcome));
    Fail("unexpected runtime load outcome");
  }
  if (!expect.dispatched) {
    Require(Count(events, kRuntimeBefore) == 0 && Count(events, kRuntimeAfter) == 0,
            "a failed runtime was dispatched");
    return;
  }
  Require(Count(events, kRuntimeDllAttach) == 1 && Count(events, kRuntimeGetApi) == 1 &&
              Count(events, kRuntimeInitialized) == 1,
          "runtime DllMain/GetApi/initialize did not run exactly once");
  const std::size_t initialized = First(events, kRuntimeInitialized);
  const std::uint64_t trampoline = events[initialized].payload;
  Require(trampoline != 0 && trampoline != reinterpret_cast<std::uintptr_t>(g_image + kPreprocessRva),
          "host context did not carry a trampoline distinct from the hooked target");
  Require(First(events, kRuntimeBefore) > load_end, "dispatch began before runtime initialization finished");
  Require(Count(events, kRuntimeBefore) > 0 &&
              Count(events, kRuntimeBefore) == Count(events, kRuntimeAfter),
          "before/after counts differ");
  Require(Count(events, kRuntimeSecondHookEnabled) == 1, "runtime's disjoint MinHook instance did not install");
  if (expect.second_hook_called) {
    Require(Count(events, kSecondDetour) == 1 && Count(events, kSecondOriginal) == 1,
            "runtime's disjoint hook did not run exactly once");
  }
  if (expect.min_threads_parked_in_load > 0) {
    std::map<DWORD, bool> parked;
    for (std::size_t index = 0; index < load_end; ++index) {
      if (events[index].event == kCallIssued) parked[events[index].thread] = true;
    }
    Require(static_cast<LONG>(parked.size()) >= expect.min_threads_parked_in_load,
            "too few threads entered the rendezvous while the runtime was loading");
  }
}

void Validate(const Expectation& expect, const char* label) {
  const std::vector<EventRecord> events = Snapshot();
  std::size_t attach_exit = kNone;
  ValidateHostAttach(events, expect, &attach_exit);
  ValidateNoEarlyRuntime(events, expect, attach_exit);
  ValidateDispatch(events, expect);
  PrintTimeline(events, label);
  std::printf("bootstrap attach test: PASS: %s (calls=%ld originals=%ld dispatched=%ld threads=%llu)\n",
              label, static_cast<long>(Count(events, kCallIssued)),
              static_cast<long>(Count(events, kOriginal)),
              static_cast<long>(Count(events, kRuntimeBefore)),
              [&events] {
                std::map<DWORD, bool> threads;
                for (const EventRecord& record : events) threads[record.thread] = true;
                return static_cast<unsigned long long>(threads.size());
              }());
}

struct WorkerArgs {
  LONG worker;
  LONG calls;
  HANDLE start;  // optional manual-reset gate
};

DWORD WINAPI CallWorker(void* raw) {
  const auto* args = static_cast<const WorkerArgs*>(raw);
  if (args->start != nullptr) WaitForSingleObject(args->start, INFINITE);
  for (LONG call = 0; call < args->calls; ++call) {
    CallTarget((static_cast<std::uintptr_t>(args->worker) + 1) << 24 |
               static_cast<std::uintptr_t>(call) << 4);
  }
  return 0;
}

void RunWorkers(LONG workers, LONG calls, bool gated) {
  std::vector<WorkerArgs> args(static_cast<std::size_t>(workers));
  std::vector<HANDLE> threads;
  HANDLE gate = gated ? CreateEventW(nullptr, TRUE, FALSE, nullptr) : nullptr;
  if (gated) Require(gate != nullptr, "could not create start gate");
  for (LONG worker = 0; worker < workers; ++worker) {
    args[static_cast<std::size_t>(worker)] = WorkerArgs{worker, calls, gate};
    HANDLE thread = CreateThread(nullptr, 0, &CallWorker, &args[static_cast<std::size_t>(worker)], 0, nullptr);
    Require(thread != nullptr, "could not create worker thread");
    threads.push_back(thread);
  }
  if (gated) SetEvent(gate);
  Require(WaitForMultipleObjects(static_cast<DWORD>(threads.size()), threads.data(), TRUE, 20000) ==
              WAIT_OBJECT_0,
          "worker threads timed out (deadlock?)");
  for (HANDLE thread : threads) CloseHandle(thread);
  if (gate != nullptr) CloseHandle(gate);
}

void CallSecondTarget() {
  const auto second = reinterpret_cast<SecondTargetFn>(
      GetProcAddress(GetModuleHandleW(nullptr), kSecondTargetExport));
  Require(second != nullptr && second(41) == 42, "disjoint second target returned a wrong result");
}

}  // namespace

#if defined(BOOTSTRAP_PROBE_STATIC_IMPORT)

extern "C" __declspec(dllimport) void DetoursExportPlaceholder();

int main() {
  BootstrapProbeRecordEvent(kMainEntered, 0);
  Require(GetModuleHandleW(L"nevr.dll") == nullptr, "nevr.dll loaded before main");
  Require(g_image != nullptr, "host never requested the game image during static-import attach");
  DetoursExportPlaceholder();  // the import that makes the loader attach the host first
  CallTarget(0x1000);
  RunWorkers(4, 50, false);
  CallSecondTarget();
  Expectation expect;
  expect.static_import = true;
  expect.second_hook_called = true;
  Validate(expect, "static-import (host attached during process start, before main)");
  return 0;
}

#else

namespace {

volatile LONG g_stop_heartbeat = 0;
volatile LONG g_heartbeats = 0;
volatile LONG g_stop_hammer = 0;
volatile LONG g_host_loaded = 0;
volatile LONG g_hammer_calls_after_load = 0;

DWORD WINAPI HeartbeatThread(void*) {
  while (InterlockedCompareExchange(&g_stop_heartbeat, 0, 0) == 0) {
    InterlockedIncrement(&g_heartbeats);
    Sleep(1);
  }
  return 0;
}

// Calls the rendezvous continuously across the host's attach: before the hook
// exists, while MinHook freezes threads and writes the JMP, and after.
DWORD WINAPI HammerThread(void*) {
  std::uintptr_t call = 0;
  while (InterlockedCompareExchange(&g_stop_hammer, 0, 0) == 0) {
    CallTarget(0x0F00'0000 | (++call & 0xFFFFF) << 4);
    if (InterlockedCompareExchange(&g_host_loaded, 0, 0) != 0) InterlockedIncrement(&g_hammer_calls_after_load);
  }
  return 0;
}

std::wstring ExecutableDirectory() {
  wchar_t path[MAX_PATH] = {};
  const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
  Require(length != 0 && length < MAX_PATH, "could not resolve executable path");
  std::wstring full(path, length);
  const std::size_t separator = full.find_last_of(L"\\/");
  Require(separator != std::wstring::npos, "executable path has no directory");
  return full.substr(0, separator + 1);
}

int RunChildScenario(const std::string& mode) {
  Expectation expect;
  LONG image_kind = kImageSupported;
  if (mode == "missing" || mode == "bad-image") {
    expect.dispatched = false;
    expect.load_outcome = kLoadLibraryFailed;
  } else if (mode == "bad-version") {
    expect.dispatched = false;
    expect.load_outcome = kLoadAbiRejectedBase + static_cast<std::uint64_t>(NevrAbi::Status::kMajorMismatch);
  } else if (mode == "missing-entry") {
    expect.dispatched = false;
    expect.load_outcome = kLoadNoEntry;
  } else if (mode == "init-failure") {
    expect.dispatched = false;
    expect.load_outcome = kLoadInitFailed;
  } else if (mode == "bad-timestamp" || mode == "bad-prologue") {
    expect.hooked = false;
    expect.dispatched = false;
    image_kind = mode == "bad-timestamp" ? kImageWrongTimestamp : kImageWrongPrologue;
  } else if (mode == "reentrant-init") {
    expect.expect_reentry_bypass = true;
    expect.lone = Expectation::Lone::kExactlyOne;
  } else if (mode == "concurrent-first") {
    expect.min_threads_parked_in_load = 2;
  } else if (mode == "hammer-attach" || mode == "hammer-held-attach") {
    expect.load_may_precede_return = true;
    expect.lone = Expectation::Lone::kAtLeastOne;
    if (mode == "hammer-held-attach") {
      expect.load_begin_inside_attach = true;
      InterlockedExchange(&g_attach_hold_ms, 100);
    }
  } else if (mode != "valid") {
    Fail("unknown child scenario");
  }
  if (!expect.dispatched) expect.lone = Expectation::Lone::kEveryCall;

  InterlockedExchange(&g_image_kind, image_kind);
  g_image = BuildGameImage(image_kind);
  Require(g_image != nullptr, "could not build the synthetic game image");

  HANDLE heartbeat = CreateThread(nullptr, 0, &HeartbeatThread, nullptr, 0, nullptr);
  Require(heartbeat != nullptr, "could not create heartbeat thread");
  HANDLE hammer = nullptr;
  if (mode == "hammer-attach" || mode == "hammer-held-attach") {
    hammer = CreateThread(nullptr, 0, &HammerThread, nullptr, 0, nullptr);
    Require(hammer != nullptr, "could not create hammer thread");
  }
  Sleep(30);
  const std::wstring host_path = ExecutableDirectory() + L"BugSplat64.dll";
  HMODULE host = LoadLibraryW(host_path.c_str());
  Require(host != nullptr, "probe host failed to load");
  BootstrapProbeRecordEvent(kHostLoadReturned, 0);
  InterlockedExchange(&g_host_loaded, 1);

  if (hammer != nullptr) {
    for (int wait = 0; wait < 500 && InterlockedCompareExchange(&g_hammer_calls_after_load, 0, 0) < 200; ++wait) {
      Sleep(10);
    }
    InterlockedExchange(&g_stop_hammer, 1);
    Require(WaitForSingleObject(hammer, 10000) == WAIT_OBJECT_0, "hammer thread did not stop");
    CloseHandle(hammer);
    Require(InterlockedCompareExchange(&g_hammer_calls_after_load, 0, 0) >= 200,
            "hammer thread stalled after attach");
  }
  InterlockedExchange(&g_stop_heartbeat, 1);
  Require(WaitForSingleObject(heartbeat, 5000) == WAIT_OBJECT_0, "heartbeat thread did not stop");
  CloseHandle(heartbeat);
  Require(InterlockedCompareExchange(&g_heartbeats, 0, 0) >= 2,
          "a concurrent thread made no progress across MinHook attach");

  if (mode == "concurrent-first") {
    RunWorkers(8, 1, true);
    RunWorkers(4, 25, false);
  } else if (mode == "reentrant-init") {
    CallTarget(0x2000);
  } else {
    CallTarget(0x1000);
    RunWorkers(4, mode == "valid" || mode == "hammer-attach" || mode == "hammer-held-attach" ? 100 : 25,
               false);
  }
  if (expect.dispatched) {
    CallSecondTarget();
    expect.second_hook_called = true;
  }
  Validate(expect, mode.c_str());
  return 0;
}

void StageRuntimeFixture(const std::wstring& root, const wchar_t* fixture) {
  const std::wstring runtime = root + L"nevr.dll";
  if (fixture == nullptr) {
    DeleteFileW(runtime.c_str());
    Require(GetFileAttributesW(runtime.c_str()) == INVALID_FILE_ATTRIBUTES, "could not remove nevr.dll");
    return;
  }
  const std::wstring source = root + L"fixtures\\" + fixture + L"\\nevr.dll";
  if (!CopyFileW(source.c_str(), runtime.c_str(), FALSE)) {
    std::fprintf(stderr, "CopyFileW %ls failed, Win32 error=%lu\n", source.c_str(),
                 static_cast<unsigned long>(GetLastError()));
    Fail("could not stage runtime fixture");
  }
}

void RunChild(const std::wstring& command_line, const char* label) {
  std::wstring command = command_line;
  // The child's timeline and PASS/FAIL lines are the evidence: hand it this
  // process's stdout/stderr explicitly (a redirected parent has no console for
  // the child to fall back to).
  STARTUPINFOW startup = {};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
  startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
  startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
  for (HANDLE handle : {startup.hStdOutput, startup.hStdError}) {
    if (handle != nullptr && handle != INVALID_HANDLE_VALUE) {
      SetHandleInformation(handle, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
    }
  }
  PROCESS_INFORMATION process = {};
  std::fflush(stdout);
  std::fflush(stderr);
  Require(CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr,
                         &startup, &process) != FALSE,
          "could not launch scenario child");
  CloseHandle(process.hThread);
  if (WaitForSingleObject(process.hProcess, 30000) != WAIT_OBJECT_0) {
    TerminateProcess(process.hProcess, 2);
    CloseHandle(process.hProcess);
    std::fprintf(stderr, "scenario %s timed out\n", label);
    Fail("scenario child timed out (attach hang or deadlock)");
  }
  DWORD exit_code = 1;
  GetExitCodeProcess(process.hProcess, &exit_code);
  CloseHandle(process.hProcess);
  if (exit_code != 0) {
    std::fprintf(stderr, "scenario %s exited 0x%lx\n", label, static_cast<unsigned long>(exit_code));
    Fail("scenario child returned failure");
  }
}

int RunSuite() {
  const std::wstring root = ExecutableDirectory();
  wchar_t self[MAX_PATH] = {};
  const DWORD length = GetModuleFileNameW(nullptr, self, MAX_PATH);
  Require(length != 0 && length < MAX_PATH, "could not resolve executable path");
  struct Scenario {
    const char* mode;
    const wchar_t* fixture;
  };
  const Scenario scenarios[] = {
      {"valid", L"good"},
      {"missing", nullptr},
      {"bad-version", L"bad-version"},
      {"missing-entry", L"missing-entry"},
      {"init-failure", L"init-failure"},
      {"bad-image", L"bad-image"},
      {"concurrent-first", L"slow-init"},
      {"hammer-attach", L"good"},
      {"hammer-held-attach", L"good"},
      {"reentrant-init", L"reenter-init"},
      {"bad-timestamp", L"good"},
      {"bad-prologue", L"good"},
  };
  for (const Scenario& scenario : scenarios) {
    StageRuntimeFixture(root, scenario.fixture);
    std::wstring mode(scenario.mode, scenario.mode + std::strlen(scenario.mode));
    RunChild(L"\"" + std::wstring(self, length) + L"\" --child " + mode, scenario.mode);
  }
  StageRuntimeFixture(root, L"good");
  RunChild(L"\"" + root + L"test_bootstrap_attach_static.exe\"", "static-import");
  std::printf("bootstrap attach test: PASS: all %llu dynamic scenarios + static-import\n",
              static_cast<unsigned long long>(sizeof(scenarios) / sizeof(scenarios[0])));
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc == 3 && std::strcmp(argv[1], "--child") == 0) return RunChildScenario(argv[2]);
  if (argc != 1) Fail("usage: test_bootstrap_attach.exe [--child <scenario>]");
  return RunSuite();
}

#endif
