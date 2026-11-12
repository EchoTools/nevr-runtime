#include "runtime/maps/probe/probe_logic.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <iterator>

#include "nevr/symbol_hash.h"

namespace nevr_maps_probe {

namespace {

constexpr std::int32_t kStateServerLoading = 6;
constexpr std::int32_t kStateLoadingLevel = 7;
constexpr std::int32_t kStateInGame = 9;

std::string_view Trim(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
  return s;
}

std::string_view Unquote(std::string_view s) {
  if (s.size() >= 2 && (s.front() == '"' || s.front() == '\'') && s.back() == s.front()) return s.substr(1, s.size() - 2);
  return s;
}

std::string Lower(std::string_view s) {
  std::string out(s);
  for (char& c : out) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  return out;
}

bool ParseSwitch(const std::string& value, bool* out) {
  if (value == "on" || value == "true" || value == "yes" || value == "1") {
    *out = true;
    return true;
  }
  if (value == "off" || value == "false" || value == "no" || value == "0") {
    *out = false;
    return true;
  }
  return false;
}

bool ParseExpect(const std::string& value, ProbeConfig* config) {
  if (value == "none") {
    config->skew_expect = SkewExpect::kNone;
    return true;
  }
  if (value == "missing") {
    config->skew_expect = SkewExpect::kMissing;
    return true;
  }
  constexpr std::string_view kPrefix = "probe@";
  if (value.size() <= kPrefix.size() || value.size() > kPrefix.size() + 2 || value.compare(0, kPrefix.size(), kPrefix) != 0)
    return false;
  std::uint32_t version = 0;
  for (std::size_t i = kPrefix.size(); i < value.size(); ++i) {
    if (value[i] < '0' || value[i] > '9') return false;
    version = version * 10 + static_cast<std::uint32_t>(value[i] - '0');
  }
  config->skew_expect = SkewExpect::kVersion;
  config->skew_version = version;
  return true;
}

std::string Format(const char* format, ...) {
  char buffer[256];
  va_list args;
  va_start(args, format);
  const int n = std::vsnprintf(buffer, sizeof(buffer), format, args);
  va_end(args);
  if (n < 0) return std::string();
  return std::string(buffer, std::min<std::size_t>(static_cast<std::size_t>(n), sizeof(buffer) - 1));
}

unsigned long long U(std::uint64_t v) { return static_cast<unsigned long long>(v); }

}  // namespace

ProbeConfig ParseProbeConfig(std::string_view text) {
  ProbeConfig config;
  constexpr std::string_view kBom = "\xEF\xBB\xBF";
  if (text.substr(0, kBom.size()) == kBom) text.remove_prefix(kBom.size());
  std::size_t pos = 0;
  while (pos <= text.size()) {
    std::size_t end = text.find('\n', pos);
    if (end == std::string_view::npos) end = text.size();
    std::string_view line = text.substr(pos, end - pos);
    pos = end + 1;
    const std::size_t hash = line.find('#');
    if (hash != std::string_view::npos) line = line.substr(0, hash);
    line = Trim(line);
    if (line.empty()) continue;
    const std::size_t colon = line.find(':');
    if (colon == std::string_view::npos) {
      ++config.ignored_lines;
      continue;
    }
    const std::string key = Lower(Trim(line.substr(0, colon)));
    const std::string value = Lower(Unquote(Trim(line.substr(colon + 1))));
    bool on = false;
    if (key == "rules" && ParseSwitch(value, &on)) {
      config.rules = on;
    } else if (key == "skew" && ParseSwitch(value, &on)) {
      config.skew = on;
    } else if (key == "sublevel" && ParseSwitch(value, &on)) {
      config.sublevel = on;
    } else if (!(key == "skew_expect" && ParseExpect(value, &config))) {
      ++config.ignored_lines;
    }
  }
  return config;
}

const char* SkewExpectName(SkewExpect expect) {
  switch (expect) {
    case SkewExpect::kNone: return "none";
    case SkewExpect::kMissing: return "missing";
    case SkewExpect::kVersion: return "version";
  }
  return "none";
}

void DiffLevels(const std::vector<std::uint64_t>& before, const std::vector<std::uint64_t>& after,
                std::vector<std::uint64_t>* added, std::vector<std::uint64_t>* removed) {
  added->clear();
  removed->clear();
  std::set_difference(after.begin(), after.end(), before.begin(), before.end(), std::back_inserter(*added));
  std::set_difference(before.begin(), before.end(), after.begin(), after.end(), std::back_inserter(*removed));
}

std::string HexLevel(std::uint64_t level) { return Format("0x%016llx", U(level)); }

void SublevelMatcher::OnB(bool enable, std::uint64_t level, std::uint64_t frame) {
  ++b_events_;
  pending_.push_back({enable, level, frame});
}

void SublevelMatcher::OnA(bool add, std::uint64_t level, std::uint64_t frame, std::vector<std::string>* lines) {
  const auto it = std::find_if(pending_.begin(), pending_.end(),
                               [&](const Pending& p) { return p.enable == add && p.level == level; });
  if (it == pending_.end()) {
    ++extra_;
    lines->push_back(Format("probe_sub extra kind=%s level=%s frame=%llu", add ? "add" : "del", HexLevel(level).c_str(),
                            U(frame)));
    return;
  }
  if (frame <= it->frame + 1) {
    ++matched_;
  } else {
    ++late_;
    lines->push_back(Format("probe_sub late kind=%s level=%s lag=%llu", add ? "enable" : "disable",
                            HexLevel(level).c_str(), U(frame - it->frame)));
  }
  pending_.erase(it);
}

void SublevelMatcher::Expire(std::uint64_t frame, bool all, std::vector<std::string>* lines) {
  auto keep = pending_.begin();
  for (const Pending& p : pending_) {
    if (all || frame > p.frame + kMissingAfterFrames) {
      ++missing_;
      lines->push_back(Format("probe_sub missing kind=%s level=%s frame=%llu", p.enable ? "enable" : "disable",
                              HexLevel(p.level).c_str(), U(p.frame)));
    } else {
      *keep++ = p;
    }
  }
  pending_.erase(keep, pending_.end());
}

std::string SublevelMatcher::Summary() const {
  return Format("probe_sub summary b_events=%u a_matched=%u a_late=%u a_missing=%u a_extra=%u pending=%u", b_events_,
                matched_, late_, missing_, extra_, static_cast<unsigned>(pending_.size()));
}

std::uint64_t ProbePin(std::uint32_t version) {
  char name[24];
  std::snprintf(name, sizeof(name), "probe@%u", version);
  return EchoVR::CSymbol64Hash(name);
}

int ProbeVersionOf(std::uint64_t pin) {
  if (pin == 0) return -1;
  for (std::uint32_t v = 0; v <= kMaxProbeVersion; ++v) {
    if (ProbePin(v) == pin) return static_cast<int>(v);
  }
  return -1;
}

void SkewClient::ResetSession() {
  key_ = SkewInput();
  loaded_ = false;
  decided_ = false;
  leave_pending_ = false;
}

void SkewClient::OnEnable(std::uint64_t level, std::uint64_t frame, std::vector<std::string>* lines) {
  if (loaded_ || level == 0 || level != key_.level) return;
  loaded_ = true;
  lines->push_back(Format("probe_skew loaded level=%s frame=%llu", HexLevel(level).c_str(), U(frame)));
}

SkewAction SkewClient::Tick(const SkewInput& in, std::uint64_t frame, std::vector<std::string>* lines) {
  SkewAction action;
  if (!have_seen_ || in.lobby != seen_.lobby || in.level != seen_.level || in.pin != seen_.pin) {
    if (in.lobby != 0 && (in.level != 0 || in.pin != 0)) {
      lines->push_back(Format("probe_skew seen level=%s pin=%s frame=%llu state=%d", HexLevel(in.level).c_str(),
                              HexLevel(in.pin).c_str(), U(frame), in.state));
    }
    have_seen_ = true;
    seen_ = in;
  }
  if (in.state == kStateLoadingLevel && last_state_ != kStateLoadingLevel) {
    lines->push_back(Format("probe_skew loading level=%s frame=%llu", HexLevel(in.level).c_str(), U(frame)));
  }
  last_state_ = in.state;

  const bool in_session = in.lobby != 0 && in.state >= kStateServerLoading && in.state <= kStateInGame;
  if (!in_session) {
    ResetSession();
    return action;
  }
  if (in.lobby != key_.lobby || in.level != key_.level) loaded_ = false;
  if (in.lobby != key_.lobby || in.level != key_.level || in.pin != key_.pin) {
    decided_ = false;
    leave_pending_ = false;
    key_ = in;
  }

  if (!decided_) {
    decided_ = true;
    if (config_.skew_expect != SkewExpect::kNone) {
      const int version = ProbeVersionOf(in.pin);
      std::string notice;
      if (in.pin == 0) {
        lines->push_back(Format("probe_skew unpinned level=%s frame=%llu", HexLevel(in.level).c_str(), U(frame)));
      } else if (config_.skew_expect == SkewExpect::kMissing) {
        notice = version >= 0 ? Format("This match uses probe version %d: not installed", version)
                              : std::string("This match uses a probe version that is not installed");
      } else if (version != static_cast<int>(config_.skew_version)) {
        notice = version >= 0 ? Format("This match uses probe version %d (yours: %u)", version, config_.skew_version)
                              : Format("This match uses an unknown probe version (yours: %u)", config_.skew_version);
      } else {
        lines->push_back(Format("probe_skew match version=%d level=%s frame=%llu", version, HexLevel(in.level).c_str(),
                                U(frame)));
      }
      if (!notice.empty()) {
        leave_pending_ = true;
        leave_deadline_ = frame + kLeaveRetryFrames;
        notice_ = notice;
        notice_written_ = false;
      }
    }
  }
  if (leave_pending_) {
    action.leave = true;
    if (!notice_written_) {
      action.notice = notice_;
      notice_written_ = true;
    }
  }
  return action;
}

void SkewClient::OnLeaveResult(bool scheduled, std::uint64_t frame, std::vector<std::string>* lines) {
  if (!leave_pending_) return;
  if (scheduled) {
    leave_pending_ = false;
    lines->push_back(Format("probe_skew leave reason=map_skew before_load=%d frame=%llu", loaded_ ? 0 : 1, U(frame)));
  } else if (frame >= leave_deadline_) {
    leave_pending_ = false;
    lines->push_back(Format("probe_skew leave reason=map_skew failed=1 before_load=%d frame=%llu", loaded_ ? 0 : 1,
                            U(frame)));
  }
}

}  // namespace nevr_maps_probe
