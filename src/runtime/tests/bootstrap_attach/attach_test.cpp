// Attach-time MinHook proof-gate harness
// (docs/adr/0008-bugsplat-host-and-nevr-runtime.md, "Proof gate").
//
// This executable stands in for echovr.exe. It exports an event recorder and a
// synthetic game image: a VirtualAlloc'd PE32+ header block with the supported
// TimeDateStamp and, at RVA 0xCD510, a function that begins with the REAL
// 13-byte GameMainWrapper prologue and then calls a C body that records the call
// and returns ExpectedResult(command_line). The host (BugSplat64.dll, the production
// host units behind host_shim.cpp) validates and MinHooks it from DLL_PROCESS_ATTACH;
// MinHook therefore relocates and executes the real prologue instructions in its
// trampoline.
//
// Built twice:
//   test_bootstrap_attach.exe        suite + dynamic-load child scenarios
//                                    (LoadLibraryW of the host while other threads run)
//   test_bootstrap_attach_static.exe statically imports the host, the way the game
//                                    does: host attach runs inside process start,
//                                    before main and before this image's CRT init; its
//                                    main enters the wrapper.
//
// Every child validates, from the recorded events (index order is a
// linearization: each record takes its slot with InterlockedIncrement) and from the
// host's own bootstrap records (nevr-bootstrap.jsonl in the scenario's directory):
//   - host attach: enter < hook live (JMP written) < exit, with nevr-runtime.dll NOT loaded
//     at attach exit; the records are the decisions in order with exactly one
//     hook_install record when hooked and none when not;
//   - no runtime operation (load, DllMain, GetApi, initialize, dispatch) before
//     attach returned — and, where the scenario allows it, before LoadLibrary /
//     main saw the return;
//   - every call issued executed the original exactly once, with the exact
//     argument, and returned the exact result;
//   - per thread, game_main enter -> original -> exit nesting is balanced;
//   - the runtime never hooks the wrapper: the host's record of its hook (target and
//     the first 16 bytes after MH_EnableHook) still equals the bytes at the wrapper
//     after the runtime's initialize and in game_main;
//   - load/API/init failures stop the game (the host's test seam records the stop with exit code 78 and the
//     original runs after it) and never dispatch.

#include "runtime/tests/bootstrap_attach/abi.h"

#include <nlohmann/json.hpp>

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace nevr_bootstrap_probe;

namespace {

struct EventRecord {
  LONG event;
  DWORD thread;
  LONGLONG qpc;
  std::uint64_t payload;
};

// Plain zero-initialised storage only: the static-import build records from the
// host's DllMain, before this image's CRT has run.
// 2^23 x 32 B = 256 MiB of .bss: the hammer scenario records every call it
// makes across the whole attach window. Native Windows thread scheduling lets
// the hammering thread issue far more calls in the same wall-clock attach
// window than Wine's scheduler does, so the capacity is sized for native
// Windows. An overflow is a real throughput finding, not a capacity artifact.
constexpr LONG kEventCapacity = 1 << 23;
EventRecord g_events[kEventCapacity];
volatile LONG g_event_count = 0;
volatile LONG g_event_overflow = 0;

enum ImageKind : LONG { kImageSupported = 0, kImageWrongTimestamp = 1, kImageWrongPrologue = 2 };
volatile LONG g_image_kind = kImageSupported;
BYTE* g_image = nullptr;
volatile LONG g_attach_hold_ms = 0;
volatile LONG g_no_install_event = 0;
constexpr SIZE_T kImageSize = 0x118000;
// A value that needs all 64 bits: a truncated argument or result changes it.
constexpr std::int64_t kCommandLine = 0x1234'5678'9ABC'DEF0;
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
extern "C" std::int64_t BootstrapProbeOriginalBody(std::int64_t command_line) {
  BootstrapProbeRecordEvent(kOriginal, static_cast<std::uint64_t>(command_line));
  return ExpectedResult(command_line);
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

  // The real prologue (sub rsp,0x38; mov qword [rsp+0x20],0), then
  //   mov rax, imm64 ; call rax ; mov [rsp+0x20], rax ; add rsp, 0x38 ; ret
  // Entry RSP is 8 mod 16; the real `sub rsp,0x38` makes it 16-aligned for the call, and
  // RCX (the command line) reaches the body untouched.
  BYTE* code = base + nevr_host::kGameMainWrapperRva;
  SIZE_T at = 0;
  for (const unsigned char byte : nevr_host::kGameMainWrapperPrologue) code[at++] = byte;
  if (kind == kImageWrongPrologue) code[kPrologueMutationOffset] = 0x28;
  const unsigned char mid[] = {0x48, 0xb8};
  for (const unsigned char byte : mid) code[at++] = byte;
  std::uintptr_t body = reinterpret_cast<std::uintptr_t>(&BootstrapProbeOriginalBody);
  for (int shift = 0; shift < 64; shift += 8) code[at++] = static_cast<BYTE>(body >> shift);
  const unsigned char tail[] = {0xff, 0xd0, 0x48, 0x89, 0x44, 0x24, 0x20,
                                0x48, 0x83, 0xc4, 0x38, 0xc3};
  for (const unsigned char byte : tail) code[at++] = byte;

  DWORD old_protection = 0;
  BYTE* code_page = base + (nevr_host::kGameMainWrapperRva & ~(kPage - 1));
  if (VirtualProtect(base, kPage, PAGE_READONLY, &old_protection) == FALSE ||
      VirtualProtect(code_page, kPage, PAGE_EXECUTE_READ, &old_protection) == FALSE) {
    return nullptr;
  }
  FlushInstructionCache(GetCurrentProcess(), code, at);
  return base;
}

}  // namespace

