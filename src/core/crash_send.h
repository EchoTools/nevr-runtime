#pragma once
// Crash report send (#1689): on every start, before the game connects to anything, the readable report a crash left in
// the crash folder (core/crash_report.h) goes to nevr-api over HTTPS, once.
//
// WHERE IT RUNS. In the lowest layer of each platform, which loads first and depends on nothing above it: on PC the
// BugSplat64.dll host (runtime/host/crash_send_host.cpp, from the detour on the game's main wrapper, before it loads
// nevr-runtime.dll); on Quest the sentinel (quest/integration/crash_send_step.cpp, from the constructor step that arms the
// crash reporter). So a report is sent whether or not the runtime loads, crashed, or ever logs in. It does not use the
// game service, a login, the remote-log channel or any runtime state.
//
// ONE IMPLEMENTATION. This header holds everything above the transport: find the pending reports, read the one text file,
// reduce it to the allowlist below, build the request body, classify the answer, mark the file. Each platform supplies
// only the POST (PC: WinHTTP loaded by name, because the host's imports are pinned to kernel32, crypt32 and advapi32 by
// tools/verify_pe_contract.py and it links no TLS stack; Quest: the sentinel's libcurl client), the random bytes for the
// install id and a log sink. Platform neutral and header only: no Windows header, no socket, no game state, no logger.
//
// WHAT GOES OUT (the allowlist; nothing else is ever read into the body, and test_crash_send.cpp fails on any other key):
//   schema, install_id, report_id, build, platform, headset, signal{name,number,code,fault_address},
//   pc{module,offset,build_id,symbol,symbol_offset}, lr{same, Quest only}, backtrace[{module,offset,symbol}] (32 at most).
// The text report only. Never the minidump (its stack memory can hold tokens, chat and names), never the registers, the
// thread name, the log tail the report carries, an account id, an IP, a path or a user name. The install id is 128 random
// bits made on the first send and kept in the crash folder; it derives from nothing about the person or the machine.
//
// ONCE. A 2xx marks the report sent (`<stem>.report.txt.sent`). 400, 413 and 422 mark it rejected
// (`.report.txt.rejected`) and it is never offered again, as does a report this parser cannot read. Anything else (429, 5xx,
// 404, no connection, a timeout) leaves the file for the next start. A 2xx lost on the way back leads to a second send of the
// same (install_id, report_id), which the service counts once.
//
// NEVER DELAYS THE GAME. The caller runs SendPending on a thread of its own. It stops at the first transport failure (a dead
// network is not retried for the other reports), sends at most kMaxReportsPerPass reports and stops starting new requests
// after Config::budget. Each request carries a connect and a total timeout.

#include <algorithm>
#include <charconv>
#include <chrono>
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

#include "core/crash_report.h"

