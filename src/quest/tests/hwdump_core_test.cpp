// Host test for the hardware dump's platform-neutral core (#335): the field schema, the procfs/sysfs readers
// against a fake root, and the record table the libr15 hooks write.
#include "quest/diag/hwdump_field.h"
#include "quest/diag/hwdump_fs.h"
#include "quest/diag/hwdump_os.h"
#include "quest/diag/hwdump_records.h"
#include "quest/diag/hwdump_report.h"

#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace {

int g_failures = 0;

#define CHECK(cond)                                                              \
  do {                                                                           \
    if (!(cond)) {                                                               \
      std::fprintf(stderr, "%s:%d CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                              \
    }                                                                            \
  } while (0)

using nevr_quest::hwdump::Answer;
using nevr_quest::hwdump::QueryFn;
using nevr_quest::hwdump::RecordSnapshot;
using nevr_quest::hwdump::RecordTable;
namespace hw = nevr_quest::hwdump;

// A failed read is a field with an error and a source, never a missing field, and never a value.
void FieldShape() {
  const nlohmann::json ok = hw::Ok("read /proc/x", 42);
  CHECK(ok.at("ok") == true && ok.at("source") == "read /proc/x" && ok.at("value") == 42);
  CHECK(!ok.contains("error"));
  const nlohmann::json bad = hw::Fail("read /proc/y", hw::ErrnoText("open", EACCES));
  CHECK(bad.contains("source") && bad.value("source", "") == "read /proc/y");
  CHECK(bad.at("ok") == false);
  CHECK(!bad.contains("value"));
  CHECK(bad.at("error").get<std::string>().find("open failed:") == 0);
  CHECK(bad.at("error").get<std::string>().find("(errno 13)") != std::string::npos);
  CHECK(hw::Fail("s", "").at("error") == "unknown error");  // an error is never blank

  nlohmann::json doc;
  doc["a"] = ok;
  doc["b"]["c"] = bad;
  doc["list"] = nlohmann::json::array({hw::Ok("s", nlohmann::json::object({{"ok", false}})), hw::Fail("t", "x")});
  std::size_t total = 0, failed = 0;
  hw::CountFields(doc, &total, &failed);
  CHECK(total == 4);   // a value that looks like a field is data, not a field
  CHECK(failed == 2);
}

std::string MakeRoot() {
  char tmpl[] = "/var/tmp/hwdump-test-XXXXXX";
  const char* dir = ::mkdtemp(tmpl);
  return dir != nullptr ? std::string(dir) : std::string();
}

void Write(const std::string& path, const std::string& text) {
  std::ofstream f(path, std::ios::binary);
  f << text;
}

void ReadersAgainstAFakeRoot() {
  const std::string root = MakeRoot();
  CHECK(!root.empty());
  ::mkdir((root + "/proc").c_str(), 0755);
  ::mkdir((root + "/sys").c_str(), 0755);
  ::mkdir((root + "/sys/cpu").c_str(), 0755);
  Write(root + "/proc/cpuinfo", "processor\t: 0\nprocessor\t: 1\n");
  Write(root + "/sys/cpu/online", "0-7\n");
  ::mkdir((root + "/sys/cpu/cpu0").c_str(), 0755);
  ::mkdir((root + "/sys/cpu/cpu1").c_str(), 0755);
  ::mkdir((root + "/sys/cpu/cpufreq").c_str(), 0755);

  const nlohmann::json raw = hw::ReadTextFile(root, "/proc/cpuinfo");
  CHECK(raw.at("ok") == true && raw.at("value") == "processor\t: 0\nprocessor\t: 1\n");
  CHECK(raw.at("source") == "read /proc/cpuinfo");  // the source names the device path, not the test root

  const nlohmann::json attr = hw::ReadAttribute(root, "/sys/cpu/online");
  CHECK(attr.at("ok") == true && attr.at("value") == "0-7");

  const nlohmann::json cut = hw::ReadTextFile(root, "/proc/cpuinfo", 5);
  CHECK(cut.at("ok") == true && cut.at("value") == "proce" && cut.value("truncated", false));

  const nlohmann::json missing = hw::ReadAttribute(root, "/sys/class/net/wlan0/address");
  CHECK(missing.at("ok") == false && !missing.contains("value"));
  CHECK(missing.at("error").get<std::string>().find("errno 2") != std::string::npos);  // ENOENT

  if (::geteuid() != 0) {  // root reads a mode-000 file anyway
    Write(root + "/sys/cpu/secret", "x\n");
    ::chmod((root + "/sys/cpu/secret").c_str(), 0);
    const nlohmann::json denied = hw::ReadAttribute(root, "/sys/cpu/secret");
    CHECK(denied.at("ok") == false);
    CHECK(denied.at("error").get<std::string>().find("errno 13") != std::string::npos);  // EACCES
  }

  const nlohmann::json listing = hw::ListDirectory(root, "/sys/cpu", "cpu");
  CHECK(listing.at("ok") == true);
  CHECK((hw::ListedNames(listing) == std::vector<std::string>{"cpu0", "cpu1", "cpufreq"}));
  const nlohmann::json no_dir = hw::ListDirectory(root, "/sys/class/thermal");
  CHECK(no_dir.at("ok") == false && hw::ListedNames(no_dir).empty());

  const nlohmann::json vfs = hw::StatVfs(root, "/proc");
  CHECK(vfs.at("ok") == true && vfs.at("value").at("total_bytes").get<unsigned long long>() > 0);
  CHECK(hw::StatVfs(root, "/nonexistent").at("ok") == false);

  ::chmod((root + "/sys/cpu/secret").c_str(), 0600);
  std::error_code ec;
  std::filesystem::remove_all(root, ec);  // exactly the directory mkdtemp made above
  CHECK(!ec);
}

Answer TextAnswer(const char* s) {
  Answer a;
  hw::SetText(&a, s);
  return a;
}

void RecordsDedupeAndKeepFirstAndLast() {
  static RecordTable table;
  Answer one;
  hw::AddFloat(&one, 72.0F);
  Answer two;
  hw::AddFloat(&two, 90.0F);
  table.Record(QueryFn::kVrapiGetSystemPropertyFloat, 4, nullptr, one);
  table.Record(QueryFn::kVrapiGetSystemPropertyFloat, 4, nullptr, two);
  table.Record(QueryFn::kVrapiGetSystemPropertyFloat, 4, nullptr, two);
  table.Record(QueryFn::kVrapiGetSystemPropertyInt, 4, nullptr, one);  // same id, other function
  table.Record(QueryFn::kSystemPropertyGet, 0, "ro.product.model", TextAnswer("Quest 2"));
  table.Record(QueryFn::kSystemPropertyGet, 0, "ro.build.version.incremental", TextAnswer("123"));
  table.Record(QueryFn::kSystemPropertyGet, 0, "ro.product.model", TextAnswer("Quest 2"));

  RecordSnapshot snap[hw::kMaxRecords];
  const std::size_t n = table.Snapshot(snap, hw::kMaxRecords);
  CHECK(n == 4);
  CHECK(snap[0].fn == QueryFn::kVrapiGetSystemPropertyFloat && snap[0].id == 4 && snap[0].calls == 3);
  CHECK(snap[0].first.num_floats == 1 && snap[0].first.floats[0] == 72.0F);
  CHECK(snap[0].last.num_floats == 1 && snap[0].last.floats[0] == 90.0F && snap[0].last_current);
  CHECK(snap[1].fn == QueryFn::kVrapiGetSystemPropertyInt && snap[1].calls == 1);
  CHECK(std::string(snap[2].name) == "ro.product.model" && snap[2].calls == 2);
  CHECK(std::string(snap[2].first.text) == "Quest 2" && snap[2].first.has_text);
  CHECK(std::string(snap[3].name) == "ro.build.version.incremental" && snap[3].calls == 1);
  CHECK(table.overflow() == 0);
}

void LongTextIsCutAndMarked() {
  std::string longText(400, 'x');
  const Answer a = TextAnswer(longText.c_str());
  CHECK(a.text_truncated && std::string(a.text).size() == hw::kTextBytes - 1);
  const Answer b = TextAnswer("short");
  CHECK(!b.text_truncated);
  Answer many;
  for (int i = 0; i < 40; ++i) hw::AddFloat(&many, static_cast<float>(i));
  CHECK(many.num_floats == hw::kMaxFloats);  // bounded, never past the array
}

void AFullTableCountsTheOverflowAndDropsOnlyTheRecord() {
  static RecordTable table;
  const Answer a;
  for (std::int64_t id = 0; id < static_cast<std::int64_t>(hw::kMaxRecords) + 5; ++id) {
    table.Record(QueryFn::kSysconf, id, nullptr, a);
  }
  table.Record(QueryFn::kSysconf, 0, nullptr, a);  // an existing key still counts after the table is full
  RecordSnapshot snap[hw::kMaxRecords];
  CHECK(table.Snapshot(snap, hw::kMaxRecords) == hw::kMaxRecords);
  CHECK(table.overflow() == 5);
  CHECK(snap[0].calls == 2);
}

void ConcurrentWritersAndAReader() {
  static RecordTable table;
  std::vector<std::thread> threads;
  for (int t = 0; t < 4; ++t) {
    threads.emplace_back([t] {
      for (int i = 0; i < 20000; ++i) {
        Answer a;
        hw::AddInt(&a, i);
        table.Record(QueryFn::kVrapiGetSystemStatusInt, i % 8, nullptr, a);
        if (t == 0 && i % 1000 == 0) {
          RecordSnapshot snap[hw::kMaxRecords];
          (void)table.Snapshot(snap, hw::kMaxRecords);
        }
      }
    });
  }
  for (auto& th : threads) th.join();
  RecordSnapshot snap[hw::kMaxRecords];
  const std::size_t n = table.Snapshot(snap, hw::kMaxRecords);
  std::uint64_t calls = 0;
  for (std::size_t i = 0; i < n; ++i) calls += snap[i].calls;
  CHECK(calls == 4U * 20000U);  // every call counted, including those whose answer update was skipped
  CHECK(n >= 8 && table.overflow() == 0);
}

bool IsField(const nlohmann::json& j) { return j.is_object() && j.contains("ok") && j.contains("source"); }

// The os section never omits a field: against an empty root every fixed path is still there, and every one
// that reads a file is a failure with an error; against a populated root those read.
void OsSectionNeverOmitsAField() {
  const std::vector<std::string> file_based = {
      "/kernel_version",  "/cpu/cpuinfo_raw",          "/cpu/sys/present",           "/cpu/sys/possible",
      "/cpu/sys/online",  "/cpu/cpufreq/listing",      "/memory/meminfo_raw",        "/memory/self_status_raw",
      "/net/sys_class_net/listing", "/thermal/zones/listing", "/battery/power_supply/listing"};

  const std::string empty = MakeRoot();
  hw::OsInputs in;
  in.root = empty;
  in.statvfs_paths = {"/data", "/"};
  const nlohmann::json bare = hw::ComposeOs(in);
  for (const std::string& path : hw::FixedOsFieldPaths()) {
    const nlohmann::json::json_pointer ptr(path);
    CHECK(bare.contains(ptr));
    if (bare.contains(ptr)) CHECK(IsField(bare.at(ptr)));
  }
  for (const std::string& path : file_based) {
    CHECK(bare.at(nlohmann::json::json_pointer(path)).at("ok") == false);
    CHECK(bare.at(nlohmann::json::json_pointer(path)).contains("error"));
  }
  CHECK(bare.at("/storage_statvfs/~1data"_json_pointer).at("ok") == false);  // no such dir under the root
  CHECK(bare.at("/storage_statvfs/~1"_json_pointer).at("ok") == true);       // the root itself exists
  CHECK(bare.at("/properties"_json_pointer).at("ok") == false);              // host: not Android
  CHECK(bare.at("/uname"_json_pointer).at("ok") == true);
  std::size_t total = 0, failed = 0;
  hw::CountFields(bare, &total, &failed);
  CHECK(total >= hw::FixedOsFieldPaths().size() + hw::WantedProperties().size());

  const std::string full = MakeRoot();
  for (const char* dir : {"/proc", "/proc/self", "/sys", "/sys/devices", "/sys/devices/system",
                          "/sys/devices/system/cpu", "/sys/devices/system/cpu/cpu0",
                          "/sys/devices/system/cpu/cpu0/cpufreq", "/sys/class", "/sys/class/net",
                          "/sys/class/net/wlan0", "/sys/class/thermal", "/sys/class/thermal/thermal_zone0",
                          "/sys/class/power_supply", "/sys/class/power_supply/battery"}) {
    ::mkdir((full + dir).c_str(), 0755);
  }
  Write(full + "/proc/version", "Linux version 5.4\n");
  Write(full + "/proc/cpuinfo", "processor\t: 0\n");
  Write(full + "/proc/meminfo", "MemTotal: 1 kB\n");
  Write(full + "/proc/self/status", "Name: x\n");
  for (const char* f : {"present", "possible", "online"}) Write(full + "/sys/devices/system/cpu/" + f, "0-7\n");
  Write(full + "/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq", "1000\n");
  Write(full + "/sys/class/net/wlan0/address", "aa:bb:cc:dd:ee:ff\n");
  Write(full + "/sys/class/thermal/thermal_zone0/temp", "41000\n");
  Write(full + "/sys/class/power_supply/battery/capacity", "88\n");
  in.root = full;
  const nlohmann::json filled = hw::ComposeOs(in);
  for (const std::string& path : file_based) CHECK(filled.at(nlohmann::json::json_pointer(path)).at("ok") == true);
  CHECK(filled.at("/cpu/cpufreq/entries/cpu0/cpufreq~1scaling_cur_freq"_json_pointer).at("value") == "1000");
  CHECK(filled.at("/cpu/cpufreq/entries/cpu0/cpufreq~1scaling_governor"_json_pointer).at("ok") == false);
  CHECK(filled.at("/net/sys_class_net/entries/wlan0/address"_json_pointer).at("value") == "aa:bb:cc:dd:ee:ff");
  CHECK(filled.at("/thermal/zones/entries/thermal_zone0/temp"_json_pointer).at("value") == "41000");
  CHECK(filled.at("/battery/power_supply/entries/battery/capacity"_json_pointer).at("value") == "88");
  std::error_code ec;
  std::filesystem::remove_all(empty, ec);
  std::filesystem::remove_all(full, ec);
}

// When the dump runs: 30 s after vrapi_Initialize, or 180 s after start when it never comes.
void TriggerTiming() {
  const std::uint64_t start = 1000;
  CHECK(!hw::Decide(start, start, false, 0).due);
  CHECK(!hw::Decide(start + hw::kFallbackAfterStartMs - 1, start, false, 0).due);
  const hw::DumpDecision fallback = hw::Decide(start + hw::kFallbackAfterStartMs, start, false, 0);
  CHECK(fallback.due && std::string(fallback.reason) == "fallback_no_vrapi_initialize");
  const std::uint64_t init = start + 20000;
  CHECK(!hw::Decide(init + hw::kSettleAfterInitializeMs - 1, start, true, init).due);
  const hw::DumpDecision settled = hw::Decide(init + hw::kSettleAfterInitializeMs, start, true, init);
  CHECK(settled.due && std::string(settled.reason) == "after_vrapi_initialize");
  // Initialize seen late: the settle time counts from Initialize, not from the fallback.
  const std::uint64_t late = start + hw::kFallbackAfterStartMs - 1000;
  CHECK(!hw::Decide(start + hw::kFallbackAfterStartMs + 1000, start, true, late).due);
}

// The log line carries counts and the path, never a value from the dump.
void LogLineCarriesNoValue() {
  hw::WriteSummary s;
  s.stage = "os";
  s.path = "/sdcard/Android/data/com.readyatdawn.r15/files/nevr-hwdump.json";
  s.written = true;
  s.fields = 412;
  s.failed = 37;
  s.hooks_installed = 18;
  s.hooks_total = 18;
  s.records = 23;
  const std::string line = hw::LogLine(s);
  CHECK(line == "hwdump written stage=os path=/sdcard/Android/data/com.readyatdawn.r15/files/nevr-hwdump.json "
                "fields=412 failed=37 hooks_installed=18/18 hook_faults=0 records=23 overflow=0");
  s.written = false;
  s.write_error = "open: Permission denied";
  CHECK(hw::LogLine(s).find("hwdump write_failed stage=os") == 0);
  CHECK(hw::LogLine(s).find("error=open: Permission denied") != std::string::npos);

  // A dump whose values carry a marker: composing its queries section and summarising it never puts the
  // marker in the line.
  RecordTable table;
  Answer a = TextAnswer("SERIAL-MARKER-9931");
  table.Record(QueryFn::kSystemPropertyGet, 0, "ro.serialno", a);
  RecordSnapshot snap[hw::kMaxRecords];
  const std::size_t n = table.Snapshot(snap, hw::kMaxRecords);
  hw::HookState hooks[1];
  hooks[0].symbol = "__system_property_get";
  hooks[0].slot = 0x36ee730;
  hooks[0].installed = true;
  hooks[0].status = "ok";
  const nlohmann::json q = hw::ComposeQueries(snap, n, hooks, 1, 0, 0);
  hw::WriteSummary w;
  w.stage = "os";
  w.path = "p";
  w.written = true;
  hw::CountFields(q, &w.fields, &w.failed);
  w.records = n;
  CHECK(hw::LogLine(w).find("SERIAL-MARKER-9931") == std::string::npos);
  CHECK(q.dump().find("SERIAL-MARKER-9931") != std::string::npos);  // the value is in the file, not the log
}

void QueriesSectionShape() {
  RecordTable table;
  Answer f;
  hw::AddFloat(&f, 72.0F);
  table.Record(QueryFn::kVrapiGetSystemPropertyFloat, 4, nullptr, f);
  RecordSnapshot snap[hw::kMaxRecords];
  const std::size_t n = table.Snapshot(snap, hw::kMaxRecords);
  hw::HookState hooks[2];
  hooks[0].symbol = "vrapi_GetSystemPropertyFloat";
  hooks[0].slot = 0x36c06f8;
  hooks[0].installed = true;
  hooks[0].status = "ok";
  hooks[0].calls = 9;
  hooks[1].symbol = "uname";  // never ran: still listed, with its status
  const nlohmann::json q = hw::ComposeQueries(snap, n, hooks, 2, 3, 1);
  for (const char* k : {"hooks", "records", "table", "not_hooked"}) {
    CHECK(q.contains(k) && IsField(q.value(k, nlohmann::json())));
  }
  if (!q.contains("not_hooked") || !q.contains("hooks")) return;
  CHECK(q.at("hooks").at("value").size() == 2);
  CHECK(q.at("hooks").at("value")[0].at("slot") == "0x36c06f8");
  CHECK(q.at("hooks").at("value")[1].at("status") == "not_run");
  const nlohmann::json rec = q.at("records").at("value")[0];
  CHECK(rec.at("function") == "vrapi_GetSystemPropertyFloat" && rec.at("id") == 4 && rec.at("calls") == 1);
  CHECK(rec.at("first").at("floats")[0] == 72.0);
  CHECK(q.at("table").at("value").at("overflow") == 3 && q.at("table").at("value").at("contended") == 1);
  CHECK(q.at("not_hooked").at("value").size() == 3);
}

}  // namespace

int main() {
  FieldShape();
  ReadersAgainstAFakeRoot();
  RecordsDedupeAndKeepFirstAndLast();
  LongTextIsCutAndMarked();
  AFullTableCountsTheOverflowAndDropsOnlyTheRecord();
  ConcurrentWritersAndAReader();
  OsSectionNeverOmitsAField();
  TriggerTiming();
  LogLineCarriesNoValue();
  QueriesSectionShape();
  if (g_failures != 0) {
    std::fprintf(stderr, "hwdump_core_test: %d check(s) failed\n", g_failures);
    return 1;
  }
  std::printf("hwdump_core_test: all checks pass\n");
  return 0;
}
