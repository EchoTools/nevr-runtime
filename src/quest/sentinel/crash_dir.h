// Where the crash reports and minidumps go, chosen once by sentinel.cpp (Arm) and read from there by everything else:
// the minidump writer, the readable report and the sender (production_steps.cpp passes sentinel::DumpDir() to
// StartCrashSend). One answer, so the files a crash leaves are the files the sender looks for.
//
// The first candidate that can be made and written wins, in this order:
//   1. NEVR_QUEST_DUMP_DIR, when the build names one (never created);
//   2. the app's Android/media folder, the one a player can reach and the game's data is read from
//      (`nevr_quest::data_root::kMediaRoot`/nevr-crashes, created below Android/media/com.readyatdawn.r15);
//   3. the Android/data files folder (created below Android/data);
//   4. the app's internal files folder, 5. /data/local/tmp.
// Each folder is made one component at a time and never through a link (sentinel_log.h, EnsureDirBelow).
//
// Header-only so the host test (sentinel_host_test.cpp) drives the shipped code; sentinel.cpp cannot be built on the
// host (breakpad). Constant data only: the sentinel runs from an ELF constructor, so nothing here may have a dynamic
// initializer or register an atexit handler.
#pragma once

#include "quest/sentinel/sentinel_log.h"

#include <android/log.h>

#include <cerrno>
#include <cstddef>
#include <cstring>
#include <iterator>
#include <string>

#include <unistd.h>

namespace sentinel::crash_dir {

inline constexpr LogDirCandidate kShipped[] = {
    {"android_media", "/sdcard/Android/media/com.readyatdawn.r15/files/nevr-crashes",
     "/sdcard/Android/media/com.readyatdawn.r15"},
    {"android_data", "/sdcard/Android/data/com.readyatdawn.r15/files/nevr-crashes", "/sdcard/Android/data"},
    {"app_internal", "/data/data/com.readyatdawn.r15/files/nevr-crashes", "/data/data/com.readyatdawn.r15"},
    {"local_tmp", "/data/local/tmp/nevr-crashes", "/data/local/tmp"},
};

// The first candidate of `list` whose folder can be made and written, or "" when none can. Each candidate that cannot
// is named on logcat (Warning) with its folder and errno, so a headset that wrote no dump says why.
inline std::string ChooseFrom(const LogDirCandidate* list, std::size_t count) {
  for (std::size_t i = 0; i < count; ++i) {
    int err = 0;
    if (EnsureDirBelow(list[i])) {
      if (::access(list[i].dir, W_OK | X_OK) == 0) return list[i].dir;
      err = errno;
    } else {
      err = errno;
    }
    __android_log_print(ANDROID_LOG_WARN, "NEVR-Sentinel",
                        "crash dir candidate unusable candidate=%s dir=%s errno=%d (%s); trying the next", list[i].name,
                        list[i].dir, err, std::strerror(err));
  }
  return std::string();
}

// The shipped order (or `list`), with the build's own folder first when `override_dir` is a non-empty path.
inline std::string Choose(const char* override_dir, const LogDirCandidate* list = kShipped,
                          std::size_t count = std::size(kShipped)) {
  if (override_dir != nullptr && override_dir[0] != '\0') {
    const LogDirCandidate first = {"override", override_dir, override_dir};
    const std::string chosen = ChooseFrom(&first, 1);
    if (!chosen.empty()) return chosen;
  }
  return ChooseFrom(list, count);
}

}  // namespace sentinel::crash_dir
