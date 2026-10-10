#include "quest/diag/hwdump_report.h"

#include "quest/diag/hwdump_field.h"

#include <cstdio>

namespace nevr_quest::hwdump {
namespace {

using nlohmann::json;

json AnswerJson(const Answer& a) {
  json out = json::object();
  if (a.has_text) {
    out["text"] = std::string(a.text);
    if (a.text_truncated) out["text_truncated"] = true;
  }
  if (a.num_ints > 0) {
    json ints = json::array();
    for (std::size_t i = 0; i < a.num_ints; ++i) ints.push_back(a.ints[i]);
    out["ints"] = ints;
  }
  if (a.num_floats > 0) {
    json floats = json::array();
    for (std::size_t i = 0; i < a.num_floats; ++i) floats.push_back(a.floats[i]);
    out["floats"] = floats;
  }
  return out;
}

}  // namespace

DumpDecision Decide(std::uint64_t now_ms, std::uint64_t start_ms, bool initialize_seen, std::uint64_t initialize_ms) {
  if (initialize_seen && now_ms >= initialize_ms + kSettleAfterInitializeMs) {
    return {true, "after_vrapi_initialize"};
  }
  if (!initialize_seen && now_ms >= start_ms + kFallbackAfterStartMs) return {true, "fallback_no_vrapi_initialize"};
  return {};
}

json ComposeQueries(const RecordSnapshot* records, std::size_t num_records, const HookState* hooks,
                    std::size_t num_hooks, std::uint64_t overflow, std::uint64_t contended) {
  json out = json::object();
  json hook_list = json::array();
  for (std::size_t i = 0; i < num_hooks; ++i) {
    const HookState& h = hooks[i];
    char slot[24];
    std::snprintf(slot, sizeof slot, "0x%llx", static_cast<unsigned long long>(h.slot));
    hook_list.push_back({{"symbol", h.symbol},
                         {"slot", slot},
                         {"installed", h.installed},
                         {"status", h.status},
                         {"calls", h.calls},
                         {"faults", h.faults}});
  }
  out["hooks"] = Ok("GotHook install + CallbackThunk counters", hook_list);
  json list = json::array();
  for (std::size_t i = 0; i < num_records; ++i) {
    const RecordSnapshot& r = records[i];
    json e = json::object();
    e["function"] = QueryFnName(r.fn);
    e["id"] = r.id;
    if (r.name[0] != '\0') {
      e["name"] = std::string(r.name);
      if (r.name_truncated) e["name_truncated"] = true;
    }
    e["calls"] = r.calls;
    e["first"] = AnswerJson(r.first);
    e["last"] = AnswerJson(r.last);
    if (!r.last_current) e["last_is_first"] = true;  // writers held `last` throughout the copy
    list.push_back(e);
  }
  out["records"] = Ok("libr15 GOT hooks (hwdump_handlers.cpp)", list);
  out["table"] = Ok("RecordTable", {{"capacity", kMaxRecords}, {"overflow", overflow}, {"contended", contended}});
  out["not_hooked"] = Ok("design (#335)",
                         json::array({{{"query", "fopen(\"/proc/cpuinfo\") in CSysInfo::GetNumberOfProcessors"},
                                       {"reason", "fopen is libr15's file I/O path; os.cpu.cpuinfo_raw reads the same file"}},
                                      {{"query", "JNI android.os.Build.SERIAL in CSysInfo::SerialNumber"},
                                       {"reason", "a JNI field read, not a GOT import; jni.build has every Build field"}},
                                      {{"query", "getifaddrs in CSysNet"},
                                       {"reason", "the answer is a linked list the game frees; os.net.ifaddrs reads it"}}}));
  return out;
}

std::string LogLine(const WriteSummary& s) {
  std::string line = "hwdump ";
  line += s.written ? "written" : "write_failed";
  line += " stage=" + s.stage + " path=" + s.path;
  if (!s.written) line += " error=" + s.write_error;
  line += " fields=" + std::to_string(s.fields) + " failed=" + std::to_string(s.failed) +
          " hooks_installed=" + std::to_string(s.hooks_installed) + "/" + std::to_string(s.hooks_total) +
          " hook_faults=" + std::to_string(s.hook_faults) + " records=" + std::to_string(s.records) +
          " overflow=" + std::to_string(s.overflow);
  return line;
}

}  // namespace nevr_quest::hwdump
