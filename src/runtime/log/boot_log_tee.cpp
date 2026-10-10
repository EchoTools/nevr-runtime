#include "runtime/log/boot_log_tee.h"

#include <windows.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "core/json_escape.h"
#include "core/logging.h"  // GetRunId (N80)
#include "runtime/log/boot_lines.h"

// ---------------------------------------------------------------------------
// Internal state
// ---------------------------------------------------------------------------

static HANDLE g_boot_handle = INVALID_HANDLE_VALUE;
static char g_boot_path[MAX_PATH] = {};  // set by Init(); survives Close() for the replay (#5)

// True from Init() to Close(), whether or not the file opened: the boot phase is
// defined by the loader lock being held, not by the file being writable.
static std::atomic<bool> g_boot_phase{false};

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void nevr_boot_log_tee::Init() {
    g_boot_phase.store(true, std::memory_order_release);
    // Get the EXE directory
    char exe_path[MAX_PATH];
    DWORD len = GetModuleFileNameA(nullptr, exe_path, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        return;
    }

    // Strip filename to get the directory
    char* slash = std::strrchr(exe_path, '\\');
    if (slash == nullptr) {
        slash = std::strrchr(exe_path, '/');
    }
    if (slash != nullptr) {
        *slash = '\0';
    }

    // Build the logs directory path
    char log_dir[MAX_PATH];
    int n = snprintf(log_dir, sizeof(log_dir), "%s\\logs", exe_path);
    if (n < 0 || n >= static_cast<int>(sizeof(log_dir))) {
        return;
    }

    // Create logs directory (ignore errors — may already exist)
    CreateDirectoryA(log_dir, nullptr);

    // Build full file path
    char log_path[MAX_PATH];
    n = snprintf(log_path, sizeof(log_path), "%s\\nevr-boot.jsonl", log_dir);
    if (n < 0 || n >= static_cast<int>(sizeof(log_path))) {
        return;
    }

    std::snprintf(g_boot_path, sizeof(g_boot_path), "%s", log_path);

    // Open for append — OPEN_ALWAYS creates if missing, does not truncate
    g_boot_handle = CreateFileA(
        log_path,
        GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,            // default security
        OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr             // no template
    );

    if (g_boot_handle == INVALID_HANDLE_VALUE) {
        // Silently ignore — TeeFprintf degrades to stderr-only
        return;
    }

    // Seek to end for append semantics
    SetFilePointer(g_boot_handle, 0, nullptr, FILE_END);

    // N80: this file accumulates every run. Without a delimiter an operator reading
    // it cannot tell where the current run's boot lines begin — the previous five
    // runs sit directly above them with nothing separating the sequences.
    TeeFprintf("[NEVR.PATCH] boot log opened run=%s", GetRunId());
}

void nevr_boot_log_tee::TeeFprintf(const char* fmt, ...) {
    // --- stderr (always) ---
    {
        va_list args;
        va_start(args, fmt);
        std::vfprintf(stderr, fmt, args);
        va_end(args);
        std::fflush(stderr);
    }

    // --- boot file (if open) ---
    if (g_boot_handle == INVALID_HANDLE_VALUE) {
        return;
    }

    // Format the message into a stack buffer
    char msg_buf[2048];
    int msg_len;
    {
        va_list args;
        va_start(args, fmt);
        msg_len = vsnprintf(msg_buf, sizeof(msg_buf), fmt, args);
        va_end(args);
    }
    if (msg_len < 0) {
        return;
    }
    if (msg_len >= static_cast<int>(sizeof(msg_buf))) {
        msg_len = static_cast<int>(sizeof(msg_buf)) - 1;
    }

    // JSON-escape the message
    char escaped[4096];
    nevr_json_escape::Into(msg_buf, msg_len, escaped, sizeof(escaped));

    // Build the JSONL line (boot_lines.h). N80: the run ID is what lets these lines be joined to
    // the runtime log and, because this file is opened in append mode across runs, what lets one
    // run's boot lines be separated from the last. #5: the ts (GetSystemTime, kernel32 only, the
    // main log's format) places the line in the main log when it replays this run's boot lines.
    SYSTEMTIME st;
    GetSystemTime(&st);
    char ts[32];
    snprintf(ts, sizeof(ts), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", st.wYear, st.wMonth, st.wDay, st.wHour,
             st.wMinute, st.wSecond, st.wMilliseconds);
    char line_buf[4288];  // 4096 escaped + overhead
    const int line_len = nevr_boot_lines::Build(line_buf, sizeof(line_buf), ts, GetRunId(), escaped);
    if (line_len < 0) {
        return;
    }

    DWORD written = 0;
    WriteFile(g_boot_handle, line_buf, static_cast<DWORD>(line_len), &written, nullptr);
    // Failure is silent — nothing to do with a failed write at boot time
}

const char* nevr_boot_log_tee::Path() { return g_boot_path; }

bool nevr_boot_log_tee::InBootPhase() { return g_boot_phase.load(std::memory_order_acquire); }

void nevr_boot_log_tee::Close() {
    g_boot_phase.store(false, std::memory_order_release);
    if (g_boot_handle != INVALID_HANDLE_VALUE) {
        CloseHandle(g_boot_handle);
        g_boot_handle = INVALID_HANDLE_VALUE;
    }
}
