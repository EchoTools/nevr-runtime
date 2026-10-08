#include "hook_log.h"

#include <time.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#ifdef __ANDROID__
#include <android/log.h>
#endif

namespace sentinel {

namespace {

std::atomic<LogSink> g_sink{nullptr};

void DefaultSink(LogLevel level, const char* line) {
#ifdef __ANDROID__
  const int priority = level == LogLevel::kError  ? ANDROID_LOG_ERROR
                       : level == LogLevel::kWarn ? ANDROID_LOG_WARN
                                                  : ANDROID_LOG_INFO;
  __android_log_write(priority, "NEVR-Sentinel", line);
#else
  static_cast<void>(level);
  std::fprintf(stderr, "NEVR-Sentinel %s\n", line);
#endif
}

const char* LevelName(LogLevel level) {
  return level == LogLevel::kError ? "error" : level == LogLevel::kWarn ? "warn" : "info";
}

constexpr std::size_t kLineCapacity = 768;
constexpr std::size_t kTail = 24;  // room for ,"truncated":true}\0

// Appends into a fixed buffer; once a write would not fit it stops and remembers.
class Writer {
 public:
  void Raw(const char* s) {
    for (; *s != '\0'; ++s) Put(*s);
  }
  void Quoted(const char* s) {
    Put('"');
    for (; *s != '\0'; ++s) {
      const unsigned char c = static_cast<unsigned char>(*s);
      if (c == '"' || c == '\\') {
        Put('\\');
        Put(static_cast<char>(c));
      } else if (c == '\n') {
        Raw("\\n");
      } else if (c == '\r') {
        Raw("\\r");
      } else if (c == '\t') {
        Raw("\\t");
      } else if (c < 0x20) {
        char esc[7];
        std::snprintf(esc, sizeof(esc), "\\u%04x", c);
        Raw(esc);
      } else if (c >= 0x80) {
        Put('?');
      } else {
        Put(static_cast<char>(c));
      }
    }
    Put('"');
  }
  void Number(long long n) {
    char digits[24];
    std::snprintf(digits, sizeof(digits), "%lld", n);
    Raw(digits);
  }
  // Room for `n` more bytes while still leaving the tail reserve.
  bool HasRoom(std::size_t n) const { return pos_ + kTail + n < kLineCapacity; }
  // Closes the object and returns the NUL-terminated line. The tail reserve
  // guarantees the marker and the brace fit.
  const char* Finish(bool truncated) {
    if (truncated) Raw(",\"truncated\":true");
    Put('}');
    buf_[pos_] = '\0';
    return buf_;
  }

 private:
  void Put(char c) {
    if (pos_ + 1 < kLineCapacity) buf_[pos_++] = c;
  }
  char buf_[kLineCapacity] = {};
  std::size_t pos_ = 0;
};

// Bytes Quoted() writes for `s`, excluding the two quotes.
std::size_t EscapedLength(const char* s) {
  std::size_t n = 0;
  for (; *s != '\0'; ++s) {
    const unsigned char c = static_cast<unsigned char>(*s);
    n += (c == '"' || c == '\\' || c == '\n' || c == '\r' || c == '\t') ? 2 : (c < 0x20 ? 6 : 1);
  }
  return n;
}

long long NowMs() {
  struct timespec ts;
  if (clock_gettime(CLOCK_REALTIME, &ts) != 0) return 0;
  return static_cast<long long>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

}  // namespace

LogSink SetLogSink(LogSink sink) { return g_sink.exchange(sink, std::memory_order_acq_rel); }

const char* HexString(char (&buf)[19], std::uint64_t value) {
  std::snprintf(buf, sizeof(buf), "0x%016llx", static_cast<unsigned long long>(value));
  return buf;
}

void LogFields(LogLevel level, const char* event, std::initializer_list<LogField> fields) {
  Writer w;
  w.Raw("{\"ts_ms\":");
  w.Number(NowMs());
  w.Raw(",\"level\":");
  w.Quoted(LevelName(level));
  w.Raw(",\"event\":");
  w.Quoted(event);
  bool truncated = false;
  for (const LogField& f : fields) {
    const std::size_t need =
        8 + EscapedLength(f.key) + (f.text != nullptr ? EscapedLength(f.text) : 20);
    if (!w.HasRoom(need)) {
      truncated = true;
      break;
    }
    w.Raw(",");
    w.Quoted(f.key);
    w.Raw(":");
    if (f.text != nullptr) {
      w.Quoted(f.text);
    } else {
      w.Number(f.number);
    }
  }
  const char* line = w.Finish(truncated);
  const LogSink sink = g_sink.load(std::memory_order_acquire);
  (sink != nullptr ? sink : DefaultSink)(level, line);
}

void LogEvent(LogLevel level, const char* format, ...) {
  char message[400];
  va_list args;
  va_start(args, format);
  std::vsnprintf(message, sizeof(message), format, args);
  va_end(args);
  LogFields(level, "message", {{"msg", message}});
}

}  // namespace sentinel
