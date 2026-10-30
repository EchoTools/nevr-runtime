#include "core/crash_report.h"

#include <atomic>
#include <cstring>

// No platform header on purpose: this file builds into nevr_core (PC) and into the Quest sentinel, and runs in a
// fault handler. Everything it holds is constant-initialized and trivially destructible, so it adds no load-time
// initializer (tools/check_quest_static_init.sh) and nothing runs at exit.

namespace nevr_crash {

namespace {

// Appends into a caller-owned buffer and stays NUL-terminated. Once a write does not fit it stops and remembers.
class Out {
 public:
  Out(char* buffer, std::size_t capacity) : buffer_(buffer), capacity_(capacity) {
    if (capacity_ > 0) buffer_[0] = '\0';
  }

  void Char(char c) {
    if (length_ + 1 < capacity_) {
      buffer_[length_++] = c;
      buffer_[length_] = '\0';
    } else {
      truncated_ = true;
    }
  }

  void Str(const char* text) {
    if (text == nullptr) return;
    for (; *text != '\0'; ++text) Char(*text);
  }

  void Dec(std::int64_t value) {
    std::uint64_t magnitude = static_cast<std::uint64_t>(value);
    if (value < 0) {
      Char('-');
      magnitude = ~magnitude + 1;
    }
    char digits[24];
    std::size_t count = 0;
    do {
      digits[count++] = static_cast<char>('0' + magnitude % 10);
      magnitude /= 10;
    } while (magnitude != 0);
    while (count > 0) Char(digits[--count]);
  }

  // "0x" and the digits, at least `min_digits` of them.
  void Hex(std::uint64_t value, int min_digits) {
    Char('0');
    Char('x');
    char digits[16];
    int count = 0;
    do {
      digits[count++] = "0123456789abcdef"[value & 0xf];
      value >>= 4;
    } while (value != 0);
    for (int pad = count; pad < min_digits; ++pad) Char('0');
    while (count > 0) Char(digits[--count]);
  }

  std::size_t size() const { return length_; }
  bool truncated() const { return truncated_; }

