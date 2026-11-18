#pragma once

// The helpers of echovr_server.exe that decide something, kept out of main() so a test can reach them (the executable
// itself is a CreateProcess wrapper): the command-line quoting, the directory of the launcher's own path, the listing of
// the logs folder and the follower that shows a run's records.

#include <cstdio>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX  // nlohmann-json (boot_replay.h) pulls <limits>; windows.h's min/max macros would break it
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "launcher/server_log.h"
#include "runtime/log/boot_replay.h"

namespace nevr_launcher {

/// The directory part of a full path, without the trailing backslash; empty when the path has no backslash.
inline std::string DirectoryOfPath(const std::string& full) {
  const auto pos = full.rfind('\\');
  if (pos == std::string::npos) {
    return {};
  }
  return full.substr(0, pos);
}

// Each backslash that precedes a double-quote (or the closing quote) is written twice, so the parser reads it back as one.
constexpr size_t kBackslashesPerEscapedBackslash = 2;

// Quote an argument for Windows CommandLineToArgvW rules.
// Returns the argument quoted-and-escaped if it contains characters that would
// be misinterpreted (whitespace, double-quotes, or empty), or verbatim if not.
// Rules (from CommandLineToArgvW documentation):
//   1. Whitespace (space, tab) or empty -> wrap in double quotes.
//   2. Backslashes before a double-quote are doubled to prevent escape.
//   3. A double-quote is escaped as \" inside a quoted argument.
//   4. Trailing backslashes before the closing quote are doubled.
inline std::string QuoteArgForWindows(const std::string& arg) {
  if (!arg.empty() && arg.find_first_of(" \t\"") == std::string::npos) {
    return arg;
  }

  std::string result = "\"";
  size_t i = 0;
  while (i < arg.size()) {
    size_t num_backslashes = 0;
    while (i < arg.size() && arg[i] == '\\') {
      ++i;
      ++num_backslashes;
    }

    if (i >= arg.size()) {
      // End of string: double trailing backslashes so the closing double-quote
      // is not consumed as part of an escape sequence.
      result.append(num_backslashes * kBackslashesPerEscapedBackslash, '\\');
    } else if (arg[i] == '"') {
      // Before a literal double-quote: double the backslashes and escape the
      // quote so CommandLineToArgvW treats it as a literal character.
      result.append(num_backslashes * kBackslashesPerEscapedBackslash, '\\');
      result.append("\\\"");
      ++i;
    } else {
      result.append(num_backslashes, '\\');
      result += arg[i];
      ++i;
    }
  }
  result += '"';
  return result;
}

// The files of the logs folder, with their last-write times. An absent folder lists nothing: the server creates it.
inline std::vector<nevr_server_log::LogFileInfo> ListLogFiles(const std::string& dir) {
  std::vector<nevr_server_log::LogFileInfo> files;
  WIN32_FIND_DATAA data{};
  HANDLE find = FindFirstFileA((dir + "\\*.jsonl").c_str(), &data);
  if (find == INVALID_HANDLE_VALUE) return files;
  do {
    if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) continue;
    ULARGE_INTEGER written{};
    written.LowPart = data.ftLastWriteTime.dwLowDateTime;
    written.HighPart = data.ftLastWriteTime.dwHighDateTime;
    files.push_back({data.cFileName, written.QuadPart});
  } while (FindNextFileA(find, &data));
  FindClose(find);
  return files;
}

/// Where the follower writes: `line` takes one rendered record (the launcher's stdout), `notice` takes one of the
/// launcher's own lines (its stderr), without the trailing newline; `flush` runs after each followed file's lines.
struct FollowerSink {
  std::function<void(const std::string&)> line;
  std::function<void(const std::string&)> notice;
  std::function<void()> flush;
};

/// The console: records to stdout, the launcher's notices to stderr.
inline FollowerSink ConsoleSink() {
  FollowerSink sink;
  sink.line = [](const std::string& text) {
    std::printf("%s\n", text.c_str());
  };
  sink.notice = [](const std::string& text) { std::fprintf(stderr, "%s\n", text.c_str()); };
  sink.flush = [] { std::fflush(stdout); };
  return sink;
}

// Shows this run's records as the server writes them. Each poll reads every run log of the folder that was written since
// the start, from where the previous poll stopped (nevr_boot_replay::ReadNew keeps the records of the one run); a file of
// another run yields no records. Rotation starts a new file for the same run, and it is followed the same way.
class RunLogFollower {
 public:
  RunLogFollower(std::string dir, std::string runId, unsigned long long since, FollowerSink sink = ConsoleSink())
      : dir_(std::move(dir)), runId_(std::move(runId)), since_(since), sink_(std::move(sink)) {}

  void Poll() {
    for (const std::string& name : nevr_server_log::RunLogCandidates(ListLogFiles(dir_), since_)) {
      Followed& followed = followed_[name];
      const std::string path = dir_ + "\\" + name;
      std::vector<nevr_boot_replay::Line> lines;
      int err = 0;
      if (!nevr_boot_replay::ReadNew(path.c_str(), runId_, followed.cursor, lines, &err)) {
        if (!followed.unreadableReported) {
          sink_.notice("[echovr_server] ERROR: log file " + path + " is not readable (errno: " + std::to_string(err) +
                       "); its lines are not shown");
          followed.unreadableReported = true;
        }
        continue;
      }
      if (!lines.empty() && !followed.announced) {
        sink_.notice("[echovr_server] Log file: " + path);
        followed.announced = true;
      }
      for (const nevr_boot_replay::Line& line : lines) {
        const std::string text = nevr_server_log::RenderLine(line);
        if (!text.empty()) sink_.line(text);
      }
      if (sink_.flush) sink_.flush();
    }
  }

 private:
  struct Followed {
    nevr_boot_replay::Cursor cursor;
    bool announced = false;
    bool unreadableReported = false;
  };

  std::string dir_;
  std::string runId_;
  unsigned long long since_;
  FollowerSink sink_;
  std::map<std::string, Followed> followed_;
};

}  // namespace nevr_launcher
