#include "runtime/log/boot_log_tee.h"

#include <winsock2.h>
#include <windows.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "core/json_escape.h"
#include "core/logging.h"  // GetRunId (N80), SetBootLogSink, GetLogLevelString
#include "runtime/log/boot_lines.h"
#include "runtime/log/run_log_writer.h"

// ---------------------------------------------------------------------------
// Internal state
// ---------------------------------------------------------------------------

static std::atomic<bool> g_boot_active{false};  // Init() to Close(), the boot phase

// Log()'s boot sink (BootLogSink, core/logging.h), set from Init() to Close() whether or not the
// file opened: the boot phase is defined by the tee being open (Init() to Close()), not by the file being
// writable. Rule 12: the level goes after the subsystem tag ("[NEVR.HEADLESS] warn; ..."), or first
// when there is no tag.
static void ReportBootLog(EchoVR::LogLevel level, const CHAR* msg) {
    const char* const lvl = GetLogLevelString(level);
    const char* const tagEnd = msg[0] == '[' ? std::strchr(msg, ']') : nullptr;
    if (tagEnd != nullptr && tagEnd[1] == ' ') {
        nevr_boot_log_tee::TeeFprintf("%.*s %s; %s\n", static_cast<int>(tagEnd + 1 - msg), msg, lvl, tagEnd + 2);
    } else {
        nevr_boot_log_tee::TeeFprintf("%s; %s\n", lvl, msg);
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void nevr_boot_log_tee::Init() {
    SetBootLogSink(&ReportBootLog);
    g_boot_active.store(true, std::memory_order_release);
    // The boot phase writes into the process's one main log, <root>\logs\nevr-<creation time>.jsonl (run_log_writer.h), the file
    // the filter and the host write to; there is no separate boot file. The writer makes the file on the first record.
    // A run that cannot open it says so on standard error and the boot lines stay on stderr (the writer is loud, never silent).
    // The line's own text keeps "boot log opened run=" (tests/smoke/score-log.sh reads it).
    TeeFprintf("[NEVR.PATCH] boot log opened run=%s path=%s\n", GetRunId(), nevr_run_log_writer::Path());
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

    // Build the JSONL record (boot_lines.h): ts in the main log's format, the run id (N80), the level, src "boot".
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
    // A record that cannot be written is counted and reported on stderr by the writer (run_log_writer.h).
    nevr_run_log_writer::Append(line_buf, static_cast<std::size_t>(line_len));
}

const char* nevr_boot_log_tee::Path() { return nevr_run_log_writer::Path(); }

bool nevr_boot_log_tee::Active() { return g_boot_active.load(std::memory_order_acquire); }

void nevr_boot_log_tee::Close() {
    SetBootLogSink(nullptr);
    g_boot_active.store(false, std::memory_order_release);
    // The main log stays open: the filter and the crash path write to it after the boot phase (run_log_writer.h).
}
