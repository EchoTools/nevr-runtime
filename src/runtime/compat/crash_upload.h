#pragma once
// Crash report upload (#1605): on the first logged-in session after a crash, the readable report the crash
// reporter wrote next to the minidump goes to the game service once, over the path the build's logs already take.
//
// No new endpoint. The remote-log channel exists on both platforms (docs/engine/remote-log.md): one
// SNSRemoteLogSetv3 frame on the login connection, which the service journals per user (server/evr_remotelogset.go,
// server/evr_remotelog_journal.go). The self-check unit already owns that sender and the logged-in user
// (compat/self_check.h); this file rides it through RegisterFlushHook and SendRemoteLogStrings, so the PC runtime
// and the Quest sentinel run the same code and only supply the crash directory, the build string and a log sink.
//
// Platform neutral and header-only, like party_share_check.h: no Windows headers, no sockets, no game state.
//
// What it reads, in the crash directory (PC: %LOCALAPPDATA%\nevr\echovr\crashes, Quest: nevr-crashes):
//   <stem>.report.txt   the readable report (the crash reporter writes it; kReportSuffix names the one spelling)
//   <stem>.dmp          the minidump beside it (or, when the report is named after the dump, <stem> ends in .dmp)
// What it writes: when the frame is handed to the login connection, the report is renamed to
// <stem>.report.txt.sent, which is the mark, so a later start finds nothing to send. The minidump stays.
//
// What goes out, as one frame (the service takes at most 50 strings and 256 KiB per set): the report text in
// chunks of at most kReportChunkBytes, each one JSON string
//   {"message":"nevr_crash_report","message_type":"NEVR_CRASH_REPORT","userid":"OVR-ORG-1","report_id":"<stem>",
//    "build":"5.7.0","part":"report","chunk":1,"chunks":2,"data":"..."}
// and, when its size allows (kMaxMinidumpBytes raw), the minidump as base64 chunks of the same shape with
// "part":"minidump","encoding":"base64","bytes":N. The service keeps an unknown `message` as a generic remote log
// and journals it per user; none of the names its filter drops.
//
// Once, and what "once" is: a report is marked sent when the sender accepted the frame. A remote log carries no
// acknowledgement (the frame is sent without the require flag), so a frame lost after the sender took it is lost;
// the report file is then renamed and the local minidump remains. A sender that refuses leaves the report in place
// and the next pass (kRetryInterval later, or the next session) tries again. Within one process a report is never
// offered twice even when its rename fails.
//
// Threading: Pump runs on whatever thread calls nevr_self_check::Flush (PC: the per-frame tick; Quest: the
// token-auth poll), at most kMaxReportsPerPass reports per pass, each file read bounded. It does file I/O and
// allocates, so it never runs under the loader lock or inside a game function (Flush does not either).

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "runtime/compat/self_check.h"