 private:
  char* buffer_;
  std::size_t capacity_;
  std::size_t length_ = 0;
  bool truncated_ = false;
};

// One slot of the ring of the last log lines. A sequence counter that is odd while a writer holds the slot lets
// the crash path skip a line it would otherwise read torn; the bytes are atomics so the race is not undefined.
struct Slot {
  std::atomic<std::uint32_t> seq;
  std::atomic<std::uint64_t> index_plus_one;
  std::atomic<std::uint32_t> length;
  std::atomic<char> text[kLogLineBytes];
};

Slot g_slots[kLogLineCount];
std::atomic<std::uint64_t> g_next{0};

std::atomic<bool> g_busy{false};
char g_report[kReportCapacity];
char g_summary[kSummaryCapacity];

// Zeroes `location`, asks the platform, and makes every field a NUL-terminated string whatever the resolver did.
bool Resolve(ResolveFn resolve, std::uint64_t address, Location* location) {
  std::memset(location, 0, sizeof(*location));
  if (resolve == nullptr) return false;
  const bool found = resolve(address, location);
  location->module[sizeof(location->module) - 1] = '\0';
  location->build_id[sizeof(location->build_id) - 1] = '\0';
  location->symbol[sizeof(location->symbol) - 1] = '\0';
  return found && location->module[0] != '\0';
}

// `module+0xoffset`, or `?+0xaddress` when no module holds the address.
void AppendModuleOffset(Out& out, std::uint64_t address, const Location& location, bool found) {
  if (found) {
    out.Str(location.module);
    out.Char('+');
    out.Hex(address - location.module_base, 1);
  } else {
    out.Str("?+");
    out.Hex(address, 1);
  }
}

// `symbol` or `symbol+0xoffset`, or `?` when the module exports nothing at or below the address.
void AppendSymbol(Out& out, const Location& location, bool found) {
  if (!found || location.symbol[0] == '\0') {
    out.Char('?');
    return;
  }
  out.Str(location.symbol);
  if (location.symbol_offset != 0) {
    out.Char('+');
    out.Hex(location.symbol_offset, 1);
  }
}

void AppendSignal(Out& out, const Report& report) {
  out.Str(report.signal_name != nullptr ? report.signal_name : "unknown");
}

void AppendAddressLine(Out& out, const char* label, std::uint64_t address, ResolveFn resolve) {
  Location location;
  const bool found = Resolve(resolve, address, &location);
  out.Str(label);
  out.Str(": ");
  AppendModuleOffset(out, address, location, found);
  out.Str(" build_id=");
  out.Str(found && location.build_id[0] != '\0' ? location.build_id : "none");
  out.Str(" symbol=");
  AppendSymbol(out, location, found);
  out.Char('\n');
}

void AppendLogLines(Out& out) {
  const std::uint64_t next = g_next.load(std::memory_order_acquire);
  const std::uint64_t first = next > kLogLineCount ? next - kLogLineCount : 0;
  const std::uint64_t wanted = next - first;
  std::uint64_t kept = 0;

  out.Str("log (last ");
  out.Dec(static_cast<std::int64_t>(wanted));
  out.Str(" lines, oldest first):\n");
  for (std::uint64_t index = first; index < next; ++index) {
    const Slot& slot = g_slots[index % kLogLineCount];
    const std::uint32_t before = slot.seq.load(std::memory_order_acquire);
    if ((before & 1u) != 0 || slot.index_plus_one.load(std::memory_order_relaxed) != index + 1) continue;
    char line[kLogLineBytes + 1];
    std::uint32_t length = slot.length.load(std::memory_order_relaxed);
    if (length > kLogLineBytes) length = kLogLineBytes;
    for (std::uint32_t i = 0; i < length; ++i) line[i] = slot.text[i].load(std::memory_order_relaxed);
    line[length] = '\0';
    std::atomic_thread_fence(std::memory_order_acquire);
    if (slot.seq.load(std::memory_order_relaxed) != before) continue;
    out.Str("  ");
    out.Str(line);
    out.Char('\n');
    ++kept;
  }
  if (kept != wanted) {
    out.Str("  (");
    out.Dec(static_cast<std::int64_t>(wanted - kept));
    out.Str(" lines were being written when the crash hit and are left out)\n");
  }
}

}  // namespace

void CopyText(char* destination, std::size_t capacity, const char* source) {
  if (destination == nullptr || capacity == 0) return;
  std::size_t length = 0;
  if (source != nullptr) {
    while (source[length] != '\0' && length + 1 < capacity) {
      destination[length] = source[length];
      ++length;
    }
  }
  destination[length] = '\0';
}

void HexText(char* destination, std::size_t capacity, const std::uint8_t* bytes, std::size_t count) {
  if (destination == nullptr || capacity == 0) return;
  std::size_t length = 0;
  for (std::size_t i = 0; bytes != nullptr && i < count && length + 2 < capacity; ++i) {
    destination[length++] = "0123456789abcdef"[bytes[i] >> 4];
    destination[length++] = "0123456789abcdef"[bytes[i] & 0xf];
  }
  destination[length] = '\0';
}

void RecordLogLine(const char* line, std::size_t length) {
  if (line == nullptr) return;
  const std::uint64_t index = g_next.fetch_add(1, std::memory_order_relaxed);
  Slot& slot = g_slots[index % kLogLineCount];
  const std::uint32_t held = slot.seq.load(std::memory_order_relaxed) | 1u;
  slot.seq.store(held, std::memory_order_relaxed);
  std::atomic_thread_fence(std::memory_order_release);
  const std::size_t count = length < kLogLineBytes ? length : kLogLineBytes;
  for (std::size_t i = 0; i < count; ++i) {
    char c = line[i];
    if (static_cast<unsigned char>(c) < 0x20 || c == 0x7f) c = ' ';
    slot.text[i].store(c, std::memory_order_relaxed);
  }
  slot.length.store(static_cast<std::uint32_t>(count), std::memory_order_relaxed);
  slot.index_plus_one.store(index + 1, std::memory_order_relaxed);
  slot.seq.store(held + 1, std::memory_order_release);
}

std::size_t FormatSummary(const Report& report, ResolveFn resolve, const char* report_path, bool written,
                          char* buffer, std::size_t capacity) {
  Out out(buffer, capacity);
  Location location;
  const bool found = Resolve(resolve, report.pc, &location);
  out.Str("crash: ");
  AppendSignal(out, report);
  out.Char(' ');
  AppendModuleOffset(out, report.pc, location, found);
  out.Char(' ');
  AppendSymbol(out, location, found);
  out.Char(' ');
  if (written) {
    out.Str("report=");
    out.Str(report_path);
  } else {
    out.Str("report=none");
    if (report_path != nullptr && report_path[0] != '\0') {
      out.Str(" path=");
      out.Str(report_path);
    }
    out.Str(" write_failed=1");
  }
  return out.size();
}

std::size_t FormatReport(const Report& report, ResolveFn resolve, const char* report_path, char* buffer,
                         std::size_t capacity) {
  Out out(buffer, capacity);
  out.Str("nevr crash report v1\n");

  char summary[kSummaryCapacity];
  FormatSummary(report, resolve, report_path, true, summary, sizeof(summary));
  out.Str("summary: ");
  out.Str(summary);
  out.Char('\n');

  out.Str("signal: ");
  AppendSignal(out, report);
  out.Str(" (");
  if (report.signal_number_hex) {
    out.Hex(static_cast<std::uint64_t>(report.signal_number), 8);
  } else {
    out.Dec(report.signal_number);
  }
  out.Str(") code=");
  out.Dec(report.fault_code);
  if (report.has_fault_address) {
    out.Str(" fault_addr=");
    out.Hex(report.fault_address, 16);
  } else {
    out.Str(" fault_addr=none");
  }
  out.Char('\n');

  out.Str("thread: name=");
  out.Str(report.thread_name[0] != '\0' ? report.thread_name : "unknown");
  out.Str(" tid=");
  out.Dec(static_cast<std::int64_t>(report.thread_id));
  out.Char('\n');

  AppendAddressLine(out, "pc", report.pc, resolve);
  if (report.has_lr) AppendAddressLine(out, "lr", report.lr, resolve);

  out.Str("registers:\n");
  const std::size_t registers = report.register_count < kMaxRegisters ? report.register_count : kMaxRegisters;
  for (std::size_t i = 0; i < registers; ++i) {
    if (i % 3 == 0) out.Str("  ");
    out.Str(report.registers[i].name);
    out.Char('=');
    out.Hex(report.registers[i].value, 16);
    out.Char((i % 3 == 2 || i + 1 == registers) ? '\n' : ' ');
  }

  out.Str("backtrace:\n");
  const std::size_t frames = report.frame_count < kMaxFrames ? report.frame_count : kMaxFrames;
  for (std::size_t i = 0; i < frames; ++i) {
    Location location;
    const bool found = Resolve(resolve, report.frames[i], &location);
    out.Str("  #");
    out.Dec(static_cast<std::int64_t>(i));
    out.Char(' ');
    AppendModuleOffset(out, report.frames[i], location, found);
    out.Char(' ');
    AppendSymbol(out, location, found);
    out.Char('\n');
  }

  AppendLogLines(out);
  out.Str("end of report\n");

  if (out.truncated()) {
    // Cut mid-line: end on a marker so a reader knows the report did not fit its buffer.
    static const char kMarker[] = "\n[report truncated]\n";
    const std::size_t marker = sizeof(kMarker) - 1;
    if (capacity > marker) {
      std::memcpy(buffer + capacity - 1 - marker, kMarker, marker + 1);
      return capacity - 1;
    }
  }
  return out.size();
}

namespace {

std::atomic<const Platform*> g_platform{nullptr};
std::atomic<bool> g_written{false};
Report g_state;
char g_path[512];
char g_temp[520];
char g_last[512];

}  // namespace

void SetPlatform(const Platform* platform) { g_platform.store(platform, std::memory_order_release); }

bool ReportWritten() { return g_written.load(std::memory_order_acquire); }

const char* LastReportPath() { return g_last; }

bool WriteReport(const CrashInfo& info) noexcept {
  const Platform* platform = g_platform.load(std::memory_order_acquire);
  if (platform == nullptr) return false;
  if (g_busy.exchange(true, std::memory_order_acq_rel)) return false;

  Report& report = g_state;
  std::memset(&report, 0, sizeof(report));
  report.signal_number = info.code;
  report.fault_code = info.fault_code;
  report.fault_address = info.fault_address;
  report.pc = info.pc;
  report.lr = info.lr;
  report.thread_id = info.thread_id;
  if (platform->describe != nullptr) platform->describe(info, &report);

  g_path[0] = '\0';
  bool have_path = platform->report_path != nullptr && platform->report_path(info, g_path, sizeof(g_path)) &&
                   g_path[0] != '\0';
  const std::size_t path_length = have_path ? std::strlen(g_path) : 0;
  // The temporary name ends in .tmp, which no scan of the crash directory for a finished report matches.
  if (have_path && path_length + 5 <= sizeof(g_temp)) {
    std::memcpy(g_temp, g_path, path_length);
    std::memcpy(g_temp + path_length, ".tmp", 5);
  } else {
    have_path = false;
  }

  bool written = false;
  if (have_path && platform->open_file != nullptr && platform->write_file != nullptr &&
      platform->close_file != nullptr && platform->rename_file != nullptr) {
    // A symbol lookup can take a lock the faulting thread holds (dladdr on Quest). So a report with raw addresses only
    // is published first, then replaced by the symbolized one: a hang in the second leaves the first, whole.
    const ResolveFn passes[2] = {nullptr, platform->resolve};
    for (std::size_t pass = 0; pass < (platform->resolve != nullptr ? 2u : 1u); ++pass) {
      const std::size_t length = FormatReport(report, passes[pass], g_path, g_report, sizeof(g_report));
      const std::intptr_t handle = platform->open_file(g_temp);
      bool pass_ok = false;
      if (handle != -1) {
        pass_ok = platform->write_file(handle, g_report, length);
        platform->close_file(handle);
      }
      if (pass_ok && platform->rename_file(g_temp, g_path)) written = true;  // a report is published, whole
    }
  }
  const std::size_t summary =
      FormatSummary(report, platform->resolve, have_path ? g_path : nullptr, written, g_summary, sizeof(g_summary));
  if (platform->log_summary != nullptr && summary > 0) platform->log_summary(g_summary);

  if (written) {
    CopyText(g_last, sizeof(g_last), g_path);
    g_written.store(true, std::memory_order_release);
  }
  g_busy.store(false, std::memory_order_release);
  return written;
}

void ResetForTest() {
  g_next.store(0, std::memory_order_relaxed);
  for (Slot& slot : g_slots) {
    slot.seq.store(0, std::memory_order_relaxed);
    slot.index_plus_one.store(0, std::memory_order_relaxed);
    slot.length.store(0, std::memory_order_relaxed);
  }
  g_platform.store(nullptr, std::memory_order_relaxed);
  g_written.store(false, std::memory_order_relaxed);
  g_last[0] = '\0';
  g_busy.store(false, std::memory_order_relaxed);
}

}  // namespace nevr_crash
