// Early-rendezvous harness (docs/adr/0008-bugsplat-host-and-nevr-runtime.md,
// "Decision: the rendezvous is the game's main wrapper").
//
// This executable stands in for echovr.exe. It exports an event recorder and a
// synthetic game image: a VirtualAlloc'd PE32+ header block with the supported
// TimeDateStamp and, at RVA 0xCD510, a function that begins with the REAL
// 13-byte GameMainWrapper prologue and then calls a C body that records the
// call and returns ExpectedResult(command_line). The host shim (BugSplat64.dll)
// validates and MinHooks it from DLL_PROCESS_ATTACH, then on the first call loads
// the sibling nevr-runtime.dll, calls its initialize and returns its game_main.
//
// Each child validates, from the recorded events:
//   - host attach installs the hook (or, on a bad prologue, leaves the game
//     unhooked) and never loads nevr-runtime.dll before attach returns;
//   - valid: the runtime's initialize ran once, then game_main ran once with the
//     exact argument, and the caller got game_main's exact result;
//   - missing: the host stopped the game once with exit code 78 (its test seam records the stop and lets the
//     original wrapper run, once, with its exact result) and wrote the game_stop record;
//   - missing-real-stop: the same without the seam: the process ends with exit code 78 inside the call;
//   - every way the host stops the game, or leaves it running without the runtime, writes ONE NEVR.STOP line to the
//     child's standard error (#1252): the parent reads it from a file and checks the fields, their order, the version
//     against the host file's own NEVR-BUILD literal, and the log path against the host log it names;
//   - reenter: a same-thread re-entry during initialize bypassed to the original
//     (one lone original) and the call that followed was dispatched;
//   - the host context carried the wrapper's trampoline and the hook record: the
//     target and the first bytes found there after the host enabled its hook;
//   - the host's records are the four decisions, in order, with the target's rva,
//     prologue, MinHook stage and status, and runtime_load saying the loader lock
//     was not held.

#include "runtime/tests/rendezvous/abi.h"

#include "runtime/host/game_targets.h"

#include <nlohmann/json.hpp>

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

using namespace nevr_rendezvous_probe;

namespace {

struct EventRecord {
  LONG event;
  DWORD thread;
  LONGLONG qpc;
  std::uint64_t payload;
};

constexpr LONG kEventCapacity = 1 << 16;
EventRecord g_events[kEventCapacity];
volatile LONG g_event_count = 0;
volatile LONG g_event_overflow = 0;

bool g_wrong_prologue = false;
bool g_wrong_image = false;       // an image the host refuses: not the supported build
bool g_fail_hook_create = false;  // MH_CreateHook "fails" in the host
bool g_real_stop = false;         // the host really ends the process when the runtime did not load
bool g_verify_runtime = false;    // the host verifies nevr-runtime.dll with the production pins (the fixture is unsigned)
std::wstring g_runtime_dir;       // the folder the host looks for the runtime in; empty: the shim's own folder
int g_launcher_mode = 0;
// A value that needs all 64 bits: a truncated argument or result changes it.
constexpr std::int64_t kCommandLine = 0x1234'5678'9ABC'DEF0;
BYTE* g_image = nullptr;
constexpr SIZE_T kImageSize = 0x118000;
constexpr SIZE_T kPage = 0x1000;

std::wstring ExecutableDirectory();
std::wstring Utf8ToWide(const char* text);

}  // namespace