namespace nevr_crash_upload {

inline constexpr const char* kReportSuffix = ".report.txt";
inline constexpr const char* kMinidumpSuffix = ".dmp";
inline constexpr const char* kSentSuffix = ".sent";

inline constexpr std::size_t kMaxReportsPerPass = 3;
inline constexpr std::size_t kMaxEntriesScanned = 512;           // directory entries looked at in one pass
inline constexpr std::size_t kMaxReportBytes = 48 * 1024;        // report text sent; the rest is cut and said so
inline constexpr std::size_t kReportChunkBytes = 16 * 1024;
inline constexpr std::size_t kMaxMinidumpBytes = 96 * 1024;      // raw; 128 KiB of base64
inline constexpr std::size_t kMinidumpChunkChars = 32 * 1024;    // base64 characters per string; a multiple of 4
inline constexpr std::size_t kMaxSetPayloadBytes = 224 * 1024;   // all strings of a frame, under the service's 256 KiB
inline constexpr std::size_t kMinReportBytes = 1024;             // the report budget never shrinks below this
inline constexpr std::chrono::seconds kRetryInterval{5};

namespace fs = std::filesystem;

struct Pending {
  fs::path report;
  fs::path minidump;  // empty when no minidump sits beside the report
  std::string stem;   // the report id the service sees
};

struct Built {
  std::vector<std::string> strings;
  std::uint64_t reportBytes = 0;        // size of the report file
  bool reportTruncated = false;         // the text sent is shorter than the file
  std::uint64_t minidumpFileBytes = 0;  // size of the minidump file; 0 when there is none
  std::uint64_t minidumpBytes = 0;      // raw bytes of it in the frame; 0 when it was left out
  std::string minidumpState;            // "included", "none", "too_large", "unreadable" or "frame_budget"
};

// One thing that happened, for the platform's own log. `event` is one of: armed, off, sent, not_sent, unreadable,
// mark_failed. `detail` says why in words for the ones that fail.
struct LogEvent {
  const char* event = "";
  std::string report;  // the report id, or the crash directory for armed/off
  std::uint64_t reportBytes = 0;
  std::uint64_t minidumpBytes = 0;
  std::string minidump;  // Built::minidumpState
  std::string detail;
};
using LogFn = void (*)(const LogEvent& event);

struct Config {
  fs::path dir;       // the crash directory; empty turns the upload off
  std::string build;  // the build string every string carries
  LogFn log = nullptr;
};

inline std::string Base64(const std::uint8_t* data, std::size_t size) {
  static const char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve((size + 2) / 3 * 4);
  std::size_t i = 0;
  for (; i + 3 <= size; i += 3) {
    const std::uint32_t v = (std::uint32_t{data[i]} << 16) | (std::uint32_t{data[i + 1]} << 8) | data[i + 2];
    out.push_back(kAlphabet[(v >> 18) & 63]);
    out.push_back(kAlphabet[(v >> 12) & 63]);
    out.push_back(kAlphabet[(v >> 6) & 63]);
    out.push_back(kAlphabet[v & 63]);
  }
  if (size - i == 1) {
    const std::uint32_t v = std::uint32_t{data[i]} << 16;
    out.push_back(kAlphabet[(v >> 18) & 63]);
    out.push_back(kAlphabet[(v >> 12) & 63]);
    out.append("==");
  } else if (size - i == 2) {
    const std::uint32_t v = (std::uint32_t{data[i]} << 16) | (std::uint32_t{data[i + 1]} << 8);
    out.push_back(kAlphabet[(v >> 18) & 63]);
    out.push_back(kAlphabet[(v >> 12) & 63]);
    out.push_back(kAlphabet[(v >> 6) & 63]);
    out.push_back('=');
  }
  return out;
}

inline bool EndsWith(const std::string& text, const std::string& suffix) {
  return text.size() >= suffix.size() && text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// The pending reports in `dir`, oldest first: a regular file named <stem>.report.txt. Looks at most
// kMaxEntriesScanned entries. A directory that cannot be read has none.
inline std::vector<Pending> FindPending(const fs::path& dir) {
  std::vector<std::pair<fs::file_time_type, Pending>> found;
  std::error_code ec;
  fs::directory_iterator it(dir, ec);
  if (ec) return {};
  std::size_t looked = 0;
  const fs::directory_iterator end;
  for (; it != end && looked < kMaxEntriesScanned; it.increment(ec), ++looked) {
    if (ec) break;
    const std::string name = it->path().filename().u8string();
    if (!EndsWith(name, kReportSuffix) || name.size() == std::char_traits<char>::length(kReportSuffix)) continue;
    std::error_code fileEc;
    if (!it->is_regular_file(fileEc) || fileEc) continue;
    Pending p;
    p.report = it->path();
    p.stem = name.substr(0, name.size() - std::char_traits<char>::length(kReportSuffix));
    // The report may be named after the dump (<dump>.report.txt) or after the dump's stem (<stem>.report.txt).
    const std::string dumpName = EndsWith(p.stem, kMinidumpSuffix) ? p.stem : p.stem + kMinidumpSuffix;
    const fs::path dump = dir / fs::u8path(dumpName);
    std::error_code dumpEc;
    if (fs::is_regular_file(dump, dumpEc) && !dumpEc) p.minidump = dump;
    fs::file_time_type when = fs::last_write_time(p.report, fileEc);
    if (fileEc) when = fs::file_time_type::min();
    found.emplace_back(when, std::move(p));
  }
  std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) {
    return a.first != b.first ? a.first < b.first : a.second.stem < b.second.stem;
  });
  std::vector<Pending> out;
  out.reserve(found.size());
  for (auto& entry : found) out.push_back(std::move(entry.second));
  return out;
}