namespace nevr_crash_send {

namespace fs = std::filesystem;

// The service's route: nevr-api, POST /v3/crash-reports, no credential, behind the portal's /api prefix. Compiled in
// because the sender runs before any config, runtime or login exists.
inline constexpr const char* kCrashReportUrl = "https://echovrce.com/api/v3/crash-reports";
// The route is live (#1689): the send is on. The one constant that switches it off for a build; while false,
// ConfiguredUrl() is empty, neither platform starts a thread or writes a record.
inline constexpr bool kSendEnabled = true;
inline const char* ConfiguredUrl() { return kSendEnabled ? kCrashReportUrl : ""; }

inline constexpr int kSchema = 1;
inline constexpr const char* kSentSuffix = ".sent";
inline constexpr const char* kRejectedSuffix = ".rejected";
inline constexpr const char* kInstallIdName = "install-id.txt";

inline constexpr std::size_t kMaxReportsPerPass = 3;
inline constexpr std::size_t kMaxEntriesScanned = 512;
inline constexpr std::size_t kMaxReportBytes = 64 * 1024;  // the writer's buffer is 32 KiB (nevr_crash::kReportCapacity)
inline constexpr std::size_t kMaxFrames = 32;
inline constexpr std::size_t kMaxBodyBytes = 16 * 1024;    // the service takes 32 KiB
inline constexpr std::size_t kMaxSymbolBytes = 256;
inline constexpr std::size_t kMaxModuleBytes = 128;

struct Timeouts {
  long connectMs = 3000;
  long totalMs = 6000;
};

struct PostResult {
  bool transportOk = false;  // an HTTP status was obtained
  long status = 0;
  long errorCode = 0;        // the platform's own code when transportOk is false (GetLastError, CURLcode)
};

// POSTs `body` as application/json to `url` without a credential, verifying the server certificate, following no
// redirect, within `timeouts`. Must not throw, must not block past them.
using PostFn = PostResult (*)(void* context, const std::string& url, const std::string& body, const Timeouts& timeouts);
// Fills `bytes` with `count` random bytes; false when it cannot.
using RandomFn = bool (*)(std::uint8_t* bytes, std::size_t count);

// One thing that happened, for the platform's own log. `event` is one of: off, sent, rejected, deferred, unreadable,
// install_id_unavailable, pass. `report` is the report id ("" for off, install_id_unavailable and pass). The text of a
// report and the body are never passed.
struct LogEvent {
  const char* event = "";
  std::string report;
  long status = 0;
  long errorCode = 0;
  std::uint64_t bytes = 0;  // the request body; for `pass`, the reports left for the next start
  std::string detail;
};
using LogFn = void (*)(const LogEvent& event);

struct Config {
  fs::path dir;          // the crash folder; empty turns the send off
  std::string url;       // empty turns the send off; must be https unless allowPlainHttp
  std::string build;     // the build string
  std::string platform;  // "pc" or "quest"
  std::string headset;   // the headset model; "" when the layer knows none
  PostFn post = nullptr;
  void* postContext = nullptr;
  RandomFn random = nullptr;
  LogFn log = nullptr;
  Timeouts timeouts;
  std::chrono::milliseconds budget{10000};
  bool allowPlainHttp = false;  // tests against a loopback server only
};

struct Outcome {
  std::size_t sent = 0;
  std::size_t rejected = 0;
  std::size_t deferred = 0;  // left for the next start
};

// ---- the allowlist's value rules (the service decodes strictly and answers 400 for a value outside them) ----

inline bool InSet(char c, bool allowSpace) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-' ||
         c == '+' || (allowSpace && c == ' ');
}

inline bool AllInSet(std::string_view text, std::size_t minLen, std::size_t maxLen, bool allowSpace) {
  if (text.size() < minLen || text.size() > maxLen) return false;
  for (char c : text) {
    if (!InSet(c, allowSpace)) return false;
  }
  return true;
}

// [A-Za-z0-9._-]{min,max}: a report id or a build id (no plus, unlike a module or a build string).
inline bool IsPlainId(std::string_view text, std::size_t minLen, std::size_t maxLen) {
  return AllInSet(text, minLen, maxLen, false) && text.find('+') == std::string_view::npos;
}

inline bool IsHex(std::string_view text) {
  if (text.empty()) return false;
  for (char c : text) {
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return false;
  }
  return true;
}

// "0x" and 1 to 16 hex digits.
inline bool IsHexValue(std::string_view text) {
  return text.size() >= 3 && text.size() <= 18 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X') && IsHex(text.substr(2));
}

// Letters, digits and underscore only: a signal or exception name.
inline bool IsSignalName(std::string_view text) {
  if (text.empty() || text.size() > 64) return false;
  for (char c : text) {
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')) return false;
  }
  return true;
}

inline bool IsInstallId(std::string_view text) {
  if (text.size() != 32) return false;
  for (char c : text) {
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  }
  return true;
}

// ---- the report text, reduced to the allowlist ----

struct Address {
  std::string module;  // "unknown" when no module held the address
  std::string offset;  // 0x...
  std::string buildId;  // "none" when the module carries none (pc and lr only)
  std::string symbol;   // "" when none
  std::string symbolOffset;  // "" when none
};

struct Parsed {
  std::string signalName;
  std::int64_t signalNumber = 0;
  std::int64_t signalCode = 0;
  std::string faultAddress;  // "" when the report says none
  Address pc;
  std::optional<Address> lr;
  std::vector<Address> backtrace;
};

