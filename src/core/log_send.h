#pragma once
// Player log upload (#1788, docs/adr/0030-player-log-upload.md): when the player taps "Send log", the last part of the current
// session's log file and of the previous session's go to nevr-api over HTTPS, and the service answers with a short code the
// player posts where a maintainer can read it.
//
// ONE IMPLEMENTATION, TWO BINARIES (#1598). This header holds everything above the transport: the fixed list of files, the
// bounded read of each file's tail, the request body, the 1 MiB cap, the classification of the answer and the check of the
// code. Each platform supplies only what it alone can: the list of file paths (its own log folder), the POST (PC: libcurl;
// Quest: the sentinel's curl client), the install id (nevr_crash_send::LoadOrCreateInstallId) and a log sink. Platform
// neutral and header only: no Windows header, no socket, no game state, no logger. The sink takes the finished line, so a
// platform routes it to its own logger (PC: Log() from core/logging.h, which Quest does not link).
//
// WHAT IS READ. Only the files named in Config::files, which the platform fixes in code. Nothing is read from a directory
// listing, and no other file (no minidump, no config, no credentials) is opened.
//
// CONSENT. Send does nothing without `consent`, which is the player's tap. There is no automatic or deferred upload.
//
// WHAT IS LOGGED (tag [NEVR.LOGSEND]). Never the text of a log or the body: one Info line with the file names and sizes, one
// Info line with the status and the code, a Warning with the transport code on a failure.
//
// THREADING. Send blocks for the duration of the POST (Config::timeouts). The caller runs it on a worker thread of its own
// with a teardown path, never on the game thread.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/crash_send.h"

