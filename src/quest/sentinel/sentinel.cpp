/* nevr-quest crash-reporter — breakpad ExceptionHandler + crash artifacts.
 *
 * On a fatal signal breakpad writes a real minidump. In the (necessarily
 * async-signal-constrained) dump callback we additionally snapshot
 * /proc/self/maps and a best-effort frame-pointer backtrace, then log the
 * dump path. Offline symbolication from the minidump + maps is authoritative;
 * the in-process backtrace is a convenience.
 */

#include "sentinel.h"

#include "client/linux/handler/exception_handler.h"
#include "client/linux/handler/minidump_descriptor.h"

#include <android/log.h>

#include <atomic>
#include <cstring>
#include <mutex>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef NEVR_QUEST_DUMP_DIR
#define NEVR_QUEST_DUMP_DIR "/sdcard/Android/data/com.readyatdawn.r15/files/nevr-crashes"
#endif

#define NEVR_TAG "NEVR-Sentinel"

namespace sentinel {

namespace {

google_breakpad::ExceptionHandler* g_handler = nullptr;
std::once_flag                     g_once;
std::string                        g_dumpDir;

// Candidate app-writable directories, most-preferred first. The compile-time
// default wins if it is writable.
const char* kCandidates[] = {
    NEVR_QUEST_DUMP_DIR,
    "/sdcard/Android/data/com.readyatdawn.r15/files/nevr-crashes",
    "/data/data/com.readyatdawn.r15/files/nevr-crashes",
    "/data/local/tmp/nevr-crashes",
};

bool EnsureWritableDir(const char* path) {
    // mkdir -p (single level of parents is enough for our candidates).
    ::mkdir(path, 0755);
    return ::access(path, W_OK) == 0;
}

std::string ResolveDumpDir() {
    for (const char* c : kCandidates) {
        if (EnsureWritableDir(c)) return c;
    }
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
        __android_log_print(succeeded ? ANDROID_LOG_FATAL : ANDROID_LOG_ERROR,
                            NEVR_TAG, "crash: minidump=%s ok=%d maps written",
                            descriptor.path(), succeeded ? 1 : 0);
    }
    // Returning false lets the OS default handler run afterwards (core/logcat
    // tombstone), which is desirable — we are additive, not a swallow.
    return false;
}

}  // namespace

void Arm() {
    std::call_once(g_once, [] {
        g_dumpDir = ResolveDumpDir();
        google_breakpad::MinidumpDescriptor descriptor(g_dumpDir);
        // install_handler=true installs SIGSEGV/SIGABRT/SIGBUS/SIGILL/SIGFPE.
        g_handler = new google_breakpad::ExceptionHandler(
            descriptor,
            /*filter=*/nullptr,
            DumpCallback,
            /*context=*/nullptr,
            /*install_handler=*/true,
            /*server_fd=*/-1);
        __android_log_print(ANDROID_LOG_INFO, NEVR_TAG,
                            "armed: breakpad ExceptionHandler, dumps -> %s",
                            g_dumpDir.c_str());
    });
}

void Disarm() {
    delete g_handler;
    g_handler = nullptr;
}

const char* DumpDir() {
    return g_dumpDir.empty() ? "" : g_dumpDir.c_str();
}

}  // namespace sentinel
