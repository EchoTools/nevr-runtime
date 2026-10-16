// Early-rendezvous harness (docs/adr/0008-bugsplat-host-and-nevr-runtime.md,
// "Early bootstrap").
//
// This executable stands in for echovr.exe. It exports an event recorder and a
// synthetic game image: a VirtualAlloc'd PE32+ header block with the supported
// TimeDateStamp and, at RVA 0x116720, a function that begins with the REAL
// 18-byte PreprocessCommandLine prologue and then calls a C body that records the
// call and returns ExpectedResult(game). The host shim (BugSplat64.dll) validates
// and MinHooks it from DLL_PROCESS_ATTACH, then loads the sibling nevr.dll on the
// first call and dispatches.
//
// Each child validates, from the recorded events:
//   - host attach installs the hook (or, on a bad prologue, leaves the game
//     unhooked) and never loads nevr.dll before attach returns;
//   - every call runs the original exactly once with the exact argument/result;
//   - the runtime's preprocess_before runs before the original, preprocess_after
//     after it, exactly once per dispatched call;
//   - server: preflight records -server before the original, and the deferred
//     bootstrap runs exactly once after it; client: the deferred bootstrap is
//     skipped; a same-thread re-entry during load bypasses to the original (seen
//     as one lone original).

#include "runtime/tests/rendezvous/abi.h"

#include "runtime/host/preprocess_target.h"

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
int g_launcher_mode = 0;
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
extern "C" std::uint64_t RendezvousProbeOriginalBody(void* game) {
  RendezvousProbeRecordEvent(kOriginal, reinterpret_cast<std::uintptr_t>(game));
  return ExpectedResult(game);
}

namespace {

// Writes the synthetic image (kernel32 only, byte loops) whose
// PreprocessCommandLine starts with the real prologue and then calls the C body.
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
  nt->FileHeader.TimeDateStamp = 0x6452dff6;
  nt->FileHeader.SizeOfOptionalHeader = sizeof(IMAGE_OPTIONAL_HEADER64);
  nt->OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR64_MAGIC;
  nt->OptionalHeader.SizeOfHeaders = kPage;
  nt->OptionalHeader.SizeOfImage = static_cast<DWORD>(kImageSize);

  BYTE* code = base + nevr_host::kPreprocessRva;
  SIZE_T at = 0;
  for (const unsigned char byte : nevr_host::kPreprocessPrologue) code[at++] = byte;
  if (wrong_prologue) code[7] = 0xb2;  // mutate the lea displacement (0xb1 -> 0xb2)
  const unsigned char mid[] = {0x48, 0x89, 0xf9, 0x48, 0xb8};
  for (const unsigned char byte : mid) code[at++] = byte;
  const std::uintptr_t body = reinterpret_cast<std::uintptr_t>(&RendezvousProbeOriginalBody);
  for (int shift = 0; shift < 64; shift += 8) code[at++] = static_cast<BYTE>(body >> shift);
  const unsigned char tail[] = {0xff, 0xd0, 0x48, 0x81, 0xc4, 0xa8, 0x00,
                                0x00, 0x00, 0x5f, 0x5d, 0xc3};
  for (const unsigned char byte : tail) code[at++] = byte;

