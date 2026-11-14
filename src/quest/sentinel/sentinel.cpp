/* nevr-quest crash-reporter — breakpad ExceptionHandler + crash artifacts.
 *
 * On a fatal signal breakpad writes a real minidump. In the (necessarily
 * async-signal-constrained) dump callback we additionally snapshot
 * /proc/self/maps and a best-effort frame-pointer backtrace, then log the
 * dump path. Offline symbolication from the minidump + maps is authoritative;
 * the in-process backtrace is a convenience.
 *
 * The signal handlers are ours, not breakpad's (#1605): breakpad is built with install_handler=false and the handler in
 * crash_first_hook.cpp calls it (HandleSignal) after the report, then the game's own handler, which the game's later
 * sigaction calls cannot displace (crash_first_hook.h).
 */

#include "sentinel.h"

#include "quest/sentinel/crash_dir.h"
#include "quest/auth/file_store.h"
#include "quest/sentinel/crash_report_quest.h"
#include "quest/shim/update_root_rules.h"
#include "quest/sentinel/hook_log.h"
#include "quest/sentinel/sentinel_log.h"

#include "client/linux/handler/exception_handler.h"
#include "client/linux/handler/minidump_descriptor.h"

#include <android/log.h>

#include "core/crash_report.h"
#include "crash_first_arm.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>

#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>

// A build may name the crash folder (tried first, never created); the shipped build does not (crash_dir.h has the list).
#ifdef NEVR_QUEST_DUMP_DIR
constexpr const char* kDumpDirOverride = NEVR_QUEST_DUMP_DIR;
#else
constexpr const char* kDumpDirOverride = nullptr;
#endif

#define NEVR_TAG "NEVR-Sentinel"