// Reads at most `maxBytes` of a file. `fileBytes` gets its full size. False when it cannot be read.
inline bool ReadFileBounded(const fs::path& path, std::size_t maxBytes, std::string* out, std::uint64_t* fileBytes) {
  std::error_code ec;
  const std::uintmax_t size = fs::file_size(path, ec);
  if (ec) return false;
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  const std::size_t want = static_cast<std::size_t>(std::min<std::uintmax_t>(size, maxBytes));
  out->assign(want, '\0');
  in.read(&(*out)[0], static_cast<std::streamsize>(want));
  out->resize(static_cast<std::size_t>(in.gcount()));
  if (fileBytes != nullptr) *fileBytes = size;
  return true;
}

// The end of the piece of `text` that starts at `from` and holds at most `maxLen` bytes, moved back so a UTF-8
// sequence is not cut (a continuation byte is 10xxxxxx).
inline std::size_t PieceEnd(const std::string& text, std::size_t from, std::size_t maxLen) {
  std::size_t end = std::min(text.size(), from + maxLen);
  if (end == text.size()) return end;
  while (end > from + 1 && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80) --end;
  return end;
}

inline std::string DumpJson(const nlohmann::json& j) {
  // Bytes that are not UTF-8 (a report quoting a log line) must not make the dump throw: replace them.
  return j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

inline std::size_t TotalBytes(const std::vector<std::string>& strings) {
  std::size_t total = 0;
  for (const std::string& s : strings) total += s.size();
  return total;
}

inline std::string UserIdText(const nevr_evr_codec::UserId& user) {
  return std::string(nevr_evr_codec::PlatformPrefix(user.platformCode)) + "-" + std::to_string(user.accountId);
}

// Builds the strings of one report's frame. False when the report itself cannot be read (an unreadable minidump
// only leaves the minidump out).
inline bool Build(const Pending& pending, const nevr_evr_codec::UserId& user, const std::string& build, Built* out) {
  *out = Built{};
  std::string text;
  if (!ReadFileBounded(pending.report, kMaxReportBytes, &text, &out->reportBytes)) return false;

  const auto entry = [&](const char* part) {
    nlohmann::json j;
    j["message"] = "nevr_crash_report";
    j["message_type"] = "NEVR_CRASH_REPORT";
    j["userid"] = UserIdText(user);
    j["report_id"] = pending.stem;
    if (!build.empty()) j["build"] = build;
    j["part"] = part;
    return j;
  };

  // JSON escaping can make a report several times its size, so the text budget shrinks until the frame fits.
  std::size_t limit = std::min(text.size(), kMaxReportBytes);
  for (;;) {
    const std::string body = text.substr(0, PieceEnd(text, 0, limit));
    std::vector<std::string> pieces;
    for (std::size_t from = 0; from < body.size();) {
      const std::size_t end = PieceEnd(body, from, kReportChunkBytes);
      pieces.push_back(body.substr(from, end - from));
      from = end;
    }
    if (pieces.empty()) pieces.emplace_back();
    out->strings.clear();
    for (std::size_t i = 0; i < pieces.size(); ++i) {
      nlohmann::json j = entry("report");
      j["chunk"] = i + 1;
      j["chunks"] = pieces.size();
      j["data"] = pieces[i];
      out->strings.push_back(DumpJson(j));
    }
    out->reportTruncated = out->reportBytes > body.size();
    if (TotalBytes(out->strings) <= kMaxSetPayloadBytes || limit <= kMinReportBytes) break;
    limit = std::max(kMinReportBytes, limit / 2);
  }

  if (pending.minidump.empty()) {
    out->minidumpState = "none";
    return true;
  }
  std::string dump;
  if (!ReadFileBounded(pending.minidump, kMaxMinidumpBytes + 1, &dump, &out->minidumpFileBytes)) {
    out->minidumpState = "unreadable";
    return true;
  }
  if (out->minidumpFileBytes > kMaxMinidumpBytes) {
    out->minidumpState = "too_large";
    return true;
  }
  const std::string encoded = Base64(reinterpret_cast<const std::uint8_t*>(dump.data()), dump.size());
  std::vector<std::string> dumpStrings;
  const std::size_t chunks = std::max<std::size_t>(1, (encoded.size() + kMinidumpChunkChars - 1) / kMinidumpChunkChars);
  for (std::size_t i = 0; i < chunks; ++i) {
    nlohmann::json j = entry("minidump");
    j["encoding"] = "base64";
    j["bytes"] = dump.size();
    j["chunk"] = i + 1;
    j["chunks"] = chunks;
    j["data"] = encoded.substr(i * kMinidumpChunkChars, kMinidumpChunkChars);
    dumpStrings.push_back(DumpJson(j));
  }
  if (TotalBytes(out->strings) + TotalBytes(dumpStrings) > kMaxSetPayloadBytes) {
    out->minidumpState = "frame_budget";
    return true;
  }
  for (std::string& s : dumpStrings) out->strings.push_back(std::move(s));
  out->minidumpBytes = dump.size();
  out->minidumpState = "included";
  return true;
}

// The mark: <stem>.report.txt becomes <stem>.report.txt.sent, which FindPending no longer matches.
inline bool MarkSent(const Pending& pending) {
  fs::path to = pending.report;
  to += kSentSuffix;
  std::error_code ec;
  fs::rename(pending.report, to, ec);
  return !ec;
}

struct State {
  std::mutex mutex;
  Config config;
  bool installed = false;
  std::set<std::string> handled;  // report ids this process already offered (or could not read): never offered twice
  std::chrono::steady_clock::time_point nextAttempt{};
  std::chrono::milliseconds retry{kRetryInterval};  // how long a pass that left reports behind waits
  std::atomic<bool> done{true};  // this session has nothing left to send; cleared by Install and by a session reset
  std::atomic<std::uint32_t> epoch{0};  // bumped by a session boundary, so a pass that spans one does not end the new session's look
  std::atomic<bool> busy{false};        // one pass at a time
};

// Never destroyed (the Quest sentinel may not register an atexit destructor, tools/check_quest_static_init.sh).
inline State& S() {
  static State* const state = new State();
  return *state;
}

inline void Emit(const Config& config, const char* event, const std::string& report, std::uint64_t reportBytes = 0,
                 std::uint64_t minidumpBytes = 0, const std::string& minidump = std::string(),
                 const std::string& detail = std::string()) {
  if (config.log == nullptr) return;
  LogEvent e;
  e.event = event;
  e.report = report;
  e.reportBytes = reportBytes;
  e.minidumpBytes = minidumpBytes;
  e.minidump = minidump;
  e.detail = detail;
  config.log(e);
}

// One pass over the directory for the logged-in user: at most kMaxReportsPerPass reports. Returns true when
// reports remain for a later pass (the pass hit its cap, or the sender refused), false when nothing is left.
inline bool UploadPending(const Config& config, std::set<std::string>* handled, std::size_t* sentCount) {
  *sentCount = 0;
  nevr_evr_codec::UserId user;
  if (!nevr_self_check::LoggedInUser(&user)) return true;
  std::size_t taken = 0;
  for (const Pending& pending : FindPending(config.dir)) {
    if (handled->count(pending.stem) != 0) continue;
    if (taken >= kMaxReportsPerPass) return true;
    ++taken;
    Built built;
    if (!Build(pending, user, config.build, &built)) {
      handled->insert(pending.stem);
      Emit(config, "unreadable", pending.stem, 0, 0, std::string(), "the report file could not be read");
      continue;
    }
    if (!nevr_self_check::SendRemoteLogStrings(built.strings, nevr_self_check::kRemoteLogLevelError)) {
      Emit(config, "not_sent", pending.stem, built.reportBytes, built.minidumpBytes, built.minidumpState,
           "the login connection did not take the frame; the report stays and is offered again");
      return true;
    }
    handled->insert(pending.stem);
    ++*sentCount;
    if (!MarkSent(pending)) {
      Emit(config, "mark_failed", pending.stem, built.reportBytes, built.minidumpBytes, built.minidumpState,
           "the report could not be renamed to .sent; it will not be offered again in this run");
    }
    Emit(config, "sent", pending.stem, built.reportBytes, built.minidumpBytes, built.minidumpState,
         built.reportTruncated ? "report text cut to the frame budget" : std::string());
  }
  return false;
}

// The self-check unit's flush hook: runs the upload at most once per session, and again after a pass that left
// reports behind, no sooner than kRetryInterval.
inline void Pump() {
  State& s = S();
  if (s.done.load(std::memory_order_acquire)) return;
  if (s.busy.exchange(true, std::memory_order_acquire)) return;
  Config config;
  std::set<std::string> handled;
  {
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.installed || std::chrono::steady_clock::now() < s.nextAttempt) {
      s.busy.store(false, std::memory_order_release);
      return;
    }
    config = s.config;
    handled = s.handled;
  }
  const std::uint32_t epoch = s.epoch.load(std::memory_order_acquire);
  std::size_t sentCount = 0;
  const bool more = UploadPending(config, &handled, &sentCount);
  {
    std::lock_guard<std::mutex> lock(s.mutex);
    s.handled = std::move(handled);
    if (more) s.nextAttempt = std::chrono::steady_clock::now() + s.retry;
  }
  if (!more && s.epoch.load(std::memory_order_acquire) == epoch) s.done.store(true, std::memory_order_release);
  s.busy.store(false, std::memory_order_release);
}