  DWORD old_protection = 0;
  BYTE* code_page = base + (nevr_host::kPreprocessRva & ~(kPage - 1));
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

namespace {

[[noreturn]] void Fail(const char* message);

NevrPreprocessCommandLineFn Target() {
  return reinterpret_cast<NevrPreprocessCommandLineFn>(g_image + nevr_host::kPreprocessRva);
}

void CallTarget(std::uintptr_t raw_game) {
  void* game = reinterpret_cast<void*>(raw_game);
  RendezvousProbeRecordEvent(kCallIssued, raw_game);
  if (Target()(game) != ExpectedResult(game)) {
    RendezvousProbeRecordEvent(kBadResult, raw_game);
  }
}

const char* EventName(LONG event) {
  static const char* const kNames[kEventLimit] = {
      "?", "host.attach_enter", "host.hook_enabled", "host.hook_failed",
      "host.attach_exit", "runtime.dll_attach", "runtime.initialized",
      "runtime.before", "original", "runtime.after", "runtime.server_detected",
      "runtime.client_detected", "runtime.deferred", "call.issued",
      "call.bad_result"};
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

// before pushes a frame; original completes the innermost open frame (or is a lone
// original when unhooked/degraded/bypassed); after closes it. Returns lone count.
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

void ValidateAttach(const std::vector<EventRecord>& events, bool expect_hooked) {
  Require(Count(events, kHostAttachEnter) == 1 && Count(events, kHostAttachExit) == 1,
          "host attach did not run exactly once");
  const std::size_t enter = First(events, kHostAttachEnter);
  const std::size_t exit = First(events, kHostAttachExit);
  Require(enter < exit, "host attach exit before enter");
  Require(events[exit].payload == 0, "nevr.dll was loaded before host DllMain returned");
  if (expect_hooked) {
    Require(Count(events, kHostHookEnabled) == 1, "host did not enable the hook");
    Require(events[First(events, kHostHookEnabled)].payload == 0xE9,
            "rendezvous does not start with a JMP rel32 after enable");
  } else {
    Require(Count(events, kHostHookEnabled) == 0 && Count(events, kHostHookFailed) == 1,
            "a rejected image was hooked, or the rejection was not recorded");
  }
}

void ValidateDispatch(const std::vector<EventRecord>& events, bool dispatched, LONG expected_lone) {
  const LONG calls = Count(events, kCallIssued);
  Require(calls > 0, "scenario issued no calls");
  Require(Count(events, kOriginal) == calls, "original did not run exactly once per call");
  Require(Count(events, kBadResult) == 0, "a caller received a result other than the original's");
  const LONG lone = CheckFrames(events);
  Require(lone == expected_lone, "unexpected number of undispatched original calls");
  if (dispatched) {
    Require(Count(events, kRuntimeDllAttach) == 1 && Count(events, kRuntimeInitialized) == 1,
            "runtime DllMain/initialize did not run exactly once");
    Require(Count(events, kRuntimeBefore) == calls - lone &&
                Count(events, kRuntimeAfter) == calls - lone,
            "before/after counts differ from dispatched calls");
    const std::uint64_t trampoline = events[First(events, kRuntimeInitialized)].payload;
    Require(trampoline != 0 &&
                trampoline != reinterpret_cast<std::uintptr_t>(g_image + nevr_host::kPreprocessRva),
            "host context did not carry a trampoline distinct from the hooked target");
  } else {
    Require(Count(events, kRuntimeBefore) == 0 && Count(events, kRuntimeAfter) == 0,
            "a failed runtime was dispatched");
  }
}

void ValidateServer(const std::vector<EventRecord>& events) {
  Require(Count(events, kRuntimeServerDetected) == 1 && Count(events, kRuntimeClientDetected) == 0,
          "preflight did not detect -server exactly once");
  Require(First(events, kRuntimeServerDetected) < First(events, kOriginal),
          "preflight detected -server after the original already ran");
  Require(Count(events, kRuntimeDeferred) == 1, "deferred bootstrap did not run exactly once");
  Require(First(events, kRuntimeDeferred) > First(events, kOriginal),
          "deferred bootstrap ran before the original returned");
}

void ValidateClient(const std::vector<EventRecord>& events) {
  Require(Count(events, kRuntimeClientDetected) == 1 && Count(events, kRuntimeServerDetected) == 0,
          "preflight did not detect the client (no -server) exactly once");
  Require(Count(events, kRuntimeDeferred) == 0, "client ran the server-only deferred bootstrap");
}

void RunChildScenario(const std::string& scenario) {
  bool expect_hooked = true;
  bool dispatched = true;
  LONG calls = 1;

  if (scenario == "bad-prologue") {
    g_wrong_prologue = true;
    expect_hooked = false;
    dispatched = false;
  } else if (scenario == "missing") {
    dispatched = false;
  } else if (scenario == "server-twice") {
    calls = 2;
  } else if (scenario == "reenter") {
    // One lone original: the nested same-thread re-entry bypassed the dispatch.
  } else if (scenario != "server" && scenario != "client") {
    Fail("unknown child scenario");
  }
  const LONG expected_lone = dispatched ? (scenario == "reenter" ? 1 : 0) : calls;

  g_image = BuildGameImage(g_wrong_prologue);
  Require(g_image != nullptr, "could not build the synthetic game image");

  const std::wstring host_path = ExecutableDirectory() + L"BugSplat64.dll";
  HMODULE host = LoadLibraryW(host_path.c_str());
  Require(host != nullptr, "host shim failed to load");

  // A rejected prologue installs no detour, so there is no first-call trigger.
  // The host's always-present deferred path flushes the fixed-memory attach
  // diagnostic on demand (the production trigger is the MiniDmpSender constructor /
  // NEVR_SetGameModule); exercise it so the parent can verify the record landed.
  if (scenario == "bad-prologue") {
    const auto flush =
        reinterpret_cast<void (*)()>(GetProcAddress(host, "nevr_host_test_FlushDeferred"));
    if (flush != nullptr) flush();
  }

  for (LONG call = 0; call < calls; ++call) {
    CallTarget(static_cast<std::uintptr_t>(0x1000) + (static_cast<std::uintptr_t>(call) << 4));
  }

  const std::vector<EventRecord> events = Snapshot();
  ValidateAttach(events, expect_hooked);
  ValidateDispatch(events, dispatched, expected_lone);
  if (scenario == "server" || scenario == "server-twice") ValidateServer(events);
  if (scenario == "client") ValidateClient(events);
  std::printf("rendezvous test: PASS: %s (calls=%ld originals=%ld dispatched=%ld lone=%ld)\n",
              scenario.c_str(), static_cast<long>(Count(events, kCallIssued)),
              static_cast<long>(Count(events, kOriginal)),
              static_cast<long>(Count(events, kRuntimeBefore)), expected_lone);
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
  const std::wstring runtime = root + L"nevr.dll";
  if (fixture == nullptr) {
    DeleteFileW(runtime.c_str());
    Require(GetFileAttributesW(runtime.c_str()) == INVALID_FILE_ATTRIBUTES, "could not remove nevr.dll");
    return;
  }
  const std::wstring source = root + L"fixtures\\" + fixture + L"\\nevr.dll";
  Require(CopyFileW(source.c_str(), runtime.c_str(), FALSE) != FALSE, "could not stage runtime fixture");
}

void RunChild(const std::wstring& command_line, const char* label, DWORD expected_exit = 0) {
  std::wstring command = command_line;  // CreateProcessW may mutate the buffer
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

// The attach decisions of a hooked run, in order, followed by what the load decided.
void ExpectHookedRecords(const std::vector<nlohmann::json>& records, const char* load_outcome) {
  const int image = Find(records, "host_attach", "image_check", "accepted");
  const int prologue = Find(records, "host_attach", "prologue_check", "accepted");
  const int hook = Find(records, "host_attach", "hook_install", "enabled");
  const int load = Find(records, "runtime_load", "", load_outcome);
  Require(image >= 0 && prologue >= 0 && hook >= 0 && load >= 0,
          "a hooked run did not record its image, prologue, hook and runtime-load decisions");
  Require(image < prologue && prologue < hook && hook < load,
          "the host's decisions were recorded out of order");
  Require(records.size() == 4, "a hooked run recorded something other than its four decisions");
  const nlohmann::json& found = records[static_cast<std::size_t>(prologue)];
  Require(found.at("rva") == "0x116720" &&
              found.at("actual_prologue") == found.at("expected_prologue"),
          "the accepted prologue record does not carry the rva and the matching bytes");
  Require(records[static_cast<std::size_t>(load)].at("degraded_action") ==
              (std::strcmp(load_outcome, "loaded") == 0 ? "dispatch" : "run_original_only"),
          "the runtime-load record names the wrong degraded action");
}

int RunSetGameModuleUnsupported() {
  // Load the shim (its DllMain installs on the supported synthetic image), then
  // call NEVR_SetGameModule with the test executable — an unsupported module. The
  // guard must terminate with the stable code.
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
  CallTarget(static_cast<std::uintptr_t>(0x2000));
  const std::vector<EventRecord> events = Snapshot();
  ValidateDispatch(events, true, 0);
  std::printf("rendezvous test: PASS: NEVR_SetGameModule supported (launcher path)\n");
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
    bool server_flag;
  };
  const Scenario scenarios[] = {
      {"server", L"good", true},
      {"server-twice", L"good", true},
      {"client", L"good", false},
      {"missing", nullptr, false},
      {"reenter", L"reenter-init", false},
      {"bad-prologue", L"good", false},
  };

  for (const Scenario& scenario : scenarios) {
    StageRuntimeFixture(root, scenario.fixture);
    const std::wstring scenario_dir = base + L"\\" + Utf8ToWide(scenario.name);
    CreateDirectoryW(scenario_dir.c_str(), nullptr);
    SetEnvironmentVariableW(L"LOCALAPPDATA", scenario_dir.c_str());
    SetEnvironmentVariableW(L"TEMP", scenario_dir.c_str());

    std::wstring command = L"\"" + std::wstring(self, length) + L"\" --child " +
                           Utf8ToWide(scenario.name);
    if (scenario.server_flag) command += L" -server";
    RunChild(command, scenario.name);

    const std::vector<nlohmann::json> records = ReadHostRecords(scenario_dir);
    if (std::strcmp(scenario.name, "bad-prologue") == 0) {
      // No detour exists, so the report came from the crash-reporter-constructor
      // trigger (nevr_host_test_FlushDeferred): the image was accepted, the prologue was
      // not, and the bytes found are in the record.
      Require(records.size() == 2, "bad-prologue recorded other than image_check and prologue_check");
      const int mismatch = Find(records, "host_attach", "prologue_check", "mismatch");
      Require(Find(records, "host_attach", "image_check", "accepted") == 0 && mismatch == 1,
              "bad-prologue did not record an accepted image then a prologue mismatch");
      const nlohmann::json& record = records[1];
      Require(record.at("degraded_action") == "run_original_only", "the mismatch names no degraded action");
      Require(record.at("expected_prologue") == "405557488d6c24b14881eca8000000488bf9" &&
                  record.at("actual_prologue") == "405557488d6c24b24881eca8000000488bf9",
              "the mismatch record does not carry the expected and the actual bytes");
    } else if (std::strcmp(scenario.name, "missing") == 0) {
      ExpectHookedRecords(records, "runtime_absent");
    } else {
      ExpectHookedRecords(records, "loaded");
    }
    std::printf("rendezvous test: [%s] %zu host records parsed and ordered\n", scenario.name,
                records.size());
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
    RunChild(L"\"" + std::wstring(self, length) + L"\" --set-game-module-unsupported",
             "set-game-module-unsupported", 0xE0420003);
    // The refusal is recorded before the process ends, after the direct-import decisions.
    const std::vector<nlohmann::json> records = ReadHostRecords(dir);
    Require(!records.empty() && records.back().at("step") == "image_check" &&
                records.back().at("outcome") == "rejected",
            "the unsupported module was not recorded as rejected before the process ended");
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

  std::printf("rendezvous test: PASS: all %llu scenarios + NEVR_SetGameModule contract\n",
              static_cast<unsigned long long>(sizeof(scenarios) / sizeof(scenarios[0])));
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc >= 3 && std::strcmp(argv[1], "--child") == 0) {
    RunChildScenario(argv[2]);  // a trailing -server is read by the fixture via GetCommandLineW
    return 0;
  }
  if (argc == 2 && std::strcmp(argv[1], "--set-game-module-unsupported") == 0) {
    return RunSetGameModuleUnsupported();
  }
  if (argc == 2 && std::strcmp(argv[1], "--set-game-module-supported") == 0) {
    return RunSetGameModuleSupported();
  }
  if (argc != 1) {
    Fail("usage: test_rendezvous.exe [--child <scenario> | --set-game-module-unsupported | "
         "--set-game-module-supported]");
  }
  return RunSuite();
}