namespace nevr_log_send {

namespace fs = std::filesystem;

// The service's route: nevr-api, POST /v3/player-logs, no credential, behind the portal's /api prefix. The platform puts
// it in Config::url once the service has the route (a 404 is "service not ready", see Status::kNotReady).
inline constexpr const char* kPlayerLogUrl = "https://echovrce.com/api/v3/player-logs";

inline constexpr int kSchema = 1;
inline constexpr std::size_t kMaxFiles = 4;
inline constexpr std::size_t kMaxTailBytes = 256 * 1024;   // the last part of each file that is sent
inline constexpr std::size_t kMaxBodyBytes = 1024 * 1024;  // the service takes 1 MiB
inline constexpr std::size_t kMaxAnswerBytes = 4096;       // the answer is {"code":"LOG-XXXX"}
inline constexpr std::size_t kMaxNameBytes = 64;
inline constexpr const char* kCodePrefix = "LOG-";
inline constexpr std::size_t kCodeSuffixLength = 4;
// The 32 symbols a code is drawn from: no I, L, O or U.
inline constexpr const char* kCodeAlphabet = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";

enum class Session { kCurrent, kPrevious };

// One file to collect. `name` is what the service sees (a plain file name); `path` is where the platform keeps it.
struct LogFile {
  std::string name;
  fs::path path;
  Session session = Session::kCurrent;
};

struct PostResult {
  bool transportOk = false;  // an HTTP status was obtained
  long status = 0;
  long errorCode = 0;        // the platform's own code when transportOk is false (GetLastError, CURLcode)
  std::string response;      // the answer's body; only read when the status is 2xx, and only its first kMaxAnswerBytes
};

// POSTs `body` as application/json to `url` without a credential, verifying the server certificate, following no redirect,
// within `timeouts`, and returns the answer's body. Must not throw, must not block past the timeouts.
using PostFn = PostResult (*)(void* context, const std::string& url, const std::string& body,
                              const nevr_crash_send::Timeouts& timeouts);

enum class LogLevel { kInfo, kWarning };
// Receives one finished line, tag included (`[NEVR.LOGSEND] ...`). Never the text of a log and never the body.
using LogFn = void (*)(LogLevel level, const std::string& line);

struct Config {
  std::vector<LogFile> files;  // fixed by the platform; at most kMaxFiles are read, in this order
  std::string url;             // must be https unless allowPlainHttp
  std::string installId;       // 32 lowercase hex (nevr_crash_send::LoadOrCreateInstallId)
  std::string build;           // the build string
  std::string platform;        // "pc" or "quest"
  std::string headset;         // the headset model; "" when the layer knows none
  PostFn post = nullptr;
  void* postContext = nullptr;
  LogFn log = nullptr;
  nevr_crash_send::Timeouts timeouts{5000, 20000};  // a 1 MiB body needs more than the crash report's 6 s
  bool allowPlainHttp = false;                      // tests against a loopback server only
};

// What a Send did. Each value but kSent means nothing was uploaded and no code exists.
enum class Status {
  kNoConsent,       // the player did not tap: nothing was read, nothing was sent
  kNotConfigured,   // no https url, no transport or no valid install id: nothing was read, nothing was sent
  kNoFiles,         // none of the files could be read
  kTooLarge,        // the body is over kMaxBodyBytes even without the previous session's file
  kTransportFailed, // no HTTP answer (no connection, a timeout, a certificate that did not verify)
  kNotReady,        // HTTP 404: the service has no route for this yet; never success
  kRefused,         // any other HTTP status that is not 2xx
  kBadAnswer,       // 2xx, but the answer holds no valid code
  kSent,            // 2xx with a valid code
};

struct Result {
  Status status = Status::kNoConsent;
  std::string code;             // set only when status is kSent
  long httpStatus = 0;          // the HTTP status, when one was obtained
  long errorCode = 0;           // the platform's code, when status is kTransportFailed
  bool droppedPrevious = false; // the previous session's file was left out to fit kMaxBodyBytes; tell the player
  std::size_t files = 0;        // files in the body
  std::size_t bodyBytes = 0;
};

inline const char* StatusName(Status status) {
  switch (status) {
    case Status::kNoConsent: return "no_consent";
    case Status::kNotConfigured: return "not_configured";
    case Status::kNoFiles: return "no_files";
    case Status::kTooLarge: return "too_large";
    case Status::kTransportFailed: return "transport_failed";
    case Status::kNotReady: return "service_not_ready";
    case Status::kRefused: return "refused";
    case Status::kBadAnswer: return "bad_answer";
    case Status::kSent: return "sent";
  }
  return "unknown";
}

// ---- the code ----

// "LOG-" and four characters of kCodeAlphabet, upper case, nothing else.
inline bool IsValidCode(std::string_view code) {
  const std::string_view prefix = kCodePrefix;
  if (code.size() != prefix.size() + kCodeSuffixLength) return false;
  if (code.substr(0, prefix.size()) != prefix) return false;
  const std::string_view alphabet = kCodeAlphabet;
  for (char c : code.substr(prefix.size())) {
    if (alphabet.find(c) == std::string_view::npos) return false;
  }
  return true;
}

// The code in the service's answer `{"code":"LOG-XXXX"}`; nullopt for anything else. Never throws.
inline std::optional<std::string> ParseAnswer(std::string_view answer) {
  if (answer.size() > kMaxAnswerBytes) return std::nullopt;
  const nlohmann::json j = nlohmann::json::parse(answer.begin(), answer.end(), nullptr, /*allow_exceptions=*/false);
  const auto it = j.find("code");  // end() for anything that is not an object with a code, and for text that is not JSON
  if (it == j.end() || !it->is_string()) return std::nullopt;
  const std::string code = it->get<std::string>();
  if (!IsValidCode(code)) return std::nullopt;
  return code;
}

// ---- reading ----

// A file as it goes into the body.
struct Collected {
  std::string name;
  Session session = Session::kCurrent;
  bool truncated = false;  // the head was cut: the file is longer than kMaxTailBytes
  std::string text;
};

// Reads the last `maxBytes` of a file. False when it cannot be read.
inline bool ReadTail(const fs::path& path, std::size_t maxBytes, std::string* text, bool* truncated) {
  std::error_code ec;
  const std::uintmax_t size = fs::file_size(path, ec);
  if (ec) return false;
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  const bool cut = size > maxBytes;
  const std::size_t want = cut ? maxBytes : static_cast<std::size_t>(size);
  if (cut) {
    in.seekg(static_cast<std::streamoff>(size - maxBytes), std::ios::beg);
    if (!in) return false;
  }
  text->assign(want, '\0');
  in.read(&(*text)[0], static_cast<std::streamsize>(want));
  text->resize(static_cast<std::size_t>(in.gcount()));
  *truncated = cut;
  return true;
}

// The files of `list`, in order, up to kMaxFiles that can be read. A name the service would refuse and a file that cannot be
// read (the previous session's may not exist) are left out. Opens nothing but the listed paths.
inline std::vector<Collected> Collect(const std::vector<LogFile>& list) {
  std::vector<Collected> out;
  for (const LogFile& file : list) {
    if (out.size() >= kMaxFiles) break;
    if (!nevr_crash_send::IsPlainId(file.name, 1, kMaxNameBytes)) continue;
    Collected c;
    c.name = file.name;
    c.session = file.session;
    if (!ReadTail(file.path, kMaxTailBytes, &c.text, &c.truncated)) continue;
    out.push_back(std::move(c));
  }
  return out;
}

// ---- the request body ----

inline const char* SessionName(Session session) { return session == Session::kPrevious ? "previous" : "current"; }

// The body for `files`. The one place a request body is made. A byte that is not UTF-8 in a log becomes U+FFFD.
inline std::string BuildBody(const Config& config, const std::vector<Collected>& files) {
  nlohmann::json body;
  body["schema"] = kSchema;
  body["install_id"] = config.installId;
  body["build"] = nevr_crash_send::Sanitized(config.build, 64, false, "unknown");
  body["platform"] = config.platform == "quest" ? "quest" : "pc";
  body["headset"] = nevr_crash_send::AllInSet(config.headset, 1, 64, true) ? config.headset : std::string();
  nlohmann::json list = nlohmann::json::array();
  for (const Collected& file : files) {
    nlohmann::json entry;
    entry["name"] = file.name;
    entry["session"] = SessionName(file.session);
    entry["truncated"] = file.truncated;
    entry["text"] = file.text;
    list.push_back(std::move(entry));
  }
  body["files"] = std::move(list);
  return body.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

struct Bounded {
  std::string body;               // empty when nothing fits
  std::vector<Collected> sent;    // the files that are in `body`
  bool droppedPrevious = false;
};

// The body under kMaxBodyBytes. One that is over it is made again without the previous session's files, which the player
// is told. Still over it, or nothing but previous files: empty, and the caller sends nothing.
inline Bounded BuildBounded(const Config& config, const std::vector<Collected>& files) {
  Bounded out;
  std::string body = BuildBody(config, files);
  if (body.size() <= kMaxBodyBytes) {
    out.body = std::move(body);
    out.sent = files;
    return out;
  }
  std::vector<Collected> current;
  for (const Collected& file : files) {
    if (file.session == Session::kCurrent) current.push_back(file);
  }
  if (current.empty()) return out;
  body = BuildBody(config, current);
  if (body.size() > kMaxBodyBytes) return out;
  out.body = std::move(body);
  out.sent = std::move(current);
  out.droppedPrevious = true;
  return out;
}

// ---- logging ----

inline void Emit(const Config& config, LogLevel level, const std::string& line) {
  if (config.log == nullptr) return;
  config.log(level, line);
}

inline const char* ConfigProblem(const Config& config) {
  if (config.post == nullptr) return "no transport";
  const bool https = config.url.rfind("https://", 0) == 0;
  if (!https && !(config.allowPlainHttp && config.url.rfind("http://", 0) == 0)) return "the service url is not https";
  if (!nevr_crash_send::IsInstallId(config.installId)) return "no valid install id";
  return nullptr;
}

// `name:size:session:cut|whole` for each file; names are plain file names and sizes are numbers, never text from the log.
inline std::string FileList(const std::vector<Collected>& files) {
  std::string list;
  for (const Collected& file : files) {
    if (!list.empty()) list += ',';
    list += file.name + ':' + std::to_string(file.text.size()) + ':' + SessionName(file.session) + ':' +
            (file.truncated ? "cut" : "whole");
  }
  return list;
}

// ---- the send ----

// One upload, started by the player's tap. Without `consent` nothing is read, sent or logged. Otherwise: collect the files,
// build the body, POST it once, and read the answer. Blocks for the POST; the caller runs it off the game thread.
inline Result Send(const Config& config, bool consent) {
  Result result;
  if (!consent) {
    result.status = Status::kNoConsent;
    return result;
  }
  if (const char* problem = ConfigProblem(config)) {
    result.status = Status::kNotConfigured;
    Emit(config, LogLevel::kWarning,
         std::string("[NEVR.LOGSEND] log send not started status=") + StatusName(result.status) + " reason=\"" + problem +
             "\" so nothing was read or sent");
    return result;
  }
  const std::vector<Collected> collected = Collect(config.files);
  if (collected.empty()) {
    result.status = Status::kNoFiles;
    Emit(config, LogLevel::kWarning,
         std::string("[NEVR.LOGSEND] log send not started status=") + StatusName(result.status) + " reason=\"none of the " +
             std::to_string(config.files.size()) + " log files could be read\" so nothing was sent");
    return result;
  }
  const Bounded bounded = BuildBounded(config, collected);
  if (bounded.body.empty()) {
    result.status = Status::kTooLarge;
    Emit(config, LogLevel::kWarning,
         std::string("[NEVR.LOGSEND] log send not started status=") + StatusName(result.status) +
             " reason=\"the body is over " + std::to_string(kMaxBodyBytes) +
             " bytes even without the previous session\" so nothing was sent files=" + FileList(collected));
    return result;
  }
  result.droppedPrevious = bounded.droppedPrevious;
  result.files = bounded.sent.size();
  result.bodyBytes = bounded.body.size();
  Emit(config, LogLevel::kInfo,
       "[NEVR.LOGSEND] log collected files=" + FileList(bounded.sent) + " body_bytes=" + std::to_string(result.bodyBytes) +
           " dropped_previous=" + (result.droppedPrevious ? "true" : "false"));

  const PostResult answer = config.post(config.postContext, config.url, bounded.body, config.timeouts);
  if (!answer.transportOk) {
    result.status = Status::kTransportFailed;
    result.errorCode = answer.errorCode;
    Emit(config, LogLevel::kWarning,
         std::string("[NEVR.LOGSEND] log send failed status=") + StatusName(result.status) +
             " error=" + std::to_string(answer.errorCode) + " so no code was issued; the player can tap again");
    return result;
  }
  result.httpStatus = answer.status;
  if (answer.status >= 200 && answer.status < 300) {
    const std::optional<std::string> code = ParseAnswer(answer.response);
    if (!code) {
      result.status = Status::kBadAnswer;
      Emit(config, LogLevel::kWarning,
           std::string("[NEVR.LOGSEND] log send failed status=") + StatusName(result.status) +
               " http=" + std::to_string(answer.status) + " answer_bytes=" + std::to_string(answer.response.size()) +
               " so no code was issued");
      return result;
    }
    result.status = Status::kSent;
    result.code = *code;
    Emit(config, LogLevel::kInfo,
         std::string("[NEVR.LOGSEND] log send result status=") + StatusName(result.status) +
             " http=" + std::to_string(answer.status) + " code=" + *code);
    return result;
  }
  if (answer.status == 404) {
    result.status = Status::kNotReady;
    Emit(config, LogLevel::kWarning,
         std::string("[NEVR.LOGSEND] log send failed status=") + StatusName(result.status) +
             " http=404 the service has no route for log uploads yet, so nothing was stored");
    return result;
  }
  result.status = Status::kRefused;
  Emit(config, LogLevel::kWarning,
       std::string("[NEVR.LOGSEND] log send failed status=") + StatusName(result.status) +
           " http=" + std::to_string(answer.status) + " so no code was issued");
  return result;
}

}  // namespace nevr_log_send
