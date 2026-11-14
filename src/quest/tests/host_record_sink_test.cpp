// Host test for the host record sink of the hook log (src/quest/sentinel/hook_log.h). The runtime library registers the shim's
// log as the sink of its records (runtime/runtime_entry.cpp); a record the sink takes must not be written by this copy's own sink,
// and one it does not take, or one written with no host sink, must be.

#include <cstdio>
#include <cstring>
#include <string>

#include "quest/sentinel/hook_log.h"
#include "quest/tests/test_check.h"

namespace {

constexpr char kEvent[] = "host_sink_probe";
constexpr char kKey[] = "reason";
constexpr char kValue[] = "unit";
constexpr long long kNumber = 7;

int g_sinkCalls = 0;
bool g_sinkTakes = true;
sentinel::LogLevel g_sinkLevel = sentinel::LogLevel::kDebug;
std::string g_sinkEvent;
std::string g_sinkFirstKey;
std::string g_sinkFirstText;
long long g_sinkSecondNumber = 0;
std::size_t g_sinkCount = 0;

int g_ownCalls = 0;
std::string g_ownLine;

bool HostSink(sentinel::LogLevel level, const char* event, const sentinel::LogField* fields, std::size_t count) {
  ++g_sinkCalls;
  g_sinkLevel = level;
  g_sinkEvent = event;
  g_sinkCount = count;
  if (count >= 1) {
    g_sinkFirstKey = fields[0].key;
    g_sinkFirstText = fields[0].text != nullptr ? fields[0].text : "";
  }
  if (count >= 2) g_sinkSecondNumber = fields[1].number;
  return g_sinkTakes;
}

void OwnSink(sentinel::LogLevel /*level*/, const char* line) {
  ++g_ownCalls;
  g_ownLine = line;
}

void Reset() {
  g_sinkCalls = 0;
  g_ownCalls = 0;
  g_ownLine.clear();
}

void WriteOne() { sentinel::LogFields(sentinel::LogLevel::kWarn, kEvent, {{kKey, kValue}, {"count", kNumber}}); }

}  // namespace

int main() {
  const sentinel::LogSink previous = sentinel::SetLogSink(&OwnSink);

  // 1. no host sink: nothing is forwarded, and the record is written by the copy's own sink.
  sentinel::SetHostRecordSink(nullptr);
  Reset();
  const sentinel::LogField probe(kKey, kValue);
  QCHECK(!sentinel::ForwardRecordToHost(sentinel::LogLevel::kInfo, kEvent, &probe, 1));
  WriteOne();
  QCHECK(g_ownCalls == 1);
  QCHECK(g_ownLine.find(kEvent) != std::string::npos);

  // 2. a host sink that takes the record: it gets the level, the event and the fields as written, and the copy's own sink gets nothing.
  sentinel::SetHostRecordSink(&HostSink);
  g_sinkTakes = true;
  Reset();
  WriteOne();
  QCHECK(g_sinkCalls == 1);
  QCHECK(g_ownCalls == 0);
  QCHECK(g_sinkLevel == sentinel::LogLevel::kWarn);
  QCHECK(g_sinkEvent == kEvent);
  QCHECK(g_sinkCount == 2);
  QCHECK(g_sinkFirstKey == kKey);
  QCHECK(g_sinkFirstText == kValue);
  QCHECK(g_sinkSecondNumber == kNumber);

  // 3. a host sink that does not take the record: the copy's own sink writes it, once.
  g_sinkTakes = false;
  Reset();
  WriteOne();
  QCHECK(g_sinkCalls == 1);
  QCHECK(g_ownCalls == 1);
  QCHECK(g_ownLine.find(kEvent) != std::string::npos);

  // 4. the sink removed: back to the copy's own sink alone.
  sentinel::SetHostRecordSink(nullptr);
  Reset();
  WriteOne();
  QCHECK(g_sinkCalls == 0);
  QCHECK(g_ownCalls == 1);

  sentinel::SetLogSink(previous);
  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "host_record_sink_test: %d check(s) failed\n", quest_test::Failures());
    return 1;
  }
  std::printf("host_record_sink_test: all checks passed\n");
  return 0;
}
