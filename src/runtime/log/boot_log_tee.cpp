#include "runtime/log/boot_log_tee.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "core/logging.h"  // GetRunId (N80)

// ---------------------------------------------------------------------------
// Internal state
// ---------------------------------------------------------------------------

static HANDLE g_boot_handle = INVALID_HANDLE_VALUE;

// ---------------------------------------------------------------------------
// JSON escape — writes directly into a caller-provided buffer
// ---------------------------------------------------------------------------
static int JsonEscape(const char* src, int src_len, char* dst, int dst_size) {
    int d = 0;
    for (int i = 0; i < src_len && d < dst_size - 2; ++i) {
        char c = src[i];
        switch (c) {
            case '"':
                if (d + 2 >= dst_size) goto done;
                dst[d++] = '\\';
                dst[d++] = '"';
                break;
            case '\\':
                if (d + 2 >= dst_size) goto done;
                dst[d++] = '\\';
                dst[d++] = '\\';
                break;
            case '\n':
                if (d + 2 >= dst_size) goto done;
                dst[d++] = '\\';
                dst[d++] = 'n';
                break;
            case '\r':
                if (d + 2 >= dst_size) goto done;
                dst[d++] = '\\';
                dst[d++] = 'r';
                break;
            case '\t':
                if (d + 2 >= dst_size) goto done;
                dst[d++] = '\\';
                dst[d++] = 't';
                break;
            default:
                dst[d++] = c;
                break;
        }
    }
done:
    dst[d] = '\0';
    return d;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void BootLogTee::Init() {
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

void BootLogTee::TeeFprintf(const char* fmt, ...) {
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
    int esc_len = JsonEscape(msg_buf, msg_len, escaped, sizeof(escaped));

    // Build the JSONL line: {"run":"<id>","level":"info","msg":"<escaped>"}\n
    // N80: the run ID is what lets these lines be joined to the runtime log (which
    // lives in a different directory) and, because this file is opened in append
    // mode across runs, what lets one run's boot lines be separated from the last.
    char line_buf[4224];  // 4096 escaped + overhead
    int line_len = snprintf(line_buf, sizeof(line_buf),
                            "{\"run\":\"%s\",\"level\":\"info\",\"msg\":\"%s\"}\n",
                            GetRunId(), escaped);
    if (line_len <= 0 || line_len >= static_cast<int>(sizeof(line_buf))) {
        return;
    }

    DWORD written = 0;
    WriteFile(g_boot_handle, line_buf, static_cast<DWORD>(line_len), &written, nullptr);
    // Failure is silent — nothing to do with a failed write at boot time
}

void BootLogTee::Close() {
    if (g_boot_handle != INVALID_HANDLE_VALUE) {
        CloseHandle(g_boot_handle);
        g_boot_handle = INVALID_HANDLE_VALUE;
    }
}