extern "C" __declspec(dllexport) DWORD BootstrapProbeNoInstallEvent() {
  return static_cast<DWORD>(g_no_install_event);
}

extern "C" __declspec(dllexport) DWORD BootstrapProbeAttachHoldMs() {
  return static_cast<DWORD>(g_attach_hold_ms);
}

extern "C" __declspec(dllexport) HMODULE BootstrapProbeGameImage() {
  if (g_image == nullptr) g_image = BuildGameImage(g_image_kind);
  return reinterpret_cast<HMODULE>(g_image);
}

namespace {

[[noreturn]] void Fail(const char* message);

NevrGameMainFn Target() {
  return reinterpret_cast<NevrGameMainFn>(g_image + nevr_host::kGameMainWrapperRva);
}

void CallTarget(std::int64_t command_line) {
  BootstrapProbeRecordEvent(kCallIssued, static_cast<std::uint64_t>(command_line));
  if (Target()(command_line) != ExpectedResult(command_line)) {
    BootstrapProbeRecordEvent(kBadResult, static_cast<std::uint64_t>(command_line));
  }
}

const char* EventName(LONG event) {
  static const char* const kNames[kEventLimit] = {
      "?", "host.attach_enter", "host.hook_enabled", "host.hook_failed", "host.attach_exit",
      "runtime.dll_attach", "runtime.get_api", "runtime.initialized", "runtime.game_main_enter",
      "original", "runtime.game_main_exit", "call.issued", "call.bad_result", "main.entered",
      "host.loadlibrary_returned", "runtime.second_hook_enabled", "second.detour",
      "second.original", "host.attach_held", "runtime.hook_target", "runtime.hook_bytes",
      "runtime.hook_mismatch", "host.stop_game"};
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

// Per-thread nesting: game_main enter pushes a frame, original completes the innermost
// open frame (or is a lone original when no frame is open: unhooked/degraded/bypassed),
// game_main exit closes it. Returns the number of lone originals.
LONG CheckFrames(const std::vector<EventRecord>& events) {
  struct Frame {
    std::uint64_t command_line;
    bool original_seen;
  };
  std::map<DWORD, std::vector<Frame>> stacks;
  LONG lone = 0;
  for (const EventRecord& record : events) {
    std::vector<Frame>& stack = stacks[record.thread];
    if (record.event == kRuntimeGameMainEnter) {
      stack.push_back(Frame{record.payload, false});
    } else if (record.event == kOriginal) {
      if (stack.empty()) {
        ++lone;
      } else {
        Require(!stack.back().original_seen, "original ran twice inside one dispatched call");
        Require(stack.back().command_line == record.payload,
                "original saw a different command line than game_main");
        stack.back().original_seen = true;
      }
    } else if (record.event == kRuntimeGameMainExit) {
      Require(!stack.empty(), "game_main exit without an open game_main entry on its thread");
      Require(stack.back().original_seen, "game_main returned before the original ran");
      Require(static_cast<std::int64_t>(record.payload) ==
                  ExpectedResult(static_cast<std::int64_t>(stack.back().command_line)),
              "game_main returned a result other than the original's");
      stack.pop_back();
    }
  }
  for (const auto& entry : stacks) Require(entry.second.empty(), "a dispatched call never left game_main");
  return lone;
}

struct Expectation {
  bool hooked = true;                  // image + prologue accepted, detour enabled
  bool dispatched = true;              // runtime loaded, validated and initialized
  const char* load_record = "loaded";  // the runtime_load record's outcome (hooked scenarios)
  bool static_import = false;
  bool load_may_precede_return = false;  // hammer: a racing thread may trigger the load
  bool no_install_event = false;  // the load cannot wait for the install: records may land after the load's
  bool call_parked_inside_attach = false;  // held attach: a call must be in flight across DllMain's exit
  enum class Lone { kNone, kAtLeastOne, kExactlyOne, kEveryCall } lone = Lone::kNone;
  bool second_hook_called = false;
  LONG min_threads_parked_in_load = 0;  // concurrent-first
  // What the host recorded when it rejected the game (the records the scenario expects).
  const char* rejected_step = "";  // "image_check" or "prologue_check" when not hooked
};

void ValidateHostAttach(const std::vector<EventRecord>& events, const Expectation& expect,
                        std::size_t* attach_exit) {
  Require(Count(events, kHostAttachEnter) == 1 && Count(events, kHostAttachExit) == 1,
          "host attach did not run exactly once");
  const std::size_t enter = First(events, kHostAttachEnter);
  *attach_exit = First(events, kHostAttachExit);
  Require(enter < *attach_exit, "host attach exit recorded before enter");
  Require(events[*attach_exit].payload == 0, "nevr-runtime.dll was loaded before host DllMain returned");
  const BYTE first_target_byte = g_image[nevr_host::kGameMainWrapperRva];
  if (expect.hooked) {
    const std::size_t enabled = First(events, kHostHookEnabled);
    Require(enabled != kNone && Count(events, kHostHookFailed) == 0,
            "host did not install the hook");
    Require(enter < enabled && enabled < *attach_exit, "host attach steps out of order");
    Require(events[enabled].payload == 0xE9 && first_target_byte == 0xE9,
            "rendezvous does not start with a JMP rel32 after enable (partial install?)");
  } else {
    Require(Count(events, kHostHookEnabled) == 0 && Count(events, kHostHookFailed) == 1,
            "host hooked a rejected image, or did not leave the game unhooked");
    Require(first_target_byte == nevr_host::kGameMainWrapperPrologue[0],
            "rejected target bytes were modified");
  }
}

// Index of the first kOriginal after `from` on `thread`, or kNone.
std::size_t NextOriginalOnThread(const std::vector<EventRecord>& events, std::size_t from, DWORD thread) {
  for (std::size_t index = from + 1; index < events.size(); ++index) {
    if (events[index].event == kOriginal && events[index].thread == thread) return index;
  }
  return kNone;
}

void ValidateNoEarlyRuntime(const std::vector<EventRecord>& events, const Expectation& expect,
                            std::size_t attach_exit) {
  const std::size_t returned =
      First(events, expect.static_import ? kMainEntered : kHostLoadReturned);
  Require(returned != kNone && attach_exit < returned,
          "attach return was not observed after host attach exit");
  // Everything below is the load itself or code inside nevr-runtime.dll, and must follow attach exit.
  const LONG runtime_events[] = {kRuntimeDllAttach, kRuntimeGetApi, kRuntimeInitialized,
                                 kRuntimeGameMainEnter, kRuntimeGameMainExit,
                                 kRuntimeSecondHookEnabled};
  if (expect.call_parked_inside_attach) {
    // A thread called the wrapper after the hook was live and before DllMain returned: its call
    // entered the detour, which parks in the runtime load (LoadLibrary needs the loader lock
    // DllMain holds) until attach returns. The original therefore ran after attach exit.
    bool parked = false;
    for (std::size_t index = 0; index < attach_exit && !parked; ++index) {
      if (events[index].event != kCallIssued) continue;
      const std::size_t original = NextOriginalOnThread(events, index, events[index].thread);
      parked = original != kNone && original > attach_exit;
    }
    Require(parked, "held attach: no call was in flight inside the detour when host DllMain returned");
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

// The host's own records for this run: parsed, each with the NEVR.HOST header.
std::vector<nlohmann::json> ReadHostRecords() {
  wchar_t local[MAX_PATH] = {};
  const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
  Require(length != 0 && length < MAX_PATH, "the scenario's LOCALAPPDATA is not set");
  const std::wstring path = std::wstring(local, length) + L"\\nevr\\echovr\\logs\\nevr-bootstrap.jsonl";
  HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  std::string text;
  if (file != INVALID_HANDLE_VALUE) {
    char buffer[512] = {};
    DWORD read = 0;
    while (ReadFile(file, buffer, sizeof(buffer), &read, nullptr) != FALSE && read != 0) {
      text.append(buffer, read);
    }
    CloseHandle(file);
  }
  std::vector<nlohmann::json> records;
  std::size_t start = 0;
  while (start < text.size()) {
    const std::size_t end = text.find('\n', start);
    Require(end != std::string::npos, "a host record is missing its terminating newline");
    nlohmann::json record = nlohmann::json::parse(text.substr(start, end - start), nullptr, false);
    Require(!record.is_discarded(), "a host record does not parse as JSON");
    Require(record.value("tag", "") == "NEVR.HOST", "a host record lacks the NEVR.HOST tag");
    records.push_back(std::move(record));
    start = end + 1;
  }
  return records;
}

std::size_t CountRecords(const std::vector<nlohmann::json>& records, const char* event, const char* step) {
  std::size_t count = 0;
  for (const nlohmann::json& record : records) {
    if (record.value("event", "") != event) continue;
    if (step[0] != '\0' && record.value("step", "") != step) continue;
    ++count;
  }
  return count;
}

void ValidateHostRecords(const Expectation& expect) {
  // The attach decisions of a run that did not hook reach the file through the crash-reporter
  // constructor's trigger; a hooked run reports them at its first call, before the load.
  if (!expect.hooked) {
    const auto flush = reinterpret_cast<void (*)()>(
        GetProcAddress(GetModuleHandleW(L"BugSplat64.dll"), "nevr_host_test_FlushDeferred"));
    Require(flush != nullptr, "the host shim's deferred flush did not resolve");
    const auto wait = reinterpret_cast<int (*)()>(
        GetProcAddress(GetModuleHandleW(L"BugSplat64.dll"), "nevr_host_test_WaitFailureFlush"));
    Require(wait != nullptr && wait() == 1, "the host's failure flush did not finish");
    const auto on_attach_thread = reinterpret_cast<std::size_t (*)()>(
        GetProcAddress(GetModuleHandleW(L"BugSplat64.dll"), "nevr_host_test_FlushesOnAttachThread"));
    Require(on_attach_thread != nullptr, "the host shim's attach-thread flush counter did not resolve");
    Require(on_attach_thread() == 0,
            "the journal was flushed on the thread inside DllMain attach (a file cannot be written under the loader lock)");
    flush();
  }
  const std::vector<nlohmann::json> records = ReadHostRecords();
  // One install record when hooked, none when the game was rejected: a second install attempt
  // or an install over a rejected image would show here.
  Require(CountRecords(records, "host_attach", "hook_install") == (expect.hooked ? 1u : 0u),
          "the host did not record exactly one hook install when hooked, or recorded one when not");
  if (!expect.hooked) {
    Require(CountRecords(records, "runtime_load", "") == 0, "a rejected run loaded the runtime");
    // The rejection, then the one host_not_attached record that says the game runs without the runtime (#1252).
    Require(records.size() >= 2 && records.back().value("event", "") == "host_not_attached" &&
                records.back().value("stopped", true) == false,
            "the host's last record is not the host_not_attached record that follows the rejection");
    const nlohmann::json& rejection = records[records.size() - 2];
    Require(rejection.value("step", "") == expect.rejected_step &&
                (rejection.value("outcome", "") == "rejected" || rejection.value("outcome", "") == "mismatch"),
            "the host's record before host_not_attached is not the rejection the scenario expects");
    if (std::strcmp(expect.rejected_step, "prologue_check") == 0) {
      Require(rejection.at("rva") == "0xcd510" &&
                  rejection.at("expected_prologue") == "4883ec3848c744242000000000" &&
                  rejection.at("actual_prologue") == "4883ec3848c744242800000000",
              "the prologue mismatch record lacks the rva, the pinned bytes or the bytes found");
    }
    return;
  }
  // A hooked run whose runtime did not load stops the game: two more records, the game_stop and then the
  // console_notice that says whether a console took the plain line (#1533), however many calls arrived.
  const bool stops = !expect.dispatched;
  Require(CountRecords(records, "versions", "") == 1, "a hooked run did not record exactly one versions record");
  Require(records.size() == (stops ? 8u : 6u),
          "a hooked run recorded something other than its six decisions (and the stop and its console notice, "
          "when it stopped)");
  if (expect.no_install_event) {
    // The load did not wait for the install, so the install record can land after the load's: all of them
    // must still be there (the load flushes the journal again), in whatever order the race gave.
    Require(CountRecords(records, "host_attach", "image_check") == 1 &&
                CountRecords(records, "host_attach", "prologue_check") == 1 &&
                CountRecords(records, "host_attach", "hook_install") == 1 &&
                CountRecords(records, "runtime_load", "") == 1,
            "a run without an install event lost a decision");
    bool unavailable = false;
    for (const nlohmann::json& record : records) {
      unavailable = unavailable || (record.value("step", "") == "install_wait" &&
                                    record.value("outcome", "") == "unavailable" &&
                                    record.value("next", "") ==
                                        "load_proceeds_unsynchronized_and_the_journal_is_flushed_again_after_load");
    }
    Require(unavailable, "the missing install event was not recorded as unavailable with its next step");
    for (const nlohmann::json& record : records) {
      if (record.value("event", "") != "runtime_load") continue;
      Require(record.value("outcome", "") == expect.load_record && record.at("loader_lock_held") == false,
              "the runtime_load record of a run without an install event is wrong");
    }
    return;
  }
  Require(records[0].value("step", "") == "image_check" && records[0].value("outcome", "") == "accepted" &&
              records[1].value("step", "") == "prologue_check" &&
              records[1].value("outcome", "") == "accepted" &&
              records[2].value("step", "") == "hook_install" && records[2].value("outcome", "") == "enabled" &&
              records[3].value("step", "") == "install_wait" && records[3].value("outcome", "") == "signalled" &&
              records[4].value("event", "") == "versions" && records[5].value("event", "") == "runtime_load",
          "the host's decisions were not recorded in order (the versions record sits between the install wait and the load)");
  Require(!records[2].contains("hook_bytes"), "the install record says the hook bytes were unread");
  Require(records[2].at("rva") == "0xcd510" && records[2].at("stage") == "enable" &&
              records[2].at("mh_status") == 0 &&
              records[2].at("actual_prologue") == records[2].at("expected_prologue"),
          "the install record lacks the rva, the enable stage, MH_OK or the matching bytes");
  Require(records[5].value("outcome", "") == expect.load_record,
          "the runtime_load record has an unexpected outcome");
  Require(records[5].contains("loader_lock_held") && records[5].at("loader_lock_held") == false,
          "the runtime_load record does not say the loader lock was not held");
  Require(records[5].value("degraded_action", "") == (stops ? "stop_game" : "dispatch"),
          "the runtime_load record names the wrong action");
  if (stops) {
    Require(records[6].value("event", "") == "game_stop" && records[6].value("reason", "") == expect.load_record &&
                records[6].value("exit_code", 0) == 78,
            "the game_stop record does not follow the load and name its cause and exit code 78");
    Require(records[7].value("event", "") == "console_notice" && records[7].value("reason", "") == expect.load_record &&
                records[7].contains("written") && records[7].at("written").is_boolean(),
            "the console_notice record does not follow the game_stop and name its cause and whether a console took the line");
  }
}

void ValidateDispatch(const std::vector<EventRecord>& events, const Expectation& expect) {
  const LONG calls = Count(events, kCallIssued);
  Require(calls > 0, "scenario issued no calls");
  Require(Count(events, kOriginal) == calls, "original did not run exactly once per call");
  Require(Count(events, kBadResult) == 0, "a caller received a result other than the original's");
  // A lone original is a call that reached the original with no game_main frame open:
  // unhooked (before attach / rejected image), degraded, or the bypassed same-thread
  // re-entry during runtime load.
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
  // The runtime never hooks the wrapper: whenever its initialize ran, the host's record of
  // its hook still equals the live bytes (checked by the fixture after initialize and in game_main).
  Require(Count(events, kRuntimeHookMismatch) == 0,
          "the host's hook record differs from the bytes at the wrapper: something else hooked it");

  Require(expect.hooked || Count(events, kHostStopGame) == 0, "the host stopped a game it never hooked");
  Require(!expect.dispatched || Count(events, kHostStopGame) == 0, "the host stopped a game whose runtime loaded");
  if (!expect.hooked) {
    Require(Count(events, kRuntimeDllAttach) == 0 && GetModuleHandleW(L"nevr-runtime.dll") == nullptr,
            "runtime was loaded although the host rejected the image");
    Require(Count(events, kRuntimeGameMainEnter) == 0, "dispatch without a hook");
    return;
  }
  if (!expect.dispatched) {
    Require(Count(events, kRuntimeGameMainEnter) == 0 && Count(events, kRuntimeGameMainExit) == 0,
            "a failed runtime was dispatched");
    // Every call into the wrapper met the stop, with the stable exit code; none was run as the game.
    Require(Count(events, kHostStopGame) == calls, "a call into the wrapper did not meet the host's stop");
    Require(events[First(events, kHostStopGame)].payload == 78, "the host stopped the game with the wrong exit code");
    return;
  }
  Require(Count(events, kRuntimeDllAttach) == 1 && Count(events, kRuntimeGetApi) == 1 &&
              Count(events, kRuntimeInitialized) == 1,
          "runtime DllMain/GetApi/initialize did not run exactly once");
  const std::size_t initialized = First(events, kRuntimeInitialized);
  const std::uint64_t trampoline = events[initialized].payload;
  Require(trampoline != 0 &&
              trampoline != reinterpret_cast<std::uintptr_t>(g_image + nevr_host::kGameMainWrapperRva),
          "host context did not carry a trampoline distinct from the hooked target");
  Require(Count(events, kRuntimeHookTarget) == 1 && Count(events, kRuntimeHookBytes) == 1 &&
              events[First(events, kRuntimeHookTarget)].payload ==
                  reinterpret_cast<std::uintptr_t>(g_image + nevr_host::kGameMainWrapperRva) &&
              (events[First(events, kRuntimeHookBytes)].payload & 0xFF) == 0xE9,
          "the host context's hook record is not the wrapper's address and the host's JMP");
  Require(First(events, kRuntimeGameMainEnter) > initialized,
          "game_main began before runtime initialization");
  Require(Count(events, kRuntimeGameMainEnter) > 0 &&
              Count(events, kRuntimeGameMainEnter) == Count(events, kRuntimeGameMainExit),
          "game_main enter/exit counts differ");
  Require(Count(events, kRuntimeSecondHookEnabled) == 1, "runtime's disjoint MinHook instance did not install");
  if (expect.second_hook_called) {
    Require(Count(events, kSecondDetour) == 1 && Count(events, kSecondOriginal) == 1,
            "runtime's disjoint hook did not run exactly once");
  }
  if (expect.min_threads_parked_in_load > 0) {
    std::map<DWORD, bool> parked;
    const std::size_t first_dispatch = First(events, kRuntimeGameMainEnter);
    for (std::size_t index = 0; index < first_dispatch; ++index) {
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
  ValidateHostRecords(expect);
  PrintTimeline(events, label);
  std::printf("bootstrap attach test: PASS: %s (calls=%ld originals=%ld dispatched=%ld threads=%llu)\n",
              label, static_cast<long>(Count(events, kCallIssued)),
              static_cast<long>(Count(events, kOriginal)),
              static_cast<long>(Count(events, kRuntimeGameMainEnter)),
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
    CallTarget(((static_cast<std::int64_t>(args->worker) + 1) << 24) | (static_cast<std::int64_t>(call) << 4));
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
  Require(GetModuleHandleW(L"nevr-runtime.dll") == nullptr, "nevr-runtime.dll loaded before main");
  Require(g_image != nullptr, "host never requested the game image during static-import attach");
  DetoursExportPlaceholder();  // the import that makes the loader attach the host first
  CallTarget(kCommandLine);  // main enters the wrapper
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

// Several threads, so MinHook has more than one to freeze and resume: the first one resumed runs
// the new jump while the others are still being resumed, which is the window a trampoline or
// host context published after MH_EnableHook would be read in.
constexpr int kHammerThreads = 8;

// Calls the rendezvous continuously across the host's attach: before the hook
// exists, while MinHook freezes threads and writes the JMP, and after.
DWORD WINAPI HammerThread(void* raw) {
  const auto index = static_cast<std::int64_t>(reinterpret_cast<std::uintptr_t>(raw));
  std::int64_t call = 0;
  while (InterlockedCompareExchange(&g_stop_hammer, 0, 0) == 0) {
    CallTarget(0x0F00'0000'0000'0000 | (index << 40) | ((++call & 0xFFFFF) << 4));
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
  if (mode == "missing") {
    expect.dispatched = false;
    expect.load_record = "runtime_absent";
  } else if (mode == "bad-image") {
    expect.dispatched = false;
    expect.load_record = "load_failed";
  } else if (mode == "bad-version") {
    expect.dispatched = false;
    expect.load_record = "abi_rejected";
  } else if (mode == "missing-entry") {
    expect.dispatched = false;
    expect.load_record = "missing_entry";
  } else if (mode == "init-failure") {
    expect.dispatched = false;
    expect.load_record = "init_failed";
  } else if (mode == "bad-timestamp" || mode == "bad-prologue") {
    expect.hooked = false;
    expect.dispatched = false;
    image_kind = mode == "bad-timestamp" ? kImageWrongTimestamp : kImageWrongPrologue;
    expect.rejected_step = mode == "bad-timestamp" ? "image_check" : "prologue_check";
  } else if (mode == "exit-at-once") {
    // The failed attach's journal must reach the disk although the process ends before the flush thread runs.
    expect.hooked = false;
    expect.dispatched = false;
    image_kind = kImageWrongPrologue;
    expect.rejected_step = "prologue_check";
  } else if (mode == "reentrant-init") {
    expect.lone = Expectation::Lone::kExactlyOne;
  } else if (mode == "concurrent-first") {
    expect.min_threads_parked_in_load = 2;
  } else if (mode == "hammer-attach" || mode == "hammer-held-attach" || mode == "hammer-no-install-event") {
    expect.load_may_precede_return = true;
    if (mode == "hammer-no-install-event") {
      expect.no_install_event = true;
      InterlockedExchange(&g_no_install_event, 1);
    }
    expect.lone = Expectation::Lone::kAtLeastOne;
    if (mode == "hammer-held-attach") {
      expect.call_parked_inside_attach = true;
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
  std::vector<HANDLE> hammers;
  if (mode == "hammer-attach" || mode == "hammer-held-attach" || mode == "hammer-no-install-event") {
    for (int index = 0; index < kHammerThreads; ++index) {
      HANDLE thread = CreateThread(nullptr, 0, &HammerThread,
                                   reinterpret_cast<void*>(static_cast<std::uintptr_t>(index)), 0, nullptr);
      Require(thread != nullptr, "could not create hammer thread");
      hammers.push_back(thread);
    }
  }
  Sleep(30);
  const std::wstring host_path = ExecutableDirectory() + L"BugSplat64.dll";
  HMODULE host = LoadLibraryW(host_path.c_str());
  Require(host != nullptr, "probe host failed to load");
  BootstrapProbeRecordEvent(kHostLoadReturned, 0);
  InterlockedExchange(&g_host_loaded, 1);
  if (mode == "exit-at-once") return 0;  // the process ends now: no wait for the flush thread, no deferred flush

  if (!hammers.empty()) {
    for (int wait = 0; wait < 500 && InterlockedCompareExchange(&g_hammer_calls_after_load, 0, 0) < 200; ++wait) {
      Sleep(10);
    }
    InterlockedExchange(&g_stop_hammer, 1);
    Require(WaitForMultipleObjects(static_cast<DWORD>(hammers.size()), hammers.data(), TRUE, 10000) ==
                WAIT_OBJECT_0,
            "hammer threads did not stop");
    for (HANDLE thread : hammers) CloseHandle(thread);
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
    CallTarget(kCommandLine);
  } else {
    CallTarget(kCommandLine);
    RunWorkers(4, mode == "valid" || mode == "hammer-attach" || mode == "hammer-held-attach" || mode == "hammer-no-install-event" ? 100 : 25,
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
  const std::wstring runtime = root + L"nevr-runtime.dll";
  if (fixture == nullptr) {
    DeleteFileW(runtime.c_str());
    Require(GetFileAttributesW(runtime.c_str()) == INVALID_FILE_ATTRIBUTES, "could not remove nevr-runtime.dll");
    return;
  }
  const std::wstring source = root + L"fixtures\\" + fixture + L"\\nevr-runtime.dll";
  if (!CopyFileW(source.c_str(), runtime.c_str(), FALSE)) {
    std::fprintf(stderr, "CopyFileW %ls failed, Win32 error=%lu\n", source.c_str(),
                 static_cast<unsigned long>(GetLastError()));
    Fail("could not stage runtime fixture");
  }
}

// `count` copies of one child at once, all sharing this process's environment (so one log directory and one named
// log mutex), each waited for.
// Returns the pid of each child, so a short count can name the children that left no record.
std::vector<DWORD> RunChildrenAtOnce(const std::wstring& command_line, int count, const char* label) {
  STARTUPINFOW startup = {};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
  startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
  // The twelve processes measure the durability of the host log under contention, not the console: each failed attach
  // also writes its one NEVR.STOP line (#1252) to standard error, and twelve writers on one shared pipe slow every
  // process's last flush past its bounded wait (measured: the prologue record was lost in 4 of 5 runs with the shared
  // pipe, 2 of 8 without; the stop line itself is checked by the rendezvous test). Their standard error is NUL.
  SECURITY_ATTRIBUTES inheritable = {sizeof(inheritable), nullptr, TRUE};
  HANDLE null_error = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &inheritable, OPEN_EXISTING,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
  Require(null_error != INVALID_HANDLE_VALUE, "could not open NUL for the children's standard error");
  startup.hStdError = null_error;
  for (HANDLE handle : {startup.hStdOutput}) {
    if (handle != nullptr && handle != INVALID_HANDLE_VALUE) {
      SetHandleInformation(handle, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
    }
  }
  std::vector<HANDLE> processes;
  std::vector<DWORD> pids;
  std::fflush(stdout);
  std::fflush(stderr);
  for (int at = 0; at < count; ++at) {
    std::wstring command = command_line;
    PROCESS_INFORMATION process = {};
    Require(CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &startup,
                           &process) != FALSE,
            "could not launch a scenario child");
    CloseHandle(process.hThread);
    processes.push_back(process.hProcess);
    pids.push_back(process.dwProcessId);
  }
  CloseHandle(null_error);
  bool failed = false;
  for (HANDLE process : processes) {
    if (WaitForSingleObject(process, 30000) != WAIT_OBJECT_0) {
      TerminateProcess(process, 2);
      failed = true;
    }
    DWORD exit_code = 1;
    GetExitCodeProcess(process, &exit_code);
    CloseHandle(process);
    if (exit_code != 0) {
      std::fprintf(stderr, "scenario %s: a child exited 0x%lx\n", label, static_cast<unsigned long>(exit_code));
      failed = true;
    }
  }
  if (failed) Fail("a scenario child failed or timed out");
  return pids;
}

// Every child in `pids` must have left its own record of `step`. A short count names the pids that left none, so a
// failure says which processes lost their record and not only that one did.
void RequireRecordFromEachPid(const std::vector<nlohmann::json>& records, const std::vector<DWORD>& pids,
                              const char* step, const char* message) {
  std::vector<unsigned long long> missing;
  for (const DWORD pid : pids) {
    bool found = false;
    for (const nlohmann::json& record : records) {
      found = found || (record.value("event", "") == "host_attach" && record.value("step", "") == step &&
                        record.value("pid", 0ull) == static_cast<unsigned long long>(pid));
    }
    if (!found) missing.push_back(pid);
  }
  if (missing.empty()) return;
  std::fprintf(stderr, "bootstrap attach test: step %s has no record from pid(s):", step);
  for (const unsigned long long pid : missing) std::fprintf(stderr, " %llu", pid);
  std::fprintf(stderr, " (of %zu children)\n", pids.size());
  Fail(message);
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
      {"hammer-no-install-event", L"good"},
      {"reentrant-init", L"reenter-init"},
      {"bad-timestamp", L"good"},
      {"bad-prologue", L"good"},
  };
  // Each child gets its own LOCALAPPDATA and TEMP, so the host's bootstrap records of one
  // scenario are the only records the child reads.
  wchar_t temp_base[MAX_PATH] = {};
  const DWORD temp_len = GetTempPathW(MAX_PATH, temp_base);
  Require(temp_len != 0 && temp_len < MAX_PATH, "no temp directory available");
  const std::wstring base = std::wstring(temp_base, temp_len) + L"nevr-attach-" +
                            std::to_wstring(GetCurrentProcessId()) + L"-" +
                            std::to_wstring(GetTickCount64());
  CreateDirectoryW(base.c_str(), nullptr);
  const auto use_scenario_directory = [&base](const std::wstring& name) {
    const std::wstring directory = base + L"\\" + name;
    CreateDirectoryW(directory.c_str(), nullptr);
    SetEnvironmentVariableW(L"LOCALAPPDATA", directory.c_str());
    SetEnvironmentVariableW(L"TEMP", directory.c_str());
  };
  for (const Scenario& scenario : scenarios) {
    StageRuntimeFixture(root, scenario.fixture);
    std::wstring mode(scenario.mode, scenario.mode + std::strlen(scenario.mode));
    use_scenario_directory(mode);
    RunChild(L"\"" + std::wstring(self, length) + L"\" --child " + mode, scenario.mode);
  }
  // A process that ends right after a failed attach: its decisive record (the prologue mismatch) must be on disk
  // every time, because the flush thread may not have run, or may be killed mid-write (#804).
  constexpr int kExitAtOnceRuns = 12;
  StageRuntimeFixture(root, L"good");
  for (int run = 0; run < kExitAtOnceRuns; ++run) {
    use_scenario_directory(L"exit-at-once-" + std::to_wstring(run));
    RunChild(L"\"" + std::wstring(self, length) + L"\" --child exit-at-once", "exit-at-once");
    const std::vector<nlohmann::json> records = ReadHostRecords();
    Require(CountRecords(records, "host_attach", "prologue_check") == 1,
            "a process that ended at once after a failed attach lost the prologue mismatch record");
    Require(CountRecords(records, "host_attach", "image_check") == 1,
            "a process that ended at once after a failed attach lost the image check record");
    std::set<unsigned long long> seqs;
    for (const nlohmann::json& record : records) {
      if (record.value("event", "") != "host_attach") continue;
      Require(record.contains("seq"), "a host_attach record lacks its seq");
      Require(seqs.insert(record.at("seq").get<unsigned long long>()).second,
              "a process that ended at once wrote a journal step twice");
    }
  }
  // Twelve of them at once into one log, under one named log mutex: a live process may hold the lock the others wait
  // for, so the last flush waits (bounded) and never writes without it. Every process leaves both its records, every
  // line parses (a blank line would fail the read), and no (pid, seq) appears twice.
  constexpr int kConcurrentRuns = 12;
  for (int round = 0; round < 3; ++round) {
    use_scenario_directory(L"exit-at-once-together-" + std::to_wstring(round));
    const std::vector<DWORD> pids =
        RunChildrenAtOnce(L"\"" + std::wstring(self, length) + L"\" --child exit-at-once", kConcurrentRuns,
                          "exit-at-once-together");
    const std::vector<nlohmann::json> records = ReadHostRecords();
    RequireRecordFromEachPid(records, pids, "prologue_check",
                             "twelve processes that ended at once did not each leave the prologue mismatch record");
    RequireRecordFromEachPid(records, pids, "image_check",
                             "twelve processes that ended at once did not each leave the image check record");
    std::set<std::pair<unsigned long long, unsigned long long>> keys;
    for (const nlohmann::json& record : records) {
      if (record.value("event", "") != "host_attach") continue;
      Require(record.contains("pid") && record.contains("seq"), "a host_attach record lacks its pid or seq");
      const bool fresh = keys.insert({record.at("pid").get<unsigned long long>(), record.at("seq").get<unsigned long long>()})
                             .second;
      Require(fresh, "a (pid, seq) appears twice in the host log");
    }
  }
  StageRuntimeFixture(root, L"good");
  use_scenario_directory(L"static-import");
  RunChild(L"\"" + root + L"test_bootstrap_attach_static.exe\"", "static-import");
  std::printf("bootstrap attach test: PASS: all %llu dynamic scenarios + %d exit-at-once runs + static-import\n",
              static_cast<unsigned long long>(sizeof(scenarios) / sizeof(scenarios[0])), kExitAtOnceRuns);
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc == 3 && std::strcmp(argv[1], "--child") == 0) return RunChildScenario(argv[2]);
  if (argc != 1) Fail("usage: test_bootstrap_attach.exe [--child <scenario>]");
  return RunSuite();
}

#endif
