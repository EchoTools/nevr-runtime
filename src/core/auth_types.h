#pragma once
// Interfaces of the platform-neutral auth core (token model, refresh, device-code
// flow). Each platform supplies an HttpClient (libcurl on Windows and Quest) and
// a Clock; tests supply fakes. No Windows headers and no game ABI, so the same
// translation units compile under MinGW and the Android NDK (ADR 0003).

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>

namespace nevr::auth {

enum class LogLevel { Debug, Info, Warning, Error };

// Receives one already-formatted line. The core never puts a token, refresh
// token, device code or full URL with a credential in a line it passes here.
using LogSink = std::function<void(LogLevel, const std::string&)>;

struct HttpResponse {
  // false: no HTTP response was obtained (DNS, connect, TLS, timeout). The
  // platform's own code goes in transport_code and is logged by number only.
  bool transport_ok = false;
  int transport_code = 0;
  long status = 0;
  std::string body;
};

class HttpClient {
 public:
  virtual ~HttpClient() = default;
  // POSTs `body` as application/json. Must be bounded in time and must verify
  // the server certificate. Must not throw.
  virtual HttpResponse PostJson(const std::string& url, const std::string& body) = 0;
  // Called from another thread at shutdown: a PostJson in flight returns promptly with
  // transport_ok=false, and later calls fail at once. Default: nothing to interrupt.
  virtual void Interrupt() {}
};

class Clock {
 public:
  virtual ~Clock() = default;
  virtual uint64_t UnixNow() = 0;
  virtual std::chrono::steady_clock::time_point SteadyNow() = 0;
  // Blocks up to `d`. Returns true when the wait was cut short because the owner
  // is shutting down, false when the full duration elapsed.
  virtual bool SleepFor(std::chrono::steady_clock::duration d) = 0;
};

}  // namespace nevr::auth