extern "C" __declspec(dllexport) void RendezvousProbeRecordEvent(LONG event,
                                                                 std::uint64_t payload) {
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
extern "C" std::int64_t RendezvousProbeOriginalBody(std::int64_t command_line) {
  RendezvousProbeRecordEvent(kOriginal, static_cast<std::uint64_t>(command_line));
  return ExpectedResult(command_line);
}

namespace {

// Writes the synthetic image (kernel32 only, byte loops) whose
// GameMainWrapper starts with the real prologue and then calls the C body.
BYTE* BuildGameImage(bool wrong_prologue) {
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
  nt->FileHeader.TimeDateStamp = g_wrong_image ? 0x6452dff7 : 0x6452dff6;
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
  if (wrong_prologue) code[8] = 0x28;  // mutate the store offset (0x20 -> 0x28): still a harmless store
  const unsigned char mid[] = {0x48, 0xb8};
  for (const unsigned char byte : mid) code[at++] = byte;
  const std::uintptr_t body = reinterpret_cast<std::uintptr_t>(&RendezvousProbeOriginalBody);
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

extern "C" __declspec(dllexport) HMODULE RendezvousProbeGameImage() {
  if (g_image == nullptr) g_image = BuildGameImage(g_wrong_prologue);
  return reinterpret_cast<HMODULE>(g_image);
}

extern "C" __declspec(dllexport) int RendezvousProbeLauncherMode() {
  return g_launcher_mode;
}

extern "C" __declspec(dllexport) int RendezvousProbeFailHookCreate() {
  return g_fail_hook_create ? 1 : 0;
}

extern "C" __declspec(dllexport) int RendezvousProbeRealStop() {
  return g_real_stop ? 1 : 0;
}

extern "C" __declspec(dllexport) int RendezvousProbeVerifyRuntime() {
  return g_verify_runtime ? 1 : 0;
}

extern "C" __declspec(dllexport) const wchar_t* RendezvousProbeRuntimeDir() {
  return g_runtime_dir.empty() ? nullptr : g_runtime_dir.c_str();
}

namespace {

[[noreturn]] void Fail(const char* message);

NevrGameMainFn Target() {
  return reinterpret_cast<NevrGameMainFn>(g_image + nevr_host::kGameMainWrapperRva);
}

void CallTarget(std::int64_t command_line) {
  RendezvousProbeRecordEvent(kCallIssued, static_cast<std::uint64_t>(command_line));
  if (Target()(command_line) != ExpectedResult(command_line)) {
    RendezvousProbeRecordEvent(kBadResult, static_cast<std::uint64_t>(command_line));
  }
}

const char* EventName(LONG event) {
  static const char* const kNames[kEventLimit] = {
      "?", "host.attach_enter", "host.hook_enabled", "host.hook_failed",
      "host.attach_exit", "runtime.dll_attach", "runtime.initialized",
      "runtime.game_main_enter", "original", "runtime.game_main_exit",
      "runtime.hook_target", "runtime.hook_bytes", "runtime.hook_mismatch",
      "call.issued", "call.bad_result", "host.stop_game"};
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
  std::printf("rendezvous test: [%s] timeline\n", label);
  for (std::size_t index = 0; index < events.size(); ++index) {
    std::printf("  #%-6llu tid=%-6lu %-28s payload=0x%llx\n",
                static_cast<unsigned long long>(index),
                static_cast<unsigned long>(events[index].thread),
                EventName(events[index].event),
                static_cast<unsigned long long>(events[index].payload));
  }
}

void Fail(const char* message) {
  std::fprintf(stderr, "rendezvous test: FAIL: %s\n", message);
  const LONG count = InterlockedCompareExchange(&g_event_count, 0, 0);
  if (count > 0 && count <= kEventCapacity) {
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

// game_main_enter pushes a frame; original completes the innermost open frame (or is a lone
// original when unhooked/degraded/bypassed); game_main_exit closes it. Returns lone count.
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

void ValidateAttach(const std::vector<EventRecord>& events, bool expect_hooked) {
  Require(Count(events, kHostAttachEnter) == 1 && Count(events, kHostAttachExit) == 1,
          "host attach did not run exactly once");
  const std::size_t enter = First(events, kHostAttachEnter);
  const std::size_t exit = First(events, kHostAttachExit);
  Require(enter < exit, "host attach exit before enter");
  Require(events[exit].payload == 0, "nevr-runtime.dll was loaded before host DllMain returned");
  if (expect_hooked) {
    Require(Count(events, kHostHookEnabled) == 1, "host did not enable the hook");
    Require(events[First(events, kHostHookEnabled)].payload == 0xE9,
            "rendezvous does not start with a JMP rel32 after enable");
  } else {
    Require(Count(events, kHostHookEnabled) == 0 && Count(events, kHostHookFailed) == 1,
            "a rejected image was hooked, or the rejection was not recorded");
  }
}

void ValidateDispatch(const std::vector<EventRecord>& events, bool dispatched, LONG expected_lone,
                      bool expect_stop = false) {
  // The host stops the game exactly when the runtime did not load, and then with the stable exit code.
  Require(Count(events, kHostStopGame) == (expect_stop ? 1 : 0),
          expect_stop ? "the host did not stop the game once for a runtime that did not load"
                      : "the host stopped the game though the runtime loaded or the game was never hooked");
  if (expect_stop) {
    Require(events[First(events, kHostStopGame)].payload == 78, "the host stopped the game with the wrong exit code");
  }
  const LONG calls = Count(events, kCallIssued);
  Require(calls > 0, "scenario issued no calls");
  Require(Count(events, kOriginal) == calls, "original did not run exactly once per call");
  Require(Count(events, kBadResult) == 0, "a caller received a result other than the original's");
  const LONG lone = CheckFrames(events);
  Require(lone == expected_lone, "unexpected number of undispatched original calls");
  if (dispatched) {
    Require(Count(events, kRuntimeDllAttach) == 1 && Count(events, kRuntimeInitialized) == 1,
            "runtime DllMain/initialize did not run exactly once");
    Require(Count(events, kRuntimeGameMainEnter) == calls - lone &&
                Count(events, kRuntimeGameMainExit) == calls - lone,
            "game_main enter/exit counts differ from dispatched calls");
    Require(First(events, kRuntimeInitialized) < First(events, kRuntimeGameMainEnter),
            "game_main ran before initialize");
    const std::uint64_t trampoline = events[First(events, kRuntimeInitialized)].payload;
    Require(trampoline != 0 &&
                trampoline != reinterpret_cast<std::uintptr_t>(g_image + nevr_host::kGameMainWrapperRva),
            "host context did not carry a trampoline distinct from the hooked target");
    // The hook record: the hooked address, its first bytes (the host's JMP), and no drift.
    Require(Count(events, kRuntimeHookTarget) == 1 && Count(events, kRuntimeHookBytes) == 1,
            "the runtime did not receive the hook record exactly once");
    Require(events[First(events, kRuntimeHookTarget)].payload ==
                reinterpret_cast<std::uintptr_t>(g_image + nevr_host::kGameMainWrapperRva),
            "the hook record names an address other than the hooked wrapper");
    Require((events[First(events, kRuntimeHookBytes)].payload & 0xFF) == 0xE9,
            "the hook record does not start with the host's JMP rel32");
    Require(Count(events, kRuntimeHookMismatch) == 0,
            "the hook record differs from the live bytes at the target");
  } else {
    Require(Count(events, kRuntimeGameMainEnter) == 0 && Count(events, kRuntimeGameMainExit) == 0,
            "a failed runtime was dispatched");
  }
}

void RunChildScenario(const std::string& scenario) {
  bool expect_hooked = true;
  bool dispatched = true;
  bool expect_stop = false;
  const LONG calls = 1;

  if (scenario == "bad-prologue") {
    g_wrong_prologue = true;
    expect_hooked = false;
    dispatched = false;
  } else if (scenario == "bad-image" || scenario == "hook-create-fails") {
    g_wrong_image = scenario == "bad-image";
    g_fail_hook_create = scenario == "hook-create-fails";
    expect_hooked = false;
    dispatched = false;
  } else if (scenario == "missing" || scenario == "missing-real-stop" || scenario == "missing-real-stop-server" ||
             scenario == "refused-real-stop" || scenario == "legacy-real-stop") {
    dispatched = false;
    expect_stop = true;
    g_real_stop = scenario != "missing";
    g_verify_runtime = scenario == "refused-real-stop";
    if (scenario == "legacy-real-stop") {
      // The runtime is looked for in an empty folder of its own; the one the parent staged beside the host is the
      // old layout, which the host does not load.
      wchar_t local[MAX_PATH] = {};
      const DWORD local_length = GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
      Require(local_length != 0 && local_length < MAX_PATH, "the scenario's LOCALAPPDATA is not set");
      g_runtime_dir = std::wstring(local, local_length) + L"\\root";
    }
  } else if (scenario != "valid" && scenario != "reenter") {
    Fail("unknown child scenario");
  }
  // reenter: one lone original, the nested same-thread call that bypassed the dispatch.
  const LONG expected_lone = dispatched ? (scenario == "reenter" ? 1 : 0) : calls;

  g_image = BuildGameImage(g_wrong_prologue);
  Require(g_image != nullptr, "could not build the synthetic game image");

  const std::wstring host_path = ExecutableDirectory() + L"BugSplat64.dll";
  HMODULE host = LoadLibraryW(host_path.c_str());
  Require(host != nullptr, "host shim failed to load");

  // A refused image, prologue or hook installs no detour, so there is no first-call trigger, and the game may
  // never construct the crash reporter. The attach itself reports the refusal from a thread; the child only
  // waits for it, so the record the parent reads is the attach's own doing.
  if (!expect_hooked) {
    const auto wait =
        reinterpret_cast<int (*)()>(GetProcAddress(host, "nevr_host_test_WaitFailureFlush"));
    Require(wait != nullptr && wait() == 1, "the attach's failure report did not finish");
  }

  for (LONG call = 0; call < calls; ++call) CallTarget(kCommandLine + (static_cast<std::int64_t>(call) << 4));
  // With the real stop the process ended inside the call, with the stable exit code; getting here is the failure.
  Require(!g_real_stop, "the call returned though the host ends the process when the runtime did not load");

  const std::vector<EventRecord> events = Snapshot();
  ValidateAttach(events, expect_hooked);
  ValidateDispatch(events, dispatched, expected_lone, expect_stop);
  std::printf("rendezvous test: PASS: %s (calls=%ld originals=%ld dispatched=%ld lone=%ld)\n",
              scenario.c_str(), static_cast<long>(Count(events, kCallIssued)),
              static_cast<long>(Count(events, kOriginal)),
              static_cast<long>(Count(events, kRuntimeGameMainEnter)), expected_lone);
}

// --- parent suite -------------------------------------------------------------

std::wstring ExecutableDirectory() {
  wchar_t path[MAX_PATH] = {};
  const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
  Require(length != 0 && length < MAX_PATH, "could not resolve executable path");
  std::wstring full(path, length);
  const std::size_t separator = full.find_last_of(L"\\/");
  Require(separator != std::wstring::npos, "executable path has no directory");
  return full.substr(0, separator + 1);
}

std::wstring Utf8ToWide(const char* text) {
  const int needed = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
  std::wstring out(static_cast<std::size_t>(needed > 0 ? needed - 1 : 0), L'\0');
  if (needed > 0) MultiByteToWideChar(CP_UTF8, 0, text, -1, out.data(), needed);
  return out;
}

void StageRuntimeFixture(const std::wstring& root, const wchar_t* fixture) {
  const std::wstring runtime = root + L"nevr-runtime.dll";
  if (fixture == nullptr) {
    DeleteFileW(runtime.c_str());
    Require(GetFileAttributesW(runtime.c_str()) == INVALID_FILE_ATTRIBUTES, "could not remove nevr-runtime.dll");
    return;
  }
  const std::wstring source = root + L"fixtures\\" + fixture + L"\\nevr-runtime.dll";
  Require(CopyFileW(source.c_str(), runtime.c_str(), FALSE) != FALSE, "could not stage runtime fixture");
}

// `stderr_file`, when not empty, is the file the child's standard error goes to (the NEVR.STOP line is read from it).
void RunChild(const std::wstring& command_line, const char* label, DWORD expected_exit = 0,
              const std::wstring& stderr_file = std::wstring()) {
  std::wstring command = command_line;  // CreateProcessW may mutate the buffer
  STARTUPINFOW startup = {};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
  startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
  startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
  HANDLE captured = INVALID_HANDLE_VALUE;
  if (!stderr_file.empty()) {
    SECURITY_ATTRIBUTES inheritable = {sizeof(inheritable), nullptr, TRUE};
    captured = CreateFileW(stderr_file.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &inheritable,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    Require(captured != INVALID_HANDLE_VALUE, "could not create the file for the child's standard error");
    startup.hStdError = captured;
  }
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
  if (captured != INVALID_HANDLE_VALUE) CloseHandle(captured);  // the child holds its own copy
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
  if (exit_code != expected_exit) {
    std::fprintf(stderr, "scenario %s exited 0x%lx, expected 0x%lx\n", label,
                 static_cast<unsigned long>(exit_code), static_cast<unsigned long>(expected_exit));
    Fail("scenario child returned an unexpected exit code");
  }
}

std::string ReadAll(const wchar_t* path) {
  HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return {};
  std::string out;
  char buffer[512] = {};
  DWORD read = 0;
  while (ReadFile(file, buffer, sizeof(buffer), &read, nullptr) != FALSE && read != 0) {
    out.append(buffer, read);
  }
  CloseHandle(file);
  return out;
}

// --- the host's JSON records ---------------------------------------------------

// Every record in `<dir>\nevr\echovr\logs\nevr-bootstrap.jsonl`, parsed. A line that
// does not parse, or lacks the host header, fails the harness.
std::vector<nlohmann::json> ReadHostRecords(const std::wstring& dir) {
  const std::string text = ReadAll((dir + L"\\nevr\\echovr\\logs\\nevr-bootstrap.jsonl").c_str());
  std::vector<nlohmann::json> records;
  std::size_t start = 0;
  while (start < text.size()) {
    const std::size_t end = text.find('\n', start);
    Require(end != std::string::npos, "a host record is missing its terminating newline");
    nlohmann::json record = nlohmann::json::parse(text.substr(start, end - start), nullptr, false);
    Require(!record.is_discarded(), "a host record does not parse as JSON");
    Require(record.value("tag", "") == "NEVR.HOST" && record.contains("ts") && record.contains("pid"),
            "a host record lacks the NEVR.HOST header");
    records.push_back(std::move(record));
    start = end + 1;
  }
  return records;
}

// The index of the record with this event and step (or outcome), or -1.
int Find(const std::vector<nlohmann::json>& records, const char* event, const char* step_or_empty,
         const char* outcome) {
  for (std::size_t index = 0; index < records.size(); ++index) {
    const nlohmann::json& record = records[index];
    if (record.value("event", "") != event) continue;
    if (step_or_empty[0] != '\0' && record.value("step", "") != step_or_empty) continue;
    if (record.value("outcome", "") != outcome) continue;
    return static_cast<int>(index);
  }
  return -1;
}

// The attach decisions of a hooked run, in order, followed by what the load decided. Each
// target record carries the wrapper's rva and prologue; the install names the MinHook stage
// and status; the load says the loader lock was not held.
void ExpectHookedRecords(const std::vector<nlohmann::json>& records, const char* load_outcome,
                         bool expect_stop = false) {
  const int image = Find(records, "host_attach", "image_check", "accepted");
  const int prologue = Find(records, "host_attach", "prologue_check", "accepted");
  const int hook = Find(records, "host_attach", "hook_install", "enabled");
  const int wait = Find(records, "host_attach", "install_wait", "signalled");
  const int load = Find(records, "runtime_load", "", load_outcome);
  Require(image >= 0 && prologue >= 0 && hook >= 0 && wait >= 0 && load >= 0,
          "a hooked run did not record its image, prologue, hook, install-wait and runtime-load decisions");
  Require(image < prologue && prologue < hook && hook < wait && wait < load,
          "the host's decisions were recorded out of order");
  const bool loaded = std::strcmp(load_outcome, "loaded") == 0;
  const int versions = Find(records, "versions", "", "");
  Require(versions >= 0, "a hooked run recorded no versions record after the verdict on the runtime");
  Require(wait < versions && versions < load, "the versions record is not between the install wait and the runtime load");
  Require(records.size() == (expect_stop ? 8u : 6u),
          "a hooked run recorded something other than its six decisions (and the stop and its console notice, "
          "when it stopped)");
  Require(expect_stop == !loaded, "a run that did not load must stop, and one that loaded must not");
  const nlohmann::json& versions_record = records[static_cast<std::size_t>(versions)];
  Require(versions_record.value("host_sha256", "").size() == 64, "the versions record does not carry the host's sha256");
  if (loaded) {
    Require(versions_record.value("runtime_sha256", "").size() == 64 && versions_record.value("runtime_version", "") == "7.7.7",
            "the versions record does not name the runtime that loaded (its sha256 and its NEVR-BUILD version)");
  } else {
    Require(versions_record.value("runtime_sha256", "") == "unavailable" &&
                versions_record.value("runtime_version", "") == "unavailable",
            "the versions record names a runtime that did not load");
  }
  Require(!records[static_cast<std::size_t>(hook)].contains("hook_bytes"),
          "the install record says the hook bytes were unread");
  for (const int index : {prologue, hook}) {
    const nlohmann::json& found = records[static_cast<std::size_t>(index)];
    Require(found.at("rva") == "0xcd510" && found.at("expected_prologue") == "4883ec3848c744242000000000" &&
                found.at("actual_prologue") == found.at("expected_prologue"),
            "a target record does not carry the rva and the matching bytes");
  }
  const nlohmann::json& install = records[static_cast<std::size_t>(hook)];
  Require(install.at("stage") == "enable" && install.at("mh_status") == 0,
          "the install record does not name the enable stage and MH_OK");
  Require(records[static_cast<std::size_t>(load)].at("degraded_action") ==
              (loaded ? "dispatch" : "stop_game"),
          "the runtime-load record names the wrong degraded action");
  if (expect_stop) {
    // The stop is followed only by the console_notice (#1533); it names the cause, the code and the host, and
    // follows the load record.
    const nlohmann::json& stop = records[records.size() - 2];
    const nlohmann::json& notice = records.back();
    Require(notice.value("event", "") == "console_notice" && notice.value("reason", "") == load_outcome &&
                notice.contains("written") && notice.at("written").is_boolean(),
            "the console_notice record does not follow the game_stop and name its cause and whether a console took "
            "the line");
    Require(stop.value("event", "") == "game_stop" && stop.value("reason", "") == load_outcome &&
                stop.value("exit_code", 0) == 78 && stop.value("host_identity", "").rfind("NEVR-BUILD ", 0) == 0,
            "the game_stop record does not name the cause, exit code 78 and the host's identity");
    Require(stop.contains("runtime_path"), "the game_stop record does not name the path searched");
    Require(static_cast<int>(records.size()) - 2 > load, "the game_stop record does not follow the runtime-load record");
  }
  Require(records[static_cast<std::size_t>(load)].contains("loader_lock_held") &&
              records[static_cast<std::size_t>(load)].at("loader_lock_held") == false,
          "the runtime-load record does not say the loader lock was not held");
}

// --- the NEVR.STOP line (#1252) --------------------------------------------------------------------------------------

std::string WideToUtf8(const std::wstring& wide) {
  if (wide.empty()) return {};
  const int size = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
  std::string out(static_cast<std::size_t>(size), '\0');
  WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), out.data(), size, nullptr, nullptr);
  return out;
}

// The version part of the host file's own `NEVR-BUILD <version> <commit40>` literal, read from the bytes of the file the
// child loaded, not from anything the code under test computes.
std::string HostVersionOfFile(const std::wstring& dll_path) {
  const std::string bytes = ReadAll(dll_path.c_str());
  const std::string marker("\0NEVR-BUILD ", 12);
  const std::size_t at = bytes.find(marker);
  Require(at != std::string::npos, "the host file carries no NEVR-BUILD literal");
  const std::size_t start = at + marker.size();
  const std::size_t end = bytes.find(' ', start);
  Require(end != std::string::npos && end > start, "the host's NEVR-BUILD literal has no version");
  return bytes.substr(start, end - start);
}

struct StopText {
  std::string text;
  nlohmann::json json;
};

// The NEVR.STOP lines of a child's standard error. Anything else in the file (a runtime's own output) is not a line of
// this tag and is left alone; a NEVR.STOP line that does not parse, or that is not newline-terminated, fails the harness.
std::vector<StopText> StopLines(const std::wstring& stderr_file) {
  const std::string text = ReadAll(stderr_file.c_str());
  std::vector<StopText> lines;
  std::size_t start = 0;
  while (start < text.size()) {
    const std::size_t end = text.find('\n', start);
    const std::string line = text.substr(start, end == std::string::npos ? std::string::npos : end - start);
    start = end == std::string::npos ? text.size() : end + 1;
    if (line.rfind("{\"tag\":\"NEVR.STOP\"", 0) != 0) continue;
    Require(end != std::string::npos, "a NEVR.STOP line is missing its terminating newline");
    nlohmann::json parsed = nlohmann::json::parse(line, nullptr, false);
    Require(!parsed.is_discarded(), "a NEVR.STOP line does not parse as JSON");
    lines.push_back(StopText{line, std::move(parsed)});
  }
  return lines;
}

struct StopExpectation {
  bool stopped;
  long long code;                // -1: the line carries no code
  const char* reason;
  const char* detail_prefix;     // the detail starts with this (empty: any)
  const char* record_event;      // the host-log record that carries the same facts
};

// `local` is the child's %LOCALAPPDATA%: the host log is `<local>\nevr\echovr\logs\nevr-bootstrap.jsonl`.
nlohmann::json ExpectStopLine(const std::wstring& stderr_file, const std::wstring& local, const std::wstring& root,
                              const StopExpectation& expected, const char* label) {
  const std::vector<StopText> lines = StopLines(stderr_file);
  if (lines.size() != 1) {
    std::fprintf(stderr, "scenario %s wrote %zu NEVR.STOP lines to standard error\n", label, lines.size());
    Fail("a stop must write exactly one NEVR.STOP line");
  }
  const StopText& line = lines[0];
  const nlohmann::json& json = line.json;

  // The fields in their documented order.
  std::size_t at = 0;
  const char* keys_with_code[] = {"tag", "ts", "pid", "event", "stopped", "code", "reason",
                                  "host_version", "runtime_version", "detail", "log"};
  const char* keys_without_code[] = {"tag", "ts", "pid", "event", "stopped", "reason",
                                     "host_version", "runtime_version", "detail", "log"};
  const char* const* keys = expected.code >= 0 ? keys_with_code : keys_without_code;
  const std::size_t key_count = expected.code >= 0 ? sizeof(keys_with_code) / sizeof(keys_with_code[0])
                                                   : sizeof(keys_without_code) / sizeof(keys_without_code[0]);
  for (std::size_t index = 0; index < key_count; ++index) {
    const std::size_t found = line.text.find(std::string("\"") + keys[index] + "\":", at);
    if (found == std::string::npos) {
      std::fprintf(stderr, "scenario %s: key %s is missing or out of order in: %s\n", label, keys[index], line.text.c_str());
      Fail("the NEVR.STOP line's fields are not the documented ones in the documented order");
    }
    at = found + 1;
  }
  Require(json.size() == key_count, "the NEVR.STOP line carries a field the document does not list");

  Require(json.at("tag") == "NEVR.STOP" && json.at("event") == "stop", "the NEVR.STOP line has the wrong tag or event");
  Require(json.at("ts").is_string() && json.at("ts").get<std::string>().size() == 24, "the NEVR.STOP line has no UTC millisecond stamp");
  Require(json.at("pid").is_number_unsigned() && json.at("pid").get<std::uint64_t>() != 0, "the NEVR.STOP line has no pid");
  Require(json.at("stopped").is_boolean() && json.at("stopped").get<bool>() == expected.stopped,
          "the NEVR.STOP line says the wrong thing about whether the game stopped");
  if (expected.code >= 0) {
    Require(json.at("code").is_number_unsigned() &&
                json.at("code").get<std::uint64_t>() == static_cast<std::uint64_t>(expected.code),
            "the NEVR.STOP line's code is not the exit code, as a number");
  }
  Require(json.at("reason") == expected.reason, "the NEVR.STOP line names the wrong reason");
  Require(json.at("host_version") == HostVersionOfFile(root + L"BugSplat64.dll"),
          "the NEVR.STOP line's host_version is not the version in the host file's NEVR-BUILD literal");
  Require(json.at("runtime_version") == "not loaded", "the NEVR.STOP line states a runtime version for a runtime that was not read");
  const std::string detail = json.at("detail").get<std::string>();
  Require(!detail.empty() && detail.rfind(expected.detail_prefix, 0) == 0, "the NEVR.STOP line's detail is empty or does not say what was expected");
  const std::wstring log_path = local + L"\\nevr\\echovr\\logs\\nevr-bootstrap.jsonl";
  Require(json.at("log") == WideToUtf8(log_path), "the NEVR.STOP line's log is not the host log the facts went to");
  Require(GetFileAttributesW(log_path.c_str()) != INVALID_FILE_ATTRIBUTES, "the host log the line names does not exist");

  // The same facts are in the host log, from the same process.
  const std::vector<nlohmann::json> records = ReadHostRecords(local);
  const nlohmann::json* record = nullptr;
  for (const nlohmann::json& candidate : records) {
    if (candidate.value("event", "") == expected.record_event) record = &candidate;
  }
  Require(record != nullptr, "the host log has no record that carries the facts of the NEVR.STOP line");
  Require(record->value("reason", "") == expected.reason && record->value("detail", "") == detail &&
              record->value("log", "") == json.at("log").get<std::string>() &&
              record->value("host_version", "") == json.at("host_version").get<std::string>() &&
              record->value("runtime_version", "") == "not loaded" && record->contains("stopped") &&
              record->at("stopped").get<bool>() == expected.stopped,
          "the host log record and the NEVR.STOP line disagree");
  Require(record->contains("pid") && record->at("pid") == json.at("pid"), "the log record and the line come from different processes");
  if (expected.code >= 0) {
    Require(record->value("exit_code", 0ll) == expected.code, "the host log record carries another exit code than the line");
  } else {
    Require(!record->contains("exit_code"), "a record of a game that kept running carries an exit code");
  }
  return json;
}

// What a client would have been shown, as the shim wrote it; empty when the child was shown nothing.
std::string StopNoticeOf(const std::wstring& local) { return ReadAll((local + L"\\stop-notice.txt").c_str()); }

// The notice decision a stopped run recorded: the `notice` field of its game_stop record (the console_notice record
// that follows it carries no such field, #1533).
std::string RecordedNotice(const std::vector<nlohmann::json>& records) {
  for (auto it = records.rbegin(); it != records.rend(); ++it) {
    if (it->value("event", "") == "game_stop") return it->value("notice", "");
  }
  return "";
}

void ExpectNoStopLine(const std::wstring& stderr_file, const char* label) {
  if (!StopLines(stderr_file).empty()) {
    std::fprintf(stderr, "scenario %s wrote a NEVR.STOP line though the runtime loaded\n", label);
    Fail("a run that loaded the runtime must write no NEVR.STOP line");
  }
}

int RunSetGameModuleUnsupported() {
  // Call NEVR_SetGameModule with the test executable — an unsupported module. The guard must
  // terminate with the stable code.
  // Launcher mode: the host did not attach to this process image, so NEVR_SetGameModule is the first
  // decision about the game image (after a direct-import attach it is refused instead).
  g_launcher_mode = 1;
  const std::wstring host_path = ExecutableDirectory() + L"BugSplat64.dll";
  HMODULE host = LoadLibraryW(host_path.c_str());
  Require(host != nullptr, "host shim failed to load");
  const auto set_game_module = reinterpret_cast<void (*)(HMODULE)>(
      GetProcAddress(host, "NEVR_SetGameModule"));
  Require(set_game_module != nullptr, "NEVR_SetGameModule export did not resolve");
  set_game_module(GetModuleHandleW(nullptr));
  Fail("NEVR_SetGameModule did not terminate on an unsupported module");
}

int RunSetGameModuleSupported() {
  // Launcher-mode success path: DllMain must NOT install
  // the detour (launcher mode), and NEVR_SetGameModule must do the full
  // Prepare -> Install -> later dispatch itself.
  g_launcher_mode = 1;
  g_image = BuildGameImage(false);
  Require(g_image != nullptr, "could not build the synthetic game image");

  const std::wstring host_path = ExecutableDirectory() + L"BugSplat64.dll";
  HMODULE host = LoadLibraryW(host_path.c_str());
  Require(host != nullptr, "host shim failed to load");
  const auto set_game_module = reinterpret_cast<void (*)(HMODULE)>(
      GetProcAddress(host, "NEVR_SetGameModule"));
  Require(set_game_module != nullptr, "NEVR_SetGameModule export did not resolve");
  set_game_module(reinterpret_cast<HMODULE>(g_image));

  // The detour must now be live: a later call dispatches through the runtime.
  // dispatched=true asserts the runtime loaded, initialized, and ran before/after
  // — impossible unless NEVR_SetGameModule installed the detour (DllMain skipped
  // it in launcher mode).
  CallTarget(kCommandLine);
  const std::vector<EventRecord> events = Snapshot();
  ValidateDispatch(events, true, 0);
  std::printf("rendezvous test: PASS: NEVR_SetGameModule supported (launcher path)\n");
  return 0;
}

// The records the host has written so far to this child's LOCALAPPDATA, as the parent will read them.
std::vector<nlohmann::json> RecordsOnDisk() {
  wchar_t local[MAX_PATH] = {};
  const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
  Require(length != 0 && length < MAX_PATH, "the scenario's LOCALAPPDATA is not set");
  return ReadHostRecords(std::wstring(local, length));
}

int RunSetGameModuleTwice() {
  // A second NEVR_SetGameModule is refused and recorded: it neither prepares the host again nor
  // journals the image check a second time.
  g_launcher_mode = 1;
  g_image = BuildGameImage(false);
  Require(g_image != nullptr, "could not build the synthetic game image");
  const std::wstring host_path = ExecutableDirectory() + L"BugSplat64.dll";
  HMODULE host = LoadLibraryW(host_path.c_str());
  Require(host != nullptr, "host shim failed to load");
  const auto set_game_module = reinterpret_cast<void (*)(HMODULE)>(GetProcAddress(host, "NEVR_SetGameModule"));
  Require(set_game_module != nullptr, "NEVR_SetGameModule export did not resolve");
  set_game_module(reinterpret_cast<HMODULE>(g_image));
  set_game_module(reinterpret_cast<HMODULE>(g_image));
  // The refusal is on disk now, before anything else could flush it: the second call flushes itself.
  Require(Find(RecordsOnDisk(), "host_attach", "set_game_module", "refused") >= 0,
          "a refused NEVR_SetGameModule was not flushed by the call that refused it");
  CallTarget(kCommandLine);
  ValidateDispatch(Snapshot(), true, 0);
  std::printf("rendezvous test: PASS: NEVR_SetGameModule called twice\n");
  return 0;
}

int RunSetGameModuleAfterAttach() {
  // The host attached to this (synthetic) game image from DllMain, so NEVR_SetGameModule has nothing
  // left to decide: it is refused and recorded, and the image check is not journalled again.
  g_image = BuildGameImage(false);
  Require(g_image != nullptr, "could not build the synthetic game image");
  const std::wstring host_path = ExecutableDirectory() + L"BugSplat64.dll";
  HMODULE host = LoadLibraryW(host_path.c_str());
  Require(host != nullptr, "host shim failed to load");
  const auto set_game_module = reinterpret_cast<void (*)(HMODULE)>(GetProcAddress(host, "NEVR_SetGameModule"));
  Require(set_game_module != nullptr, "NEVR_SetGameModule export did not resolve");
  set_game_module(reinterpret_cast<HMODULE>(g_image));
  Require(Find(RecordsOnDisk(), "host_attach", "set_game_module", "refused") >= 0,
          "NEVR_SetGameModule after a direct-import attach was not refused and flushed");
  CallTarget(kCommandLine);
  ValidateDispatch(Snapshot(), true, 0);
  std::printf("rendezvous test: PASS: NEVR_SetGameModule after a direct-import attach\n");
  return 0;
}

int RunSuite() {
  const std::wstring root = ExecutableDirectory();
  wchar_t self[MAX_PATH] = {};
  const DWORD length = GetModuleFileNameW(nullptr, self, MAX_PATH);
  Require(length != 0 && length < MAX_PATH, "could not resolve executable path");

  wchar_t temp_base[MAX_PATH] = {};
  const DWORD temp_len = GetTempPathW(MAX_PATH, temp_base);
  Require(temp_len != 0 && temp_len < MAX_PATH, "no temp directory available");
  const std::wstring base = std::wstring(temp_base, temp_len) + L"nevr-rendezvous-" +
                            std::to_wstring(GetCurrentProcessId()) + L"-" +
                            std::to_wstring(GetTickCount64());
  CreateDirectoryW(base.c_str(), nullptr);

  struct Scenario {
    const char* name;
    const wchar_t* fixture;
  };
  const Scenario scenarios[] = {
      {"valid", L"good"},
      {"missing", nullptr},
      {"reenter", L"reenter-init"},
      {"bad-prologue", L"good"},
      {"bad-image", L"good"},
      {"hook-create-fails", L"good"},
  };

  for (const Scenario& scenario : scenarios) {
    StageRuntimeFixture(root, scenario.fixture);
    const std::wstring scenario_dir = base + L"\\" + Utf8ToWide(scenario.name);
    CreateDirectoryW(scenario_dir.c_str(), nullptr);
    SetEnvironmentVariableW(L"LOCALAPPDATA", scenario_dir.c_str());
    SetEnvironmentVariableW(L"TEMP", scenario_dir.c_str());

    const std::wstring command = L"\"" + std::wstring(self, length) + L"\" --child " +
                                 Utf8ToWide(scenario.name);
    const std::wstring stderr_file = scenario_dir + L"\\stderr.txt";
    RunChild(command, scenario.name, 0, stderr_file);

    const std::vector<nlohmann::json> records = ReadHostRecords(scenario_dir);
    if (std::strcmp(scenario.name, "bad-prologue") == 0) {
      // No detour exists, so the report came from the crash-reporter-constructor
      // trigger (nevr_host_test_FlushDeferred): the image was accepted, the prologue was
      // not, and the bytes found are in the record.
      Require(records.size() == 3, "bad-prologue recorded other than image_check, prologue_check and host_not_attached");
      const int mismatch = Find(records, "host_attach", "prologue_check", "mismatch");
      Require(Find(records, "host_attach", "image_check", "accepted") == 0 && mismatch == 1,
              "bad-prologue did not record an accepted image then a prologue mismatch");
      const nlohmann::json& record = records[1];
      Require(record.at("degraded_action") == "run_original_only", "the mismatch names no degraded action");
      Require(record.at("rva") == "0xcd510", "the mismatch record does not name the wrapper's rva");
      Require(record.at("expected_prologue") == "4883ec3848c744242000000000" &&
                  record.at("actual_prologue") == "4883ec3848c744242800000000",
              "the mismatch record does not carry the expected and the actual bytes");
      // The game keeps running without the runtime: the line says which step failed, and carries no exit code.
      ExpectStopLine(stderr_file, scenario_dir, root,
                     {false, -1, "host_not_attached", "prologue_check rejected", "host_not_attached"}, scenario.name);
    } else if (std::strcmp(scenario.name, "bad-image") == 0) {
      Require(records.size() == 2 && Find(records, "host_attach", "image_check", "rejected") == 0,
              "a refused image was not on disk as the image_check rejected record, then host_not_attached");
      ExpectStopLine(stderr_file, scenario_dir, root,
                     {false, -1, "host_not_attached", "image_check rejected", "host_not_attached"}, scenario.name);
    } else if (std::strcmp(scenario.name, "hook-create-fails") == 0) {
      Require(records.size() == 4, "a refused hook recorded other than image_check, prologue_check, hook_install, host_not_attached");
      const int failed = Find(records, "host_attach", "hook_install", "failed");
      Require(Find(records, "host_attach", "image_check", "accepted") == 0 &&
                  Find(records, "host_attach", "prologue_check", "accepted") == 1 && failed == 2,
              "a failed MH_CreateHook was not on disk as a hook_install failed record after the two accepted checks");
      Require(records[2].at("stage") == "create", "the hook_install failure does not name the create stage");
      ExpectStopLine(stderr_file, scenario_dir, root,
                     {false, -1, "host_not_attached", "hook_install failed", "host_not_attached"}, scenario.name);
    } else if (std::strcmp(scenario.name, "missing") == 0) {
      ExpectHookedRecords(records, "runtime_absent", true);
      // The host's stop seam lets the child go on, so the child's own standard error holds the one line.
      ExpectStopLine(stderr_file, scenario_dir, root,
                     {true, 78, "runtime_absent", "nevr-runtime.dll is missing; path=", "game_stop"}, scenario.name);
      // A client is shown the sentence, then the version and the log (the shim writes the text instead of a dialog).
      const std::string notice = StopNoticeOf(scenario_dir);
      const std::string expected_tail = "\n\nnEVR " + HostVersionOfFile(root + L"BugSplat64.dll") + "\nLog: " +
                                        WideToUtf8(scenario_dir + L"\\nevr\\echovr\\logs\\nevr-bootstrap.jsonl");
      Require(notice.rfind("nEVR couldn't start: nevr-runtime.dll is missing.", 0) == 0 &&
                  notice.size() > expected_tail.size() &&
                  notice.compare(notice.size() - expected_tail.size(), expected_tail.size(), expected_tail) == 0,
              "the client's notice is not the sentence followed by the version and the log path");
    } else {
      ExpectHookedRecords(records, "loaded");
      ExpectNoStopLine(stderr_file, scenario.name);
    }
    std::printf("rendezvous test: [%s] %zu host records parsed and ordered\n", scenario.name,
                records.size());
  }

  // The stop for real: no seam, so the host ends the child inside the first call with the stable exit code, after it
  // wrote the game_stop record. The child never reaches its own checks; the exit code and the record are the proof.
  {
    StageRuntimeFixture(root, nullptr);
    const std::wstring dir = base + L"\\missing-real-stop";
    CreateDirectoryW(dir.c_str(), nullptr);
    SetEnvironmentVariableW(L"LOCALAPPDATA", dir.c_str());
    SetEnvironmentVariableW(L"TEMP", dir.c_str());
    const std::wstring stderr_file = dir + L"\\stderr.txt";
    RunChild(L"\"" + std::wstring(self, length) + L"\" --child missing-real-stop", "missing-real-stop", 78, stderr_file);
    ExpectHookedRecords(ReadHostRecords(dir), "runtime_absent", true);
    ExpectStopLine(stderr_file, dir, root, {true, 78, "runtime_absent", "nevr-runtime.dll is missing; path=", "game_stop"},
                   "missing-real-stop");
    Require(!StopNoticeOf(dir).empty() && RecordedNotice(ReadHostRecords(dir)) == "shown",
            "a client was not shown the notice before the host ended the game");
    std::printf("rendezvous test: [missing-real-stop] the host ended the game with exit code 78 and wrote one NEVR.STOP line\n");
  }

  // The same for a dedicated server: no dialog (nobody is there), the console line is what its operator reads.
  {
    StageRuntimeFixture(root, nullptr);
    const std::wstring dir = base + L"\\missing-real-stop-server";
    CreateDirectoryW(dir.c_str(), nullptr);
    SetEnvironmentVariableW(L"LOCALAPPDATA", dir.c_str());
    SetEnvironmentVariableW(L"TEMP", dir.c_str());
    const std::wstring stderr_file = dir + L"\\stderr.txt";
    RunChild(L"\"" + std::wstring(self, length) + L"\" --child missing-real-stop-server -server",
             "missing-real-stop-server", 78, stderr_file);
    const std::vector<nlohmann::json> records = ReadHostRecords(dir);
    ExpectHookedRecords(records, "runtime_absent", true);
    ExpectStopLine(stderr_file, dir, root, {true, 78, "runtime_absent", "nevr-runtime.dll is missing; path=", "game_stop"},
                   "missing-real-stop-server");
    Require(StopNoticeOf(dir).empty() && RecordedNotice(records) == "suppressed_server_or_headless",
            "a -server run was shown a notice");
    std::printf("rendezvous test: [missing-real-stop-server] exit code 78, one NEVR.STOP line, no dialog\n");
  }

  // The old layout: a nevr-runtime.dll beside BugSplat64.dll and none in the root. The host does not load it, and the
  // line says where it is and where to move it.
  {
    StageRuntimeFixture(root, L"good");
    const std::wstring dir = base + L"\\legacy-real-stop";
    CreateDirectoryW(dir.c_str(), nullptr);
    CreateDirectoryW((dir + L"\\root").c_str(), nullptr);
    SetEnvironmentVariableW(L"LOCALAPPDATA", dir.c_str());
    SetEnvironmentVariableW(L"TEMP", dir.c_str());
    const std::wstring stderr_file = dir + L"\\stderr.txt";
    RunChild(L"\"" + std::wstring(self, length) + L"\" --child legacy-real-stop -server", "legacy-real-stop", 78,
             stderr_file);
    ExpectHookedRecords(ReadHostRecords(dir), "runtime_absent", true);
    const nlohmann::json line = ExpectStopLine(stderr_file, dir, root,
                                               {true, 78, "runtime_absent", "nevr-runtime.dll is missing; path=", "game_stop"},
                                               "legacy-real-stop");
    const std::string host_dir = WideToUtf8(root.substr(0, root.size() - 1));
    const std::string expected = "nevr-runtime.dll is missing; path=" + WideToUtf8(dir) + "\\root\\nevr-runtime.dll; " +
                                 "a nevr-runtime.dll is beside BugSplat64.dll in " + host_dir +
                                 ", which the host does not load: move it to " + WideToUtf8(dir) + "\\root";
    if (line.at("detail") != expected) {
      std::fprintf(stderr, "legacy detail: %s\nexpected:      %s\n", line.at("detail").get<std::string>().c_str(), expected.c_str());
      Fail("the stop line does not name the old-layout runtime beside the host and where to move it");
    }
    std::printf("rendezvous test: [legacy-real-stop] the old-layout runtime beside the host is named\n");
  }

  // A refused runtime, for real: the host verifies the (unsigned) fixture with the production pins, never maps it, and
  // ends the child with the stable exit code inside the first call, after the versions, runtime-load and game_stop
  // records. The fixture's DllMain would have recorded its attach: the file was never mapped.
  {
    StageRuntimeFixture(root, L"good");
    const std::wstring dir = base + L"\\refused-real-stop";
    CreateDirectoryW(dir.c_str(), nullptr);
    SetEnvironmentVariableW(L"LOCALAPPDATA", dir.c_str());
    SetEnvironmentVariableW(L"TEMP", dir.c_str());
    const std::wstring stderr_file = dir + L"\\stderr.txt";
    RunChild(L"\"" + std::wstring(self, length) + L"\" --child refused-real-stop", "refused-real-stop", 78, stderr_file);
    const std::vector<nlohmann::json> records = ReadHostRecords(dir);
    ExpectHookedRecords(records, "runtime_refused", true);
    ExpectStopLine(stderr_file, dir, root, {true, 78, "runtime_refused", "unsigned", "game_stop"}, "refused-real-stop");
    const int load = Find(records, "runtime_load", "", "runtime_refused");
    Require(load >= 0 && records[static_cast<std::size_t>(load)].value("verify", "") == "refused" &&
                records[static_cast<std::size_t>(load)].value("verify_reason", "").rfind("unsigned", 0) == 0,
            "the runtime_load record does not say the verifier refused an unsigned file");
    std::printf("rendezvous test: [refused-real-stop] the host refused the unsigned runtime and ended the game with exit code 78\n");
  }

  // External NEVR_SetGameModule contract: resolves, is callable with void(HMODULE),
  // and terminates with the stable game-image-guard exit code on an unsupported
  // module (the launcher path is an external contract — validated, not exercised
  // as a full init).
  {
    StageRuntimeFixture(root, L"good");
    const std::wstring dir = base + L"\\set-game-module-unsupported";
    CreateDirectoryW(dir.c_str(), nullptr);
    SetEnvironmentVariableW(L"LOCALAPPDATA", dir.c_str());
    SetEnvironmentVariableW(L"TEMP", dir.c_str());
    const std::wstring stderr_file = dir + L"\\stderr.txt";
    RunChild(L"\"" + std::wstring(self, length) + L"\" --set-game-module-unsupported",
             "set-game-module-unsupported", 0xE0420003, stderr_file);
    // The refusal is recorded before the process ends, after the direct-import decisions, and the stop that follows
    // it is the last record.
    const std::vector<nlohmann::json> records = ReadHostRecords(dir);
    Require(records.size() >= 2 && records[records.size() - 2].at("step") == "image_check" &&
                records[records.size() - 2].at("outcome") == "rejected",
            "the unsupported module was not recorded as rejected before the process ended");
    Require(records.back().value("event", "") == "game_stop" && records.back().value("reason", "") == "unsupported_image",
            "the unsupported image's stop was not the last record");
    ExpectStopLine(stderr_file, dir, root, {true, 0xE0420003ll, "unsupported_image", "the game image is not the supported echovr.exe build", "game_stop"},
                   "set-game-module-unsupported");
  }

  // External NEVR_SetGameModule contract, success path: the
  // launcher drives Prepare -> Install and a later call dispatches through the
  // runtime — the supported branch, not just the termination branch.
  {
    StageRuntimeFixture(root, L"good");
    const std::wstring dir = base + L"\\set-game-module-supported";
    CreateDirectoryW(dir.c_str(), nullptr);
    SetEnvironmentVariableW(L"LOCALAPPDATA", dir.c_str());
    SetEnvironmentVariableW(L"TEMP", dir.c_str());
    RunChild(L"\"" + std::wstring(self, length) + L"\" --set-game-module-supported",
             "set-game-module-supported", 0);
    ExpectHookedRecords(ReadHostRecords(dir), "loaded");
  }

  {
    StageRuntimeFixture(root, L"good");
    const std::wstring dir = base + L"\\set-game-module-twice";
    CreateDirectoryW(dir.c_str(), nullptr);
    SetEnvironmentVariableW(L"LOCALAPPDATA", dir.c_str());
    SetEnvironmentVariableW(L"TEMP", dir.c_str());
    RunChild(L"\"" + std::wstring(self, length) + L"\" --set-game-module-twice", "set-game-module-twice", 0);
    const std::vector<nlohmann::json> records = ReadHostRecords(dir);
    int images = 0;
    for (const nlohmann::json& record : records) {
      images += record.value("step", "") == "image_check" ? 1 : 0;
    }
    Require(images == 1, "a second NEVR_SetGameModule journalled the image check again");
    const int refused = Find(records, "host_attach", "set_game_module", "refused");
    Require(refused >= 0 && records[static_cast<std::size_t>(refused)].at("reason") == "already_called" &&
                records[static_cast<std::size_t>(refused)].at("next") == "ignored",
            "a second NEVR_SetGameModule was not recorded as refused");
    Require(Find(records, "host_attach", "hook_install", "enabled") >= 0 &&
                Find(records, "runtime_load", "", "loaded") >= 0,
            "the first NEVR_SetGameModule's install and load were not recorded");
  }

  {
    StageRuntimeFixture(root, L"good");
    const std::wstring dir = base + L"\\set-game-module-after-attach";
    CreateDirectoryW(dir.c_str(), nullptr);
    SetEnvironmentVariableW(L"LOCALAPPDATA", dir.c_str());
    SetEnvironmentVariableW(L"TEMP", dir.c_str());
    RunChild(L"\"" + std::wstring(self, length) + L"\" --set-game-module-after-attach",
             "set-game-module-after-attach", 0);
    const std::vector<nlohmann::json> records = ReadHostRecords(dir);
    int images = 0;
    for (const nlohmann::json& record : records) images += record.value("step", "") == "image_check" ? 1 : 0;
    Require(images == 1, "NEVR_SetGameModule after a direct-import attach journalled the image check again");
    const int refused = Find(records, "host_attach", "set_game_module", "refused");
    Require(refused >= 0 && records[static_cast<std::size_t>(refused)].at("reason") == "attached_at_dll_main" &&
                records[static_cast<std::size_t>(refused)].at("next") == "ignored",
            "NEVR_SetGameModule after a direct-import attach was not recorded as refused");
  }

  std::printf("rendezvous test: PASS: all %llu scenarios + NEVR_SetGameModule contract\n",
              static_cast<unsigned long long>(sizeof(scenarios) / sizeof(scenarios[0])));
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc >= 3 && std::strcmp(argv[1], "--child") == 0) {
    RunChildScenario(argv[2]);
    return 0;
  }
  if (argc == 2 && std::strcmp(argv[1], "--set-game-module-unsupported") == 0) {
    return RunSetGameModuleUnsupported();
  }
  if (argc == 2 && std::strcmp(argv[1], "--set-game-module-supported") == 0) {
    return RunSetGameModuleSupported();
  }
  if (argc == 2 && std::strcmp(argv[1], "--set-game-module-twice") == 0) {
    return RunSetGameModuleTwice();
  }
  if (argc == 2 && std::strcmp(argv[1], "--set-game-module-after-attach") == 0) {
    return RunSetGameModuleAfterAttach();
  }
  if (argc != 1) {
    Fail("usage: test_rendezvous.exe [--child <scenario> | --set-game-module-unsupported | "
         "--set-game-module-supported]");
  }
  return RunSuite();
}