namespace sentinel {

namespace {

google_breakpad::ExceptionHandler* g_handler = nullptr;
std::once_flag                     g_once;
// A namespace-scope std::string has a dynamic initializer that runs after the ELF constructor
// that calls Arm() and would blank the resolved directory, so it is a function-local static.
std::string& DumpDirStorage() {
    static std::string dir;
    return dir;
}

// The first candidate folder that can be made and written (crash_dir.h): the app's Android/media folder, then
// Android/data, then the app-internal and /data/local/tmp folders. Everything that reads the crash folder asks
// DumpDir(), which returns this one answer.
std::string ResolveDumpDir() {
    const std::string chosen = crash_dir::Choose(kDumpDirOverride);
    if (!chosen.empty()) return chosen;
    // Last resort: current directory (module dir at load time).
    return ".";
}

// ---- async-signal-safe-ish helpers (used only inside the dump callback) ----

// Copy /proc/self/maps -> <dump_path>.maps using raw fd I/O. Best-effort.
void SnapshotMaps(const char* dump_path) {
    char maps_path[512];
    // Derive "<dump>.maps" without libc string formatting surprises.
    size_t n = strlen(dump_path);
    if (n + 6 >= sizeof(maps_path)) return;
    memcpy(maps_path, dump_path, n);
    memcpy(maps_path + n, ".maps", 6);  // includes NUL

    int in = ::open("/proc/self/maps", O_RDONLY | O_CLOEXEC);
    if (in < 0) return;
    int out = ::open(maps_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (out < 0) { ::close(in); return; }

    char buf[4096];
    ssize_t r;
    while ((r = ::read(in, buf, sizeof(buf))) > 0) {
        ssize_t off = 0;
        while (off < r) {
            ssize_t w = ::write(out, buf + off, r - off);
            if (w <= 0) break;
            off += w;
        }
    }
    ::close(in);
    ::close(out);
}

// breakpad dump callback. Runs in a compromised process — keep it minimal.
bool DumpCallback(const google_breakpad::MinidumpDescriptor& descriptor,
                  void* /*context*/, bool succeeded) {
    if (descriptor.path()) {
        SnapshotMaps(descriptor.path());
        // The readable report (#1605) goes beside the minidump under its stem: moved there when a handler already
        // wrote it with the faulting thread's registers, written here (without them) when none did.
        char report[kReportPathBytes];
        const bool have_report = FinishCrashReport(descriptor.path(), report, sizeof(report));
        __android_log_print(succeeded ? ANDROID_LOG_FATAL : ANDROID_LOG_ERROR,
                            NEVR_TAG, "crash: minidump=%s ok=%d maps written report=%s",
                            descriptor.path(), succeeded ? 1 : 0, have_report ? report : "none");
    }
    // Returning false lets the OS default handler run afterwards (core/logcat
    // tombstone), which is desirable — we are additive, not a swallow.
    return false;
}

// ---- the crash path (crash_first_hook.h: report, dump, restore, the game's handler) ----

// The report: the signal's facts and its ucontext, handed to the shared writer (core/crash_report.h).
void ReportAction(int signo, siginfo_t* info, void* ucontext) noexcept {
    // The self-update's crash marker goes first: it is the one thing a crash must leave, whatever the report and the dump do
    // (docs/adr/0024-pc-self-update.md, section 6).
    MarkUpdateCrashed();
    nevr_crash::CrashInfo crash{};
    crash.code = signo;
    crash.fault_code = info != nullptr ? info->si_code : 0;
    crash.fault_address = info != nullptr ? static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(info->si_addr)) : 0;
    crash.context = ucontext;
    crash.thread_id = static_cast<std::uint64_t>(syscall(__NR_gettid));
#if defined(__aarch64__)
    if (ucontext != nullptr) {
        const ucontext_t* context = static_cast<const ucontext_t*>(ucontext);
        crash.pc = context->uc_mcontext.pc;
        crash.lr = context->uc_mcontext.regs[30];
    }
#endif
    nevr_crash::WriteReport(crash);
}

// The minidump, written by breakpad on this thread; DumpCallback logs where it went.
void DumpAction(int signo, siginfo_t* info, void* ucontext) noexcept {
    if (g_handler != nullptr) g_handler->HandleSignal(signo, info, ucontext);
}

// One logcat line after the game's handler returned: whether the game had a handler of its own to run after ours, and how
// many times it asked to install one (each of those would have displaced ours).
void NoteAction(int signo, bool chained, std::uint32_t displaced) noexcept {
    __android_log_print(ANDROID_LOG_FATAL, NEVR_TAG, "crash_first: signal=%d game_handler_called=%d displaced_requests=%u",
                        signo, chained ? 1 : 0, displaced);
}

}  // namespace

// The hook backend's JSON lines go to logcat and to the process's log file. A line is one JSON object, so Emit flattens its fields
// into the record (sentinel_log.h). Set by Arm, which only the shim runs.
void WriteLineToDiskLog(LogLevel level, const char* line) {
    nevr_quest::LogLevel mapped = nevr_quest::LogLevel::kInfo;
    if (level == LogLevel::kWarn) mapped = nevr_quest::LogLevel::kWarn;
    if (level == LogLevel::kError) mapped = nevr_quest::LogLevel::kError;
    // The sink is called from hook_log.cpp, which is built without exceptions: a failure must not leave this frame.
    EmitNoThrow(mapped, line);
}

void Arm() {
#ifdef NEVR_QUEST_RUNTIME_LIBRARY
    // The runtime copy never arms: the shim arms first and owns the crash handler and the log file of the process. The runtime's
    // records reach that log through the host sink (hook_log.h, ForwardRecordToHost). The one thing the runtime needs from this
    // step is the crash folder, which its crash send reads (production_steps.cpp); it is the same folder the shim resolves.
    DumpDirStorage() = ResolveDumpDir();
    return;
#endif
    std::call_once(g_once, [] {
        SetLogSink(&WriteLineToDiskLog);
        DumpDirStorage() = ResolveDumpDir();
        // The shared crash report's Quest platform, registered before any handler can reach it.
        RegisterCrashReportPlatform();
        // The self-update's crash marker, armed with the nevr root: the folder that holds update/, below the app's internal files
        // folder (shim/update_root_rules.h). It builds its paths and opens the log's record descriptor now, so the crash path
        // opens nothing but the marker file.
        ArmUpdateCrashMarker(nevr_quest::update_root::RootFromFilesDir(nevr::quest_auth::AppInternalFilesDir()).c_str());
        google_breakpad::MinidumpDescriptor descriptor(DumpDirStorage());
        // install_handler=false: the signal handlers are ours (crash_first_hook.cpp), so the game's later sigaction
        // cannot displace them, and they call breakpad's HandleSignal for the minidump.
        g_handler = new google_breakpad::ExceptionHandler(
            descriptor,
            /*filter=*/nullptr,
            DumpCallback,
            /*context=*/nullptr,
            /*install_handler=*/false,
            /*server_fd=*/-1);
        nevr_quest::crash_first::InstallHandlers({&ReportAction, &DumpAction, &NoteAction});
        __android_log_print(ANDROID_LOG_INFO, NEVR_TAG,
                            "armed: breakpad ExceptionHandler, dumps -> %s",
                            DumpDirStorage().c_str());
    });
}

void Disarm() {
    nevr_quest::crash_first::RestoreHandlers();
    delete g_handler;
    g_handler = nullptr;
}

const char* DumpDir() {
    return DumpDirStorage().empty() ? "" : DumpDirStorage().c_str();
}

}  // namespace sentinel