inline std::string_view TrimEnd(std::string_view s) {
  while (!s.empty() && (s.back() == '\r' || s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
  return s;
}

inline bool ParseSigned(std::string_view text, std::int64_t* out) {
  if (text.empty()) return false;
  bool negative = false;
  if (text.front() == '-') {
    negative = true;
    text.remove_prefix(1);
  }
  int base = 10;
  if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
    base = 16;
    text.remove_prefix(2);
  }
  std::uint64_t value = 0;
  const auto result = std::from_chars(text.data(), text.data() + text.size(), value, base);
  if (result.ec != std::errc() || result.ptr != text.data() + text.size()) return false;
  if (value > static_cast<std::uint64_t>(INT64_MAX)) return false;
  *out = negative ? -static_cast<std::int64_t>(value) : static_cast<std::int64_t>(value);
  return true;
}

// "module+0xoffset" as the report writes it. A module that is not a plain file name (the writer prints `?` when no
// module holds the address) is sent as "unknown", so a path or odd text from a module table never reaches the service.
inline bool ParseModuleOffset(std::string_view token, Address* out) {
  const std::size_t plus = token.rfind("+0x");
  if (plus == std::string_view::npos || plus == 0) return false;
  const std::string_view module = token.substr(0, plus);
  const std::string_view offset = token.substr(plus + 1);
  if (!IsHexValue(offset)) return false;
  out->module = AllInSet(module, 1, kMaxModuleBytes, false) ? std::string(module) : std::string("unknown");
  out->offset = std::string(offset);
  return true;
}

// `symbol` or `symbol+0xoff` or `?`. A symbol the service would refuse (a slash, a control byte, too long) is left out.
inline void ParseSymbol(std::string_view text, Address* out) {
  out->symbol.clear();
  out->symbolOffset.clear();
  text = TrimEnd(text);
  if (text.empty() || text == "?") return;
  std::string_view symbol = text;
  std::string_view offset;
  const std::size_t plus = text.rfind("+0x");
  if (plus != std::string_view::npos && plus > 0 && IsHexValue(text.substr(plus + 1))) {
    symbol = text.substr(0, plus);
    offset = text.substr(plus + 1);
  }
  if (symbol.size() > kMaxSymbolBytes) return;
  for (char c : symbol) {
    const unsigned char u = static_cast<unsigned char>(c);
    if (u < 0x20 || u >= 0x7f || c == '/' || c == '\\') return;
  }
  out->symbol = std::string(symbol);
  out->symbolOffset = std::string(offset);
}

// `pc: module+0xoff build_id=<id|none> symbol=<rest of line>`.
inline bool ParseAddressLine(std::string_view rest, Address* out) {
  const std::size_t space = rest.find(' ');
  if (space == std::string_view::npos) return false;
  if (!ParseModuleOffset(rest.substr(0, space), out)) return false;
  rest.remove_prefix(space + 1);
  static constexpr std::string_view kBuildId = "build_id=";
  static constexpr std::string_view kSymbol = " symbol=";
  if (rest.substr(0, kBuildId.size()) != kBuildId) return false;
  rest.remove_prefix(kBuildId.size());
  const std::size_t symbolAt = rest.find(kSymbol);
  if (symbolAt == std::string_view::npos) return false;
  const std::string_view id = rest.substr(0, symbolAt);
  out->buildId = IsPlainId(id, 1, 128) ? std::string(id) : std::string("none");
  ParseSymbol(rest.substr(symbolAt + kSymbol.size()), out);
  return true;
}

// `signal: NAME (NUMBER) code=N fault_addr=0x...|none`.
inline bool ParseSignalLine(std::string_view rest, Parsed* out) {
  const std::size_t open = rest.find(" (");
  if (open == std::string_view::npos) return false;
  const std::string_view name = rest.substr(0, open);
  if (!IsSignalName(name)) return false;
  rest.remove_prefix(open + 2);
  const std::size_t close = rest.find(')');
  if (close == std::string_view::npos) return false;
  if (!ParseSigned(rest.substr(0, close), &out->signalNumber)) return false;
  rest.remove_prefix(close + 1);
  static constexpr std::string_view kCode = " code=";
  static constexpr std::string_view kFault = " fault_addr=";
  if (rest.substr(0, kCode.size()) != kCode) return false;
  rest.remove_prefix(kCode.size());
  const std::size_t faultAt = rest.find(kFault);
  if (faultAt == std::string_view::npos) return false;
  if (!ParseSigned(rest.substr(0, faultAt), &out->signalCode)) return false;
  const std::string_view fault = TrimEnd(rest.substr(faultAt + kFault.size()));
  out->signalName = std::string(name);
  out->faultAddress = IsHexValue(fault) ? std::string(fault) : std::string();
  return true;
}

// `  #N module+0xoff symbol`.
inline bool ParseFrameLine(std::string_view line, Address* out) {
  if (line.substr(0, 3) != "  #") return false;
  line.remove_prefix(3);
  const std::size_t space = line.find(' ');
  if (space == std::string_view::npos) return false;
  line.remove_prefix(space + 1);
  const std::size_t second = line.find(' ');
  const std::string_view token = second == std::string_view::npos ? line : line.substr(0, second);
  if (!ParseModuleOffset(TrimEnd(token), out)) return false;
  out->buildId.clear();
  ParseSymbol(second == std::string_view::npos ? std::string_view() : line.substr(second + 1), out);
  out->symbolOffset.clear();  // the service's frame has no symbol offset
  return true;
}

// Reads the allowlisted facts out of a report. The backtrace is read only between its own heading and the next one, so a
// logged line that looks like a frame (the report ends with the last log lines, indented the same way) is never taken
// for one. nullopt when the report does not carry a signal line and a pc line: nothing to group it by.
inline std::optional<Parsed> ParseReport(std::string_view text) {
  Parsed parsed;
  bool haveSignal = false;
  bool havePc = false;
  bool inBacktrace = false;
  while (!text.empty()) {
    const std::size_t end = text.find('\n');
    const std::string_view line = TrimEnd(text.substr(0, end));
    text = end == std::string_view::npos ? std::string_view() : text.substr(end + 1);
    if (inBacktrace) {
      Address frame;
      if (line.substr(0, 3) == "  #") {
        if (parsed.backtrace.size() < kMaxFrames && ParseFrameLine(line, &frame)) parsed.backtrace.push_back(std::move(frame));
        continue;
      }
      inBacktrace = false;
    }
    if (line.substr(0, 8) == "signal: " && !haveSignal) {
      haveSignal = ParseSignalLine(line.substr(8), &parsed);
    } else if (line.substr(0, 4) == "pc: " && !havePc) {
      havePc = ParseAddressLine(line.substr(4), &parsed.pc);
    } else if (line.substr(0, 4) == "lr: " && !parsed.lr) {
      Address lr;
      if (ParseAddressLine(line.substr(4), &lr)) parsed.lr = std::move(lr);
    } else if (line == "backtrace:") {
      inBacktrace = true;
    } else if (line.substr(0, 5) == "log (") {
      break;  // everything after is the log tail
    }
  }
  if (!haveSignal || !havePc) return std::nullopt;
  return parsed;
}

// ---- the request body ----

inline nlohmann::json AddressJson(const Address& a, bool withBuildId, bool withSymbolOffset) {
  nlohmann::json j;
  j["module"] = a.module;
  j["offset"] = a.offset;
  if (withBuildId) j["build_id"] = a.buildId;
  j["symbol"] = a.symbol;
  if (withSymbolOffset) j["symbol_offset"] = a.symbolOffset;
  return j;
}

inline std::string Sanitized(std::string_view text, std::size_t maxLen, bool allowSpace, const char* fallback) {
  return AllInSet(text, 1, maxLen, allowSpace) ? std::string(text) : std::string(fallback);
}

// The body for one report. The one place a request body is made: every value comes from `parsed` or from the
// identity strings, each checked against the rule the service enforces.
inline std::string BuildBody(const Parsed& parsed, const std::string& installId, const std::string& reportId,
                             const std::string& build, const std::string& platform, const std::string& headset) {
  nlohmann::json body;
  body["schema"] = kSchema;
  body["install_id"] = installId;
  body["report_id"] = reportId;
  body["build"] = Sanitized(build, 64, false, "unknown");
  body["platform"] = platform == "quest" ? "quest" : "pc";
  body["headset"] = AllInSet(headset, 1, 64, true) ? headset : std::string();
  nlohmann::json signal;
  signal["name"] = parsed.signalName;
  signal["number"] = parsed.signalNumber;
  signal["code"] = parsed.signalCode;
  signal["fault_address"] = parsed.faultAddress;
  body["signal"] = signal;
  body["pc"] = AddressJson(parsed.pc, true, true);
  if (parsed.lr) body["lr"] = AddressJson(*parsed.lr, true, true);
  nlohmann::json frames = nlohmann::json::array();
  for (const Address& frame : parsed.backtrace) frames.push_back(AddressJson(frame, false, false));
  body["backtrace"] = std::move(frames);
  return body.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

// ---- the crash folder ----

struct Pending {
  fs::path report;
  std::string stem;  // the report id the service sees
};

inline bool EndsWith(const std::string& text, const std::string& suffix) {
  return text.size() >= suffix.size() && text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// The pending reports in `dir`, oldest first: a regular file named <stem>.report.txt whose stem the service accepts as
// a report id. Looks at most kMaxEntriesScanned entries. A directory that cannot be read has none.
inline std::vector<Pending> FindPending(const fs::path& dir) {
  std::vector<std::pair<fs::file_time_type, Pending>> found;
  std::error_code ec;
  fs::directory_iterator it(dir, ec);
  if (ec) return {};
  const std::size_t suffixLength = std::char_traits<char>::length(nevr_crash::kReportSuffix);
  std::size_t looked = 0;
  const fs::directory_iterator end;
  for (; it != end && looked < kMaxEntriesScanned; it.increment(ec), ++looked) {
    if (ec) break;
    const std::string name = it->path().filename().u8string();
    if (!EndsWith(name, nevr_crash::kReportSuffix) || name.size() == suffixLength) continue;
    std::error_code fileEc;
    if (!it->is_regular_file(fileEc) || fileEc) continue;
    Pending p;
    p.report = it->path();
    p.stem = name.substr(0, name.size() - suffixLength);
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

// Reads at most `maxBytes` of a file. False when it cannot be read.
inline bool ReadFileBounded(const fs::path& path, std::size_t maxBytes, std::string* out) {
  std::error_code ec;
  const std::uintmax_t size = fs::file_size(path, ec);
  if (ec) return false;
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  const std::size_t want = static_cast<std::size_t>(std::min<std::uintmax_t>(size, maxBytes));
  out->assign(want, '\0');
  in.read(&(*out)[0], static_cast<std::streamsize>(want));
  out->resize(static_cast<std::size_t>(in.gcount()));
  return true;
}

// <stem>.report.txt becomes <stem>.report.txt<suffix>, which FindPending no longer matches.
inline bool Mark(const Pending& pending, const char* suffix) {
  fs::path to = pending.report;
  to += suffix;
  std::error_code ec;
  fs::rename(pending.report, to, ec);
  return !ec;
}

// The install id: 32 lowercase hex characters in `<dir>/install-id.txt`, made from 16 random bytes the first time a
// report is to be sent. Empty when it cannot be read or made (then nothing is sent: a send without it could not be
// counted once).
inline std::string LoadOrCreateInstallId(const fs::path& dir, RandomFn random) {
  const fs::path path = dir / kInstallIdName;
  std::string text;
  if (ReadFileBounded(path, 64, &text)) {
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) text.pop_back();
    if (IsInstallId(text)) return text;
  }
  if (random == nullptr) return std::string();
  std::uint8_t bytes[16] = {};
  if (!random(bytes, sizeof(bytes))) return std::string();
  static const char kHex[] = "0123456789abcdef";
  std::string id;
  for (std::uint8_t b : bytes) {
    id.push_back(kHex[b >> 4]);
    id.push_back(kHex[b & 0xf]);
  }
  fs::path temp = path;
  temp += ".tmp";
  {
    std::ofstream out(temp, std::ios::binary | std::ios::trunc);
    if (!out) return std::string();
    out << id << '\n';
    out.flush();
    if (!out) return std::string();
  }
  std::error_code ec;
  fs::rename(temp, path, ec);
  if (ec) {
    fs::remove(temp, ec);
    return std::string();
  }
  return id;
}

inline void Emit(const Config& config, const char* event, const std::string& report = std::string(), long status = 0,
                 long errorCode = 0, std::uint64_t bytes = 0, const std::string& detail = std::string()) {
  if (config.log == nullptr) return;
  LogEvent e;
  e.event = event;
  e.report = report;
  e.status = status;
  e.errorCode = errorCode;
  e.bytes = bytes;
  e.detail = detail;
  config.log(e);
}

// What an answer means for the report that was sent.
enum class Verdict { kAccepted, kRejected, kRetryLater };

inline Verdict Classify(const PostResult& result) {
  if (!result.transportOk) return Verdict::kRetryLater;
  if (result.status >= 200 && result.status < 300) return Verdict::kAccepted;
  if (result.status == 400 || result.status == 413 || result.status == 422) return Verdict::kRejected;
  return Verdict::kRetryLater;
}

// One pass over the crash folder: at most kMaxReportsPerPass reports, oldest first, inside Config::budget. A pass that
// found reports ends with one `pass` event, which says what was sent and rejected and how many are left; a folder with none
// writes nothing.
inline Outcome SendPending(const Config& config) {
  Outcome outcome;
  if (config.dir.empty() || config.url.empty() || config.post == nullptr) {
    Emit(config, "off", std::string(), 0, 0, 0,
         config.url.empty() ? "the send is switched off in this build (kSendEnabled)" : (config.dir.empty() ? "no crash folder" : "no transport"));
    return outcome;
  }
  const bool https = config.url.rfind("https://", 0) == 0;
  if (!https && !(config.allowPlainHttp && config.url.rfind("http://", 0) == 0)) {
    Emit(config, "off", std::string(), 0, 0, 0, "the service URL is not https");
    return outcome;
  }
  const auto deadline = std::chrono::steady_clock::now() + config.budget;
  const std::vector<Pending> pending = FindPending(config.dir);
  if (pending.empty()) return outcome;  // nothing crashed since the last start: no record, every start would write one
  std::size_t taken = 0;
  std::string installId;
  bool stop = false;
  for (const Pending& item : pending) {
    if (stop) break;
    if (taken >= kMaxReportsPerPass || std::chrono::steady_clock::now() >= deadline) break;
    ++taken;
    std::string text;
    std::optional<Parsed> parsed;
    if (ReadFileBounded(item.report, kMaxReportBytes, &text)) parsed = ParseReport(text);
    if (!IsPlainId(item.stem, 1, 96)) parsed.reset();
    if (!parsed) {
      // A report this parser cannot read will not be readable next start either.
      Mark(item, kRejectedSuffix);
      ++outcome.rejected;
      Emit(config, "unreadable", item.stem, 0, 0, text.size(), "the report has no signal and pc line, or its name is not a report id");
      continue;
    }
    if (installId.empty()) installId = LoadOrCreateInstallId(config.dir, config.random);
    if (installId.empty()) {
      Emit(config, "install_id_unavailable", item.stem, 0, 0, 0, "the install id could not be read or made; nothing is sent");
      ++outcome.deferred;
      break;
    }
    const std::string body = BuildBody(*parsed, installId, item.stem, config.build, config.platform, config.headset);
    if (body.size() > kMaxBodyBytes) {
      Mark(item, kRejectedSuffix);
      ++outcome.rejected;
      Emit(config, "rejected", item.stem, 0, 0, body.size(), "the body is over the size the service takes");
      continue;
    }
    Timeouts timeouts = config.timeouts;
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
    timeouts.totalMs = std::max<long>(1000, std::min<long>(timeouts.totalMs, static_cast<long>(left.count())));
    timeouts.connectMs = std::min(timeouts.connectMs, timeouts.totalMs);
    const PostResult result = config.post(config.postContext, config.url, body, timeouts);
    switch (Classify(result)) {
      case Verdict::kAccepted:
        ++outcome.sent;
        if (!Mark(item, kSentSuffix)) {
          Emit(config, "sent", item.stem, result.status, 0, body.size(),
               "the report could not be renamed to .sent; the service counts a second send once");
        } else {
          Emit(config, "sent", item.stem, result.status, 0, body.size());
        }
        break;
      case Verdict::kRejected:
        Mark(item, kRejectedSuffix);
        ++outcome.rejected;
        Emit(config, "rejected", item.stem, result.status, 0, body.size(), "the service refused the report for good; it is not offered again");
        break;
      case Verdict::kRetryLater:
        ++outcome.deferred;
        stop = true;  // a dead network or a busy service: the next start tries again
        Emit(config, "deferred", item.stem, result.status, result.errorCode, body.size(),
             result.transportOk ? "the service did not take the report; it stays for the next start"
                                : "no answer from the service; it stays for the next start");
        break;
    }
  }
  const std::size_t left = pending.size() > outcome.sent + outcome.rejected ? pending.size() - outcome.sent - outcome.rejected : 0;
  Emit(config, "pass", std::string(), 0, 0, left, "sent=" + std::to_string(outcome.sent) + " rejected=" + std::to_string(outcome.rejected));
  return outcome;
}

}  // namespace nevr_crash_send
