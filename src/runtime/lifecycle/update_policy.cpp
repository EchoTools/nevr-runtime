#include "runtime/lifecycle/update_policy.h"

#include <algorithm>
#include <cctype>

#include <nlohmann/json.hpp>

#include "core/update/names.h"

namespace nevr_update {
namespace {

using nlohmann::json;
namespace names = nevr_update_names;

bool IsSha256Hex(const std::string& s) {
  return s.size() == 64 && std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isxdigit(c) != 0 && !std::isupper(c); });
}

bool IsChannelName(const std::string& s) {
  if (s.empty() || s.size() > 32) return false;
  return std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isalnum(c) != 0 || c == '.' || c == '_' || c == '-'; });
}

// A short, plain word: the action and reason fields of the core's result go into a log line.
bool IsWord(const std::string& s) {
  return s.size() <= 64 && std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isalnum(c) != 0 || c == '_' || c == '-' || c == '.'; });
}

std::optional<json> ParseObject(const std::string& text) {
  json doc = json::parse(text, nullptr, false);
  if (doc.is_discarded() || !doc.is_object()) return std::nullopt;
  return doc;
}

}  // namespace

const char* DisabledWord(Disabled reason) {
  switch (reason) {
    case Disabled::kNone: return "none";
    case Disabled::kDevBuild: return "dev_build";
    case Disabled::kNoRoot: return "no_root";
    case Disabled::kFlag: return "flag";
    case Disabled::kConfig: return "config";
    case Disabled::kHostCannotSwap: return "host_cannot_swap";
  }
  return "none";
}

Disabled Decide(const Switches& s) {
  if (s.dev_build) return Disabled::kDevBuild;
  if (s.no_root) return Disabled::kNoRoot;
  if (s.flag_no_update) return Disabled::kFlag;
  if (s.config_enabled.has_value() && !*s.config_enabled) return Disabled::kConfig;
  if (!s.host_can_swap) return Disabled::kHostCannotSwap;
  return Disabled::kNone;
}

const char* ChannelSourceWord(ChannelSource source) {
  switch (source) {
    case ChannelSource::kConfig: return "config";
    case ChannelSource::kState: return "state";
    case ChannelSource::kDefault: return "default";
  }
  return "default";
}

Channel ResolveChannel(const std::optional<std::string>& config_channel, const std::optional<std::string>& state_channel) {
  Channel out;
  if (config_channel.has_value() && !config_channel->empty()) {
    if (IsChannelName(*config_channel)) {
      out.name = *config_channel;
      out.source = ChannelSource::kConfig;
      return out;
    }
    out.config_rejected = true;
  }
  if (state_channel.has_value() && IsChannelName(*state_channel)) {
    out.name = *state_channel;
    out.source = ChannelSource::kState;
  }
  return out;
}

std::string BuildCurrentJson(const std::string& version, const std::string& runtime_sha256,
                             const std::vector<QuarantineEntry>& quarantine) {
  json q = json::array();
  for (const QuarantineEntry& e : quarantine) q.push_back({{"version", e.version}, {"runtime_sha256", e.runtime_sha256}});
  return json{{"version", version}, {"runtime_sha256", runtime_sha256}, {"quarantine", q}}.dump();
}

