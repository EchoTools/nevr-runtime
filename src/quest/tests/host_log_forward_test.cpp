// Host test for how the Quest runtime library forwards its records to the shim's log (quest/runtime/host_log_forward.h): the level
// mappings, and what the first tag, the first message, the integer fields, a skipped null key and a record longer than the cap
// become in the ABI's call.

#include <cstdint>
#include <cstring>
#include <iterator>
#include <string>
#include <vector>

#include "quest/runtime/host_log_forward.h"
#include "quest/tests/test_check.h"

namespace {

namespace fwd = nevr_quest::host_log_forward;

struct Seen {
  int calls = 0;
  std::uint32_t level = 0;
  bool tagNull = false, messageNull = false;
  std::string tag, event, message;
  std::vector<std::pair<std::string, std::string>> fields;
};
Seen g_seen;

void FakeHostLog(std::uint32_t level, const char* tag, const char* event, const char* message, const NevrQuestLogField* fields,
                 std::uint32_t count) {
  ++g_seen.calls;
  g_seen.level = level;
  g_seen.tagNull = tag == nullptr;
  g_seen.messageNull = message == nullptr;
  g_seen.tag = tag != nullptr ? tag : "";
  g_seen.event = event;
  g_seen.message = message != nullptr ? message : "";
  g_seen.fields.clear();
  for (std::uint32_t i = 0; i < count; ++i) g_seen.fields.emplace_back(fields[i].key, fields[i].value);
}

void LevelMappings() {
  namespace rl = nevr_quest::runtime_load;
  QCHECK(fwd::HostLevel(sentinel::LogLevel::kDebug) == rl::kLogLevelDebug);
  QCHECK(fwd::HostLevel(sentinel::LogLevel::kInfo) == rl::kLogLevelInfo);
  QCHECK(fwd::HostLevel(sentinel::LogLevel::kWarn) == rl::kLogLevelWarning);
  QCHECK(fwd::HostLevel(sentinel::LogLevel::kError) == rl::kLogLevelError);
  QCHECK(fwd::UpdateLevel(nevr_update::kDebug) == sentinel::LogLevel::kDebug);
  QCHECK(fwd::UpdateLevel(nevr_update::kInfo) == sentinel::LogLevel::kInfo);
  QCHECK(fwd::UpdateLevel(nevr_update::kWarning) == sentinel::LogLevel::kWarn);
  QCHECK(fwd::UpdateLevel(nevr_update::kError) == sentinel::LogLevel::kError);
  constexpr int kOutOfRangeLevel = 99;
  QCHECK(fwd::UpdateLevel(kOutOfRangeLevel) == sentinel::LogLevel::kInfo);  // an unknown engine level is not dropped or raised
  constexpr int kNegativeLevel = -1;
  QCHECK(fwd::UpdateLevel(kNegativeLevel) == sentinel::LogLevel::kInfo);
}

void NoHostLogLeavesTheRecordToTheCopysWriter() {
  g_seen = Seen();
  const sentinel::LogField fields[] = {{"tag", "NEVR.TEST"}};
  QCHECK(!fwd::ForwardRecord(nullptr, sentinel::LogLevel::kInfo, "event", fields, std::size(fields)));
  QCHECK(g_seen.calls == 0);
}

void TagAndMessageAreLiftedOutAndTheRestFollowInOrder() {
  g_seen = Seen();
  constexpr long long kNumber = -42;
  constexpr long long kSecondNumber = 7;
  const sentinel::LogField fields[] = {{"first", "a"}, {"tag", "NEVR.UPDATE"}, {"count", kNumber},
                                       {"message", "staged"}, {"tag", "second tag stays a field"}, {"message", "second message stays a field"},
                                       {"last", kSecondNumber}};
  QCHECK(fwd::ForwardRecord(&FakeHostLog, sentinel::LogLevel::kWarn, "update", fields, std::size(fields)));
  QCHECK(g_seen.calls == 1);
  QCHECK(g_seen.level == nevr_quest::runtime_load::kLogLevelWarning);
  QCHECK(g_seen.event == "update");
  QCHECK(g_seen.tag == "NEVR.UPDATE");
  QCHECK(g_seen.message == "staged");
  constexpr std::size_t kForwardedExpected = 5;  // the seven fields less the lifted tag and message
  QCHECK(g_seen.fields.size() == kForwardedExpected);
  if (g_seen.fields.size() == kForwardedExpected) {
    constexpr std::size_t kFirst = 0, kCount = 1, kSecondTag = 2, kSecondMessage = 3, kLast = 4;
    QCHECK(g_seen.fields[kFirst] == std::make_pair(std::string("first"), std::string("a")));
    QCHECK(g_seen.fields[kCount] == std::make_pair(std::string("count"), std::string("-42")));  // an integer arrives as decimal text
    QCHECK(g_seen.fields[kSecondTag] == std::make_pair(std::string("tag"), std::string("second tag stays a field")));
    QCHECK(g_seen.fields[kSecondMessage] == std::make_pair(std::string("message"), std::string("second message stays a field")));
    QCHECK(g_seen.fields[kLast] == std::make_pair(std::string("last"), std::string("7")));
  }
}

void NoTagOrMessageGoesAcrossAsNull() {
  g_seen = Seen();
  const sentinel::LogField fields[] = {{"only", "x"}};
  QCHECK(fwd::ForwardRecord(&FakeHostLog, sentinel::LogLevel::kInfo, "event", fields, std::size(fields)));
  QCHECK(g_seen.tagNull);
  QCHECK(g_seen.messageNull);
  QCHECK(g_seen.fields.size() == std::size(fields));
  g_seen = Seen();
  QCHECK(fwd::ForwardRecord(&FakeHostLog, sentinel::LogLevel::kError, "event", nullptr, 0));
  QCHECK(g_seen.calls == 1);
  QCHECK(g_seen.level == nevr_quest::runtime_load::kLogLevelError);
  QCHECK(g_seen.fields.empty());
}

void IntegerTagAndMessageKeepTheirOwnText() {
  g_seen = Seen();
  constexpr long long kTagNumber = 5, kMessageNumber = 6, kLaterNumber = 7;
  const sentinel::LogField fields[] = {{"tag", kTagNumber}, {"message", kMessageNumber}, {"later", kLaterNumber}};
  QCHECK(fwd::ForwardRecord(&FakeHostLog, sentinel::LogLevel::kInfo, "event", fields, std::size(fields)));
  QCHECK(g_seen.tag == "5");  // each integer field has its own text; a later one does not overwrite it
  QCHECK(g_seen.message == "6");
  QCHECK(g_seen.fields.size() == 1);
  if (g_seen.fields.size() == 1) QCHECK(g_seen.fields[0] == std::make_pair(std::string("later"), std::string("7")));
}

void AFieldWithNoKeyIsDropped() {
  g_seen = Seen();
  sentinel::LogField keyless("x", "dropped");
  keyless.key = nullptr;
  const sentinel::LogField fields[] = {keyless, {"kept", "y"}};
  QCHECK(fwd::ForwardRecord(&FakeHostLog, sentinel::LogLevel::kInfo, "event", fields, std::size(fields)));
  constexpr std::size_t kKeptOnly = 1;
  QCHECK(g_seen.fields.size() == kKeptOnly);
  if (!g_seen.fields.empty()) QCHECK(g_seen.fields[0].first == "kept");
}

constexpr std::size_t kOverTheCap = 5;

void ALongRecordIsCutAtTheCapAndSaysSo() {
  g_seen = Seen();
  const std::size_t over = fwd::kMaxForwardedFields + kOverTheCap;
  std::vector<std::string> keys;
  for (std::size_t i = 0; i < over; ++i) keys.push_back("k" + std::to_string(i));
  std::vector<sentinel::LogField> fields;
  for (const std::string& key : keys) fields.emplace_back(key.c_str(), "v");
  QCHECK(fwd::ForwardRecord(&FakeHostLog, sentinel::LogLevel::kInfo, "event", fields.data(), fields.size()));
  QCHECK(g_seen.fields.size() == fwd::kMaxForwardedFields + 1);  // the cap, then the marker
  if (g_seen.fields.size() == fwd::kMaxForwardedFields + 1) {
    QCHECK(g_seen.fields[fwd::kMaxForwardedFields - 1].first == "k" + std::to_string(fwd::kMaxForwardedFields - 1));
    QCHECK(g_seen.fields.back() == std::make_pair(std::string("truncated"), std::string("true")));  // the words the shim reads
  }
  // exactly the cap is not a cut
  g_seen = Seen();
  QCHECK(fwd::ForwardRecord(&FakeHostLog, sentinel::LogLevel::kInfo, "event", fields.data(), fwd::kMaxForwardedFields));
  QCHECK(g_seen.fields.size() == fwd::kMaxForwardedFields);
}

}  // namespace

int main() {
  LevelMappings();
  NoHostLogLeavesTheRecordToTheCopysWriter();
  TagAndMessageAreLiftedOutAndTheRestFollowInOrder();
  NoTagOrMessageGoesAcrossAsNull();
  IntegerTagAndMessageKeepTheirOwnText();
  AFieldWithNoKeyIsDropped();
  ALongRecordIsCutAtTheCapAndSaysSo();
  return quest_test::Failures() == 0 ? 0 : 1;
}