// A session boundary: the next logged-in session looks again (a report a refused send left, or one written since).
inline void OnSessionReset() {
  State& s = S();
  std::lock_guard<std::mutex> lock(s.mutex);
  s.nextAttempt = std::chrono::steady_clock::time_point{};
  s.epoch.fetch_add(1, std::memory_order_acq_rel);
  s.done.store(!s.installed, std::memory_order_release);
}

// Turns the upload on for this process. Call once, after the self-check unit's sender is set. An empty directory
// leaves it off and says so.
inline void Install(Config config) {
  State& s = S();
  if (config.dir.empty()) {
    Emit(config, "off", std::string(), 0, 0, std::string(), "no crash directory");
    return;
  }
  const std::string dir = config.dir.u8string();
  const Config announce = config;
  {
    std::lock_guard<std::mutex> lock(s.mutex);
    s.config = std::move(config);
    s.installed = true;
    s.nextAttempt = std::chrono::steady_clock::time_point{};
    s.done.store(false, std::memory_order_release);
  }
  nevr_self_check::RegisterSessionReset(&OnSessionReset);
  nevr_self_check::RegisterFlushHook(&Pump);
  Emit(announce, "armed", dir);
}

// Test seam: how long a pass that left reports behind waits before the next one.
inline void SetRetryIntervalForTest(std::chrono::milliseconds retry) {
  State& s = S();
  std::lock_guard<std::mutex> lock(s.mutex);
  s.retry = retry;
}

// Test seam: forgets the configuration and what was offered.
inline void ResetForTest() {
  State& s = S();
  std::lock_guard<std::mutex> lock(s.mutex);
  s.config = Config();
  s.installed = false;
  s.handled.clear();
  s.retry = kRetryInterval;
  s.nextAttempt = std::chrono::steady_clock::time_point{};
  s.done.store(true, std::memory_order_release);
  s.busy.store(false, std::memory_order_release);
}

}  // namespace nevr_crash_upload