bool ParseStageResult(const std::string& text, StageResult* out, std::string* error) {
  const auto fail = [error](const char* why) {
    if (error != nullptr) *error = why;
    return false;
  };
  const std::optional<json> doc = ParseObject(text);
  if (!doc) return fail("the stage result is not a JSON object");
  const auto str = [&](const char* key, std::string* into) {
    if (!doc->contains(key) || !(*doc)[key].is_string()) return false;
    *into = (*doc)[key].get<std::string>();
    return true;
  };
  StageResult r;
  if (!str("action", &r.action) || !str("version", &r.version)) return fail("the stage result has no action or version");
  if (r.action != "none" && r.action != "staged" && r.action != "blocked") return fail("the stage result's action is not none, staged or blocked");
  if (!names::IsVersionName(r.version)) return fail("the stage result's version is outside the version vocabulary");
  if (doc->contains("direction") && (*doc)["direction"].is_string()) r.direction = (*doc)["direction"].get<std::string>();
  if (doc->contains("reason") && (*doc)["reason"].is_string()) r.reason = (*doc)["reason"].get<std::string>();
  if (!IsWord(r.direction) || !IsWord(r.reason)) return fail("the stage result's direction or reason is not a plain word");
  if (doc->contains("files")) {
    if (!(*doc)["files"].is_array()) return fail("the stage result's files is not a list");
    for (const json& f : (*doc)["files"]) {
      StagedFile file;
      names::Member member;
      if (!f.is_object() || !f.contains("path") || !f["path"].is_string() || !f.contains("size") || !f["size"].is_number_unsigned() ||
          !f.contains("sha256") || !f["sha256"].is_string()) {
        return fail("a file of the stage result has no path, size and sha256");
      }
      file.path = f["path"].get<std::string>();
      file.size = f["size"].get<std::uint64_t>();
      file.sha256 = f["sha256"].get<std::string>();
      if (!names::ParseMember(file.path, &member)) return fail("a file of the stage result is outside the member vocabulary");
      if (!IsSha256Hex(file.sha256)) return fail("a file of the stage result has a sha256 that is not 64 lowercase hex characters");
      r.files.push_back(std::move(file));
    }
  }
  int runtimes = 0;
  for (const StagedFile& f : r.files) runtimes += f.path == names::kRuntimeMember ? 1 : 0;
  if (r.action == "staged" && runtimes != 1) return fail("a staged result must list exactly one runtime");
  *out = std::move(r);
  return true;
}

std::string BuildStagedJson(const StageResult& staged) {
  json files = json::array();
  for (const StagedFile& f : staged.files) files.push_back({{"path", f.path}, {"size", f.size}, {"sha256", f.sha256}});
  return json{{"version", staged.version}, {"files", files}}.dump();
}

bool ParsePendingTarget(const std::string& text, PendingTarget* out) {
  const std::optional<json> doc = ParseObject(text);
  if (!doc || !doc->contains("to") || !(*doc)["to"].is_object()) return false;
  const json& to = (*doc)["to"];
  if (!to.contains("version") || !to["version"].is_string() || !to.contains("sha256") || !to["sha256"].is_string()) return false;
  PendingTarget t;
  t.version = to["version"].get<std::string>();
  t.sha256 = to["sha256"].get<std::string>();
  if (!names::IsVersionName(t.version) || !IsSha256Hex(t.sha256)) return false;
  *out = std::move(t);
  return true;
}

std::optional<std::string> StateChannel(const std::string& state_text) {
  const std::optional<json> doc = ParseObject(state_text);
  if (!doc || !doc->contains("channel") || !(*doc)["channel"].is_string()) return std::nullopt;
  return (*doc)["channel"].get<std::string>();
}

std::vector<QuarantineEntry> StateQuarantine(const std::string& state_text) {
  std::vector<QuarantineEntry> out;
  const std::optional<json> doc = ParseObject(state_text);
  if (!doc || !doc->contains("quarantine") || !(*doc)["quarantine"].is_array()) return out;
  for (const json& e : (*doc)["quarantine"]) {
    if (!e.is_object() || !e.contains("version") || !e["version"].is_string() || !e.contains("runtime_sha256") ||
        !e["runtime_sha256"].is_string()) {
      continue;
    }
    out.push_back({e["version"].get<std::string>(), e["runtime_sha256"].get<std::string>()});
  }
  return out;
}

bool ConfirmState(const std::string& state_text, const std::string& version, const std::string& runtime_sha256, std::string* out) {
  std::optional<json> doc = ParseObject(state_text);
  if (!doc) return false;
  (*doc)["current"] = {{"version", version}, {"runtime_sha256", runtime_sha256}};
  *out = doc->dump();
  return true;
}

bool ConfirmDue(std::uint64_t first_ms, std::uint64_t now_ms) { return now_ms >= first_ms && now_ms - first_ms >= kConfirmAfterMs; }

}  // namespace nevr_update
